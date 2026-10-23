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

namespace DCLF::Features::Shadows::Local
{
	struct SunExclusion;

	/**
	 * @brief The point lights' shadow culls without the entries DCLF draws (dclf-architecture.md, "Point lights' shadow culls
	 * without DCLF's entries"). AE and SE. The point lights' culls run on the render thread; the detours and registration hook on any.
	 *
	 * BSShadowParabolicLight::Accumulate (0x14151b960) culls through BSCullingProcess::Process (FUN_1414bf320) with the static
	 * BSParabolicCullingProcess (0x14335bfa0); without a light's own root list (sceneAccumArray) it walks the scene's whole object
	 * root, every frame, every point light: BSFadeNode::OnVisible on hundreds of DCLF's roots, whose casters DCLF's paraboloid
	 * views draw and the registration then withholds (PassCapture). The exclusion is BuildSunExclusion's for the paraboloid
	 * mode, over the light candidates (FrameView::GetLightCandidates): an entry all of whose shadow casters are that mode's
	 * inputs. The process's Process1 (vtable slot 0x16) skips such an entry while the set withholds that mode's casters this
	 * frame and the exclusion was built for the current candidates.
	 * On the frames Policy::FilterLightCulls keeps whole nothing is skipped, and LightCulls (Diagnostics/) counts a paraboloid pass the
	 * registration did not withhold under an excluded entry as one the skip would lose.
	 */
	namespace LocalLightCull
	{
		/** @brief Main thread, the Point source's hooks (after the Sun's: its registration hooks must be in). */
		void Install();
		/** @brief Whether the skip went in (the Point source's culls: Api::ShadowSourceHooks::CullsInstalled). */
		bool Installed();
		/** @brief Install, CS_DCLF_STRICT_LIGHT_CULLS (unset or 1): the portal-strict point lights take the empty culls too. */
		void SetStrictCulls(bool a_on);
		/**
		 * @brief Render thread, the Point source's frameStart (FrameDriver at the scene frame's start): this frame's exclusion, from the
		 * one the shadow epoch published after it drew the paraboloid views (a_pending: ParabolicExclusionSlot::Pending; null: none).
		 */
		void SelectFrame(std::uint32_t a_frame, std::shared_ptr<SunExclusion> a_pending);

		/** @brief The counters since the last Take, reset (Diagnostics/PointCullLines.cpp: the point lights' lines). */
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
			std::int64_t buildTicks = 0;  // on the pool
			std::uint64_t buildsStale = 0, buildsRefused = 0;  // builds landed for an older verdict, or refused a lease
			std::size_t filterNodes = 0;
			// The lights' bits.
			std::uint64_t bitLights = 0, overflow = 0, given = 0, withBits = 0;
			// The empty culls: lights culled empty, and those that could not be (a view not DCLF's; portal-strict); main registrations given
			// their bits (with one), without a slot in the frame's set; parity spheres checked against the cull's, and differing.
			std::uint64_t emptyLights = 0, emptyNotCovered = 0, emptyPortalStrict = 0, emptyGiven = 0, emptyWithBits = 0, emptyUnstamped = 0;
			std::uint64_t spheresChecked = 0, spheresDiffer = 0;
			// Portal-strict lights culled empty; their bits' gate tests, refused by the gates, tested without a portal graph (nothing to gate),
			// and tested without the light's context otherwise.
			std::uint64_t strictLights = 0, strictTested = 0, strictRefused = 0, strictNoGraph = 0, strictNoContext = 0;
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
		/**
		 * @brief Any thread, the attach and detach detours (SceneTracker), through the stand-in observer's structure: a child attached to
		 * or detached from a_parent (an engine NiNode, or null).
		 */
		void NoteStructure(const void* a_parent);
		/**
		 * The point lights' bits. A point light's registration (FUN_1414b2140) ORs its bit into the activeLightMask of every
		 * geometry its cull reaches, which the main pass reads (GetRenderPasses: the shadow lights). A cut entry's geometries
		 * take them from DCLF instead, at their main registration: per light whose cull reached the entry (its category node in
		 * the filter, or the entry where Process1 skipped it), the light's bit when the geometry's bound passes the light's
		 * test (FUN_14151a1e0, its sphere), or untested where the cull reached it untested (cull mode 1). A portal-strict light's
		 * compound frustum is not tested. On the frames the culls keep whole the engine's bits stand (the observer compares them).
		 *
		 * The empty culls: a light whose every view DCLF draws this frame, with its casters claimed by the set, is not culled at all - its
		 * Accumulate keeps only its bookkeeping (the first accumulator's light index and bit, the shadow-light count). Its cull would serve
		 * nothing else: the paraboloid registrations are all withheld, and a point light's process runs with cameraRelatedUpdates off (no
		 * fades, no leaf LOD in OnVisible). Every geometry's first clearing main registration of the frame then takes the bit of each such
		 * light whose sphere (its first view's camera position and its radius, as FUN_141519d20 sets them) its bound is in, and for a
		 * portal-strict light whose strict gates it passes (Portals/StrictGates.h: the rooms and shared nodes its walk reached, the room
		 * frustum; the light's views and DCLF's receivers apply the same gates on the GPU). The parity frames, a light past the mask's 32 and
		 * frames without the exclusion are culled as before (portal-strict lights too with CS_DCLF_STRICT_LIGHT_CULLS=0); the parity frames
		 * compare the predicted spheres with the culls' and the sphere bits, gates included, with the engine's.
		 */
		/**
		 * @brief The Sun source's registration hook (Features/Shadows/Sun), a main registration (+0x160 0xFFFF: it reads the mask, then clears it) of
		 * a geometry DCLF does not draw in the main pass, before the call: a cut entry's takes the lights' bits.
		 */
		void NoteMainRegistration(RE::BSGeometry* a_geometry);
		/** @brief At Present, render thread (the Point source's frameEnd): retired filters are released (two Presents later). */
		void EndFrame();
	}
}
