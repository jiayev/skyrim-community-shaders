#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct DxvkOrgInteropDeviceInfo;

namespace org
{
	class PersistentGraphHost;
}

/**
 * @brief GPU hardware counters per named range through NVIDIA's Nsight Perf SDK (its range profiler).
 *
 * A capture measures a set of metrics over the ranges State::BeginPerfEvent / EndPerfEvent open (and, on the Vulkan
 * backend, the render graph's passes and the ranges graph-recorded work pushes with PushCommandBufferRange). Each frame
 * is one profiler pass, bracketed around the swap chain's Present (OnPresentBegin / OnPresentEnd); the metrics need as
 * many passes as the chip's counters allow per pass, so a sample takes several frames. A capture collects one or more
 * samples, then publishes a CaptureResult (and appends it to a CSV next to the log).
 *
 * One front end, two backends, chosen once by the device in use:
 * - D3D11: NVPW's D3D11 range profiler on the game's immediate context (the system D3D11 runtime).
 * - Vulkan: OpenRenderGraph's Telemetry/NvPerfCapture on DXVK's graphics queue, once the render graph adopted DXVK's
 *   Vulkan device (RegisterVulkanDevice). Under DXVK the D3D11 profiler cannot work, so it is never chosen there.
 *
 * Requests come from any thread (the Remote Control panel, the devbench tool) and, for headless runs, from the
 * CS_NVPERF_* switches; every profiler call runs on the render thread (Vulkan: also DXVK's threads). Only built with
 * NVPERF_ENABLED (CMake NVPERF_SUPPORT); otherwise every call reports that it is unavailable. The SDK's
 * nvperf_grfx_host.dll is delay-loaded from Data/SKSE/Plugins/CommunityShaders/bin, so a missing file disables
 * profiling without failing the plugin load.
 */
namespace NvPerf
{
	/// One metric as the metrics evaluator names it (e.g. "sm__throughput"), with the evaluator's enums.
	struct MetricRequest
	{
		std::string name;
		std::uint8_t metricType = 0;  // NVPW_MetricType
		std::uint8_t rollupOp = 0;    // NVPW_RollupOp
		std::uint16_t submetric = 0;  // NVPW_Submetric
	};

	struct CaptureConfiguration
	{
		std::vector<MetricRequest> metrics;  // empty: DefaultMetrics()
		// Range names to measure; a trailing '*' matches every name with that prefix. Empty: every range.
		std::vector<std::string> ranges;
		std::uint32_t samples = 1;        // frames' worth of counters to collect, one after another
		std::uint32_t delayFrames = 0;    // frames to wait before the first pass
		// Selected ranges nested this deep are measured; each level adds passes. D3D11 only: the Vulkan backend measures
		// one level (OpenRenderGraph's profiler configures one), and refuses more.
		std::uint16_t nestingLevels = 1;
	};

	/// One measured range of one sample; values follow CaptureResult::metrics.
	struct RangeResult
	{
		std::uint32_t sample = 0;
		std::string name;                    // the range's own name
		// Its enclosing selected ranges and itself, joined with " / ", and the enclosing ones, outermost first. The Vulkan
		// backend measures one nesting level and reports a range by its own name: path is the name, enclosing is empty.
		std::string path;
		std::vector<std::string> enclosing;
		std::uint32_t occurrence = 0;
		std::vector<double> values;
	};

	/// The ranges matching one filter (or one name, with no filters), per frame: counters summed, rates weighted by each
	/// range's gpu__time_duration. A range nested in one that matches the same filter is already part of it and is left
	/// out. A value a replay pass could not complete (NaN) is left out and counted.
	struct RangeSummary
	{
		std::string name;
		double occurrencesPerFrame = 0.0;
		std::uint32_t incomplete = 0;
		std::vector<double> values;
	};

	struct CaptureResult
	{
		std::uint64_t id = 0;
		bool success = false;
		std::string error;
		std::string chipName;
		std::uint64_t startFrame = 0;
		std::uint64_t endFrame = 0;
		std::uint32_t samples = 0;
		std::size_t passesPerSample = 0;
		std::uint64_t droppedRanges = 0;
		std::uint64_t droppedTraceBytes = 0;
		std::vector<std::string> ranges;  // the requested filters
		std::vector<MetricRequest> metrics;
		std::vector<MetricRequest> unsupportedMetrics;
		std::vector<RangeResult> rangeResults;
		std::vector<RangeSummary> summary;
		// Selected graph passes that ran on a queue the profiler does not measure (Vulkan: compute, copy): never in rangeResults.
		std::vector<std::string> otherQueueRanges;
		std::filesystem::path csvPath;
	};

	enum class CaptureState
	{
		Idle,
		Waiting,    // requested; waiting for its first pass
		Capturing,  // passes are being submitted
		Decoding,   // every pass of a sample is submitted; waiting for the GPU
		Done,
		Failed,
	};

	struct Status
	{
		CaptureState state = CaptureState::Idle;
		std::uint64_t captureId = 0;
		std::uint32_t sample = 0;
		std::uint32_t samples = 0;
		std::size_t pass = 0;
		std::size_t passes = 0;
		std::string error;
	};

	struct DeviceInfo
	{
		bool built = false;      // compiled with the SDK
		bool probed = false;     // Probe() has run
		bool available = false;  // library loaded and initialized
		bool gpuSupported = false;
		// "D3D11" or "Vulkan" once a backend is chosen. The Vulkan backend learns the chip at its first capture: until
		// then chipName is empty and gpuSupported means the library initialized (the capture fails if the chip lacks it).
		std::string backend;
		std::string deviceName;
		std::string chipName;
		std::string error;
		std::filesystem::path libraryPath;
	};

	/// Chooses the backend, loads the library and identifies the GPU, once. Thread-safe; needs the D3D11 device, and
	/// under DXVK the render graph's registration (until then it reports unavailable and retries on the next call).
	/// @return Whether the GPU supports the range profiler.
	bool Probe();
	DeviceInfo GetDeviceInfo();

	/// Time, throughput of the main units, the instruction mix and the warp stall reasons.
	std::vector<MetricRequest> DefaultMetrics();
	/// Parses "name[:counter|ratio|throughput|active|elapsed|percycle]" (default counter).
	bool ParseMetric(std::string_view text, MetricRequest& metric, std::string& error);
	/// The inverse of ParseMetric.
	std::string FormatMetric(const MetricRequest& metric);
	/// The name a metric's values are reported under (e.g. "sm__throughput.avg.pct_of_peak_sustained_elapsed").
	std::string MetricLabel(const MetricRequest& metric);

	/**
	 * @brief Once the render graph adopted DXVK's Vulkan device, before the graph's first frame: the Vulkan backend's
	 * inputs (DXVK's device and queue, its module for the queue and command buffer callbacks, the graph host for its pass
	 * ranges and frame slots). Installs the callbacks, which only test a flag until a capture arms; the library loads at
	 * Probe.
	 */
	void RegisterVulkanDevice(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, org::PersistentGraphHost& a_host);
	/// Whether CS_NVPERF_RANGES (a capture) or CS_NVPERF_TEMPLATE (a trace, NvPerfTrace.h) requests a headless run: read at
	/// startup, so the engine's perf events are turned on before it (most engine passes are only hooked with Frame
	/// Annotations on at startup).
	bool EnvironmentRequested();

	/// Queues a capture; it starts at the next Present. Fails while another is pending or running.
	bool RequestCapture(const CaptureConfiguration& configuration, std::uint64_t& captureId, std::string& error);
	/// Stops the pending or running capture; it completes as failed.
	void CancelCapture();
	Status GetStatus();
	std::shared_ptr<const CaptureResult> LatestResult();
	/// Called on the render thread whenever a capture completes or fails.
	void SetCompletionCallback(std::function<void(const CaptureResult&)> callback);
	std::filesystem::path CsvPath();

	/// Render thread: before the frame's Present (ends the frame's pass, so the overlay is not measured).
	void OnPresentBegin();
	/// Render thread: after the frame's Present (decodes, then begins the next frame's pass).
	void OnPresentEnd();

	/// Whether a pass is open, so ranges are recorded. Cheap: called on every perf event.
	bool RangesActive() noexcept;
	/// A perf event's range, measured when the capture selects its name (D3D11: on the immediate context; Vulkan: in
	/// DXVK's command stream). Pair with PopRange.
	void PushRange(std::string_view name);
	void PopRange();

	/// Whether graph-recorded work should push its own ranges (a capture is armed on the Vulkan backend, or the pass being
	/// recorded on this thread is timed by a timestamp trace). Cheap.
	bool CommandBufferRangesActive() noexcept;
	/**
	 * @brief A range inside a graph pass's own recording (e.g. the parts of DCLF's colour pass), in its native command
	 * buffer (VkCommandBuffer), when a capture is armed and selects a_name. The profiler measures one nesting level, so
	 * select these names without the enclosing pass's. While a timestamp trace times the pass's recording, it is also a
	 * timestamp sub-range (Timestamps::Range::nested). No-op (false) on the D3D11 backend without a trace.
	 * @return Whether it was pushed: pass it to PopCommandBufferRange in the same command buffer.
	 */
	bool PushCommandBufferRange(void* a_nativeCommandBuffer, const char* a_name);
	void PopCommandBufferRange(void* a_nativeCommandBuffer, bool a_pushed);
}
