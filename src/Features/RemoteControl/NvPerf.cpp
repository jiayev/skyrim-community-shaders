#include "Features/RemoteControl/NvPerf.h"

#include "Features/RemoteControl/GpuTimestamps.h"
#include "Features/RemoteControl/NvPerfBackend.h"
#include "Features/RemoteControl/NvPerfTrace.h"

#include "DxvkLoader.h"
#include "Features/RemoteControl/NvPerfHost.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#if defined(CS_HAS_RENDER_GRAPH)
#	include <Render/Runtime/IStatisticsService.h>
#endif

// NvPerf's front end: requests, status, results, the CS_NVPERF_* switches, and the choice of backend
// (NvPerfBackend.h). Every profiler call is the backend's.
namespace NvPerf
{
	using namespace detail;

	namespace
	{
		constexpr std::array<std::string_view, 4> kRollupNames{ "avg", "max", "min", "sum" };
		constexpr std::array<std::string_view, 22> kSubmetricNames{ "", "peak_sustained", "peak_sustained_active", "peak_sustained_active.per_second",
			"peak_sustained_elapsed", "peak_sustained_elapsed.per_second", "peak_sustained_frame", "peak_sustained_frame.per_second", "peak_sustained_region",
			"peak_sustained_region.per_second", "per_cycle_active", "per_cycle_elapsed", "per_cycle_in_frame", "per_cycle_in_region", "per_second",
			"pct_of_peak_sustained_active", "pct_of_peak_sustained_elapsed", "pct_of_peak_sustained_frame", "pct_of_peak_sustained_region", "max_rate", "pct",
			"ratio" };

		struct MetricKind
		{
			std::string_view name;
			std::uint8_t metricType;
			std::uint8_t rollupOp;
			std::uint16_t submetric;
		};

		// A counter is summed; "active"/"elapsed" average it as a percentage of its sustained peak over the unit's active
		// cycles (occupancy: sm__warps_active) or the elapsed ones (how much of the time a unit worked: sm__cycles_active),
		// and "percycle" as the raw rate per active cycle.
		constexpr std::array<MetricKind, 6> kMetricKinds{ {
			{ "counter", kCounter, kRollupSum, kSubmetricNone },
			{ "ratio", kRatio, kRollupAvg, kSubmetricRatio },
			{ "throughput", kThroughput, kRollupAvg, kSubmetricPctOfPeakElapsed },
			{ "active", kCounter, kRollupAvg, kSubmetricPctOfPeakActive },
			{ "elapsed", kCounter, kRollupAvg, kSubmetricPctOfPeakElapsed },
			{ "percycle", kCounter, kRollupAvg, kSubmetricPerCycleActive },
		} };
	}

	std::vector<MetricRequest> DefaultMetrics()
	{
		const auto metric = [](const char* a_name, std::string_view a_kind) {
			MetricRequest request;
			const std::string text = std::format("{}:{}", a_name, a_kind);
			std::string error;
			ParseMetric(text, request, error);
			return request;
		};
		return {
			metric("gpu__time_duration", "counter"),
			metric("sm__throughput", "throughput"),
			metric("l1tex__throughput", "throughput"),
			metric("lts__throughput", "throughput"),
			metric("dram__throughput", "throughput"),
			metric("smsp__inst_executed", "counter"),
			metric("sm__warps_active", "active"),
			metric("smsp__warp_issue_stalled_long_scoreboard_per_warp_active", "ratio"),
			metric("smsp__warp_issue_stalled_short_scoreboard_per_warp_active", "ratio"),
			metric("smsp__warp_issue_stalled_tex_throttle_per_warp_active", "ratio"),
			metric("smsp__warp_issue_stalled_math_pipe_throttle_per_warp_active", "ratio"),
			metric("smsp__warp_issue_stalled_mio_throttle_per_warp_active", "ratio"),
			metric("smsp__warp_issue_stalled_lg_throttle_per_warp_active", "ratio"),
			metric("smsp__warp_issue_stalled_wait_per_warp_active", "ratio"),
			metric("smsp__warp_issue_stalled_no_instruction_per_warp_active", "ratio"),
		};
	}

	bool ParseMetric(std::string_view a_text, MetricRequest& a_metric, std::string& a_error)
	{
		const auto trim = [](std::string_view text) {
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
				text.remove_prefix(1);
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
				text.remove_suffix(1);
			return text;
		};
		const auto colon = a_text.find(':');
		const std::string_view name = trim(a_text.substr(0, colon));
		const std::string_view kind = colon == std::string_view::npos ? std::string_view("counter") : trim(a_text.substr(colon + 1));
		if (name.empty()) {
			a_error = "metric name cannot be empty";
			return false;
		}
		const auto found = std::ranges::find_if(kMetricKinds, [&](const MetricKind& entry) { return entry.name == kind; });
		if (found == kMetricKinds.end()) {
			a_error = std::format("unknown metric kind '{}' (counter, ratio, throughput, active, elapsed, percycle)", kind);
			return false;
		}
		a_metric.name = std::string(name);
		a_metric.metricType = found->metricType;
		a_metric.rollupOp = found->rollupOp;
		a_metric.submetric = found->submetric;
		return true;
	}

	std::string FormatMetric(const MetricRequest& a_metric)
	{
		const auto found = std::ranges::find_if(kMetricKinds, [&](const MetricKind& entry) {
			return entry.metricType == a_metric.metricType && entry.rollupOp == a_metric.rollupOp && entry.submetric == a_metric.submetric;
		});
		return found == kMetricKinds.end() ? MetricLabel(a_metric) : std::format("{}:{}", a_metric.name, found->name);
	}

	std::string MetricLabel(const MetricRequest& a_metric)
	{
		std::string label = a_metric.name;
		if (a_metric.metricType != kRatio && a_metric.rollupOp < kRollupNames.size())
			label += std::format(".{}", kRollupNames[a_metric.rollupOp]);
		if (a_metric.submetric != kSubmetricNone && a_metric.submetric < kSubmetricNames.size())
			label += std::format(".{}", kSubmetricNames[a_metric.submetric]);
		return label;
	}

	namespace
	{
		std::atomic<bool> g_rangesActive{ false };
	}

	namespace detail
	{
		std::string ToUtf8(const std::filesystem::path& a_path)
		{
			const auto text = a_path.u8string();
			return { reinterpret_cast<const char*>(text.data()), text.size() };
		}

		bool IsSummed(const MetricRequest& a_metric)
		{
			return a_metric.metricType == kCounter && a_metric.rollupOp == kRollupSum;
		}

		bool NameMatches(std::string_view a_filter, std::string_view a_name)
		{
			if (!a_filter.empty() && a_filter.back() == '*')
				return a_name.starts_with(a_filter.substr(0, a_filter.size() - 1));
			return a_filter == a_name;
		}

		const std::string* MatchingFilter(const std::vector<std::string>& a_filters, std::string_view a_name)
		{
			const auto found = std::ranges::find_if(a_filters, [&](const std::string& filter) { return NameMatches(filter, a_name); });
			return found == a_filters.end() ? nullptr : &*found;
		}

		bool PerfEventSelected(const std::vector<std::string>& a_filters, std::string_view a_name)
		{
			constexpr std::string_view kDrawMarkerPrefix = "Draw: CS ";
			return a_filters.empty() ? !a_name.starts_with(kDrawMarkerPrefix) : MatchingFilter(a_filters, a_name) != nullptr;
		}

		void SetRangesActive(bool a_active) noexcept
		{
			g_rangesActive.store(a_active, std::memory_order_relaxed);
		}

		bool LoadHostLibrary(DeviceInfo& a_device)
		{
			a_device.libraryPath = DxvkLoader::GetRuntimeDir() / L"nvperf_grfx_host.dll";
			if (::GetModuleHandleW(L"nvperf_grfx_host.dll") || ::LoadLibraryW(a_device.libraryPath.c_str()))
				return true;
			a_device.error = std::format("could not load {}", ToUtf8(a_device.libraryPath));
			logger::info("[NvPerf] {}", a_device.error);
			return false;
		}
	}

	namespace
	{
		std::string CsvEscape(std::string_view a_text)
		{
			if (a_text.find_first_of("\",\r\n") == std::string_view::npos)
				return std::string(a_text);
			std::string escaped = "\"";
			for (char ch : a_text) {
				if (ch == '"')
					escaped.push_back('"');
				escaped.push_back(ch);
			}
			escaped.push_back('"');
			return escaped;
		}

		// Per filter, per frame. Every sample measures the same frame shape, so occurrences and summed counters are
		// divided by the sample count.
		std::vector<RangeSummary> Summarize(const CaptureResult& a_result)
		{
			const auto& metrics = a_result.metrics;
			std::size_t timeMetric = metrics.size();
			for (std::size_t m = 0; m < metrics.size(); ++m)
				if (metrics[m].name == "gpu__time_duration")
					timeMetric = m;
			struct Totals
			{
				std::uint32_t occurrences = 0;
				std::uint32_t incomplete = 0;
				std::vector<double> values, weights;
			};
			std::map<std::string, Totals> byName;
			std::vector<std::string> order;
			const auto keyOf = [&](const std::string& a_name) -> const std::string& {
				const std::string* filter = MatchingFilter(a_result.ranges, a_name);
				return filter ? *filter : a_name;
			};
			for (const auto& range : a_result.rangeResults) {
				const std::string& key = keyOf(range.name);
				// With nesting levels, a selected range inside one with the same key is part of that range's counters.
				if (std::ranges::any_of(range.enclosing, [&](const std::string& a_parent) { return keyOf(a_parent) == key; }))
					continue;
				auto [entry, inserted] = byName.try_emplace(key);
				if (inserted)
					order.push_back(key);
				auto& totals = entry->second;
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
					const double weight = IsSummed(metrics[m]) || std::isnan(time) ? 1.0 : time;
					totals.values[m] += value * weight;
					totals.weights[m] += weight;
				}
				totals.incomplete += incomplete;
				++totals.occurrences;
			}
			const double samples = std::max<double>(a_result.samples, 1.0);
			std::vector<RangeSummary> summary;
			summary.reserve(order.size());
			for (const auto& name : order) {
				const auto& totals = byName[name];
				RangeSummary entry;
				entry.name = name;
				entry.occurrencesPerFrame = totals.occurrences / samples;
				entry.incomplete = totals.incomplete;
				for (std::size_t m = 0; m < metrics.size(); ++m) {
					if (!totals.weights[m])
						entry.values.push_back(std::nan(""));
					else if (IsSummed(metrics[m]))
						entry.values.push_back(totals.values[m] / samples);
					else
						entry.values.push_back(totals.values[m] / totals.weights[m]);
				}
				summary.push_back(std::move(entry));
			}
			return summary;
		}

		void LogResult(const CaptureResult& a_result)
		{
			if (!a_result.success && a_result.rangeResults.empty()) {
				logger::warn("[NvPerf] capture {} failed: {}", a_result.id, a_result.error);
				return;
			}
			logger::info("[NvPerf] capture {} (frames {}-{}, {} samples x {} passes, chip {}): {} ranges{}", a_result.id, a_result.startFrame,
				a_result.endFrame, a_result.samples, a_result.passesPerSample, a_result.chipName, a_result.rangeResults.size(),
				a_result.success ? "" : std::format(" ({})", a_result.error));
			if (!a_result.otherQueueRanges.empty()) {
				std::string names;
				for (const auto& name : a_result.otherQueueRanges)
					names += std::format("{}{}", names.empty() ? "" : ", ", name);
				logger::warn("[NvPerf]   {} selected passes ran on another queue and were not measured: {}", a_result.otherQueueRanges.size(), names);
			}
			for (const auto& entry : a_result.summary) {
				std::string text;
				for (std::size_t m = 0; m < a_result.metrics.size() && m < entry.values.size(); ++m)
					text += std::format("{}{}={:.4g}", text.empty() ? "" : ", ", MetricLabel(a_result.metrics[m]), entry.values[m]);
				logger::info("[NvPerf]   {} (x{:.3g}/frame{}): {}", entry.name, entry.occurrencesPerFrame,
					entry.incomplete ? std::format(", {} incomplete", entry.incomplete) : "", text);
			}
		}

		void WriteCsv(const CaptureResult& a_result)
		{
			if (a_result.csvPath.empty() || a_result.rangeResults.empty())
				return;
			std::error_code ec;
			std::filesystem::create_directories(a_result.csvPath.parent_path(), ec);
			const bool header = !std::filesystem::exists(a_result.csvPath, ec) || std::filesystem::file_size(a_result.csvPath, ec) == 0;
			std::ofstream csv(a_result.csvPath, std::ios::app);
			if (!csv) {
				logger::warn("[NvPerf] could not write {}", ToUtf8(a_result.csvPath));
				return;
			}
			// Metric columns change between captures, so each capture repeats its header when it differs from a new file's.
			if (!header)
				csv << '\n';
			csv << "capture,sample,start_frame,range,path,occurrence";
			for (const auto& metric : a_result.metrics)
				csv << ',' << CsvEscape(MetricLabel(metric));
			csv << '\n';
			for (const auto& range : a_result.rangeResults) {
				csv << a_result.id << ',' << range.sample << ',' << a_result.startFrame << ',' << CsvEscape(range.name) << ',' << CsvEscape(range.path) << ','
					<< range.occurrence;
				for (double value : range.values)
					csv << ',' << value;
				csv << '\n';
			}
		}

#ifdef NVPERF_ENABLED
		constexpr bool kBuilt = true;
#else
		constexpr bool kBuilt = false;
#endif
		std::string NotBuilt()
		{
			return std::format("built without Nsight Perf ({})", Host::BuildHint());
		}

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

		struct Profiler
		{
			std::mutex mutex;  // everything below up to `capture`
			DeviceInfo device;
			Backend* vulkan = nullptr;  // registered once the render graph adopted DXVK's device
			std::uint64_t nextCaptureId = 1;
			std::optional<std::pair<std::uint64_t, CaptureConfiguration>> pending;
			bool cancelRequested = false;
			Status status;
			std::shared_ptr<const CaptureResult> result;
			std::function<void(const CaptureResult&)> onComplete;

			// Render thread only.
			std::optional<Capture> capture;
			std::uint64_t presents = 0;
			bool environmentHandled = false;  // the CS_NVPERF_RANGES capture was queued (or could not be)
		};

		Profiler& Get()
		{
			static Profiler profiler;
			return profiler;
		}

		// The chosen backend. Never destroyed: the Vulkan backend's callbacks stay installed in DXVK and the graph host.
		std::atomic<Backend*> g_backend{ nullptr };

		// Chooses the backend, once: Vulkan when the render graph registered DXVK's device; never D3D11 under DXVK, whose
		// D3D11 runtime the D3D11 profiler's driver cannot see (until the registration, nothing is chosen). Caller holds the
		// mutex.
		Backend* SelectLocked(Profiler& a_profiler)
		{
			if (auto* backend = g_backend.load(std::memory_order_acquire))
				return backend;
			Backend* backend = a_profiler.vulkan;
			if (!backend && DxvkLoader::IsLoaded()) {
				a_profiler.device.error = "DXVK is in use and the render graph has not adopted its Vulkan device (the D3D11 profiler cannot run under DXVK)";
				return nullptr;
			}
			if (!backend)
				backend = MakeD3D11Backend().release();
			if (!backend)
				return nullptr;
			a_profiler.device.backend = backend->Name();
			g_backend.store(backend, std::memory_order_release);
			return backend;
		}

		// Chooses the backend, loads and initializes the library and identifies the device. Caller holds the mutex.
		bool ProbeLocked(Profiler& a_profiler)
		{
			auto& device = a_profiler.device;
			if (device.probed)
				return device.gpuSupported;
			if (!kBuilt) {
				device.error = NotBuilt();
				return false;
			}
			auto* backend = SelectLocked(a_profiler);
			return backend && backend->Probe(device);
		}

		bool Validate(const CaptureConfiguration& a_configuration, bool a_limitDelay, std::string& a_error)
		{
			if (a_configuration.samples < 1 || a_configuration.samples > 64) {
				a_error = "samples must be between 1 and 64";
				return false;
			}
			if (a_configuration.nestingLevels < 1 || a_configuration.nestingLevels > 8) {
				a_error = "nestingLevels must be between 1 and 8";
				return false;
			}
			if (a_limitDelay && a_configuration.delayFrames > 10000) {
				a_error = "delayFrames must be at most 10000";
				return false;
			}
			for (const auto& metric : a_configuration.metrics) {
				if (metric.name.empty()) {
					a_error = "metric name cannot be empty";
					return false;
				}
			}
			if (const auto* backend = g_backend.load(std::memory_order_acquire))
				a_error = backend->Validate(a_configuration);
			return a_error.empty();
		}

		bool Queue(Profiler& a_profiler, const CaptureConfiguration& a_configuration, std::uint64_t& a_captureId, std::string& a_error)
		{
			std::lock_guard lock(a_profiler.mutex);
			const bool busy = a_profiler.pending || a_profiler.status.state == CaptureState::Waiting || a_profiler.status.state == CaptureState::Capturing ||
			                  a_profiler.status.state == CaptureState::Decoding;
			if (busy) {
				a_error = std::format("capture {} is still in progress", a_profiler.pending ? a_profiler.pending->first : a_profiler.status.captureId);
				return false;
			}
			a_captureId = a_profiler.nextCaptureId++;
			a_profiler.pending.emplace(a_captureId, a_configuration);
			a_profiler.cancelRequested = false;
			a_profiler.status = Status{};
			a_profiler.status.state = CaptureState::Waiting;
			a_profiler.status.captureId = a_captureId;
			a_profiler.status.samples = a_configuration.samples;
			return true;
		}

		void SetStatus(Profiler& a_profiler, const Capture& a_capture, CaptureState a_state)
		{
			std::lock_guard lock(a_profiler.mutex);
			a_profiler.status.state = a_state;
			a_profiler.status.captureId = a_capture.id;
			a_profiler.status.sample = a_capture.sample;
			a_profiler.status.samples = a_capture.configuration.samples;
			a_profiler.status.pass = a_capture.passesSubmitted;
			a_profiler.status.passes = a_capture.passesPerSample;
			a_profiler.status.error.clear();
		}

		// Ends the capture: the backend closes what it opened, then its result is published and the annotations it forced
		// on are restored.
		void Finish(Profiler& a_profiler, std::string a_error)
		{
			auto& capture = *a_profiler.capture;
			if (auto* backend = g_backend.load(std::memory_order_acquire)) {
				backend->End(capture);
				for (auto& name : backend->TakeOtherQueueRanges())
					if (PerfEventSelected(capture.configuration.ranges, name))
						capture.result.otherQueueRanges.push_back(std::move(name));
			}
			Host::SetFrameAnnotations(capture.previousFrameAnnotations);

			auto& result = capture.result;
			result.endFrame = Host::FrameCount();
			result.samples = capture.sample;
			result.passesPerSample = capture.passesPerSample;
			if (a_error.empty() && (result.droppedRanges || result.droppedTraceBytes))
				a_error = std::format("the profiler dropped {} ranges and {} trace bytes; select fewer ranges", result.droppedRanges, result.droppedTraceBytes);
			if (a_error.empty() && result.rangeResults.empty())
				a_error = "no selected range was measured";
			result.success = a_error.empty();
			result.error = std::move(a_error);
			result.summary = Summarize(result);
			LogResult(result);
			WriteCsv(result);

			auto published = std::make_shared<const CaptureResult>(std::move(result));
			std::function<void(const CaptureResult&)> onComplete;
			{
				std::lock_guard lock(a_profiler.mutex);
				a_profiler.status.state = published->success ? CaptureState::Done : CaptureState::Failed;
				a_profiler.status.error = published->error;
				a_profiler.result = published;
				// The Vulkan backend names the chip at its first capture.
				if (a_profiler.device.chipName.empty())
					a_profiler.device.chipName = published->chipName;
				onComplete = a_profiler.onComplete;
			}
			a_profiler.capture.reset();
			if (onComplete)
				onComplete(*published);
		}

		// A pending request becomes the capture in flight: the device, the backend's configuration, the annotations it needs.
		void Start(Profiler& a_profiler, std::uint64_t a_id, CaptureConfiguration a_configuration)
		{
			auto& capture = a_profiler.capture.emplace();
			capture.id = a_id;
			capture.configuration = std::move(a_configuration);
			if (capture.configuration.metrics.empty())
				capture.configuration.metrics = DefaultMetrics();
			capture.delayFrames = capture.configuration.delayFrames;
			capture.previousFrameAnnotations = Host::FrameAnnotations();
			// Perf events are the ranges, and most are only emitted with annotations on.
			Host::SetFrameAnnotations(true);

			auto& result = capture.result;
			result.id = a_id;
			result.ranges = capture.configuration.ranges;
			result.startFrame = Host::FrameCount();
			result.csvPath = CsvPath();

			std::string chipName;
			std::string error;
			Backend* backend = nullptr;
			{
				std::lock_guard lock(a_profiler.mutex);
				if (!ProbeLocked(a_profiler))
					error = a_profiler.device.error.empty() ? "Nsight Perf is unavailable" : a_profiler.device.error;
				chipName = a_profiler.device.chipName;
				backend = g_backend.load(std::memory_order_acquire);
			}
			result.chipName = chipName;
			if (error.empty() && !backend->Begin(capture, chipName, error) && error.empty())
				error = "could not configure the capture";
			if (!error.empty()) {
				Finish(a_profiler, std::move(error));
				return;
			}
			SetStatus(a_profiler, capture, CaptureState::Waiting);
		}

		// Once per frame after Present: the delay, then the backend's next step.
		void Advance(Profiler& a_profiler)
		{
			auto& capture = *a_profiler.capture;
			if (capture.delayFrames) {
				--capture.delayFrames;
				return;
			}
			auto step = g_backend.load(std::memory_order_acquire)->Advance(capture);
			if (step.finished)
				Finish(a_profiler, std::move(step.error));
			else
				SetStatus(a_profiler, capture, step.state);
		}

		// CS_NVPERF_RANGES: a headless run's capture, queued as a RequestCapture once the backend is ready. Its result is
		// logged per range filter like every capture's. CS_NVPERF_START_FRAME (default 1200) is the present it starts at,
		// CS_NVPERF_SAMPLES (default 3) its samples, CS_NVPERF_METRICS its metrics (ParseMetric's form, comma-separated;
		// default DefaultMetrics()), CS_NVPERF_CSV overrides CsvPath().
		void QueueEnvironmentCapture(Profiler& a_profiler)
		{
			// CS_NVPERF_RANGES alone: EnvironmentRequested also answers for CS_NVPERF_TEMPLATE, whose trace queues its own captures.
			static const bool requested = !Split(Env("CS_NVPERF_RANGES")).empty();
			if (a_profiler.environmentHandled || !requested)
				return;
			{
				std::lock_guard lock(a_profiler.mutex);
				if (!ProbeLocked(a_profiler)) {
					// Under DXVK the render graph may not have registered yet: try again next frame.
					if (a_profiler.device.probed || !kBuilt) {
						a_profiler.environmentHandled = true;
						logger::warn("[NvPerf] CS_NVPERF_RANGES: no capture: {}", a_profiler.device.error);
					}
					return;
				}
			}
			a_profiler.environmentHandled = true;
			CaptureConfiguration configuration;
			configuration.ranges = Split(Env("CS_NVPERF_RANGES"));
			for (const auto& entry : Split(Env("CS_NVPERF_METRICS"))) {
				MetricRequest metric;
				std::string error;
				if (ParseMetric(entry, metric, error))
					configuration.metrics.push_back(std::move(metric));
				else
					logger::warn("[NvPerf] CS_NVPERF_METRICS: '{}' skipped: {}", entry, error);
			}
			std::uint64_t startFrame = 1200;
			if (const auto start = Env("CS_NVPERF_START_FRAME"); !start.empty())
				startFrame = std::strtoull(start.c_str(), nullptr, 10);
			configuration.samples = 3;
			if (const auto samples = Env("CS_NVPERF_SAMPLES"); !samples.empty())
				configuration.samples = static_cast<std::uint32_t>(std::strtoul(samples.c_str(), nullptr, 10));
			const std::uint64_t delay = startFrame > a_profiler.presents ? startFrame - a_profiler.presents : 0;
			configuration.delayFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(delay, std::numeric_limits<std::uint32_t>::max()));
			std::uint64_t captureId = 0;
			std::string error;
			if (!Validate(configuration, false, error) || !Queue(a_profiler, configuration, captureId, error)) {
				logger::warn("[NvPerf] CS_NVPERF_RANGES: no capture: {}", error);
				return;
			}
			logger::info("[NvPerf] CS_NVPERF_RANGES: capture {} of {} range filters from frame {}, {} samples, rows to {}", captureId, configuration.ranges.size(),
				startFrame, configuration.samples, Util::PathHelpers::ToUtf8(CsvPath()));
		}
	}

	std::filesystem::path CsvPath()
	{
		if (const auto csv = Env("CS_NVPERF_CSV"); !csv.empty())
			return std::filesystem::path(csv);
		return Host::LogDirectory() / "CommunityShaders-NvPerf.csv";
	}

	bool Probe()
	{
		auto& profiler = Get();
		std::lock_guard lock(profiler.mutex);
		return ProbeLocked(profiler);
	}

	DeviceInfo GetDeviceInfo()
	{
		auto& profiler = Get();
		std::lock_guard lock(profiler.mutex);
		DeviceInfo info = profiler.device;
		info.built = kBuilt;
		if (!kBuilt)
			info.error = NotBuilt();
		return info;
	}

	void RegisterVulkanDevice(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, org::GraphHost& a_host)
	{
		auto backend = MakeVulkanBackend(a_info, a_dxvkModule, a_host);
		if (!backend)
			return;
		auto& profiler = Get();
		std::lock_guard lock(profiler.mutex);
		// Its callbacks are installed: it lives on whether or not it is chosen.
		Backend* registered = backend.release();
		if (const auto* chosen = g_backend.load(std::memory_order_acquire)) {
			logger::warn("[NvPerf] the {} backend was chosen before the render graph adopted DXVK's device; the Vulkan backend is not used", chosen->Name());
			return;
		}
		profiler.vulkan = registered;
	}

	bool EnvironmentRequested()
	{
		static const bool requested = !Split(Env("CS_NVPERF_RANGES")).empty() || Trace::EnvironmentRequested();
		return requested;
	}

	bool RequestCapture(const CaptureConfiguration& a_configuration, std::uint64_t& a_captureId, std::string& a_error)
	{
		if (!kBuilt) {
			a_error = NotBuilt();
			return false;
		}
		return Validate(a_configuration, true, a_error) && Queue(Get(), a_configuration, a_captureId, a_error);
	}

	void CancelCapture()
	{
		auto& profiler = Get();
		std::lock_guard lock(profiler.mutex);
		profiler.cancelRequested = true;
	}

	Status GetStatus()
	{
		auto& profiler = Get();
		std::lock_guard lock(profiler.mutex);
		return profiler.status;
	}

	std::shared_ptr<const CaptureResult> LatestResult()
	{
		auto& profiler = Get();
		std::lock_guard lock(profiler.mutex);
		return profiler.result;
	}

	void SetCompletionCallback(std::function<void(const CaptureResult&)> a_callback)
	{
		auto& profiler = Get();
		std::lock_guard lock(profiler.mutex);
		profiler.onComplete = std::move(a_callback);
	}

	void OnPresentBegin()
	{
		auto& profiler = Get();
		if (auto* backend = g_backend.load(std::memory_order_acquire))
			backend->PresentBegin(profiler.capture ? &*profiler.capture : nullptr);
	}

	void OnPresentEnd()
	{
		auto& profiler = Get();
		++profiler.presents;
		Timestamps::OnPresentEnd(profiler.presents);
		if (kBuilt)
			QueueEnvironmentCapture(profiler);
		// Timestamp traces need no SDK: the trace runner runs either way, and its captures fail without one.
		Trace::OnPresentEnd(profiler.presents);
		if (!kBuilt)
			return;
		std::optional<std::pair<std::uint64_t, CaptureConfiguration>> pending;
		bool cancel = false;
		{
			std::lock_guard lock(profiler.mutex);
			pending.swap(profiler.pending);
			cancel = std::exchange(profiler.cancelRequested, false);
		}
		if (pending && !profiler.capture)
			Start(profiler, pending->first, std::move(pending->second));
		if (!profiler.capture)
			return;
		if (cancel) {
			Finish(profiler, "cancelled");
			return;
		}
		Advance(profiler);
	}

	bool RangesActive() noexcept
	{
		return g_rangesActive.load(std::memory_order_relaxed);
	}

	void PushRange(std::string_view a_name)
	{
		auto& profiler = Get();
		if (!RangesActive() || !profiler.capture)
			return;
		g_backend.load(std::memory_order_relaxed)->PushRange(*profiler.capture, a_name);
	}

	void PopRange()
	{
		if (!RangesActive())
			return;
		g_backend.load(std::memory_order_relaxed)->PopRange();
	}

	namespace
	{
		// The command buffer ranges pushed on this thread (a pass records on one thread), innermost last: whether each pushed the
		// profiler's range and the recording's timestamp sub-range, so PopCommandBufferRange ends what its push began.
		struct PushedRange
		{
			bool profiler = false;
			bool timestamps = false;
		};
		thread_local std::vector<PushedRange> t_pushedRanges;

		bool TimestampSubRangesActive() noexcept
		{
#if defined(CS_HAS_RENDER_GRAPH)
			return org::runtime::RecordingSubRangesActive();
#else
			return false;
#endif
		}
	}

	bool CommandBufferRangesActive() noexcept
	{
		const auto* backend = g_backend.load(std::memory_order_relaxed);
		return (backend && backend->CommandBufferRangesActive()) || TimestampSubRangesActive();
	}

	bool PushCommandBufferRange(void* a_nativeCommandBuffer, const char* a_name)
	{
		PushedRange pushed;
		auto* backend = g_backend.load(std::memory_order_relaxed);
		pushed.profiler = backend && a_nativeCommandBuffer && backend->PushCommandBufferRange(a_nativeCommandBuffer, a_name);
#if defined(CS_HAS_RENDER_GRAPH)
		// A kept recording's own timestamps (a timestamp trace): the same range, nested in the pass's.
		pushed.timestamps = org::runtime::BeginRecordingSubRange(a_name);
#endif
		if (!pushed.profiler && !pushed.timestamps)
			return false;
		t_pushedRanges.push_back(pushed);
		return true;
	}

	void PopCommandBufferRange(void* a_nativeCommandBuffer, bool a_pushed)
	{
		if (!a_pushed || t_pushedRanges.empty())
			return;
		const auto pushed = t_pushedRanges.back();
		t_pushedRanges.pop_back();
		if (pushed.profiler)
			if (auto* backend = g_backend.load(std::memory_order_relaxed))
				backend->PopCommandBufferRange(a_nativeCommandBuffer, true);
#if defined(CS_HAS_RENDER_GRAPH)
		if (pushed.timestamps)
			org::runtime::EndRecordingSubRange();
#endif
	}
}
