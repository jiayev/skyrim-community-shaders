#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <string>

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
	class BSRenderPass;
	class NiAVObject;
	class NiNode;
}

namespace DCLF
{
	struct SunExclusion;

	/**
	 * @brief The point lights' shadow culls without the entries DCLF draws (drawcall-limit-fix.md, "Point lights' shadow culls
	 * without DCLF's entries"). AE only.
	 *
	 * BSShadowParabolicLight::Accumulate (0x14151b960) culls through BSCullingProcess::Process (FUN_1414bf320) with the static
	 * BSParabolicCullingProcess (0x14335bfa0); without a light's own root list (sceneAccumArray) it walks the scene's whole object
	 * root, every frame, every point light: BSFadeNode::OnVisible on hundreds of DCLF's roots, whose casters DCLF's paraboloid
	 * views draw and the registration then withholds (PassCapture). The exclusion is BuildSunExclusion's for the
	 * paraboloid mode, over the light candidates (Coordinator::GetLightCandidates): an entry all of whose shadow casters are that
	 * mode's inputs. The process's Process1 (vtable slot 0x16)
	 * skips such an entry while the set withholds that mode's casters this frame and the exclusion was built for the current candidates.
	 * On the frames Policy::FilterLightCulls keeps whole nothing is skipped, and Diagnostics::LightCulls counts a paraboloid pass the
	 * registration did not withhold under an excluded entry as one the skip would lose.
	 */
	namespace LocalLightCull
	{
		void Install();
		/** @brief Render thread, after the shadow epoch drew the paraboloid views: next frame's exclusion (null: none). */
		void Publish(std::shared_ptr<SunExclusion> a_exclusion);
		/** @brief Render thread, at the scene frame's start: this frame's exclusion. */
		void SelectFrame(std::uint32_t a_frame);

		/** @brief The counters since the last Take, reset (Reports::PrimaryCullLines' point lights' lines). */
		inline constexpr std::uint32_t kReachKindCount = 3;  // untested, sphere, portals (and "not reached" after them)
		extern const std::array<const char*, kReachKindCount + 1> kReachNames;
		struct Counters
		{
			std::uint64_t accumulateCalls = 0;
			std::int64_t accumulateTicks = 0;
			bool installed = false, filterInstalled = false;
			// The shadow culls' skip.
			std::uint64_t frames = 0, live = 0, noExclusion = 0, noClaims = 0, stale = 0, masked = 0, visited = 0, skipped = 0;
			// The category filter.
			std::uint64_t lights = 0, filtered = 0, walkedNotLive = 0, nodes = 0, children = 0, cut = 0, dirtyNodes = 0, builds = 0, nodesRebuilt = 0;
			std::int64_t buildTicks = 0;
			std::size_t filterNodes = 0;
			// The lights' bits.
			std::uint64_t bitLights = 0, overflow = 0, given = 0, withBits = 0;
		};
		Counters Take();

		/**
		 * The category filter (with the skip): inside a point light's cull, a category node (an exact
		 * NiNode that is the parent of excluded entries) culls its children less those entries, through NiNode::OnVisible's
		 * vtable slot, instead of reaching each to skip it. Everything above (the cells and their multibound tests, kAllPass,
		 * the portals' compound frustum) stays the engine's, so every light takes it, portal-strict ones included. Built at
		 * the selection for a new exclusion; a node a child is attached to or detached from is walked natively until the
		 * next selection builds it again. The frames the culls keep whole filter nothing (the observer checks that every
		 * caster the engine registers under a filtered node hangs from a child it keeps).
		 */
		/** @brief Any thread, the attach and detach detours (SceneTracker): a_child attached to or detached from a_parent. */
		void NoteStructure(const RE::NiNode* a_parent, RE::NiAVObject* a_child, bool a_attached);
		/**
		 * The point lights' bits. A point light's registration (FUN_1414b2140) ORs its bit into the activeLightMask of every
		 * geometry its cull reaches, which the main pass reads (GetRenderPasses: the shadow lights). A cut entry's geometries
		 * take them from DCLF instead, at their main registration: per light whose cull reached the entry (its category node in
		 * the filter, or the entry where Process1 skipped it), the light's bit when the geometry's bound passes the light's
		 * test (FUN_14151a1e0, its sphere), or untested where the cull reached it untested (cull mode 1). A portal-strict light's
		 * compound frustum is not tested. On the frames the culls keep whole the engine's bits stand (the observer compares them).
		 */
		/**
		 * @brief SunAccumulation's registration hook, a main registration (+0x160 0xFFFF: it reads the mask, then clears it) of
		 * a geometry DCLF does not draw in the main pass, before the call: a cut entry's takes the lights' bits.
		 */
		void NoteMainRegistration(RE::BSGeometry* a_geometry);
		/** @brief At Present, render thread: retired filters are released (two Presents later). */
		void EndFrame();
	}
}
