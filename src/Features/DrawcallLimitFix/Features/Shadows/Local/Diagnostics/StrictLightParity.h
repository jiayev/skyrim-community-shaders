#pragma once

#include <cstdint>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
}

namespace DCLF::Features::Shadows::Local::StrictLightParity
{
	/**
	 * @brief CS_DCLF_STRICT_LIGHT_PARITY=1 (AE and SE): a portal-strict point light's native cull against DCLF's model of it, on the
	 * parity frames (Diagnostics::ParityDue) whose light culls skip and filter nothing (Policy::FilterLightCulls).
	 *
	 * The engine's side: every geometry BSParabolicCullingProcess::AppendNonAccum (vtable slot 0x19) receives during the light's
	 * Accumulate, with the hemisphere code it was appended under (alphaGroupStopIndex: 1 front, 2 back, 3 both, 0 neither).
	 * The model: the same root walked as the AE functions do it for a point light's request (dclf-portal-query-batches.md,
	 * "Strict point-light traversal"), the rooms' compound frusta from the light's own context (PortalViews::WalkLight's
	 * snapshot), the sphere and plane as the cull left them in the process. Lines: agreement, engine-only and model-only
	 * geometries, differing codes, the subtrees the model does not stand for, and how often an untested append inherits a
	 * state written outside its ancestry (<- STRICT LIGHT on any difference).
	 */
	bool Enabled();
	/** @brief Install, with the switch's value: the comparison's subscription to LightSelectionMade. */
	void Configure(bool a_on);
	/** @brief Render thread, each frame's start before the lights' culls (once, with Enabled): the AppendNonAccum trace. */
	void Install();
	/**
	 * @brief Render thread, around a native Accumulate the frame's culls leave whole (a_whole): starts the light's trace when due.
	 * Returns whether End must follow.
	 */
	bool Begin(const RE::BSShadowLight& a_light, bool a_whole, std::uint32_t a_frame);
	/** @brief Render thread, after the Accumulate Begin traced: the model of the same cull, compared with the trace. */
	void End(const RE::BSShadowLight& a_light, const RE::NiAVObject* a_root, std::uint32_t a_frame);
}
