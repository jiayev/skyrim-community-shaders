#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraphRuntime.h"

#include "Aftermath.h"
#include "DxvkLoader.h"
#include "Features/Upscaling/DXVKInteropInterfaces.h"
#include "Globals.h"
#include "Profiler.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/NvPerfBridge.h"
#include "GpuIdleTrace.h"
#include "State.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#	include <ORGModuleServices/ShaderCompiler.h>
#endif
#include <Resources/Resource.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
	// ORG host frames (epochs) in flight: up to five epochs per game frame, ~3 game frames.
	constexpr std::uint32_t kHostFramesInFlight = 16;

	// Everything BasicRHI's Vulkan backend needs beyond what DXVK enables for itself.
	// DXVK already requires the rest (timeline semaphores, buffer device address,
	// descriptor indexing, scalar block layout, dynamic rendering, maintenance5).
	// DXVK enables vulkanMemoryModel, which makes the Device-scope atomics DXC emits
	// invalid unless vulkanMemoryModelDeviceScope is enabled as well.
	// Drawcall Limit Fix draws through device-generated commands (optional: without it the graph
	// runs, and DCLF stays off).
	constexpr DxvkOrgInteropFeature kRequestedFeatures[] = {
		{ VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, "descriptorHeap" },
		{ nullptr, "vulkanMemoryModelDeviceScope" },
		{ VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME, "deviceGeneratedCommands" },
	};

	bool EnvDisabled()
	{
		char buf[8] = {};
		return GetEnvironmentVariableA("CS_ORG", buf, sizeof(buf)) && buf[0] == '0';
	}

	// Diagnostic: CS_ORG=features requests the interop device features but never creates the
	// graph, separating side effects of the device configuration from those of the graph's work.
	bool EnvEquals(const char* a_name, const char* a_value)
	{
		char buf[32] = {};
		return GetEnvironmentVariableA(a_name, buf, sizeof(buf)) && std::strcmp(buf, a_value) == 0;
	}

	bool EnvFeaturesOnly()
	{
		return EnvEquals("CS_ORG", "features");
	}

	template <class T>
	T ResolveExport(HMODULE a_module, const char* a_name)
	{
		return a_module ? reinterpret_cast<T>(reinterpret_cast<void*>(::GetProcAddress(a_module, a_name))) : nullptr;
	}

	bool HasExtension(const DxvkOrgInteropDeviceInfo& a_info, const char* a_name)
	{
		for (uint32_t i = 0; i < a_info.enabledExtensionCount; ++i)
			if (a_info.enabledExtensions[i] && std::strcmp(a_info.enabledExtensions[i], a_name) == 0)
				return true;
		return false;
	}
}

struct RenderGraphRuntime::Impl
{
	winrt::com_ptr<IDXGIVkInteropDevice> interop;
	PFN_dxvkCreateBufferFromVkBuffer createBufferFromVkBuffer = nullptr;
	PFN_dxvkSetDeviceTeardownCallback setTeardownCallback = nullptr;
	PFN_dxvkEnqueueInteropSubmission enqueueSubmission = nullptr;
	// Optional (newer DXVK): an epoch's submissions go to DXVK as one enqueue (CS_ORG_BATCH_SUBMIT=0 turns it off).
	PFN_dxvkEnqueueInteropSubmissions enqueueSubmissions = nullptr;
	PFN_dxvkGetInteropResourceInfo getResourceInfo = nullptr;
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	std::unique_ptr<org::services::ShaderCompiler> shaderCompiler;
#endif
	PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
	std::shared_ptr<rhi::DevicePtr> device = std::make_shared<rhi::DevicePtr>();
	std::unique_ptr<org::PersistentGraphHost> host;
	std::atomic<bool> faulted = false;

	// The thread that drives the D3D11 immediate context (the one running epochs).
	// Its submissions go into DXVK's command stream; other threads submit directly.
	std::atomic<DWORD> streamThread = 0;
	// Submissions handed to DXVK's stream, and how many of those reached the queue.
	std::atomic<uint64_t> streamEnqueued = 0;
	std::atomic<uint64_t> streamSubmitted = 0;

	// CS_ORG_EPOCH_STATS=1: render-thread CPU time per epoch, by segment and by phase of the host's frame,
	// logged every kEpochStatsWindow epochs.
	static constexpr uint32_t kEpochStatsWindow = 600;
	bool epochStats = false;
	// CS_ORG_EARLY_FLUSH (default on, =0 off): hand DXVK's pending D3D11 work to the GPU when an epoch starts.
	bool earlyFlush = true;
	uint32_t epochCount = 0;
	// Queue submissions the graph hands to the hook during the current epoch (each is one DXVK enqueue).
	uint32_t epochHookCalls = 0;
	// DXVK enqueues during the current epoch: one per hook call, or one for the whole epoch when batching.
	uint32_t epochEnqueues = 0;

	// While an epoch runs with enqueueSubmissions, the hook's submissions are copied here (the submit info is
	// only valid during the call) and handed to DXVK together when the epoch ends. Nothing CPU-waits on them
	// before that: the epoch's own work never waits for itself, and the next epoch's slot wait comes later.
	struct PendingSubmit
	{
		std::vector<VkSemaphoreSubmitInfo> waits;
		std::vector<VkCommandBufferSubmitInfo> commandBuffers;
		std::vector<VkSemaphoreSubmitInfo> signals;
	};
	bool batching = false;
	std::atomic<uint64_t> offThreadSubmits = 0;
	std::vector<PendingSubmit> pendingSubmits;  // [0, pendingCount) in use; capacity kept across epochs
	std::size_t pendingCount = 0;
	std::vector<VkSubmitInfo2> pendingInfos;

	// Hands the epoch's collected submissions to DXVK as one enqueue. False when DXVK rejected them.
	bool FlushPendingSubmits(const char* a_label)
	{
		batching = false;
		if (!pendingCount)
			return true;
		pendingInfos.assign(pendingCount, VkSubmitInfo2{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 });
		for (std::size_t i = 0; i < pendingCount; ++i) {
			const auto& pending = pendingSubmits[i];
			auto& info = pendingInfos[i];
			info.waitSemaphoreInfoCount = static_cast<uint32_t>(pending.waits.size());
			info.pWaitSemaphoreInfos = pending.waits.data();
			info.commandBufferInfoCount = static_cast<uint32_t>(pending.commandBuffers.size());
			info.pCommandBufferInfos = pending.commandBuffers.data();
			info.signalSemaphoreInfoCount = static_cast<uint32_t>(pending.signals.size());
			info.pSignalSemaphoreInfos = pending.signals.data();
		}
		DxvkOrgInteropSubmissionBatch batch{};
		batch.version = DXVK_ORG_INTEROP_VERSION;
		batch.submitCount = static_cast<uint32_t>(pendingCount);
		batch.submits = pendingInfos.data();
		batch.onSubmitted = &Impl::OnStreamSubmitted;
		batch.user = this;
		batch.label = a_label;
		pendingCount = 0;
		++epochEnqueues;
		streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
		if (FAILED(enqueueSubmissions(globals::d3d::device, &batch))) {
			streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
			return false;
		}
		return true;
	}
	static constexpr std::size_t kEpochPhases = 12;
	static constexpr std::array<const char*, kEpochPhases> kEpochPhaseNames{
		"wait", "releases", "inputs", "prepare", "retire", "admission", "acquire", "record", "submit", "commit", "signal", "other"
	};
	// With async epochs the render thread's phases are SubmitEpoch's (the rest runs on the host's thread).
	static constexpr std::array<const char*, kEpochPhases> kAsyncEpochPhaseNames{
		"ticket wait", "releases", "inputs", "check", "-", "-", "-", "uploads", "submit", "-", "-", "other"
	};
	struct EpochSegmentStats
	{
		uint32_t epochs = 0;
		double totalUs = 0.0;
		double maxUs = 0.0;
		std::array<double, kEpochPhases> phaseUs{};
		uint64_t hookCalls = 0;
		uint64_t enqueues = 0;
		uint64_t segments = 0;
		uint64_t cachedAdmissions = 0;
		uint64_t batches = 0;
		// The feature's whole call around the epoch (EpochBodyScope), and the ExecuteEpoch time inside those calls.
		uint32_t bodies = 0;
		double bodyUs = 0.0;
		double bodyEpochUs = 0.0;
		double bodyJoinUs = 0.0;
	};
	// The open EpochBodyScope (render thread only): whether an epoch ran inside it, its time, and its joins.
	bool bodyOpen = false;
	bool bodyRanEpoch = false;
	double bodyEpochUs = 0.0;
	double bodyJoinUs = 0.0;
	std::array<EpochSegmentStats, static_cast<std::size_t>(Segment::Count)> epochSegmentStats{};

	// ORG's pass timestamps, per segment. An epoch is one host frame; the segment it ran is remembered by
	// frame number until its timestamps come back, framesInFlight frames later.
	static constexpr std::size_t kSegments = static_cast<std::size_t>(Segment::Count);
	static constexpr std::size_t kSegmentRing = 64;  // > kHostFramesInFlight: a frame's segment outlives its slot's reuse
	struct PassTime
	{
		double inclusiveMs = 0.0;
		double exclusiveMs = 0.0;
	};
	struct SegmentTime
	{
		std::map<std::string, PassTime> passes;
		double spanMs = 0.0;
		std::uint32_t epochs = 0;
	};
	// Written by the render thread when an epoch executes, read when its timestamps come back - on the host's
	// own thread with async epochs.
	std::array<std::atomic<Segment>, kSegmentRing> frameSegments{};
	std::array<SegmentTime, kSegments> segmentTimes{};
	std::uint32_t timedEpochs = 0;
	std::uint32_t completedFrames = 0;  // callbacks, timed or not: tells "no readback" from "no timestamps"

	// One epoch's pass timings, read back. With async epochs they are read on the host's thread and handed to
	// the render thread (which owns segmentTimes and the profiler) through completedInbox: the host's thread
	// publishes a batch only when the previous one was taken, and keeps accumulating otherwise.
	struct CompletedPass
	{
		std::string name;
		double inclusiveMs = 0.0;
		double exclusiveMs = 0.0;
	};
	struct CompletedEpoch
	{
		std::size_t segment = kSegments;
		double spanMs = 0.0;
		bool timed = false;
		std::vector<CompletedPass> passes;
	};
	bool asyncEpochs = false;
	std::vector<CompletedEpoch> hostPending;  // the host's thread only
	std::atomic<std::shared_ptr<std::vector<CompletedEpoch>>> completedInbox;

	void Aggregate(const CompletedEpoch& a_epoch)
	{
		++completedFrames;
		if (!a_epoch.timed || a_epoch.segment >= kSegments)
			return;
		auto& segmentTime = segmentTimes[a_epoch.segment];
		const auto epochSegment = static_cast<Segment>(a_epoch.segment);
		for (const auto& completed : a_epoch.passes) {
			auto& pass = segmentTime.passes[completed.name];
			pass.inclusiveMs += completed.inclusiveMs;
			pass.exclusiveMs += completed.exclusiveMs;
			// Into the main profiling window. A pass with nothing to do costs a few microseconds of empty
			// timestamps: leave those out rather than list them.
			if (globals::profiler && completed.exclusiveMs >= kProfilerMinPassMs)
				globals::profiler->AddExternalSample(ProfilerName(epochSegment, completed.name), static_cast<float>(completed.exclusiveMs));
		}
		segmentTime.spanMs += a_epoch.spanMs;
		++segmentTime.epochs;
		++timedEpochs;
	}

	// Render thread: the timings the host's thread has read back since the last call.
	void TakeCompletedEpochs()
	{
		if (auto batch = completedInbox.exchange(nullptr, std::memory_order_acq_rel))
			for (const auto& epoch : *batch)
				Aggregate(epoch);
	}

	void OnCompletedFrame(std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats)
	{
		CompletedEpoch epoch;
		ReadCompletedEpoch(a_frameNumber, a_stats, epoch);
		if (!asyncEpochs) {
			Aggregate(epoch);
			return;
		}
		hostPending.push_back(std::move(epoch));
		std::shared_ptr<std::vector<CompletedEpoch>> expected;
		auto batch = std::make_shared<std::vector<CompletedEpoch>>(std::move(hostPending));
		if (!completedInbox.compare_exchange_strong(expected, batch, std::memory_order_acq_rel))
			hostPending = std::move(*batch);  // not taken yet: publish with the next one
		else
			hostPending.clear();
	}

	void ReadCompletedEpoch(std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats, CompletedEpoch& a_out)
	{
		const auto segmentIndex = static_cast<std::size_t>(frameSegments[a_frameNumber % kSegmentRing].load(std::memory_order_acquire));
		a_out.segment = segmentIndex;
		if (segmentIndex >= kSegments)
			return;
		const auto& stats = a_stats.GetPassStats();
		const auto& names = a_stats.GetPassNames();
		const double toMs = a_stats.GetGpuTicksToMilliseconds();
		const std::uint64_t serial = a_stats.GetFrameSerial();
		static std::uint32_t described = 0;
		if (described < 3) {
			++described;
			std::size_t matching = 0, zeroTicks = 0;
			std::uint64_t newest = 0;
			for (const auto& pass : stats) {
				newest = (std::max)(newest, pass.gpuSampleSerial);
				matching += pass.gpuSampleSerial == serial;
				zeroTicks += pass.gpuSampleSerial == serial && !pass.gpuBeginTick;
			}
			logger::info("[ORG] completed frame {}: {} passes registered, statistics serial {}, {} read back for it ({} without ticks), newest sample serial {}, {:.6f} ms per tick",
				a_frameNumber, stats.size(), serial, matching, zeroTicks, newest, toMs);
		}
		struct Sample
		{
			std::size_t pass;
			std::uint64_t begin;
			std::uint64_t end;
		};
		std::vector<Sample> samples;
		for (std::size_t i = 0; i < stats.size() && i < names.size(); ++i) {
			if (stats[i].gpuSampleSerial == serial && stats[i].gpuEndTick >= stats[i].gpuBeginTick && stats[i].gpuBeginTick)
				samples.push_back({ i, stats[i].gpuBeginTick, stats[i].gpuEndTick });
		}
		if (samples.empty() || toMs <= 0.0)
			return;
		std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.begin < b.begin; });
		std::uint64_t previousEnd = samples.front().begin;
		for (const auto& sample : samples) {
			CompletedPass pass{ names[sample.pass] };
			pass.inclusiveMs = double(sample.end - sample.begin) * toMs;
			const std::uint64_t from = (std::max)(previousEnd, sample.begin);
			pass.exclusiveMs = sample.end > from ? double(sample.end - from) * toMs : 0.0;
			previousEnd = (std::max)(previousEnd, sample.end);
			a_out.passes.push_back(std::move(pass));
		}
		a_out.spanMs = double(previousEnd - samples.front().begin) * toMs;
		a_out.timed = true;
	}

	static constexpr double kProfilerMinPassMs = 0.003;

	// "<Feature>::<segment> / <pass>", so the profiling window groups graph passes under their feature. These are
	// the only timings of an epoch: a D3D11 timer around ExecuteEpoch would straddle its submission and also count
	// the queue's idle time between DXVK's command lists.
	static std::string ProfilerName(Segment a_segment, const std::string& a_pass)
	{
		switch (a_segment) {
		case Segment::LightCulling:
			return "LightLimitFix::RenderGraphCull / " + a_pass;
		case Segment::ZPrepass:
			return "DrawcallLimitFix::Z-prepass / " + a_pass;
		case Segment::MainOpaque:
			return "DrawcallLimitFix::Main opaque / " + a_pass;
		case Segment::ShadowView:
			return "DrawcallLimitFix::Shadow views / " + a_pass;
		case Segment::SkyOcclusion:
			return "DrawcallLimitFix::Skylighting occlusion / " + a_pass;
		}
		return "RenderGraph::" + a_pass;
	}

	void RecordEpoch(Segment a_segment, std::chrono::steady_clock::duration a_elapsed)
	{
		const double us = std::chrono::duration<double, std::micro>(a_elapsed).count();
		const auto index = static_cast<std::size_t>(a_segment);
		if (index < epochSegmentStats.size()) {
			auto& stats = epochSegmentStats[index];
			const auto& t = host->LastFrameTimings();
			const auto& e = t.execute;
			const std::array<double, kEpochPhases> phases = t.async ?
				std::array<double, kEpochPhases>{ t.ticketWaitUs, t.releaseUs, t.beforePrepareUs, t.checkUs, 0.0, 0.0, 0.0, t.uploadsUs, t.submitUs, 0.0, 0.0, 0.0 } :
				std::array<double, kEpochPhases>{ t.waitUs, t.releaseUs, t.beforePrepareUs, t.updateUs, e.retireUs, e.admissionUs, e.acquireUs, e.recordUs, e.submitUs, e.commitUs, t.signalUs, 0.0 };
			double accounted = 0.0;
			for (std::size_t i = 0; i + 1 < kEpochPhases; ++i) {
				stats.phaseUs[i] += phases[i];
				accounted += phases[i];
			}
			stats.phaseUs[kEpochPhases - 1] += (std::max)(0.0, us - accounted);
			++stats.epochs;
			stats.totalUs += us;
			stats.maxUs = (std::max)(stats.maxUs, us);
			stats.hookCalls += epochHookCalls;
			stats.enqueues += epochEnqueues;
			stats.segments += e.segments;
			stats.cachedAdmissions += e.cachedAdmissions;
			stats.batches += e.batches;
		}
		if (++epochCount < kEpochStatsWindow)
			return;
		for (std::size_t i = 0; i < epochSegmentStats.size(); ++i) {
			auto& stats = epochSegmentStats[i];
			if (!stats.epochs)
				continue;
			std::string phases;
			for (std::size_t p = 0; p < kEpochPhases; ++p)
				if ((asyncEpochs ? kAsyncEpochPhaseNames : kEpochPhaseNames)[p][0] != '-')
					phases += fmt::format("{}{} {:.1f}", p ? ", " : "", (asyncEpochs ? kAsyncEpochPhaseNames : kEpochPhaseNames)[p], stats.phaseUs[p] / stats.epochs);
			if (stats.bodies) {
				// Outside ExecuteEpoch: the feature's own render-thread work around it (inputs, frame blocks, the
				// stale inline build, stats). Joins: waits on workers anywhere in the body, "inputs" included.
				logger::info("[ORG] Epoch body {}: avg {:.1f} us over {} calls; ExecuteEpoch {:.1f}, outside it {:.1f}, joins {:.1f}",
					SegmentLabel(static_cast<Segment>(i)), stats.bodyUs / stats.bodies, stats.bodies, stats.bodyEpochUs / stats.bodies,
					(stats.bodyUs - stats.bodyEpochUs) / stats.bodies, stats.bodyJoinUs / stats.bodies);
			}
			logger::info("[ORG] Epoch CPU {} ({}): avg {:.1f} us, max {:.1f} us over {} epochs; {:.1f} batches, {:.1f} queue submissions ({:.1f} DXVK enqueues) each; {}/{} segment admissions cached; by phase (us): {}",
				SegmentLabel(static_cast<Segment>(i)), enqueueSubmission ? "stream" : "flush", stats.totalUs / stats.epochs, stats.maxUs, stats.epochs,
				double(stats.batches) / stats.epochs, double(stats.hookCalls) / stats.epochs, double(stats.enqueues) / stats.epochs, stats.cachedAdmissions, stats.segments, phases);
			stats = {};
		}
		if (asyncEpochs) {
			const auto async = host->TakeAsyncStats();
			logger::info("[ORG] Async epochs: {} submitted, {} waited for their ticket, {} prepared again after the feature's inputs changed, {} carried uploads",
				async.submitted, async.waited, async.stale, async.uploads);
		}
		epochCount = 0;
	}

	static void LockQueue(void* a_user, VkQueue)
	{
		static_cast<Impl*>(a_user)->interop->LockSubmissionQueue();
	}

	static void UnlockQueue(void* a_user, VkQueue)
	{
		static_cast<Impl*>(a_user)->interop->ReleaseSubmissionQueue();
	}

	static void OnStreamSubmitted(void* a_user, VkResult a_result)
	{
		auto* self = static_cast<Impl*>(a_user);
		if (a_result != VK_SUCCESS && !self->faulted.exchange(true))
			logger::error("[ORG] DXVK failed to submit render graph work ({}); disabling the graph", static_cast<int>(a_result));
		self->streamSubmitted.fetch_add(1, std::memory_order_release);
	}

	// Blocks until everything handed to DXVK's stream has reached the queue, or the timeout expires.
	bool WaitForStreamSubmissions(std::chrono::milliseconds a_timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + a_timeout;
		const uint64_t target = streamEnqueued.load(std::memory_order_acquire);
		for (uint64_t done = streamSubmitted.load(std::memory_order_acquire); done < target; done = streamSubmitted.load(std::memory_order_acquire)) {
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			// Only reached by submissions from threads other than the D3D11 one, which the
			// graph does not issue per frame, so a short poll costs nothing in steady state.
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
		return true;
	}

	// BasicRHI hands every queue submission here instead of calling vkQueueSubmit.
	static const char* SegmentLabel(Segment a_segment)
	{
		switch (a_segment) {
		case Segment::LightCulling:
			return "CS LLF: light culling";
		case Segment::ZPrepass:
			return "CS DCLF: Z-prepass";
		case Segment::MainOpaque:
			return "CS DCLF: main opaque";
		case Segment::ShadowView:
			return "CS DCLF: shadow view";
		case Segment::SkyOcclusion:
			return "CS DCLF: Skylighting occlusion";
		}
		return "CS render graph";
	}

	// The feature's input build inside the epoch, named after the segment's feature so the GPU idle trace's
	// summary sums it under that feature rather than under the shared runtime.
	static const char* InputsLabel(Segment a_segment)
	{
		switch (a_segment) {
		case Segment::LightCulling:
			return "CS LLF: light culling inputs";
		case Segment::ZPrepass:
			return "CS DCLF: Z-prepass inputs";
		case Segment::MainOpaque:
			return "CS DCLF: main opaque inputs";
		case Segment::ShadowView:
			return "CS DCLF: shadow view inputs";
		case Segment::SkyOcclusion:
			return "CS DCLF: Skylighting occlusion inputs";
		}
		return "CS render graph: feature inputs";
	}

	static VkResult Submit(void* a_user, VkQueue a_queue, const VkSubmitInfo2& a_submit)
	{
		auto* self = static_cast<Impl*>(a_user);
		++self->epochHookCalls;
		if (::GetCurrentThreadId() == self->streamThread.load(std::memory_order_relaxed)) {
			if (self->batching) {
				if (self->pendingCount == self->pendingSubmits.size())
					self->pendingSubmits.emplace_back();
				auto& pending = self->pendingSubmits[self->pendingCount++];
				pending.waits.assign(a_submit.pWaitSemaphoreInfos, a_submit.pWaitSemaphoreInfos + a_submit.waitSemaphoreInfoCount);
				pending.commandBuffers.assign(a_submit.pCommandBufferInfos, a_submit.pCommandBufferInfos + a_submit.commandBufferInfoCount);
				pending.signals.assign(a_submit.pSignalSemaphoreInfos, a_submit.pSignalSemaphoreInfos + a_submit.signalSemaphoreInfoCount);
				return VK_SUCCESS;
			}
			// In D3D11 stream order: after every D3D11 command issued so far, before every later
			// one. Nothing is waited for here.
			++self->epochEnqueues;
			DxvkOrgInteropSubmission submission{};
			submission.version = DXVK_ORG_INTEROP_VERSION;
			submission.waitCount = a_submit.waitSemaphoreInfoCount;
			submission.waits = a_submit.pWaitSemaphoreInfos;
			submission.commandBufferCount = a_submit.commandBufferInfoCount;
			submission.commandBuffers = a_submit.pCommandBufferInfos;
			submission.signalCount = a_submit.signalSemaphoreInfoCount;
			submission.signals = a_submit.pSignalSemaphoreInfos;
			submission.onSubmitted = &Impl::OnStreamSubmitted;
			submission.user = self;
			// Nsight and RenderDoc group the epoch's command buffers under this (DXVK adds it only
			// while a capture tool is attached).
			submission.label = SegmentLabel(RenderGraphRuntime::Get().CurrentSegment());
			self->streamEnqueued.fetch_add(1, std::memory_order_acq_rel);
			if (FAILED(self->enqueueSubmission(globals::d3d::device, &submission))) {
				self->streamEnqueued.fetch_sub(1, std::memory_order_acq_rel);
				return VK_ERROR_UNKNOWN;
			}
			return VK_SUCCESS;
		}

		// Another thread: keep the graph's submissions in order by letting everything already in
		// DXVK's stream reach the queue first, then submit directly under DXVK's queue lock. With async epochs
		// nothing but the render thread submits (the host's thread only prepares): count it if something does.
		if (self->asyncEpochs && self->offThreadSubmits.fetch_add(1, std::memory_order_relaxed) == 0)
			logger::warn("[ORG] A graph submission came from a thread other than the render thread while epochs are async");
		if (!self->WaitForStreamSubmissions(std::chrono::seconds(10))) {
			logger::error("[ORG] Timed out waiting for queued render graph submissions");
			return VK_TIMEOUT;
		}
		self->interop->LockSubmissionQueue();
		const VkResult result = self->queueSubmit2(a_queue, 1, &a_submit, VK_NULL_HANDLE);
		self->interop->ReleaseSubmissionQueue();
		return result;
	}

	static void OnDxvkTeardown(void*, VkDevice)
	{
		logger::info("[ORG] DXVK is destroying its device; retiring the render graph first");
		GpuIdleTrace::Shutdown();
		RenderGraphRuntime::Get().Shutdown();
	}
};

RenderGraphRuntime& RenderGraphRuntime::Get()
{
	static RenderGraphRuntime s_runtime;
	return s_runtime;
}

void RenderGraphRuntime::RequestDeviceFeatures(HMODULE a_dxvkD3D11)
{
	if (EnvDisabled())
		return;
	auto request = ResolveExport<PFN_dxvkRequestDeviceFeatures>(a_dxvkD3D11, "dxvkRequestDeviceFeatures");
	if (!request) {
		logger::warn("[ORG] dxvkRequestDeviceFeatures is unavailable - this DXVK build cannot host the render graph");
		return;
	}
	const DxvkOrgInteropFeatureRequest desc{ DXVK_ORG_INTEROP_VERSION, static_cast<uint32_t>(std::size(kRequestedFeatures)), kRequestedFeatures };
	if (FAILED(request(&desc)))
		logger::warn("[ORG] DXVK rejected the render graph device feature request");
}

bool RenderGraphRuntime::Initialize()
{
	if (impl)
		return true;
	if (attempted)
		return false;
	attempted = true;

	auto disable = [&](std::string a_reason) {
		disabledReason = std::move(a_reason);
		logger::info("[ORG] Render graph disabled: {}", disabledReason);
		impl.reset();
		return false;
	};

	if (EnvDisabled())
		return disable("CS_ORG=0");
	if (EnvFeaturesOnly())
		return disable("CS_ORG=features (device features requested, graph not created)");
	if (!DxvkLoader::IsLoaded())
		return disable("DXVK is not loaded (native D3D11)");

	HMODULE d3d11 = ::GetModuleHandleW(L"dxvk_d3d11.dll");
	auto getInfo = ResolveExport<PFN_dxvkGetInteropDeviceInfo>(d3d11, "dxvkGetInteropDeviceInfo");
	auto state = std::make_unique<Impl>();
	state->createBufferFromVkBuffer = ResolveExport<PFN_dxvkCreateBufferFromVkBuffer>(d3d11, "dxvkCreateBufferFromVkBuffer");
	state->setTeardownCallback = ResolveExport<PFN_dxvkSetDeviceTeardownCallback>(d3d11, "dxvkSetDeviceTeardownCallback");
	if (!getInfo || !state->createBufferFromVkBuffer || !state->setTeardownCallback)
		return disable("the DXVK build lacks the render graph interop exports");
	// Optional: game resources for the graph (Drawcall Limit Fix).
	state->getResourceInfo = ResolveExport<PFN_dxvkGetInteropResourceInfo>(d3d11, "dxvkGetInteropResourceInfo");
	// Optional: without it, each epoch flushes and waits for DXVK's command stream instead.
	state->enqueueSubmission = ResolveExport<PFN_dxvkEnqueueInteropSubmission>(d3d11, "dxvkEnqueueInteropSubmission");
	if (EnvEquals("CS_ORG_SUBMIT", "flush"))
		state->enqueueSubmission = nullptr;
	if (state->enqueueSubmission && !EnvEquals("CS_ORG_BATCH_SUBMIT", "0"))
		state->enqueueSubmissions = ResolveExport<PFN_dxvkEnqueueInteropSubmissions>(d3d11, "dxvkEnqueueInteropSubmissions");
	state->epochStats = EnvEquals("CS_ORG_EPOCH_STATS", "1");
	state->earlyFlush = !EnvEquals("CS_ORG_EARLY_FLUSH", "0");

	auto d3dDevice = globals::d3d::device;
	if (!d3dDevice || FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), state->interop.put_void())))
		return disable("IDXGIVkInteropDevice is unavailable");

	DxvkOrgInteropDeviceInfo info{};
	info.version = DXVK_ORG_INTEROP_VERSION;
	if (FAILED(getInfo(d3dDevice, &info)))
		return disable("DXVK could not describe its Vulkan device");
	if (!HasExtension(info, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME)) {
		// The one cause seen in practice, named so the menu does not leave it to be guessed.
		if (::GetModuleHandleW(L"renderdoc.dll"))
			return disable("the device was created without VK_EXT_descriptor_heap: RenderDoc is loaded, and RenderDoc does not support "
						   "descriptor heaps yet (its capture layer hides the extension). Turn off RenderDoc capture to use the render graph.");
		return disable("the device was created without VK_EXT_descriptor_heap (the driver or a Vulkan layer does not expose it)");
	}

	rhi::vulkan::AdoptedVulkanDeviceInfo adopt{};
	adopt.getInstanceProcAddr = info.getInstanceProcAddr;
	adopt.instance = info.instance;
	adopt.instanceApiVersion = info.instanceApiVersion;
	adopt.physicalDevice = info.physicalDevice;
	adopt.device = info.device;
	adopt.enabledDeviceExtensions = info.enabledExtensions;
	adopt.enabledDeviceExtensionCount = info.enabledExtensionCount;
	adopt.diagnostics.registerMarker = &Aftermath::RegisterOrgCheckpoint;
	adopt.diagnostics.detailedCommands = EnvEquals("CS_ORG_DETAILED_CHECKPOINTS", "1");
	// Whether VK_EXT_debug_utils is on, so that BasicRHI names objects and labels passes only then.
	adopt.enabledInstanceExtensions = info.enabledInstanceExtensions;
	adopt.enabledInstanceExtensionCount = info.enabledInstanceExtensionCount;
	adopt.enabledFeatureChain = info.enabledFeatures;
	adopt.queues[0] = { info.graphicsQueue, info.graphicsQueueFamily, info.graphicsQueueIndex };
	adopt.submissionHooks = { state.get(), &Impl::LockQueue, &Impl::UnlockQueue, nullptr };
	if (state->enqueueSubmission) {
		auto getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(info.getInstanceProcAddr(info.instance, "vkGetDeviceProcAddr"));
		state->queueSubmit2 = getDeviceProcAddr ? reinterpret_cast<PFN_vkQueueSubmit2>(getDeviceProcAddr(info.device, "vkQueueSubmit2")) : nullptr;
		if (state->queueSubmit2) {
			adopt.submissionHooks.submit = &Impl::Submit;
			state->streamThread = ::GetCurrentThreadId();
		} else {
			state->enqueueSubmission = nullptr;
		}
	}
	if (rhi::vulkan::AdoptVulkanDevice(adopt, *state->device) != rhi::Result::Ok || !*state->device)
		return disable("BasicRHI could not adopt DXVK's Vulkan device");

	try {
		org::PersistentGraphHost::Desc desc{};
		desc.device = state->device->Get();
		desc.backend = rhi::Backend::Vulkan;
		// Everything DXVK submitted before an epoch and everything it submits after
		// is ordered against the graph by these barriers (same queue, submission order).
		desc.queueBoundary = { .entry = true, .exit = true };
		// Host frames are epochs: several per game frame. Slots are reused this many epochs later, so the
		// ring covers ~3 game frames of epochs and the slot wait is backpressure, not a wait on the GPU
		// finishing an earlier epoch of the same frame.
		desc.framesInFlight = kHostFramesInFlight;
		// CS_ORG_CLOSED (default on with epochs, =0 off): each epoch leaves every resource in its home state
		// and starts with a full barrier, so its admission is independent of the epochs before it and cached.
		desc.closedExecutions = EpochsEnabled() && !EnvEquals("CS_ORG_CLOSED", "0");
		// The segments in the order a frame runs them: the shadow views at AfterShadowMaps, the Z-prepass at
		// the depth pass, Skylighting's map, Light Limit Fix's culling at Prepass, the colour pass where the main
		// pass's opaque batches end.
		if (EpochsEnabled())
			desc.epochOrder = { EpochOf(Segment::ShadowView), EpochOf(Segment::ZPrepass), EpochOf(Segment::SkyOcclusion), EpochOf(Segment::LightCulling),
				EpochOf(Segment::MainOpaque) };
		const bool closed = desc.closedExecutions;
		state->host = std::make_unique<org::PersistentGraphHost>(std::move(desc));
		// CS_ORG_ASYNC_EPOCHS (default on, =0 off): each epoch's work is prepared, admitted and recorded ahead of
		// its epoch point on the host's own thread; at the point the render thread only writes the epoch's
		// latches, records the queued uploads and submits. Needs epochs and closed executions.
		state->asyncEpochs = !EnvEquals("CS_ORG_ASYNC_EPOCHS", "0") && EpochsEnabled() && closed;
	} catch (const std::exception& e) {
		return disable(std::string("graph host creation failed: ") + e.what());
	}

	state->setTeardownCallback(&Impl::OnDxvkTeardown, nullptr);
	state->host->SetCompletedFrameCallback([self = state.get()](std::uint64_t a_frameNumber, const org::runtime::IStatisticsService& a_stats) {
		self->OnCompletedFrame(a_frameNumber, a_stats);
	});
	if (state->asyncEpochs) {
		try {
			std::vector<std::uint32_t> epochs{ EpochOf(Segment::ShadowView), EpochOf(Segment::ZPrepass), EpochOf(Segment::SkyOcclusion),
				EpochOf(Segment::LightCulling), EpochOf(Segment::MainOpaque) };
			state->host->SetAsyncEpochs(std::move(epochs));
			logger::info("[ORG] Async epochs: each epoch is prepared and recorded ahead on the graph host's thread; the render thread submits");
		} catch (const std::exception& e) {
			logger::warn("[ORG] Async epochs unavailable ({}); epochs run synchronously", e.what());
			state->asyncEpochs = false;
		}
	}
	impl = std::move(state);
	disabledReason.clear();
	{
		// CS_NVPERF_RANGES: Nsight Perf counters per range; nvperf_grfx_host.dll ships beside the DXVK DLLs.
		wchar_t dxvkPath[MAX_PATH]{};
		if (::GetModuleFileNameW(d3d11, dxvkPath, MAX_PATH))
			NvPerfBridge::Initialize(info, d3d11, std::filesystem::path(dxvkPath).parent_path().c_str(), *impl->host);
	}
	logger::info("[ORG] Render graph adopted DXVK's Vulkan device (queue family {}, index {}); submissions {}",
		info.graphicsQueueFamily, info.graphicsQueueIndex,
		impl->enqueueSubmission ? "go through DXVK's command stream" : "flush DXVK each epoch");
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	{
		// DXC ships beside the DXVK DLLs; load it from there, not by name (another dxcompiler.dll,
		// without SPIR-V support, may already be in the process).
		wchar_t dxvkPath[MAX_PATH]{};
		std::filesystem::path compilerDirectory;
		if (::GetModuleFileNameW(d3d11, dxvkPath, MAX_PATH))
			compilerDirectory = std::filesystem::path(dxvkPath).parent_path();
		impl->shaderCompiler = std::make_unique<org::services::ShaderCompiler>(std::filesystem::path(L"Data/ShaderCache/ORG"), compilerDirectory);
		logger::info("[ORG] Runtime SPIR-V compilation {}", impl->shaderCompiler->Available() ? "available" : "unavailable (no dxcompiler.dll beside the DXVK DLLs)");
		if (!impl->shaderCompiler->Available())
			impl->shaderCompiler.reset();
	}
#endif
	IndirectCommandsFeatureInfo indirect{};
	if ((*impl->device)->QueryFeatureInfo(&indirect.header) == rhi::Result::Ok) {
		logger::info("[ORG] Indirect commands: generated commands {}, index buffer arguments {}, pipeline sets {} (up to {} pipelines); game resource export {}",
			indirect.constantArguments, indirect.indexBufferArguments, indirect.pipelineSets, indirect.maxPipelineSetCount,
			impl->getResourceInfo ? "available" : "missing");
	}
	return true;
}

void RenderGraphRuntime::Shutdown()
{
	if (!impl)
		return;
	auto state = std::move(impl);
	if (state->setTeardownCallback)
		state->setTeardownCallback(nullptr, nullptr);
	// Graph work still in DXVK's stream must reach the queue before the host waits on its
	// timelines. If DXVK dropped it (its context is already gone), waiting would never end:
	// leak the graph instead, as at process exit.
	if (state->enqueueSubmission && !state->WaitForStreamSubmissions(std::chrono::seconds(5))) {
		logger::warn("[ORG] Queued render graph submissions never reached the queue; leaking the graph");
		if (state->device && *state->device)
			rhi::vulkan::abandon_device(state->device->Get());
		(void)state->host.release();
		(void)state.release();
		disabledReason = "shut down";
		return;
	}
	try {
		// Waits only for the graph's own timelines, then releases every graph object.
		state->host.reset();
	} catch (const std::exception& e) {
		logger::error("[ORG] Render graph shutdown failed: {}", e.what());
	}
	// Non-owning: BasicRHI never destroys the adopted VkDevice.
	state->device.reset();
	disabledReason = "shut down";
}

RenderGraphRuntime::~RenderGraphRuntime()
{
	// Static destruction at process exit: DXVK may already be gone, so no Vulkan
	// call is safe. Leak the graph state instead of tearing it down.
	if (impl) {
		if (impl->device && *impl->device)
			rhi::vulkan::abandon_device(impl->device->Get());
		(void)impl->host.release();
		(void)impl.release();
	}
}

bool RenderGraphRuntime::IsActive() const
{
	return impl && impl->host && !impl->faulted;
}

const std::string& RenderGraphRuntime::GetDisabledReason() const
{
	return disabledReason;
}

org::PersistentGraphHost* RenderGraphRuntime::Host()
{
	return IsActive() ? impl->host.get() : nullptr;
}

std::shared_ptr<const void> RenderGraphRuntime::DeviceOwner() const
{
	return IsActive() ? impl->device : std::shared_ptr<const void>{};
}

RenderGraphRuntime::EpochBodyScope::EpochBodyScope(Segment a_segment) :
	segment(a_segment), start(std::chrono::steady_clock::now())
{
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.impl || !runtime.impl->epochStats)
		return;
	auto& state = *runtime.impl;
	state.bodyOpen = true;
	state.bodyRanEpoch = false;
	state.bodyEpochUs = state.bodyJoinUs = 0.0;
}

RenderGraphRuntime::EpochBodyScope::~EpochBodyScope()
{
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.impl || !runtime.impl->bodyOpen)
		return;
	auto& state = *runtime.impl;
	state.bodyOpen = false;
	const auto index = static_cast<std::size_t>(segment);
	if (!state.bodyRanEpoch || index >= state.epochSegmentStats.size())
		return;
	auto& stats = state.epochSegmentStats[index];
	++stats.bodies;
	stats.bodyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
	stats.bodyEpochUs += state.bodyEpochUs;
	stats.bodyJoinUs += state.bodyJoinUs;
}

void RenderGraphRuntime::AddEpochJoinWait(std::chrono::steady_clock::duration a_waited)
{
	auto& runtime = RenderGraphRuntime::Get();
	if (runtime.impl && runtime.impl->bodyOpen)
		runtime.impl->bodyJoinUs += std::chrono::duration<double, std::micro>(a_waited).count();
}

bool RenderGraphRuntime::EpochsEnabled()
{
	static const bool enabled = !EnvEquals("CS_ORG_EPOCHS", "0");
	return enabled;
}

std::uint32_t RenderGraphRuntime::EpochOf(Segment a_segment)
{
	return EpochsEnabled() ? static_cast<std::uint32_t>(a_segment) : UINT32_MAX;
}

bool RenderGraphRuntime::ExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare,
	std::shared_ptr<const void> a_resourceOwner)
{
	ZoneScopedN("CS.ORG.ExecuteEpoch");
	if (!IsActive())
		return false;
	// Read by passes while the frame prepares and records, all before ExecuteFrame returns.
	segment = a_segment;
	ZoneText(Impl::SegmentLabel(a_segment), std::strlen(Impl::SegmentLabel(a_segment)));
	// Where the epoch's submissions land in the D3D11 stream; the submissions themselves carry a queue
	// label of the same name (Submit). The event spans the epoch's CPU side: the feature's inputs, the
	// graph's preparation and recording, and the hand-off to DXVK.
	ScopedPerfEvent epochEvent(Impl::SegmentLabel(a_segment));
	const auto start = std::chrono::steady_clock::now();
	if (impl->enqueueSubmission) {
		// The graph's batches go into DXVK's command stream at this point, so they land between
		// the D3D11 work issued before and after; the entry/exit barriers order the memory.
		impl->streamThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		// DXVK closes its current command list only when the epoch's first submission is enqueued, after the
		// graph has prepared and recorded: the D3D11 work issued just before the epoch would sit on the CPU for
		// that whole time while the GPU runs dry. Flushing here (asynchronous under DXVK) submits it now; the
		// enqueue then finds nothing pending, so the number of submissions does not change.
		if (impl->earlyFlush) {
			ZoneScopedN("CS.ORG.FlushNativeProducer");
			globals::d3d::context->Flush();
		}
	} else {
		// Older DXVK (or CS_ORG_SUBMIT=flush): submit and wait for every D3D11 command recorded
		// so far, then submit the graph directly under DXVK's queue lock.
		impl->interop->FlushRenderingCommands();
	}
	const bool async = impl->asyncEpochs;
	if (!async)
		impl->frameSegments[impl->host->FramesExecuted() % Impl::kSegmentRing].store(a_segment, std::memory_order_release);
	impl->TakeCompletedEpochs();
	impl->epochHookCalls = 0;
	impl->epochEnqueues = 0;
	impl->batching = impl->enqueueSubmission && impl->enqueueSubmissions;
	impl->pendingCount = 0;
	try {
		std::function<void(org::RenderGraph&)> beforePrepare;
		if (a_beforePrepare) {
			beforePrepare = [&](org::RenderGraph& a_graph) {
				ScopedPerfEvent inputsEvent(Impl::InputsLabel(a_segment));
				a_beforePrepare(a_graph);
			};
		}
		if (async) {
			impl->host->SubmitEpoch(EpochOf(a_segment), beforePrepare, a_resourceOwner);
			impl->frameSegments[impl->host->LastHostFrame() % Impl::kSegmentRing].store(a_segment, std::memory_order_release);
		} else {
			impl->host->ExecuteFrame(nullptr, beforePrepare, EpochOf(a_segment), a_resourceOwner);
		}
		if (impl->batching && !impl->FlushPendingSubmits(Impl::SegmentLabel(a_segment)))
			throw std::runtime_error("DXVK rejected the epoch's submissions");
		if (impl->epochStats) {
			const auto elapsed = std::chrono::steady_clock::now() - start;
			impl->RecordEpoch(a_segment, elapsed);
			if (impl->bodyOpen) {
				impl->bodyRanEpoch = true;
				impl->bodyEpochUs += std::chrono::duration<double, std::micro>(elapsed).count();
			}
		}
		return true;
	} catch (const std::exception& e) {
		// What the graph did submit still goes to DXVK in order (it has committed it); then the graph stops.
		if (impl->batching)
			(void)impl->FlushPendingSubmits(Impl::SegmentLabel(a_segment));
		// A failed graph must not take the game down: callers fall back to D3D11.
		logger::error("[ORG] Render graph epoch failed, disabling the graph: {}", e.what());
		impl->faulted = true;
		disabledReason = e.what();
		return false;
	}
}

void RenderGraphRuntime::ReportGpuTimings(std::uint32_t a_frames, bool a_log)
{
	if (!impl || !a_frames)
		return;
	impl->TakeCompletedEpochs();
	const double frames = double(a_frames);
	std::string summary;
	if (a_log)
		logger::info("[ORG] GPU time from ORG's pass timestamps, ms per frame over {} frames ({} epochs read back, {} completed):", a_frames, impl->timedEpochs, impl->completedFrames);
	for (std::size_t s = 0; s < Impl::kSegments; ++s) {
		auto& segmentTime = impl->segmentTimes[s];
		if (!segmentTime.epochs)
			continue;
		const char* label = Impl::SegmentLabel(static_cast<Segment>(s));
		std::vector<std::pair<std::string, Impl::PassTime>> passes(segmentTime.passes.begin(), segmentTime.passes.end());
		std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.second.exclusiveMs > b.second.exclusiveMs; });
		double exclusiveTotal = 0.0;
		std::string detail;
		for (const auto& [name, time] : passes) {
			exclusiveTotal += time.exclusiveMs;
			// Every epoch runs every pass; the ones with nothing to do in this segment cost ~nothing.
			if (time.exclusiveMs / frames >= 0.005)
				detail += fmt::format("{}{} {:.3f} ({:.3f} incl)", detail.empty() ? "" : ", ", name, time.exclusiveMs / frames, time.inclusiveMs / frames);
		}
		if (a_log)
			logger::info("[ORG]   {}: {:.3f} ms span, {:.3f} ms in passes, {:.1f} epochs/frame; by pass (exclusive): {}",
				label, segmentTime.spanMs / frames, exclusiveTotal / frames, segmentTime.epochs / frames, detail.empty() ? "-" : detail);
		summary += fmt::format("{}{}: {:.3f} ms/frame ({:.1f} epochs/frame)", summary.empty() ? "" : "\n", label, segmentTime.spanMs / frames, segmentTime.epochs / frames);
		segmentTime = {};
	}
	impl->timedEpochs = 0;
	impl->completedFrames = 0;
	gpuTimingSummary = std::move(summary);
}

org::services::ShaderCompiler* RenderGraphRuntime::ShaderCompiler()
{
#if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	return impl ? impl->shaderCompiler.get() : nullptr;
#else
	return nullptr;
#endif
}

const std::vector<std::filesystem::path>& RenderGraphRuntime::ShaderSourceFiles()
{
	static const std::vector<std::filesystem::path> files = [] {
		std::vector<std::filesystem::path> found;
		std::error_code ec;
		for (auto it = std::filesystem::recursive_directory_iterator(kShaderDirectory, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			const auto extension = it->path().extension();
			if (it->is_regular_file() && (extension == ".hlsl" || extension == ".hlsli"))
				found.push_back(it->path());
		}
		return found;
	}();
	return files;
}

bool RenderGraphRuntime::DescribeResource(IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info)
{
	if (!impl || !impl->getResourceInfo || !a_object)
		return false;
	a_info = {};
	a_info.version = DXVK_ORG_INTEROP_VERSION;
	return SUCCEEDED(impl->getResourceInfo(globals::d3d::device, a_object, &a_info));
}

winrt::com_ptr<ID3D11Buffer> RenderGraphRuntime::WrapBuffer(org::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc)
{
	winrt::com_ptr<ID3D11Buffer> result;
	if (!IsActive())
		return result;
	rhi::VulkanResourceInfo info{};
	const auto api = a_buffer.GetAPIResource();
	if (!rhi::vulkan::get_resource_info(api, info) || !info.resource) {
		logger::error("[ORG] WrapBuffer: '{}' has no Vulkan buffer", a_buffer.GetName());
		return result;
	}
	const auto buffer = rhi::vulkan::from_native_void<VkBuffer>(info.resource);
	if (FAILED(impl->createBufferFromVkBuffer(globals::d3d::device, &a_desc, buffer, result.put()))) {
		logger::error("[ORG] WrapBuffer: DXVK rejected '{}'", a_buffer.GetName());
		result = nullptr;
	}
	return result;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph (CS_ORG_ROOT missing): the runtime never activates
// and every feature keeps its D3D11 path.
#	include "RenderGraphRuntime.h"

struct RenderGraphRuntime::Impl
{};

RenderGraphRuntime& RenderGraphRuntime::Get()
{
	static RenderGraphRuntime s_runtime;
	return s_runtime;
}

void RenderGraphRuntime::RequestDeviceFeatures(HMODULE) {}

bool RenderGraphRuntime::Initialize()
{
	disabledReason = "built without OpenRenderGraph";
	return false;
}

void RenderGraphRuntime::Shutdown() {}
RenderGraphRuntime::~RenderGraphRuntime() = default;
bool RenderGraphRuntime::IsActive() const { return false; }
const std::string& RenderGraphRuntime::GetDisabledReason() const { return disabledReason; }
org::PersistentGraphHost* RenderGraphRuntime::Host() { return nullptr; }
std::shared_ptr<const void> RenderGraphRuntime::DeviceOwner() const { return {}; }
bool RenderGraphRuntime::ExecuteEpoch(Segment, const std::function<void(org::RenderGraph&)>&,
	std::shared_ptr<const void>) { return false; }
bool RenderGraphRuntime::DescribeResource(IUnknown*, DxvkOrgInteropResourceInfo&) { return false; }
org::services::ShaderCompiler* RenderGraphRuntime::ShaderCompiler() { return nullptr; }
const std::vector<std::filesystem::path>& RenderGraphRuntime::ShaderSourceFiles()
{
	static const std::vector<std::filesystem::path> none;
	return none;
}
winrt::com_ptr<ID3D11Buffer> RenderGraphRuntime::WrapBuffer(org::Resource&, const D3D11_BUFFER_DESC&) { return nullptr; }
void RenderGraphRuntime::ReportGpuTimings(std::uint32_t, bool) {}
#endif
