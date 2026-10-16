#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
}

namespace DCLF::Api
{
	class ShadowSourceCamera;
}

namespace DCLF::Features::Shadows::Local
{
	/**
	 * @brief The focus shadows' views: from the cameras BSShadowDirectionalLight::sub (0x1414f0480) has just aimed at the focus targets,
	 * for whichever light hosts them. Per focus descriptor below the target count: the camera (no caster volume: the focus accumulation,
	 * FUN_1414f0b90, culls nothing), and target i's node, whose subtree the engine registers whole (DCLF's casters for the view are the
	 * members whose fade root is that node: BuildDrawsLatch::focusRoot).
	 *
	 * Render thread: written by the focus camera hook (from CalculateAndDrawShadowCasterLights and CalculateActiveShadowCasterLights),
	 * read by Core through the Focus source's camera (the shadow views' capture) later in the frame.
	 */
	struct FocusFrame
	{
		static constexpr std::uint32_t kMaxFocus = 4;  // focusShadowmapDescriptors

		struct Host
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<ShadowCameras::Camera> views;  // by focus descriptor
		};
		std::vector<Host> hosts;  // this frame's
		std::array<const RE::NiAVObject*, kMaxFocus> targets{};
		std::uint32_t targetCount = 0;
		std::uint32_t sceneFrame = ~0u;

		/** @brief a_light's focus view a_descriptor this frame, or null (its sub did not run this frame, or past the targets). */
		const ShadowCameras::Camera* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			if (a_sceneFrame != sceneFrame)
				return nullptr;
			for (const auto& host : hosts)
				if (host.light == a_light)
					return a_descriptor < host.views.size() && host.views[a_descriptor].valid ? &host.views[a_descriptor] : nullptr;
			return nullptr;
		}
		/** @brief Focus target a_descriptor's node this frame (the casters' root), or null. */
		const RE::NiAVObject* TargetOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const
		{
			return a_sceneFrame == sceneFrame && a_descriptor < targetCount ? targets[a_descriptor] : nullptr;
		}
	};

	/** @brief The board (render thread; see FocusFrame). Written in place by the focus camera hook: its storage is reused. */
	FocusFrame& FocusBoard();

	/**
	 * @brief The focus shadows' camera hook: BSShadowDirectionalLight::sub (0x1414f0480), which writes the focus views into the feature's
	 * board (FocusBoard) from the cameras it has just aimed at the focus targets, for whichever light hosts them (the sun, or a spot
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

	/** @brief The Focus source's camera (Api::ShadowSourceContribution::camera): a_camera's focus view and target this frame, from the board. */
	void FocusCamera(Api::ShadowSourceCamera& a_camera);
}
