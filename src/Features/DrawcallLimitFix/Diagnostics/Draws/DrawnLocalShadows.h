#pragma once

#include <cstdint>

namespace DCLF::Diagnostics
{
	/** @brief An object's local shadow lights as BuildDrawsCS last wrote them into its colour draw (CS_DCLF_BUILD_PARITY's readback). */
	struct DrawnLocalShadows
	{
		std::uint32_t mask = 0;   // kObjectLocalShadowMask's bits: 1 << the light's mask index
		std::uint32_t gated = 0;  // the portal-strict lights' bits among the frame's (StrictGateRejects decided them)
	};

	/**
	 * @brief Render thread: a_object's, from the newest build parity readback (BuildParity.cpp); false when that readback drew none of
	 * it, or build parity is off. Three main epochs old: compare in a still scene.
	 */
	bool DrawnLocalShadowsOf(std::uint32_t a_object, DrawnLocalShadows& a_out);
}
