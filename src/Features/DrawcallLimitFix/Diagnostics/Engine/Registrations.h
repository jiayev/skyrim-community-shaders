#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace DCLF::Diagnostics::Registrations
{
	/**
	 * @brief CS_DCLF_SET_PARITY's observers of the members' passes the main camera's registration withholds (PassCapture's hooks, on
	 * whatever thread the engine registers):
	 * - <- FORWARD: the blended passes (accumulation hint 1, the main modes' list insertion) - what the engine would have drawn of the
	 *   forward class, against the forward objects the colour segment found in view;
	 * - <- REFRACTION: the refraction passes (hint 4, geometry group 5: GetRenderPasses' Utility RENDER_NORMAL pass), likewise;
	 * - <- EFFECT LIGHT: of the blended passes, those of the Effect shader's Lighting variant (checked), and those whose light 0 is not
	 *   the scene's sun light (other): the port draws them with the sun light's DLightColor (LightingConstants.h, SampleEffectLight),
	 *   which holds while FUN_1414fcf80 gives them ShadowSceneNode +0x200.
	 * The particle members' (<- PARTICLES) are their feature's (Features/Particles).
	 */
	void Install();

	/** @brief The blended passes withheld since the last call. Render thread, once a colour epoch; 0 without the parity. */
	std::uint32_t TakeForward();
	/** @brief The refraction passes withheld since the last call. Render thread, once a colour epoch; 0 without the parity. */
	std::uint32_t TakeRefraction();
	/** @brief The Effect Lighting passes checked and those with another light 0, since the last call. Render thread, once a report. */
	std::pair<std::uint32_t, std::uint32_t> TakeEffectLight();
}
