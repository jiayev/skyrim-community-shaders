#pragma once

#include <string_view>

struct DxvkOrgInteropDeviceInfo;

namespace org
{
	class PersistentGraphHost;
}

/**
 * @brief Nsight Perf hardware counters per range (OpenRenderGraph's Telemetry/NvPerfCapture) on DXVK's graphics queue.
 *
 * Off unless `CS_NVPERF_RANGES` names the ranges to measure (comma-separated; a trailing '*' matches a prefix): render
 * graph passes by their pass names (e.g. cs.dclf.main-opaque) and the engine's own work by its perf event names (e.g.
 * BSShaderAccumulator::RenderBatches*). The two kinds sit side by side on the same queue, so they are measured alike.
 *
 * - Frame boundaries are DXVK queue callbacks enqueued at Present: each frame is one profiler pass, and the profiler's
 *   queue calls run on DXVK's submission thread with the queue locked, between its submissions.
 * - The graph's passes get their ranges through the graph host's per-pass hooks; the engine's selected perf events
 *   through DXVK command buffer callbacks, in its command stream. A range must open and close in one command buffer: at
 *   each DXVK flush the open engine ranges close and reopen in the next, so an event that spans one (e.g. around a graph
 *   epoch) is measured in pieces, summed in the log.
 * - Ranges record from the capture's arming; its first pass begins the host's frame slots later, once every command
 *   buffer that can execute in it (the graph's epochs are recorded ahead) was recorded while armed.
 *
 * Other switches: CS_NVPERF_METRICS (name[:counter|ratio|throughput], comma-separated; default: time, unit throughput,
 * warp stalls), CS_NVPERF_START_FRAME (default 1200), CS_NVPERF_SAMPLES (captures in a row, default 3) and
 * CS_NVPERF_CSV (default CommunityShaders-nvperf.csv beside the log). Each capture is logged per range.
 */
namespace NvPerfBridge
{
	/** @brief After the graph adopted DXVK's device; loads nvperf_grfx_host.dll from a_binDirectory. */
	void Initialize(const DxvkOrgInteropDeviceInfo& a_info, void* a_dxvkModule, const wchar_t* a_binDirectory, org::PersistentGraphHost& a_host);
	bool Active();
	/** @brief Whether a capture was requested (CS_NVPERF_RANGES), before Initialize: the engine's perf events must be on. */
	bool Requested();

	/** @brief Render-thread perf event hooks (State). */
	void BeginEvent(std::string_view a_name);
	void EndEvent();

	/** @brief Once per frame at Present on the render thread. */
	void OnPresent();

	/**
	 * @brief A range inside a graph pass's own recording (the parts of DCLF's colour pass), pushed when a capture is armed
	 * and a_name is selected. The profiler measures one nesting level, so select these names without the pass's own.
	 */
	bool PushPassRange(void* a_vkCommandBuffer, const char* a_name);
	void PopPassRange(void* a_vkCommandBuffer, bool a_pushed);
}
