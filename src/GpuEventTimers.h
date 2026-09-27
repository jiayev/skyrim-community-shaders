#pragma once

#include <string_view>

/**
 * @brief GPU time of every perf event on the render thread, by its path in the event tree.
 *
 * Diagnostics, off unless `CS_GPU_EVENT_TIMERS=<frames>` is set: every that many collected frames it logs, per event
 * path, the GPU time inclusive of the events inside it and its own (self) time, averaged per frame, and the frame's
 * GPU time from one Present to the next. The perf events are the engine's render phases (Frame Annotations, which
 * this turns on without its per-geometry events), the profiler's passes and every ScopedPerfEvent, so the engine's
 * passes are accounted for beside Community Shaders' own. What no event covers shows as the frame's unannotated time.
 *
 * Each event is a pair of D3D11 timestamp queries. DXVK writes them in its command stream, and the render graph's
 * epochs are enqueued on the same queue in order, so an event around an epoch includes the epoch's GPU time. A GPU
 * that sits idle inside an event counts too: this measures where the frame's GPU time goes, busy or not.
 */
namespace GpuEventTimers
{
	/** @brief Whether the timers were requested (CS_GPU_EVENT_TIMERS). Cheap; safe from any thread. */
	bool Requested();

	/** @brief Render-thread perf event hooks (State). */
	void BeginEvent(std::string_view a_name);
	void EndEvent();

	/** @brief Once per frame at Present on the render thread: closes the frame, collects finished frames, logs. */
	void OnPresent();
}
