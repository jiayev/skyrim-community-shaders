#pragma once

#include "SunViews.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
}

namespace DCLF
{
	/**
	 * @brief The focus shadows' views as DCLF computes them (T3c, 2026-10-09): from the cameras BSShadowDirectionalLight::sub
	 * (0x1414f0480) has just aimed at the focus targets, for whichever light hosts them (the sun, or a spot light: LightSelection
	 * ports which). Per focus descriptor i below the target count (0x14332a498), SunViews' arithmetic: the matrices, the eye and
	 * the viewport of the camera's port; no caster volume (the focus accumulation, FUN_1414f0b90, culls nothing). And target i's
	 * node (0x14332a488: {distance, node}), whose subtree the engine registers whole: DCLF's casters for the view are the members
	 * whose fade root is that node (BuildDrawsLatch::focusRoot).
	 *
	 * Render thread: written in the detour (from CalculateAndDrawShadowCasterLights and CalculateActiveShadowCasterLights), read by
	 * the shadow views' capture later in the frame.
	 */
	class FocusViews
	{
	public:
		static constexpr std::uint32_t kMaxFocus = 4;  // focusShadowmapDescriptors

		static FocusViews& Get();
		void Install();

		/** @brief a_light's focus view a_descriptor this frame, or null (its sub did not run this frame, or past the targets). */
		const SunViews::Cascade* CascadeOf(const RE::BSShadowLight* a_light, std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const;
		/** @brief Focus target a_descriptor's node this frame (the casters' root), or null. */
		const RE::NiAVObject* TargetOf(std::uint32_t a_descriptor, std::uint32_t a_sceneFrame) const;
		/** @brief The engine's focus targets now (0x14332a488): target a_index's node, or null. */
		static const RE::NiAVObject* EngineTarget(std::uint32_t a_index);
		static std::uint32_t EngineTargetCount();

		SunViews::Parity& GetParity() { return parity; }
		/** @brief The parity's line (`<- FOCUS VIEW`), with the registration hook's membership check (`<- FOCUS MEMBERS`); resets both. */
		void Report();
		/** @brief The registration hook (parity only): a geometry the engine registered into focus view a_descriptor's renderer. */
		void NoteRegistration(std::uint32_t a_descriptor, const void* a_geometry);

	private:
		FocusViews() = default;
		struct Hooks;
		friend struct Hooks;
		void Update(const RE::BSShadowLight* a_light);

		struct Host
		{
			const RE::BSShadowLight* light = nullptr;
			std::vector<SunViews::Cascade> views;  // by focus descriptor
		};
		std::vector<Host> hosts;  // this frame's
		std::array<const RE::NiAVObject*, kMaxFocus> targets{};
		std::uint32_t targetCount = 0;
		std::uint32_t sceneFrame = ~0u;
		std::uint32_t reportFrames = 0;
		SunViews::Parity parity;
		// The membership check, from the engine's registration threads.
		std::atomic<std::uint64_t> registrations{ 0 }, outside{ 0 };
		std::mutex firstMutex;
		std::string firstOutside;  // named when met: the geometry need not outlive the window
		bool installed = false;
	};
}
