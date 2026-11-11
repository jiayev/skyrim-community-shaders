#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/RemoteControl/Gpa.h"

#	include "Globals.h"
#	include "RenderGraph/DxvkOrgInterop.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <gpu_performance_api/gpu_perf_api_vk.h>

#	include <atomic>
#	include <cstring>
#	include <format>
#	include <map>
#	include <mutex>
#	include <set>
#	include <string>
#	include <unordered_set>
#	include <vector>

/*
 * GPUPerfAPI's Vulkan backend: DXVK's device (AMD GPUs), with the counters recorded in the command buffers the work is in.
 *
 * GPA samples live in command buffers: a sample is begun and ended in one, inside a GPA command list (a driver counter
 * session, vkCmdBeginGpaSessionAMD) of its own, so several follow one another in a command buffer without nesting. The
 * engine's selected perf events record theirs in DXVK's command stream (command buffer callbacks; at a DXVK flush the open
 * one ends in the old command buffer and a new one begins in the next, so an event across a flush is measured in pieces);
 * the graph's passes and graph-recorded ranges (PushCommandBufferRange) in the graph's own command lists. Only one sample is
 * open per command buffer: a selected range inside an open one is not measured (one nesting level), so select either a pass
 * or the ranges inside it.
 *
 * Each pass of a sample is a GPA session counting one group of the capture's counters (gpa::Schedule):
 * - Arming: the session begins and the graph is rebuilt, so the recordings it keeps across frames (DCLF's epochs) are
 *   recorded again with their samples.
 * - Measuring: once that rebuild is done, one frame's engine events record their samples.
 * - Draining: the graph is rebuilt again without samples, and the session ends once every command buffer recorded while
 *   armed has executed for the last time (the host's frame slots later): GPA reads a sample from the memory its command
 *   buffer wrote, and deleting the session frees it.
 * A recording kept across frames reports its last execution. A pass recorded every frame while armed reports one
 * occurrence per frame recorded (the armed window, a few frames), so a graph range's per-frame sums can be several frames'
 * worth while its averaged metrics (percentages, rates) are exact; the engine's events are one frame's.
 */
namespace GpuCounters::detail
{
	namespace
	{
		constexpr const char* kQueueName = "Graphics";
		constexpr std::uint64_t kArmTimeoutFrames = 600;
		constexpr std::uint32_t kResultFrameLimit = 240;

		class GpaVulkanBackend;
		// The one Vulkan backend: DXVK and the graph host keep callbacks into it until the process ends.
		GpaVulkanBackend* g_backend = nullptr;
		// Set while the graph's recordings record samples (arming to draining). Read on the graph's recording threads.
		std::atomic<bool> g_recording{ false };
		// The session samples are recorded into (null: none).
		std::atomic<GpaSessionId> g_session{ nullptr };
		// The capture's range filters, for the graph's threads; set before g_recording.
		std::vector<std::string> g_filters;

		// Per recording thread: the graph pass ranges open, innermost last, and their command lists (null: not measured).
		thread_local std::vector<GpaCommandListId> t_passLists;
		// Per recording thread: PushCommandBufferRange's open ranges.
		thread_local std::vector<GpaCommandListId> t_rangeLists;

		std::mutex g_otherQueueMutex;
		std::set<std::string> g_otherQueueRanges;

		Step Finished(std::string a_error)
		{
			return Step{ .finished = true, .error = std::move(a_error) };
		}

		// GPA loads vkGetPhysicalDeviceProperties2KHR and vkGetPhysicalDeviceFeatures2KHR, which only exist with
		// VK_KHR_get_physical_device_properties2 enabled; DXVK's instance (Vulkan 1.3, created by dxvk_dxgi.dll) does not
		// enable it. The core functions are the same: GPA gets them under the KHR names.
		PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;
		PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddrForGpa(VkInstance a_instance, const char* a_name)
		{
			if (a_name && std::strcmp(a_name, "vkGetPhysicalDeviceProperties2KHR") == 0)
				a_name = "vkGetPhysicalDeviceProperties2";
			else if (a_name && std::strcmp(a_name, "vkGetPhysicalDeviceFeatures2KHR") == 0)
				a_name = "vkGetPhysicalDeviceFeatures2";
			return g_getInstanceProcAddr(a_instance, a_name);
		}

		class GpaVulkanBackend final : public Backend
		{
		public:
			const char* Name() const noexcept override { return "Vulkan"; }
			const char* Profiler() const noexcept override { return "GPUPerfAPI"; }
			std::vector<MetricRequest> DefaultMetrics() const override { return gpa::DefaultMetrics(); }

			bool Register(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, org::GraphHost& a_host)
			{
				const auto module = static_cast<HMODULE>(a_dxvkModule);
				emitCommandBufferCallback = reinterpret_cast<PFN_dxvkEmitCommandBufferCallback>(::GetProcAddress(module, "dxvkEmitCommandBufferCallback"));
				const auto setBoundaryCallbacks = reinterpret_cast<PFN_dxvkSetCommandBufferBoundaryCallbacks>(::GetProcAddress(module, "dxvkSetCommandBufferBoundaryCallbacks"));
				if (!emitCommandBufferCallback || !setBoundaryCallbacks) {
					unavailable = "the DXVK build lacks the command buffer callbacks";
					logger::warn("[GPA] {}; no Vulkan capture", unavailable);
					return false;
				}
				for (std::uint32_t i = 0; i < a_info.enabledExtensionCount; ++i)
					gpaInterface |= std::strcmp(a_info.enabledExtensions[i], "VK_AMD_gpa_interface") == 0;
				openInfo.instance = a_info.instance;
				openInfo.physical_device = a_info.physicalDevice;
				openInfo.device = a_info.device;
				g_getInstanceProcAddr = a_info.getInstanceProcAddr;
				openInfo.get_instance_proc_addr = &GetInstanceProcAddrForGpa;
				openInfo.get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(a_info.getInstanceProcAddr(a_info.instance, "vkGetDeviceProcAddr"));
				lead = a_host.FrameSlots() + 1;
				host = &a_host;

				g_backend = this;
				if (FAILED(setBoundaryCallbacks(globals::d3d::device, &CloseInDxvk, &ReopenInDxvk, nullptr)))
					logger::warn("[GPA] DXVK refused the command buffer boundary callbacks: engine samples across its flushes will be unbalanced");
				a_host.SetGpuPassRangeCallbacks(
					[](rhi::CommandList a_commandList, rhi::Queue, const char* a_queueName, const char* a_passName) {
						GpaCommandListId list = nullptr;
						if (g_recording.load(std::memory_order_acquire) && a_passName && PerfEventSelected(g_filters, a_passName)) {
							if (a_queueName && std::strcmp(a_queueName, kQueueName) != 0) {
								std::lock_guard lock(g_otherQueueMutex);
								g_otherQueueRanges.emplace(a_passName);
							} else {
								list = g_backend->BeginSample(rhi::vulkan::get_cmd_list(a_commandList), a_passName);
							}
						}
						t_passLists.push_back(list);
					},
					[](rhi::CommandList a_commandList, rhi::Queue) {
						if (t_passLists.empty())
							return;
						const auto list = t_passLists.back();
						t_passLists.pop_back();
						g_backend->EndSample(rhi::vulkan::get_cmd_list(a_commandList), list);
					});
				return true;
			}

			bool Probe(DeviceInfo& a_device) override
			{
				a_device.probed = true;
				if (!unavailable.empty()) {
					a_device.error = unavailable;
					return false;
				}
				const auto* table = gpa::Table();
				if (!table) {
					a_device.error = gpa::Unavailable();
					return false;
				}
				if (!gpaInterface) {
					a_device.error = "DXVK's device lacks VK_AMD_gpa_interface (GPUPerfAPI was not prepared before the device was created)";
					return false;
				}
				a_device.available = true;
				// Clocks stay as the driver runs them: the counters describe the frame as it is played.
				const GpaOpenContextFlags flags = static_cast<GpaOpenContextFlags>(kGpaOpenContextVkUseInfoType2) | static_cast<GpaOpenContextFlags>(kGpaOpenContextClockModeNoneBit);
				if (!gpa::Check(table->GpaOpenContext(&openInfo, flags, &context), "GpaOpenContext")) {
					a_device.error = "GPUPerfAPI could not open a context on DXVK's device";
					return false;
				}
				GpaContextSampleTypeFlags types = 0;
				(void)table->GpaGetSupportedSampleTypes(context, &types);
				gpa::Describe(context, a_device);
				a_device.gpuSupported = (types & kGpaContextSampleTypeDiscreteCounter) != 0;
				if (!a_device.gpuSupported)
					a_device.error = "GPUPerfAPI has no discrete counters on this GPU";
				logger::info("[GPA] Vulkan context on '{}' ({}): discrete counters {}", a_device.deviceName, a_device.chipName, a_device.gpuSupported);
				return a_device.gpuSupported;
			}

			std::string Validate(const CaptureConfiguration& a_configuration) const override
			{
				if (a_configuration.nestingLevels != 1)
					return "GPUPerfAPI measures one nesting level; set nestingLevels to 1";
				return {};
			}

			bool Begin(Capture& a_capture, const std::string&, std::string& a_error) override
			{
				if (phase != Phase::Idle) {
					a_error = "the previous capture is still finishing on the GPU; retry in a few frames";
					return false;
				}
				if (a_error = Validate(a_capture.configuration); !a_error.empty())
					return false;
				if (!gpa::BuildSchedule(context, a_capture.configuration.metrics, schedule, a_error))
					return false;
				a_capture.result.metrics = schedule.metrics;
				a_capture.result.unsupportedMetrics = schedule.unsupported;
				a_capture.passesPerSample = schedule.groups.size();
				g_filters = a_capture.configuration.ranges;
				merger.Reset(schedule.metrics.size());
				pass = 0;
				logger::info("[GPA] capture {}: {} metrics in {} single-pass groups ({} unsupported)", a_capture.id, schedule.metrics.size(), schedule.groups.size(),
					schedule.unsupported.size());
				if (!schedule.unsupported.empty()) {
					std::string list;
					for (const auto& metric : schedule.unsupported)
						list += std::format("{}{}", list.empty() ? "" : ", ", metric.name);
					logger::info("[GPA]   not measured (no such counter, or more than one pass alone): {}", list);
				}
				return true;
			}

			void PresentBegin(Capture* a_capture) override
			{
				++frame;
				// The measured frame ends: its engine samples close in DXVK's stream.
				if (phase == Phase::Measuring && engineActive) {
					engineActive = false;
					SetRangesActive(false);
					CloseEngineRanges();
					if (a_capture)
						a_capture->passesSubmitted = pass + 1;
				}
			}

			Step Advance(Capture& a_capture) override
			{
				switch (phase) {
				case Phase::Idle:
					{
						std::string error;
						const GpaSessionId session = gpa::BeginPassSession(context, schedule, pass, error);
						if (!session)
							return Finished(error);
						{
							std::lock_guard lock(mutex);
							samples.clear();
							open.clear();
							nextSample = 0;
						}
						g_session.store(session, std::memory_order_release);
						g_recording.store(true, std::memory_order_release);
						host->RequestRebuild();
						armGeneration = host->BuildGeneration();
						phaseFrame = frame;
						phase = Phase::Arming;
						return Step{ .state = CaptureState::Waiting };
					}
				case Phase::Arming:
					if (host->BuildGeneration() > armGeneration) {
						// The graph recorded again with samples: this frame's engine events are measured.
						phase = Phase::Measuring;
						engineActive = true;
						SetRangesActive(true);
						return Step{ .state = CaptureState::Capturing };
					}
					if (frame - phaseFrame > kArmTimeoutFrames)
						return Fail("the graph did not rebuild within 600 frames of arming (none builds on a loading screen or the main menu)");
					return Step{ .state = CaptureState::Waiting };
				case Phase::Measuring:
					// PresentBegin closed the measured frame: no more samples are recorded.
					g_recording.store(false, std::memory_order_release);
					host->RequestRebuild();
					disarmGeneration = host->BuildGeneration();
					drainFrame = 0;
					phase = Phase::Draining;
					return Step{ .state = CaptureState::Decoding };
				case Phase::Draining:
					if (!drainFrame && host->BuildGeneration() > disarmGeneration)
						drainFrame = frame + lead;
					if (!drainFrame || frame < drainFrame) {
						if (frame - phaseFrame > kArmTimeoutFrames)
							return Fail("the graph did not rebuild after the measured frame");
						return Step{ .state = CaptureState::Decoding };
					}
					LogPassSamples();
					if (!gpa::Check(gpa::Table()->GpaEndSession(g_session.load()), "GpaEndSession"))
						return Fail("could not end the GPUPerfAPI session");
					sessionEnded = true;
					waitFrames = 0;
					phase = Phase::Reading;
					[[fallthrough]];
				case Phase::Reading:
					{
						const GpaSessionId session = g_session.load();
						const GpaStatus status = gpa::Table()->GpaIsSessionComplete(session);
						if (status == kGpaStatusResultNotReady) {
							if (++waitFrames > kResultFrameLimit)
								return Fail("the GPU did not finish a GPUPerfAPI pass");
							return Step{ .state = CaptureState::Decoding };
						}
						std::vector<std::pair<GpaUInt32, std::string>> taken;
						{
							std::lock_guard lock(mutex);
							taken.swap(samples);
						}
						std::string error;
						const bool read = gpa::Check(status, "GpaIsSessionComplete") && merger.AddPass(session, schedule, pass, taken, error);
						ReleaseSession();
						if (!read)
							return Finished(error.empty() ? "could not read a GPUPerfAPI pass" : error);
						if (taken.empty())
							return Finished("no selected range was recorded in the measured frame; check the range names");
						if (++pass >= schedule.groups.size()) {
							merger.Emit(a_capture.sample, a_capture.result);
							pass = 0;
							if (++a_capture.sample >= a_capture.configuration.samples)
								return Finished({});
						}
						return Step{ .state = CaptureState::Waiting };
					}
				}
				return Step{ .state = CaptureState::Waiting };
			}

			void End(Capture&) override
			{
				if (phase == Phase::Idle)
					return;
				engineActive = false;
				SetRangesActive(false);
				CloseEngineRanges();
				if (g_recording.exchange(false))
					host->RequestRebuild();
				// The session's command buffers may still execute: it is released once they cannot (Advance is not called
				// without a capture, so the next capture's Begin waits for Idle and this drains on its own frames).
				ReleaseSession();
			}

			std::vector<std::string> TakeOtherQueueRanges() override
			{
				std::lock_guard lock(g_otherQueueMutex);
				std::vector<std::string> taken(g_otherQueueRanges.begin(), g_otherQueueRanges.end());
				g_otherQueueRanges.clear();
				return taken;
			}

			void PushRange(Capture& a_capture, std::string_view a_name) override
			{
				bool pushed = false;
				if (engineActive && PerfEventSelected(a_capture.configuration.ranges, a_name)) {
					const char* interned = names.insert(std::string(a_name)).first->c_str();
					pushed = SUCCEEDED(emitCommandBufferCallback(globals::d3d::device, &PushInDxvk, const_cast<char*>(interned)));
				}
				events.push_back(pushed);
			}

			void PopRange() override
			{
				if (events.empty())
					return;
				const bool pushed = events.back();
				events.pop_back();
				if (pushed)
					emitCommandBufferCallback(globals::d3d::device, &PopInDxvk, nullptr);
			}

			bool CommandBufferRangesActive() const noexcept override
			{
				return g_recording.load(std::memory_order_relaxed);
			}

			bool PushCommandBufferRange(void* a_commandBuffer, const char* a_name) override
			{
				if (!g_recording.load(std::memory_order_acquire) || !a_name || !PerfEventSelected(g_filters, a_name))
					return false;
				const auto list = BeginSample(a_commandBuffer, a_name);
				if (!list)
					return false;
				t_rangeLists.push_back(list);
				return true;
			}

			void PopCommandBufferRange(void* a_commandBuffer, bool a_pushed) override
			{
				if (!a_pushed || t_rangeLists.empty())
					return;
				const auto list = t_rangeLists.back();
				t_rangeLists.pop_back();
				EndSample(a_commandBuffer, list);
			}

		private:
			enum class Phase
			{
				Idle,
				Arming,
				Measuring,
				Draining,
				Reading,
			};

			// Any recording thread: a GPA command list in a_commandBuffer with one sample named a_name in it. Null when the
			// command buffer already holds an open sample, no session records, or GPA refused.
			GpaCommandListId BeginSample(void* a_commandBuffer, const char* a_name)
			{
				const GpaSessionId session = g_session.load(std::memory_order_acquire);
				if (!session || !a_commandBuffer)
					return nullptr;
				const auto* table = gpa::Table();
				std::lock_guard lock(mutex);
				if (!open.insert(a_commandBuffer).second)
					return nullptr;
				GpaCommandListId list = nullptr;
				const GpaUInt32 id = nextSample++;
				if (table->GpaBeginCommandList(session, 0, a_commandBuffer, kGpaCommandListPrimary, &list) != kGpaStatusOk) {
					open.erase(a_commandBuffer);
					++refused;
					return nullptr;
				}
				if (table->GpaBeginSample(id, list) != kGpaStatusOk) {
					(void)table->GpaEndCommandList(list);
					open.erase(a_commandBuffer);
					++refused;
					return nullptr;
				}
				samples.emplace_back(id, a_name);
				return list;
			}

			void EndSample(void* a_commandBuffer, GpaCommandListId a_list)
			{
				if (!a_list)
					return;
				const auto* table = gpa::Table();
				std::lock_guard lock(mutex);
				(void)gpa::Check(table->GpaEndSample(a_list), "GpaEndSample");
				(void)gpa::Check(table->GpaEndCommandList(a_list), "GpaEndCommandList");
				open.erase(a_commandBuffer);
			}

			// DXVK's worker thread, in its command stream.
			static void PushInDxvk(void* a_user, VkCommandBuffer a_commandBuffer)
			{
				const auto name = static_cast<const char*>(a_user);
				g_backend->engineOpen.push_back({ name, g_backend->BeginSample(a_commandBuffer, name) });
			}
			static void PopInDxvk(void*, VkCommandBuffer a_commandBuffer)
			{
				auto& open = g_backend->engineOpen;
				if (open.empty())
					return;
				const auto entry = open.back();
				open.pop_back();
				g_backend->EndSample(a_commandBuffer, entry.list);
			}
			// A DXVK flush: the open samples end in the old command buffer and begin again (as further occurrences) in the new one.
			static void CloseInDxvk(void*, VkCommandBuffer a_commandBuffer)
			{
				for (auto entry = g_backend->engineOpen.rbegin(); entry != g_backend->engineOpen.rend(); ++entry) {
					g_backend->EndSample(a_commandBuffer, entry->list);
					entry->list = nullptr;
					entry->reopen = true;
				}
			}
			static void ReopenInDxvk(void*, VkCommandBuffer a_commandBuffer)
			{
				for (auto& entry : g_backend->engineOpen)
					if (std::exchange(entry.reopen, false))
						entry.list = g_backend->BeginSample(a_commandBuffer, entry.name);
			}

			// The pass's samples by name, once its session ends (the first sample's pass only, to keep the log short).
			void LogPassSamples()
			{
				std::map<std::string, std::uint32_t> counts;
				std::size_t total = 0;
				{
					std::lock_guard lock(mutex);
					for (const auto& [id, name] : samples)
						++counts[name];
					total = samples.size();
				}
				std::string text;
				for (const auto& [name, count] : counts)
					text += std::format("{}{} x{}", text.empty() ? "" : ", ", name, count);
				logger::info("[GPA] pass {}: {} samples, {} still open, {} refused: {}", pass, total, open.size(), refused, text);
			}

			// The engine events still open in DXVK's stream close, so its command buffers stay balanced.
			void CloseEngineRanges()
			{
				for (auto pushed = events.rbegin(); pushed != events.rend(); ++pushed)
					if (*pushed)
						emitCommandBufferCallback(globals::d3d::device, &PopInDxvk, nullptr);
				for (auto&& pushed : events)
					pushed = false;
			}

			Step Fail(std::string a_error)
			{
				engineActive = false;
				SetRangesActive(false);
				CloseEngineRanges();
				if (g_recording.exchange(false))
					host->RequestRebuild();
				ReleaseSession();
				return Finished(std::move(a_error));
			}

			// The session is deleted, or (its command buffers may still execute) left for the driver to finish: GPA frees a
			// session's memory at deletion, so one deleted early is ended and kept until the process ends instead.
			void ReleaseSession()
			{
				const GpaSessionId session = g_session.exchange(nullptr);
				if (session && phase == Phase::Reading)
					gpa::DeleteSession(session, false);
				else if (session && !sessionEnded)
					(void)gpa::Table()->GpaEndSession(session);
				if (refused) {
					logger::warn("[GPA] {} samples were refused (GpaBeginCommandList / GpaBeginSample)", refused);
					refused = 0;
				}
				sessionEnded = false;
				phase = Phase::Idle;
			}

			struct EngineRange
			{
				const char* name;
				GpaCommandListId list;  // null: not measured (or closed at a flush)
				bool reopen = false;
			};

			// Registration.
			PFN_dxvkEmitCommandBufferCallback emitCommandBufferCallback = nullptr;
			GpaVkContextOpenInfo2 openInfo{};
			bool gpaInterface = false;
			std::string unavailable;
			std::uint64_t lead = 4;
			org::GraphHost* host = nullptr;

			// Render thread.
			GpaContextId context = nullptr;
			gpa::Schedule schedule;
			gpa::Merger merger;
			std::size_t pass = 0;
			Phase phase = Phase::Idle;
			std::uint64_t frame = 0, phaseFrame = 0, drainFrame = 0;
			std::uint64_t armGeneration = 0, disarmGeneration = 0;
			std::uint32_t waitFrames = 0;
			bool engineActive = false;  // the measured frame: engine events record samples
			bool sessionEnded = false;
			std::set<std::string> names;  // interned: DXVK's callbacks keep pointers into it
			std::vector<bool> events;     // per open perf event: whether it emitted a sample

			// DXVK's worker thread only, in stream order.
			std::vector<EngineRange> engineOpen;

			// Every recording thread.
			std::mutex mutex;
			std::unordered_set<void*> open;  // command buffers with a sample open
			GpaUInt32 nextSample = 0;
			std::vector<std::pair<GpaUInt32, std::string>> samples;  // the session's, in the order they began
			std::uint64_t refused = 0;
		};
	}

	std::uint32_t VulkanVendor(const DxvkOrgInteropDeviceInfo& a_info)
	{
		const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(a_info.getInstanceProcAddr(a_info.instance, "vkGetPhysicalDeviceProperties"));
		if (!getProperties)
			return 0;
		VkPhysicalDeviceProperties properties{};
		getProperties(a_info.physicalDevice, &properties);
		return properties.vendorID;
	}

	std::unique_ptr<Backend> MakeGpaVulkanBackend(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, org::GraphHost& a_host)
	{
		auto backend = std::make_unique<GpaVulkanBackend>();
		backend->Register(a_info, a_dxvkModule, a_host);
		return backend;
	}
}

#else

#	include "Features/RemoteControl/GpuCountersBackend.h"

namespace GpuCounters::detail
{
	std::uint32_t VulkanVendor(const DxvkOrgInteropDeviceInfo&)
	{
		return 0;
	}

	std::unique_ptr<Backend> MakeGpaVulkanBackend(const DxvkOrgInteropDeviceInfo&, void*, org::GraphHost&)
	{
		return nullptr;
	}
}

#endif
