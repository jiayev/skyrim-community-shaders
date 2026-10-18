#pragma once

#include "Features/RemoteControl/NvPerf.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief Internal to NvPerf: the capture in flight as the front end (NvPerf.cpp) tracks it, and the interface of the
 * profiler backends that take it (NvPerfD3D11.cpp, NvPerfVulkan.cpp).
 *
 * The front end owns everything both backends share: requests, status, the delay before the first pass, the result's
 * summary, log and CSV, the completion callback. A backend owns its profiler: configuring a capture, its passes, the
 * ranges, decoding samples into CaptureResult::rangeResults.
 */
namespace NvPerf::detail
{
	// NVPW's enums (nvperf_host.h), so the metric helpers build without the SDK.
	inline constexpr std::uint8_t kCounter = 0, kRatio = 1, kThroughput = 2;
	inline constexpr std::uint8_t kRollupAvg = 0, kRollupSum = 3;
	inline constexpr std::uint16_t kSubmetricNone = 0, kSubmetricPerCycleActive = 10, kSubmetricPctOfPeakActive = 15, kSubmetricPctOfPeakElapsed = 16,
								   kSubmetricRatio = 21;

	bool IsSummed(const MetricRequest& a_metric);
	bool NameMatches(std::string_view a_filter, std::string_view a_name);
	const std::string* MatchingFilter(const std::vector<std::string>& a_filters, std::string_view a_name);
	/// Whether a perf event of that name is measured by a capture with these filters. No filters: every range except
	/// State::Draw's per-shader-change markers (hundreds per frame of ranges without GPU work), measured only when named.
	bool PerfEventSelected(const std::vector<std::string>& a_filters, std::string_view a_name);

	/// Sets RangesActive(): whether perf events are forwarded to the backend.
	void SetRangesActive(bool a_active) noexcept;
	/// Loads nvperf_grfx_host.dll by path (Data/SKSE/Plugins/CommunityShaders/bin) before the first NVPW call, which
	/// would otherwise raise rather than fail on the delay-loaded import. Sets a_device.libraryPath, and its error on failure.
	bool LoadHostLibrary(DeviceInfo& a_device);

	/// The capture in flight, on the render thread.
	struct Capture
	{
		std::uint64_t id = 0;
		CaptureConfiguration configuration;  // metrics: DefaultMetrics() when the request named none
		std::uint32_t delayFrames = 0;       // frames left before the backend's first Advance
		std::uint32_t sample = 0;            // samples completed
		std::size_t passesSubmitted = 0;     // of the current sample
		std::size_t passesPerSample = 0;
		bool previousFrameAnnotations = false;
		CaptureResult result;
	};

	/// What a backend's Advance left the capture in: still running in a state, or finished (failed when error is set).
	struct Step
	{
		CaptureState state = CaptureState::Capturing;
		bool finished = false;
		std::string error;
	};

	class Backend
	{
	public:
		virtual ~Backend() = default;

		/// "D3D11" or "Vulkan".
		virtual const char* Name() const noexcept = 0;
		/// Loads the library and identifies the device into a_device; sets a_device.probed once the answer is final. The
		/// front end's mutex is held.
		virtual bool Probe(DeviceInfo& a_device) = 0;
		/// Why the backend cannot take this request (empty: it can).
		virtual std::string Validate(const CaptureConfiguration&) const { return {}; }

		/// Render thread: the capture becomes the one in flight. False with a_error: it finishes as failed.
		virtual bool Begin(Capture& a_capture, const std::string& a_chipName, std::string& a_error) = 0;
		/// Render thread, before every Present, with the capture in flight or null.
		virtual void PresentBegin(Capture* a_capture) = 0;
		/// Render thread, after Present, once the capture's delay elapsed: decode a finished sample, begin the next pass.
		virtual Step Advance(Capture& a_capture) = 0;
		/// Render thread: the capture ends (finished, failed or cancelled). Closes what is open and sets result.metrics.
		virtual void End(Capture& a_capture) = 0;

		/// Render thread, while RangesActive(): a perf event opens / closes.
		virtual void PushRange(Capture& a_capture, std::string_view a_name) = 0;
		virtual void PopRange() = 0;

		virtual bool CommandBufferRangesActive() const noexcept { return false; }
		virtual bool PushCommandBufferRange(void*, const char*) { return false; }
		virtual void PopCommandBufferRange(void*, bool) {}
	};

	/// The D3D11 backend (NVPERF_ENABLED), or null.
	std::unique_ptr<Backend> MakeD3D11Backend();
	/// The Vulkan backend on DXVK's device (NVPERF_ENABLED with the render graph), or null.
	std::unique_ptr<Backend> MakeVulkanBackend(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, org::PersistentGraphHost& a_host);
}
