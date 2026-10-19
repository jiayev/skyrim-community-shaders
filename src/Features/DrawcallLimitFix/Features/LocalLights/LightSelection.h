#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

namespace RE
{
	class BSShadowLight;
}

namespace DCLF::Diagnostics
{
	struct LightSelectionView;
}

namespace DCLF
{
	/**
	 * @brief Which shadow lights draw this frame, as DCLF selects them: CalculateActiveShadowCasterLights
	 * (0x1414cc570) ported. The engine walks ShadowSceneNode::activeShadowLights (+0x148) in its order and keeps a light while
	 * fewer than four are kept (the sun first, when it draws: 0x14338c911 clear), its UpdateCamera (vfunc 0x10) finds it visible and
	 * its descriptor 0's culling process shares a room with the world camera's list process. A kept light takes the next slot of
	 * shadowLightsAccum (the sun one per cascade, each light's Accumulate one) and the next mask index (+0x520; the sun 0).
	 *
	 * What DCLF reads, and when:
	 * - at its return, everything. The room test is DCLF's: FUN_140e14300 between the light's rooms as DCLF walks them
	 *   this frame (PortalViews::WalkLight, kept per light) and the world camera's, this frame's PortalViews walk. The engine tests its
	 *   entries as their last walks left them (a light's: when it was last kept, or moved as a dynamic light); those, sampled at
	 *   the function's entry, are the observer's (`<- LIGHT ROOMS`), a light moved since its engine walk tallied apart.
	 *   Also the lights, their NiLights (hidden flag, position, radius), the world camera, and each
	 *   spot light's camera as its UpdateCamera set it up (as the sources' camera reads do). The visibility tests are DCLF's:
	 *   FUN_14150aba0's frustum intersection for a spot light, BSMultiBoundSphere::Func41's sphere test for a point light, the
	 *   lodFade cutoff (0x1433dcfac), each in the engine's operation order.
	 * The engine's results (shadowLightsAccum, maskIndex, the light count 0x14338c900, frustrumCull) are the observer's alone
	 * (`<- LIGHT SELECTION`). The scissor rectangle (+0x544..) stays the engine's: DCLF does not read it.
	 *
	 * The focus shadows' host (the Focus board's): a light whose drawFocusShadows (+0x558) is set draws focus views 0..count-1 (0x14332a498 targets).
	 * The sun when it draws and the focus shadows are on (0x142032fd0 and a target); else last frame's host (0x1433dcfb8) if it is
	 * still a candidate, else the first kept light whose vfunc 0x20 is true (a directional or spot light). The flag persists across
	 * frames, so DCLF keeps its own per light, written where the engine writes +0x558; the engine's is the observer's.
	 *
	 * Render thread: written in the detour, read by the Spot and Point sources (their lightKept), ShadowViews (the LightSlots board) and
	 * LocalShadowLights later in the frame.
	 */
	class LightSelection
	{
	public:
		struct Selected
		{
			const RE::BSShadowLight* light = nullptr;
			std::uint32_t slot = 0;                  // its shadowLightsAccum slot
			std::uint32_t maskIndex = 0;             // +0x520
			Portals::ContextHold portalContext;      // owned strict-light snapshot, never mutable walk scratch
			std::uint32_t portalContextIndex = ~0u;  // directory entry in Frame::portalContexts; no strict context: ~0u
		};
		struct Frame
		{
			std::uint32_t sceneFrame = ~0u;
			const RE::BSShadowLight* sun = nullptr;  // when it draws
			std::uint32_t sunSlots = 0;
			std::vector<Selected> locals;                      // the kept lights after the sun, in the engine's order
			std::vector<const RE::BSShadowLight*> slots;       // shadowLightsAccum as DCLF fills it: the sun per slot, then the lights (null: a slot a light took but did not fill)
			std::vector<const RE::BSShadowLight*> focusHosts;  // the drawn lights with the focus flag
			std::uint32_t focusCount = 0;                      // the focus views each draws (the targets, at most 4)
			std::vector<std::uint32_t> portalContexts;         // owned atlas of the camera and selected strict-light contexts
			bool valid = false;
		};

		static LightSelection& Get();
		void Install();

		/** @brief This frame's selection, or null before CalculateActiveShadowCasterLights has run this frame. */
		const Frame* Current(std::uint32_t a_sceneFrame) const { return frame.valid && frame.sceneFrame == a_sceneFrame ? &frame : nullptr; }

	private:
		LightSelection() = default;
		struct Hooks;
		friend struct Hooks;
		friend struct Diagnostics::LightSelectionView;
		/** @brief The engine's room condition for a candidate, from DCLF's walk (PortalViews::WalkLight). */
		bool SharesRoomWithCamera(const RE::BSShadowLight* a_light);
		void Select();
		/** @brief The kept spot and point lights, to their sources (the end of Select). */
		static void TellKeptLights(const Frame& a_frame);

		Frame frame;
		// Each light's portal-graph entry as DCLF keeps it (a light not walked keeps its last), and this frame's room verdicts.
		std::unordered_map<const RE::BSShadowLight*, PortalViews::LightRooms> lightRooms;
		std::unordered_map<const RE::BSShadowLight*, bool> roomVerdicts;
		// The focus flag (+0x558) per light as DCLF writes it, and last frame's host (0x1433dcfb8).
		std::unordered_map<const RE::BSShadowLight*, bool> focusFlags;
		const RE::BSShadowLight* lastFocusHost = nullptr;

		bool installed = false;
	};
}
