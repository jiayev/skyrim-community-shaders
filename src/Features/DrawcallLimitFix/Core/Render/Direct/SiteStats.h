#pragma once

#include <cstddef>
#include <cstdint>

#include <rhi.h>

/**
 * @brief Diagnostics (CS_DCLF_SITE_STATS=1): Vulkan pipeline statistics of the CPU pipeline's sites, per site and frame.
 *
 * Each site callback's commands in DXVK's command buffer are wrapped in one pipeline statistics query (outside any render
 * pass, where the callback runs). Logged every 300 frames: input primitives (what the draws submit), clipper input (what
 * survived the driver's primitive culling) and output, vertex and fragment shader invocations, comparable with the
 * engine's D3D11 statistics (CS_GPU_EVENT_STATS) on native D3D11.
 */
namespace DCLF::Direct::SiteStats
{
	bool Enabled();
	/** @brief DXVK's CS thread: begins a_site's query in a_commandBuffer (VkCommandBuffer). Returns its slot, or ~0u (none). */
	std::uint32_t Begin(rhi::Device a_device, void* a_commandBuffer, std::size_t a_site);
	void End(void* a_commandBuffer, std::uint32_t a_slot);
	/** @brief Render thread, once a frame: reads the finished queries, logs every 300 frames. */
	void Collect();
}
