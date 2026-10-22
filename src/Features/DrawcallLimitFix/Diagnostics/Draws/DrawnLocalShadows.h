#pragma once

#include <array>
#include <cstdint>

namespace DCLF::Diagnostics
{
	/** @brief An object's local shadow lights as BuildDrawsCS last wrote them into its colour draw (CS_DCLF_BUILD_PARITY's readback). */
	struct DrawnLocalShadows
	{
		std::uint32_t mask = 0;   // kObjectLocalShadowMask's bits: 1 << the light's mask index
		std::uint32_t gated = 0;  // the portal-strict lights' bits among the frame's (StrictGateRejects decided them)
		float bound[4]{};         // its geometry's world bound when the readback was armed (a receiver that moved since may differ)
		std::array<const void*, 4> channels{};  // each mask channel's light when the readback was armed (ChannelLights)
	};

	/**
	 * @brief Render thread: a_object's, from the newest build parity readback (BuildParity.cpp); false when that readback drew none of
	 * it, or build parity is off. Three main epochs old: compare in a still scene.
	 */
	bool DrawnLocalShadowsOf(std::uint32_t a_object, DrawnLocalShadows& a_out);

	/**
	* @brief Render thread: each local shadow mask channel's light now, by the engine's active shadow lights' mask index (BSShadowLight
	* +0x520), null for a channel none holds. A selection reassigns channels as the lights in view change, so a readback armed before
	* names its bits by other lights.
	*/
	std::array<const void*, 4> ChannelLights();
}
