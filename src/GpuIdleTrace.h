#pragma once

#include <cstdint>
#include <string_view>

/**
 * @brief Finds the GPU's idle gaps inside a frame and what the render thread was doing during each one.
 *
 * Diagnostics, off unless `CS_GPU_IDLE_TRACE=<frames>` is set (every that many frames, the most recent
 * complete frame is analysed and logged). It needs the DXVK fork's submission trace
 * (dxvkSetSubmissionTrace / dxvkReadSubmissionTrace), which timestamps every submission on DXVK's graphics
 * queue, and turns Frame Annotations on so the engine's render phases are marked.
 *
 * Sources:
 * - GPU busy spans: DXVK's command lists and enqueued interop submissions (the render graph's epochs) from
 *   the DXVK trace, and the Streamline interop ring's buffers (AddInteropSpan), which DXVK does not see.
 * - CPU: every perf event (State::BeginPerfEvent / EndPerfEvent / SetPerfMarker) on the render thread, with
 *   its QueryPerformanceCounter time.
 *
 * GPU timestamps are placed on the CPU clock by the smallest observed (GPU begin - queue submit) difference:
 * the GPU cannot start a submission before it is submitted, so that bound is tight whenever the queue was idle.
 */
namespace GpuIdleTrace
{
	/** @brief Whether the trace was requested (CS_GPU_IDLE_TRACE). Cheap; safe from any thread. */
	bool Requested();

	/** @brief Resolves the DXVK exports and turns the DXVK trace on. Call once the D3D11 device exists. */
	void Initialize();

	/** @brief Render-thread perf event hooks (State). */
	void BeginEvent(std::string_view a_name);
	void EndEvent();
	void Mark(std::string_view a_name);

	/** @brief GPU span of a submission DXVK does not see (the Streamline interop ring). Any thread. */
	void AddInteropSpan(const char* a_label, std::uint64_t a_gpuBegin, std::uint64_t a_gpuEnd, std::int64_t a_submitQpc);

	/** @brief Called once per frame at Present on the render thread: collects records, analyses and logs. */
	void OnPresent();

	/** @brief Logs the idle summary of the analysed frames since the last one (the periodic summary covers a killed run). */
	void Shutdown();
}
