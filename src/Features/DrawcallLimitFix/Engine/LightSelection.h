#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace RE
{
	class BSShadowLight;
}

namespace DCLF
{
	/**
	 * @brief Which shadow lights draw this frame, as DCLF selects them (T3b, 2026-10-09): CalculateActiveShadowCasterLights
	 * (0x1414cc570) ported. The engine walks ShadowSceneNode::activeShadowLights (+0x148) in its order and keeps a light while
	 * fewer than four are kept (the sun first, when it draws: 0x14338c911 clear), its UpdateCamera (vfunc 0x10) finds it visible and
	 * its descriptor 0's culling process shares a room with the world camera's list process. A kept light takes the next slot of
	 * shadowLightsAccum (the sun one per cascade, each light's Accumulate one) and the next mask index (+0x520; the sun 0).
	 *
	 * What DCLF reads, and when:
	 * - at the function's entry, each candidate's room test (FUN_140e14300 on the portal-graph entries, +0x30190): the light's
	 *   rooms are its last accumulation's, and the world camera's this frame's portal walk's. A transitional engine read, the
	 *   user's choice for T3b, removed with the portal walk (T5);
	 * - at its return, everything else: the lights, their NiLights (hidden flag, position, radius), the world camera, and each
	 *   spot light's camera as its UpdateCamera set it up (as T2a and T3a read cameras). The visibility tests are DCLF's:
	 *   FUN_14150aba0's frustum intersection for a spot light, BSMultiBoundSphere::Func41's sphere test for a point light, the
	 *   lodFade cutoff (0x1433dcfac), each in the engine's operation order.
	 * The engine's results (shadowLightsAccum, maskIndex, the light count 0x14338c900, frustrumCull) are the parity's alone
	 * (`<- LIGHT SELECTION`). The scissor rectangle (+0x544..) stays the engine's: DCLF does not read it.
	 *
	 * The focus shadows' host (T3c): a light whose drawFocusShadows (+0x558) is set draws focus views 0..count-1 (0x14332a498 targets).
	 * The sun when it draws and the focus shadows are on (0x142032fd0 and a target); else last frame's host (0x1433dcfb8) if it is
	 * still a candidate, else the first kept light whose vfunc 0x20 is true (a directional or spot light). The flag persists across
	 * frames, so DCLF keeps its own per light, written where the engine writes +0x558; the engine's is the parity's.
	 *
	 * Render thread: written in the detour, read by LightViews, ShadowViews and LocalShadowLights later in the frame.
	 */
	class LightSelection
	{
	public:
		struct Selected
		{
			const RE::BSShadowLight* light = nullptr;
			std::uint32_t slot = 0;       // its shadowLightsAccum slot
			std::uint32_t maskIndex = 0;  // +0x520
		};
		struct Frame
		{
			std::uint32_t sceneFrame = ~0u;
			const RE::BSShadowLight* sun = nullptr;   // when it draws
			std::uint32_t sunSlots = 0;
			std::vector<Selected> locals;             // the kept lights after the sun, in the engine's order
			std::vector<const RE::BSShadowLight*> slots;  // shadowLightsAccum as DCLF fills it: the sun per slot, then the lights (null: a slot a light took but did not fill)
			std::vector<const RE::BSShadowLight*> focusHosts;  // the drawn lights with the focus flag
			std::uint32_t focusCount = 0;                      // the focus views each draws (the targets, at most 4)
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
		void SampleRooms();
		void Select();
		void CheckParity();
		void Report();

		Frame frame;
		// The candidates' room tests, sampled at the entry.
		std::unordered_map<const RE::BSShadowLight*, bool> rooms;
		// The focus flag (+0x558) per light as DCLF writes it, and last frame's host (0x1433dcfb8).
		std::unordered_map<const RE::BSShadowLight*, bool> focusFlags;
		const RE::BSShadowLight* lastFocusHost = nullptr;

		struct Parity
		{
			std::uint64_t frames = 0, kept = 0, slotsDiffer = 0, masksDiffer = 0, countsDiffer = 0, focusDiffer = 0, focusHosts = 0, framesDiffer = 0;
			std::string first;
		};
		Parity parity;
		std::uint32_t reportFrames = 0;
		bool installed = false;
	};
}
