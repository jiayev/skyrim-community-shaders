#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "NvPerfBridge.h"

#	include "Globals.h"
#	include "RenderGraph/DxvkOrgInterop.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Telemetry/NvPerfCapture.h>

#	include <ShlObj.h>

#	include <algorithm>
#	include <atomic>
#	include <cmath>
#	include <cstdlib>
#	include <map>
#	include <set>
#	include <sstream>
#	include <vector>

namespace NvPerfBridge
{
	namespace
	{
		namespace nvperf = org::telemetry::nvperf;

		constexpr const char* kQueueName = "Graphics";

		// NVPW's enums (nvperf_host.h), so this file needs none of the SDK's headers.
		constexpr std::uint8_t kCounter = 0, kRatio = 1, kThroughput = 2;
		constexpr std::uint8_t kRollupAvg = 0, kRollupSum = 3;
		constexpr std::uint16_t kSubmetricNone = 0, kSubmetricPerCycleActive = 10, kSubmetricPctOfPeakActive = 15, kSubmetricPctOfPeak = 16, kSubmetricRatio = 21;

		// The Win32 environment: the switches file is applied with SetEnvironmentVariable after the CRT copied its own.
		std::string Env(const char* a_name)
		{
			char value[4096]{};
			const DWORD length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
			return length && length < sizeof(value) ? std::string(value, length) : std::string{};
		}

		std::vector<std::string> Split(const std::string& a_text)
		{
			std::vector<std::string> parts;
			std::stringstream stream(a_text);
			for (std::string part; std::getline(stream, part, ',');) {
				while (!part.empty() && part.front() == ' ')
					part.erase(part.begin());
				while (!part.empty() && part.back() == ' ')
					part.pop_back();
				if (!part.empty())
					parts.push_back(part);
			}
			return parts;
		}

		struct State
		{
			nvperf::QueueTarget target;
			PFN_dxvkEnqueueQueueCallback enqueueQueueCallback = nullptr;
			PFN_dxvkEmitCommandBufferCallback emitCommandBufferCallback = nullptr;
			std::uint64_t frame = 0;
			std::uint64_t startFrame = 1200;
			std::uint32_t samples = 3;
			std::uint32_t sampled = 0;
			bool armed = false;  // render thread's view: ranges record while set
			// Frames between arming and the first pass: every command buffer that executes in a pass must have been
			// recorded while armed, and the graph host records its epochs up to its frame slots ahead.
			std::uint64_t lead = 4;
			std::uint64_t firstPassFrame = 0;
			std::vector<std::string> filters;  // CS_NVPERF_RANGES, for the log's grouping
			std::set<std::string> names;  // interned: the command buffer callbacks keep pointers into it
			std::vector<bool> events;     // per open perf event: whether it pushed a range
			// DXVK's worker thread only, in stream order: the engine ranges open in the command buffer it records, outermost
			// first (null: the range was not pushed). A DXVK flush closes them in the old command buffer and reopens them in
			// the new one, so that each command buffer's ranges are balanced; one event then counts as several occurrences.
			std::vector<const char*> open;
			std::atomic<std::uint64_t> misplacedPops{ 0 };
		};
		State* g_state = nullptr;

		// DXVK's worker thread, in its command stream.
		void PushInDxvk(void* a_user, VkCommandBuffer a_commandBuffer)
		{
			const auto name = static_cast<const char*>(a_user);
			g_state->open.push_back(nvperf::PushRange(g_state->target, a_commandBuffer, kQueueName, name) ? name : nullptr);
		}
		void PopInDxvk(void*, VkCommandBuffer a_commandBuffer)
		{
			if (g_state->open.empty())
				return;
			const bool pushed = g_state->open.back() != nullptr;
			g_state->open.pop_back();
			if (pushed && !nvperf::PopRange(rhi::Backend::Vulkan, a_commandBuffer))
				g_state->misplacedPops.fetch_add(1, std::memory_order_relaxed);
		}
		void CloseInDxvk(void*, VkCommandBuffer a_commandBuffer)
		{
			for (auto name = g_state->open.rbegin(); name != g_state->open.rend(); ++name)
				if (*name && !nvperf::PopRange(rhi::Backend::Vulkan, a_commandBuffer))
					g_state->misplacedPops.fetch_add(1, std::memory_order_relaxed);
		}
		void ReopenInDxvk(void*, VkCommandBuffer a_commandBuffer)
		{
			for (auto& name : g_state->open)
				if (name && !nvperf::PushRange(g_state->target, a_commandBuffer, kQueueName, name))
					name = nullptr;  // the capture ended in between
		}

		// DXVK's submission thread, with the queue locked: the frame's pass ends and the next one begins.
		void FrameBoundary(void* a_user, VkQueue)
		{
			const auto frame = reinterpret_cast<std::uintptr_t>(a_user);
			nvperf::EndFrameCapture(g_state->target, frame);
			nvperf::BeginFrameCapture(g_state->target, frame + 1);
		}

		void LogResult(const nvperf::CaptureResult& a_result)
		{
			if (!a_result.success && a_result.ranges.empty()) {
				logger::warn("[NVPerf] capture {} failed: {}", a_result.sampleId, a_result.error);
				return;
			}
			// Per range filter (an engine event's name carries per-call detail, so one filter matches many names): counters
			// summed, rates weighted by each range's time. A value the capture could not complete (NaN: the range was
			// missing from a replay pass) is left out and counted.
			const auto& metrics = a_result.metrics;
			std::size_t timeMetric = metrics.size();
			for (std::size_t m = 0; m < metrics.size(); ++m)
				if (metrics[m].name == "gpu__time_duration")
					timeMetric = m;
			struct Totals
			{
				std::uint32_t occurrences = 0;
				std::vector<double> values, weights;
				std::uint32_t incomplete = 0;
			};
			std::map<std::string, Totals> byFilter;
			for (const auto& range : a_result.ranges) {
				const auto filter = std::ranges::find_if(g_state->filters, [&](const std::string& f) {
					return f.back() == '*' ? range.passName.starts_with(std::string_view(f).substr(0, f.size() - 1)) : range.passName == f;
				});
				auto& totals = byFilter[filter == g_state->filters.end() ? range.passName : *filter];
				totals.values.resize(metrics.size());
				totals.weights.resize(metrics.size());
				const double time = timeMetric < range.values.size() ? range.values[timeMetric] : 1.0;
				bool incomplete = false;
				for (std::size_t m = 0; m < metrics.size() && m < range.values.size(); ++m) {
					const double value = range.values[m];
					if (std::isnan(value)) {
						incomplete = true;
						continue;
					}
					const bool summed = metrics[m].metricType == kCounter && metrics[m].rollupOp == kRollupSum;
					const double weight = summed || std::isnan(time) ? 1.0 : time;
					totals.values[m] += value * weight;
					totals.weights[m] += weight;
				}
				totals.incomplete += incomplete;
				++totals.occurrences;
			}
			logger::info("[NVPerf] capture {} (frames {}-{}, {} replay passes, chip {}): {} ranges{}", a_result.sampleId, a_result.startFrame, a_result.endFrame,
				a_result.scheduledPasses, a_result.chipName, a_result.ranges.size(), a_result.success ? "" : fmt::format(" ({})", a_result.error));
			for (const auto& [name, totals] : byFilter) {
				std::string text;
				for (std::size_t m = 0; m < metrics.size(); ++m) {
					const auto& metric = metrics[m];
					const bool summed = metric.metricType == kCounter && metric.rollupOp == kRollupSum;
					const double value = !totals.weights[m] ? std::nan("") : summed ? totals.values[m] : totals.values[m] / totals.weights[m];
					text += fmt::format("{}{}={:.4g}", text.empty() ? "" : ", ", metric.outputName.empty() ? metric.name : metric.outputName, value);
				}
				logger::info("[NVPerf]   {} (x{}{}): {}", name, totals.occurrences, totals.incomplete ? fmt::format(", {} incomplete", totals.incomplete) : "", text);
			}
			if (const auto misplaced = g_state->misplacedPops.load())
				logger::warn("[NVPerf]   {} engine range pops found no open range in their command buffer", misplaced);
		}
	}

	void Initialize(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, const wchar_t* a_binDirectory, org::PersistentGraphHost& a_host)
	{
		const auto ranges = Split(Env("CS_NVPERF_RANGES"));
		if (ranges.empty())
			return;
		auto state = std::make_unique<State>();
		state->filters = ranges;
		const auto module = static_cast<HMODULE>(a_dxvkModule);
		state->enqueueQueueCallback = reinterpret_cast<PFN_dxvkEnqueueQueueCallback>(::GetProcAddress(module, "dxvkEnqueueQueueCallback"));
		state->emitCommandBufferCallback = reinterpret_cast<PFN_dxvkEmitCommandBufferCallback>(::GetProcAddress(module, "dxvkEmitCommandBufferCallback"));
		const auto setBoundaryCallbacks = reinterpret_cast<PFN_dxvkSetCommandBufferBoundaryCallbacks>(::GetProcAddress(module, "dxvkSetCommandBufferBoundaryCallbacks"));
		if (!state->enqueueQueueCallback || !state->emitCommandBufferCallback || !setBoundaryCallbacks) {
			logger::warn("[NVPerf] the DXVK build lacks the queue and command buffer callbacks; no capture");
			return;
		}
		bool semaphores = false;
		for (std::uint32_t i = 0; i < a_info.enabledExtensionCount; ++i)
			semaphores |= std::strcmp(a_info.enabledExtensions[i], "VK_KHR_external_semaphore_win32") == 0;
		if (!semaphores)
			logger::warn("[NVPerf] DXVK's device lacks VK_KHR_external_semaphore_win32, which the profiler needs");
		if (!nvperf::LoadLibraryFrom(std::filesystem::path(a_binDirectory) / L"nvperf_grfx_host.dll") || !nvperf::Available()) {
			logger::warn("[NVPerf] nvperf_grfx_host.dll is unavailable; no capture");
			return;
		}

		auto& target = state->target;
		target.backend = rhi::Backend::Vulkan;
		target.instance = a_info.instance;
		target.physicalDevice = a_info.physicalDevice;
		target.device = a_info.device;
		target.queue = a_info.graphicsQueue;
		target.getInstanceProcAddr = reinterpret_cast<void*>(a_info.getInstanceProcAddr);
		target.getDeviceProcAddr = reinterpret_cast<void*>(a_info.getInstanceProcAddr(a_info.instance, "vkGetDeviceProcAddr"));
		target.name = kQueueName;
		nvperf::SetQueueControl(nvperf::QueueControl::Boundaries);
		nvperf::LogStartupProbe(target);

		nvperf::CaptureConfiguration configuration{};
		for (const auto& range : ranges)
			configuration.passes.push_back({ range, kQueueName });
		for (const auto& entry : Split(Env("CS_NVPERF_METRICS"))) {
			nvperf::MetricRequest metric{};
			const auto colon = entry.find(':');
			metric.name = entry.substr(0, colon);
			const std::string kind = colon == std::string::npos ? "counter" : entry.substr(colon + 1);
			if (kind == "ratio") {
				metric.metricType = kRatio;
				metric.rollupOp = kRollupAvg;
				metric.submetric = kSubmetricRatio;
			} else if (kind == "active" || kind == "elapsed" || kind == "percycle") {
				// A counter averaged as a percentage of its sustained peak over the unit's active cycles (occupancy:
				// sm__warps_active, issue: smsp__issue_active) or over the elapsed ones (how much of the time a unit worked:
				// sm__cycles_active), or as the raw rate per active cycle.
				metric.metricType = kCounter;
				metric.rollupOp = kRollupAvg;
				metric.submetric = kind == "active" ? kSubmetricPctOfPeakActive : kind == "elapsed" ? kSubmetricPctOfPeak : kSubmetricPerCycleActive;
			} else if (kind == "throughput") {
				metric.metricType = kThroughput;
				metric.rollupOp = kRollupAvg;
				metric.submetric = kSubmetricPctOfPeak;
			} else {
				metric.metricType = kCounter;
				metric.rollupOp = kRollupSum;
				metric.submetric = kSubmetricNone;
			}
			metric.required = false;
			configuration.metrics.push_back(std::move(metric));
		}
		std::string error;
		if (!nvperf::ConfigureCapture(configuration, error)) {
			logger::warn("[NVPerf] {}", error);
			return;
		}
		std::filesystem::path csv = Env("CS_NVPERF_CSV");
		if (csv.empty()) {
			wchar_t* documents = nullptr;
			if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents)
				csv = std::filesystem::path(documents) / L"My Games" / L"Skyrim Special Edition" / L"SKSE" / L"CommunityShaders-nvperf.csv";
			::CoTaskMemFree(documents);
		}
		nvperf::SetCsvPath(csv);
		if (const auto start = Env("CS_NVPERF_START_FRAME"); !start.empty())
			state->startFrame = std::strtoull(start.c_str(), nullptr, 10);
		if (const auto samples = Env("CS_NVPERF_SAMPLES"); !samples.empty())
			state->samples = static_cast<std::uint32_t>(std::strtoul(samples.c_str(), nullptr, 10));

		state->lead = a_host.FrameSlots() + 1;
		g_state = state.release();
		if (FAILED(setBoundaryCallbacks(globals::d3d::device, &CloseInDxvk, &ReopenInDxvk, nullptr)))
			logger::warn("[NVPerf] DXVK refused the command buffer boundary callbacks: engine ranges across its flushes will be unbalanced");
		// The graph's passes: their ranges recorded in their own command lists (under QueueControl::Boundaries a range only
		// records; the frame boundaries make every queue call).
		a_host.SetGpuPassRangeCallbacks(
			[](rhi::CommandList a_commandList, rhi::Queue, const char* a_queueName, const char* a_passName) {
				nvperf::PushRange(g_state->target, rhi::vulkan::get_cmd_list(a_commandList), a_queueName, a_passName);
			},
			[](rhi::CommandList a_commandList, rhi::Queue) {
				nvperf::PopRange(rhi::Backend::Vulkan, rhi::vulkan::get_cmd_list(a_commandList));
			});
		logger::info("[NVPerf] capturing {} range filters from frame {}, {} samples, rows to {}", ranges.size(), g_state->startFrame, g_state->samples, csv.string());
	}

	bool Active()
	{
		return g_state != nullptr;
	}

	bool PushPassRange(void* a_vkCommandBuffer, const char* a_name)
	{
		return g_state && nvperf::PushRange(g_state->target, a_vkCommandBuffer, kQueueName, a_name);
	}

	void PopPassRange(void* a_vkCommandBuffer, bool a_pushed)
	{
		if (a_pushed)
			nvperf::PopRange(rhi::Backend::Vulkan, a_vkCommandBuffer);
	}

	bool Requested()
	{
		static const bool requested = !Split(Env("CS_NVPERF_RANGES")).empty();
		return requested;
	}

	void BeginEvent(std::string_view a_name)
	{
		if (!g_state)
			return;
		const std::string name(a_name);
		bool pushed = false;
		if (g_state->armed && nvperf::RangeSelected(kQueueName, name.c_str())) {
			const char* interned = g_state->names.insert(name).first->c_str();
			pushed = SUCCEEDED(g_state->emitCommandBufferCallback(globals::d3d::device, &PushInDxvk, const_cast<char*>(interned)));
		}
		g_state->events.push_back(pushed);
	}

	void EndEvent()
	{
		if (!g_state || g_state->events.empty())
			return;
		const bool pushed = g_state->events.back();
		g_state->events.pop_back();
		if (pushed)
			g_state->emitCommandBufferCallback(globals::d3d::device, &PopInDxvk, nullptr);
	}

	void OnPresent()
	{
		if (!g_state)
			return;
		auto& state = *g_state;
		++state.frame;
		if (state.armed && nvperf::CaptureComplete()) {
			if (auto result = nvperf::TakeCaptureResult())
				LogResult(*result);
			state.armed = false;
			if (++state.sampled < state.samples)
				state.startFrame = state.frame + 1;
		}
		if (!state.armed && state.sampled < state.samples && state.frame >= state.startFrame) {
			std::string error;
			state.armed = nvperf::ArmCapture(state.sampled, error);
			state.firstPassFrame = state.frame + state.lead;
			state.misplacedPops.store(0, std::memory_order_relaxed);
			if (!state.armed) {
				logger::warn("[NVPerf] {}", error);
				state.sampled = state.samples;
			}
		}
		// While a capture is armed, each Present ends the frame's profiler pass and begins the next, in the queue's order.
		if (state.armed && state.frame >= state.firstPassFrame)
			state.enqueueQueueCallback(globals::d3d::device, &FrameBoundary, reinterpret_cast<void*>(static_cast<std::uintptr_t>(state.frame)));
	}
}

#else

#	include "NvPerfBridge.h"

namespace NvPerfBridge
{
	void Initialize(const DxvkOrgInteropDeviceInfo&, void*, const wchar_t*, org::PersistentGraphHost&) {}
	bool Active() { return false; }
	bool Requested() { return false; }
	void BeginEvent(std::string_view) {}
	void EndEvent() {}
	void OnPresent() {}
}

#endif
