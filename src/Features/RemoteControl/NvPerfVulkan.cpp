#if defined(CS_HAS_RENDER_GRAPH) && defined(NVPERF_ENABLED)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/RemoteControl/NvPerfBackend.h"

#	include "Globals.h"
#	include "RenderGraph/DxvkOrgInterop.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Telemetry/NvPerfCapture.h>

#	include <atomic>
#	include <cstring>
#	include <set>
#	include <vector>

/*
 * NvPerf's Vulkan backend: OpenRenderGraph's Telemetry/NvPerfCapture on DXVK's graphics queue, under
 * QueueControl::Boundaries (the queue is DXVK's, so the profiler's queue calls are made only at its frame boundaries).
 *
 * - Frame boundaries are DXVK queue callbacks enqueued before Present: each frame is one profiler pass, and the
 *   profiler's queue calls run on DXVK's submission thread with the queue locked, between its submissions.
 * - The graph's passes get their ranges through the graph host's per-pass hooks; the engine's selected perf events
 *   through DXVK command buffer callbacks, in its command stream; graph-recorded work (DCLF's colour pass parts) through
 *   PushCommandBufferRange. They sit side by side on the same queue, so they are measured alike. A range must open and
 *   close in one command buffer: at each DXVK flush the open engine ranges close and reopen in the next, so an event
 *   that spans one (e.g. around a graph epoch) is measured in pieces, summed in the summary.
 * - Ranges record from the capture's arming; its first pass begins the host's frame slots later, once every command
 *   buffer that can execute in it (the graph's epochs are recorded ahead) was recorded while armed. Arming rebuilds the
 *   graph, so recordings kept across frames (a revision's epochs) are recorded again with their ranges, and so does
 *   disarming, so that none carries a session's ranges into the next.
 * - A sample is one armed capture of the profiler; a capture of several samples arms it again after each one.
 *
 * Differences from the D3D11 backend: one nesting level (the profiler configures one; a request for more is refused),
 * so a range's path is its own name; the chip is named by the first capture; the profiler decodes on DXVK's submission
 * thread at the last pass's boundary, so a sample is never seen Decoding.
 */
namespace NvPerf::detail
{
	namespace
	{
		namespace nvperf = org::telemetry::nvperf;
		namespace gpu_capture = basic_telemetry::gpu_capture;

		constexpr const char* kQueueName = "Graphics";

		class VulkanBackend;
		// The one Vulkan backend: DXVK and the graph host keep callbacks into it until the process ends, so it is never
		// destroyed.
		VulkanBackend* g_backend = nullptr;
		// Set while a capture is armed: the graph's pass ranges and graph-recorded ranges are pushed. Read on the graph
		// host's recording threads.
		std::atomic<bool> g_recording{ false };

		// Per recording thread: whether each open graph pass pushed its range (a bit per nesting level, innermost lowest),
		// so a pass ends its range only when it began one, even when arming changes in between.
		thread_local std::uint64_t t_passRangeBits = 0;
		thread_local std::uint32_t t_passRangeDepth = 0;

		Step Finished(std::string a_error)
		{
			return Step{ .finished = true, .error = std::move(a_error) };
		}

		MetricRequest FromCapture(const gpu_capture::MetricRequest& a_metric)
		{
			MetricRequest metric;
			metric.name = a_metric.name;
			metric.metricType = a_metric.metricType;
			metric.rollupOp = a_metric.rollupOp;
			metric.submetric = a_metric.submetric;
			return metric;
		}

		class VulkanBackend final : public Backend
		{
		public:
			const char* Name() const noexcept override { return "Vulkan"; }

			// DXVK's exports, its device's queue, the graph host's hooks. Nothing here loads the library.
			bool Register(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, org::PersistentGraphHost& a_host)
			{
				const auto module = static_cast<HMODULE>(a_dxvkModule);
				enqueueQueueCallback = reinterpret_cast<PFN_dxvkEnqueueQueueCallback>(::GetProcAddress(module, "dxvkEnqueueQueueCallback"));
				emitCommandBufferCallback = reinterpret_cast<PFN_dxvkEmitCommandBufferCallback>(::GetProcAddress(module, "dxvkEmitCommandBufferCallback"));
				const auto setBoundaryCallbacks = reinterpret_cast<PFN_dxvkSetCommandBufferBoundaryCallbacks>(::GetProcAddress(module, "dxvkSetCommandBufferBoundaryCallbacks"));
				if (!enqueueQueueCallback || !emitCommandBufferCallback || !setBoundaryCallbacks) {
					unavailable = "the DXVK build lacks the queue and command buffer callbacks";
					logger::warn("[NvPerf] {}; no Vulkan capture", unavailable);
					return false;
				}
				for (std::uint32_t i = 0; i < a_info.enabledExtensionCount; ++i)
					semaphores |= std::strcmp(a_info.enabledExtensions[i], "VK_KHR_external_semaphore_win32") == 0;

				target.backend = rhi::Backend::Vulkan;
				target.instance = a_info.instance;
				target.physicalDevice = a_info.physicalDevice;
				target.device = a_info.device;
				target.queue = a_info.graphicsQueue;
				target.getInstanceProcAddr = reinterpret_cast<void*>(a_info.getInstanceProcAddr);
				target.getDeviceProcAddr = reinterpret_cast<void*>(a_info.getInstanceProcAddr(a_info.instance, "vkGetDeviceProcAddr"));
				target.name = kQueueName;
				if (const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(a_info.getInstanceProcAddr(a_info.instance, "vkGetPhysicalDeviceProperties"))) {
					VkPhysicalDeviceProperties properties{};
					getProperties(a_info.physicalDevice, &properties);
					deviceName = properties.deviceName;
				}
				lead = a_host.FrameSlots() + 1;
				host = &a_host;

				g_backend = this;
				if (FAILED(setBoundaryCallbacks(globals::d3d::device, &CloseInDxvk, &ReopenInDxvk, nullptr)))
					logger::warn("[NvPerf] DXVK refused the command buffer boundary callbacks: engine ranges across its flushes will be unbalanced");
				// The graph's passes: their ranges recorded in their own command lists (under QueueControl::Boundaries a range
				// only records; the frame boundaries make every queue call). Set now, while no frame executes; until a capture
				// arms they only test g_recording.
				a_host.SetGpuPassRangeCallbacks(
					[](rhi::CommandList a_commandList, rhi::Queue, const char* a_queueName, const char* a_passName) {
						const bool pushed = g_recording.load(std::memory_order_acquire) &&
						                    nvperf::PushRange(g_backend->target, rhi::vulkan::get_cmd_list(a_commandList), a_queueName, a_passName);
						t_passRangeBits = (t_passRangeBits << 1) | (pushed ? 1u : 0u);
						++t_passRangeDepth;
					},
					[](rhi::CommandList a_commandList, rhi::Queue) {
						if (!t_passRangeDepth)
							return;
						const bool pushed = (t_passRangeBits & 1u) != 0;
						t_passRangeBits >>= 1;
						--t_passRangeDepth;
						if (pushed)
							nvperf::PopRange(rhi::Backend::Vulkan, rhi::vulkan::get_cmd_list(a_commandList));
					});
				return true;
			}

			bool Probe(DeviceInfo& a_device) override
			{
				a_device.probed = true;
				a_device.deviceName = deviceName;
				if (!unavailable.empty()) {
					a_device.error = unavailable;
					return false;
				}
				if (!LoadHostLibrary(a_device))
					return false;
				if (!nvperf::LoadLibraryFrom(a_device.libraryPath) || !nvperf::Available()) {
					a_device.error = "the Nsight Perf library could not be initialized (non-NVIDIA GPU or old driver?)";
					return false;
				}
				a_device.available = true;
				if (!semaphores)
					logger::warn("[NvPerf] DXVK's device lacks VK_KHR_external_semaphore_win32, which the profiler needs");
				nvperf::SetQueueControl(nvperf::QueueControl::Boundaries);
				// Logs the chip and whether the profiler supports it; the capture itself fails cleanly when it does not.
				nvperf::LogStartupProbe(target);
				a_device.gpuSupported = true;
				return true;
			}

			std::string Validate(const CaptureConfiguration& a_configuration) const override
			{
				if (a_configuration.nestingLevels != 1)
					return "the Vulkan backend measures one nesting level; set nestingLevels to 1";
				return {};
			}

			bool Begin(Capture& a_capture, const std::string&, std::string& a_error) override
			{
				if (draining) {
					a_error = "the previous capture is still finishing on the GPU; retry in a few frames";
					return false;
				}
				if (a_error = Validate(a_capture.configuration); !a_error.empty())
					return false;
				gpu_capture::CaptureConfiguration configuration{};
				for (const auto& range : a_capture.configuration.ranges)
					configuration.passes.push_back({ range, kQueueName });
				for (const auto& metric : a_capture.configuration.metrics) {
					gpu_capture::MetricRequest request{};
					request.name = metric.name;
					request.metricType = metric.metricType;
					request.rollupOp = metric.rollupOp;
					request.submetric = metric.submetric;
					// A metric the chip lacks is reported unsupported, as on D3D11, rather than failing the capture.
					request.required = false;
					configuration.metrics.push_back(std::move(request));
				}
				if (!nvperf::ConfigureCapture(configuration, a_error))
					return false;
				// The front end writes the one CSV, from the result.
				nvperf::SetCsvPath({});
				armed = false;
				return true;
			}

			void PresentBegin(Capture* a_capture) override
			{
				++frame;
				// A capture that ended before the profiler finished: its passes still need their boundaries until it does
				// (with nothing recording, it fails within a few frames).
				if (draining) {
					if (nvperf::CaptureComplete()) {
						(void)nvperf::TakeCaptureResult();
						draining = false;
					} else {
						EnqueueBoundary();
					}
					return;
				}
				// While a capture is armed, each Present ends the frame's profiler pass and begins the next, in the queue's order.
				if (a_capture && armed && frame >= firstPassFrame) {
					EnqueueBoundary();
					if (!a_capture->passesPerSample || a_capture->passesSubmitted < a_capture->passesPerSample)
						++a_capture->passesSubmitted;
				}
			}

			Step Advance(Capture& a_capture) override
			{
				if (armed && nvperf::CaptureComplete()) {
					auto result = nvperf::TakeCaptureResult();
					Disarm();
					if (!result)
						return Finished("the profiler completed without a result");
					if (!result->success && !result->droppedRanges && !result->droppedTraceBytes)
						return Finished(result->error.empty() ? "the profiler failed" : result->error);
					Collect(a_capture, *result);
					if (++a_capture.sample >= a_capture.configuration.samples)
						return Finished({});
					// The next sample arms once every command buffer recorded under this one (its ranges in it) has executed:
					// one executing in the next session's passes crashes the profiler.
					nextArmFrame = frame + lead;
				}
				if (!armed && frame < nextArmFrame)
					return Step{ .state = CaptureState::Waiting };
				if (!armed) {
					std::string error;
					if (!nvperf::ArmCapture(a_capture.sample, error))
						return Finished(error);
					armed = true;
					g_recording.store(true, std::memory_order_release);
					SetRangesActive(true);
					// A revision's epochs (DCLF's) are recorded once and submitted until the revision changes, so recordings
					// made before arming would execute in the passes without their ranges: a rebuild records them all again,
					// and the frame after it waits for the new ones, well within the lead.
					host->RequestRebuild();
					firstPassFrame = frame + lead;
					misplacedPops.store(0, std::memory_order_relaxed);
					a_capture.passesSubmitted = 0;
				}
				if (frame < firstPassFrame)
					return Step{ .state = CaptureState::Waiting };
				// The profiler sizes the sample at its first pass, on DXVK's submission thread: for the status only.
				if (a_capture.sample == 0)
					a_capture.passesPerSample = nvperf::ScheduledPassCount();
				return Step{ .state = CaptureState::Capturing };
			}

			void End(Capture&) override
			{
				if (!armed)
					return;
				Disarm();
				if (nvperf::CaptureComplete())
					(void)nvperf::TakeCaptureResult();
				else
					draining = true;
			}

			void PushRange(Capture& a_capture, std::string_view a_name) override
			{
				const std::string name(a_name);
				bool pushed = false;
				if (armed && PerfEventSelected(a_capture.configuration.ranges, name) && nvperf::RangeSelected(kQueueName, name.c_str())) {
					const char* interned = names.insert(name).first->c_str();
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
				return g_recording.load(std::memory_order_relaxed) && nvperf::PushRange(target, a_commandBuffer, kQueueName, a_name);
			}

			void PopCommandBufferRange(void* a_commandBuffer, bool a_pushed) override
			{
				if (a_pushed)
					nvperf::PopRange(rhi::Backend::Vulkan, a_commandBuffer);
			}

		private:
			// DXVK's worker thread, in its command stream.
			static void PushInDxvk(void* a_user, VkCommandBuffer a_commandBuffer)
			{
				const auto name = static_cast<const char*>(a_user);
				g_backend->open.push_back(nvperf::PushRange(g_backend->target, a_commandBuffer, kQueueName, name) ? name : nullptr);
			}
			static void PopInDxvk(void*, VkCommandBuffer a_commandBuffer)
			{
				auto& open = g_backend->open;
				if (open.empty())
					return;
				const bool pushed = open.back() != nullptr;
				open.pop_back();
				if (pushed && !nvperf::PopRange(rhi::Backend::Vulkan, a_commandBuffer))
					g_backend->misplacedPops.fetch_add(1, std::memory_order_relaxed);
			}
			static void CloseInDxvk(void*, VkCommandBuffer a_commandBuffer)
			{
				auto& open = g_backend->open;
				for (auto name = open.rbegin(); name != open.rend(); ++name)
					if (*name && !nvperf::PopRange(rhi::Backend::Vulkan, a_commandBuffer))
						g_backend->misplacedPops.fetch_add(1, std::memory_order_relaxed);
			}
			static void ReopenInDxvk(void*, VkCommandBuffer a_commandBuffer)
			{
				for (auto& name : g_backend->open)
					if (name && !nvperf::PushRange(g_backend->target, a_commandBuffer, kQueueName, name))
						name = nullptr;  // the capture ended in between
			}

			// DXVK's submission thread, with the queue locked: the frame's pass ends and the next one begins.
			static void FrameBoundary(void* a_user, VkQueue)
			{
				const auto frame = reinterpret_cast<std::uintptr_t>(a_user);
				nvperf::EndFrameCapture(g_backend->target, frame);
				nvperf::BeginFrameCapture(g_backend->target, frame + 1);
			}

			void EnqueueBoundary()
			{
				enqueueQueueCallback(globals::d3d::device, &FrameBoundary, reinterpret_cast<void*>(static_cast<std::uintptr_t>(frame)));
			}

			// Ranges stop recording. A perf event still open closes in DXVK's stream now, so its command buffers stay balanced.
			void Disarm()
			{
				armed = false;
				g_recording.store(false, std::memory_order_release);
				SetRangesActive(false);
				// The recordings kept across frames (a revision's epochs) recorded while armed hold this session's ranges: recorded
				// again, so that none executes in a later session.
				host->RequestRebuild();
				for (auto pushed = events.rbegin(); pushed != events.rend(); ++pushed)
					if (*pushed)
						emitCommandBufferCallback(globals::d3d::device, &PopInDxvk, nullptr);
				events.clear();
				if (const auto misplaced = misplacedPops.load())
					logger::warn("[NvPerf]   {} engine range pops found no open range in their command buffer", misplaced);
			}

			// A sample's ranges, in the front end's form; the first sample also names the metrics and the chip.
			void Collect(Capture& a_capture, const gpu_capture::CaptureResult& a_result)
			{
				auto& result = a_capture.result;
				if (!a_capture.sample) {
					result.metrics.clear();
					for (const auto& metric : a_result.metrics)
						result.metrics.push_back(FromCapture(metric));
					result.unsupportedMetrics.clear();
					for (const auto& metric : a_result.unsupportedMetrics)
						result.unsupportedMetrics.push_back(FromCapture(metric));
					if (!a_result.chipName.empty())
						result.chipName = a_result.chipName;
				}
				if (a_result.scheduledPasses)
					a_capture.passesPerSample = a_result.scheduledPasses;
				result.droppedRanges += a_result.droppedRanges;
				result.droppedTraceBytes += a_result.droppedTraceBytes;
				for (const auto& range : a_result.ranges) {
					RangeResult entry;
					entry.sample = a_capture.sample;
					entry.name = range.passName;
					entry.path = range.passName;
					entry.occurrence = range.occurrence;
					entry.values = range.values;
					result.rangeResults.push_back(std::move(entry));
				}
			}

			// Registration.
			nvperf::QueueTarget target;
			PFN_dxvkEnqueueQueueCallback enqueueQueueCallback = nullptr;
			PFN_dxvkEmitCommandBufferCallback emitCommandBufferCallback = nullptr;
			std::string unavailable;  // why no capture can run (empty: none)
			std::string deviceName;
			bool semaphores = false;
			// Frames between arming and the first pass: every command buffer that executes in a pass must have been
			// recorded while armed, and the graph host records its epochs up to its frame slots ahead.
			std::uint64_t lead = 4;
			org::PersistentGraphHost* host = nullptr;  // the graph host, rebuilt at arming and disarming

			// Render thread.
			std::uint64_t frame = 0;
			std::uint64_t firstPassFrame = 0;
			std::uint64_t nextArmFrame = 0;  // a capture's next sample arms from this frame
			bool armed = false;     // ranges record while set
			bool draining = false;  // a capture ended while the profiler was still armed
			std::set<std::string> names;  // interned: the command buffer callbacks keep pointers into it
			std::vector<bool> events;     // per open perf event: whether it pushed a range

			// DXVK's worker thread only, in stream order: the engine ranges open in the command buffer it records, outermost
			// first (null: the range was not pushed). A DXVK flush closes them in the old command buffer and reopens them in
			// the new one, so that each command buffer's ranges are balanced; one event then counts as several occurrences.
			std::vector<const char*> open;
			std::atomic<std::uint64_t> misplacedPops{ 0 };
		};
	}

	std::unique_ptr<Backend> MakeVulkanBackend(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, org::PersistentGraphHost& a_host)
	{
		auto backend = std::make_unique<VulkanBackend>();
		backend->Register(a_info, a_dxvkModule, a_host);
		return backend;
	}
}

#else

#	include "Features/RemoteControl/NvPerfBackend.h"

namespace NvPerf::detail
{
	std::unique_ptr<Backend> MakeVulkanBackend(const DxvkOrgInteropDeviceInfo&, void*, org::PersistentGraphHost&)
	{
		return nullptr;
	}
}

#endif
