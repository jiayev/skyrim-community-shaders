#pragma once

#include <cstdint>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
}

namespace DCLF::Features::Shadows::Local
{
	/**
	 * @brief The focus shadows' camera hook: BSShadowDirectionalLight::sub (0x1414f0480), which writes the focus views into Core's board
	 * (ShadowCameras::Focus) from the cameras it has just aimed at the focus targets, for whichever light hosts them (the sun, or a spot
	 * light: LightSelection ports which). Per focus descriptor i below the target count (0x14332a498), ShadowCameras' arithmetic: the
	 * matrices, the eye and the viewport of the camera's port; no caster volume (the focus accumulation, FUN_1414f0b90, culls nothing).
	 * And target i's node (0x14332a488: {distance, node}), whose subtree the engine registers whole.
	 *
	 * Render thread: written in the detour (from CalculateAndDrawShadowCasterLights and CalculateActiveShadowCasterLights), read by
	 * the shadow views' capture later in the frame. Installed from the feature's source hooks, for the Focus source
	 * (Api::Feature::sourceHooks); the targets are read through EngineFocusTarget.
	 */
	class FocusViews
	{
	public:
		static FocusViews& Get();
		void Install();

	private:
		FocusViews() = default;
		struct Hooks;
		friend struct Hooks;
		void Update(const RE::BSShadowLight* a_light);

		bool installed = false;
	};

	/**
	 * @brief The engine's current focus targets (0x14332a488: {float distance; NiAVObject* node} x the count at 0x14332a498): target
	 * a_index's node, or null past the count. What the focus camera hook records (FocusFrame::targets) and the focus parity compares
	 * the registrations with (Diagnostics/FocusParity.cpp). Any thread that reads the engine's focus state.
	 */
	const RE::NiAVObject* EngineFocusTarget(std::uint32_t a_index);
	std::uint32_t EngineFocusTargetCount();
}
