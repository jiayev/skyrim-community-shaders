#pragma once

#include <cstdint>

namespace DCLF::Diagnostics::Skylight
{
	/**
	 * @brief CS_DCLF_SKYLIGHT_PARITY=1: every 120th frame an occlusion map (0 Skylighting's, 1 the precipitation mask) is rendered both
	 * ways, the engine's first. Whether this frame renders a_map's reference (Policy::RenderOcclusionReference asks). Render thread.
	 */
	bool ReferenceDue(std::uint32_t a_map);

	/** @brief Install: the observers of the occlusion maps' events. */
	void Install();
}
