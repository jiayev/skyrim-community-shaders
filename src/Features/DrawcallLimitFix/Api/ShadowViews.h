#pragma once

// The shadow-view contract (Api::InstallContext::RegisterOcclusionMap, RegisterShadowSource), the view contract's sibling
// (Api/Views.h): the occlusion maps and the shadow sources that Core's one shadow engine draws (one payload, one set of resources,
// one latch block and one key group; the occlusion epoch shares the shadow commit), taken by features as data and event callbacks.
// Casters are keyed by the engine's render mode, not by source: the sources own no set phase or view bit.

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Core-defined (Core/Extensions/ShadowViews.h): what a map's and a source's callbacks are handed.
	class OcclusionTargetImport;
	class OccluderLeaf;
	class OcclusionCamera;
	class OcclusionAccumulator;
	class ShadowLightKept;

	/**
	 * @brief An occlusion map: a view of the precipitation accumulator (render mode 0x1C) the engine would fill by culling the scene
	 * lists. The order is Core's slot order (kOcclusionSky, kOcclusionPrecipitation): each map's mode, view slot, depth target and
	 * technique column, and the order its epoch, readiness and reports run in.
	 */
	enum class OcclusionMap : std::uint8_t
	{
		Sky,
		Precipitation,
		Count,
	};
	/** @brief What a map draws into: the engine's mask (the precipitation occlusion target), or a texture the feature imports. */
	enum class OcclusionTarget : std::uint8_t
	{
		EngineMask,
		ImportedTexture,
	};
	/** @brief A map's names: its view-mask parity list's, its report lines', and its imported target's. Copied at install. */
	struct OcclusionLabels
	{
		std::string_view list, report, target;
	};
	/**
	 * @brief An occlusion map contribution: the map is DCLF's while it is available and its toggle is on. Read where the shadow epochs,
	 * the set decisions and the classification run, as data (resolved once at install; its occluder rule called per walked entry).
	 *
	 * - map: the map, one feature's. toggle: a registered toggle. available: a load-time flag the feature owns, read as data whenever
	 *   the toggle is (required).
	 * - bits: the map's set phase and view bit (the feature's own: Api/ViewCommon.h). target, labels: see their types.
	 * - technique (required, pure): the scene work, per walked entry while the map is on, for an object with a Lighting property that
	 *   is not LOD (LOD occludes neither map: Precipitation::SetupMask culls the scene lists, which do not hold the LOD root): the
	 *   Utility technique the map draws it with (Skylighting::OcclusionTechnique's bits; 0x80, alpha-tested, samples the diffuse), 0:
	 *   it occludes nothing.
	 * - minRadius: the map's cull draws nothing of an object whose bound radius is at most 32 (Skylighting::OcclusionTechnique's size
	 *   test).
	 * - camera (required): render thread, as the map's view is set up (DrawcallLimitFix::OcclusionView) while DCLF runs: the camera the
	 *   map is drawn from (none: the view is not set up).
	 * - ownsAccumulator (required): render thread, as a 0x1C accumulator finishes accumulating while DCLF runs, in slot order until one
	 *   does: whether the accumulator is drawing the map (the map's capture).
	 * - importTarget (ImportedTexture, required): render thread, once, as the shadow capability sets the maps' modes up
	 *   (IndirectDraws::UpdateShadowCapability), in Core's import order (Precipitation, then Sky): the texture to import as the map's
	 *   depth target.
	 * - referenceDue (optional, parity): render thread, where the engine asks whether this frame renders the map's reference
	 *   (OcclusionReferenceFrame): whether it does.
	 */
	struct OcclusionMapContribution
	{
		OcclusionMap map = OcclusionMap::Count;
		ToggleId toggle;
		const bool* available = nullptr;
		ViewBits bits;
		std::uint32_t (*technique)(const OccluderLeaf& a_leaf) = nullptr;
		bool minRadius = false;
		OcclusionTarget target = OcclusionTarget::EngineMask;
		OcclusionLabels labels;
		void (*importTarget)(OcclusionTargetImport& a_import) = nullptr;
		bool (*referenceDue)() = nullptr;
		void (*camera)(OcclusionCamera& a_camera) = nullptr;
		bool (*ownsAccumulator)(const OcclusionAccumulator& a_accumulator) = nullptr;
	};

	/**
	 * @brief A shadow source: the lights whose views the shadow engine draws. Sun and Spot share the clamped mode and its caster phase,
	 * Point is the paraboloid, Focus the focus views (any host's); the order is Core's (ShadowViews::Rebuild's views map to it by kind).
	 */
	enum class ShadowSource : std::uint8_t
	{
		Sun,
		Spot,
		Point,
		Focus,
		Count,
	};
	/**
	 * @brief A shadow source contribution: the source's views are drawn while it is registered (a view of a source none registered stays
	 * the engine's). Its cameras are the feature's to compute, into Core's boards (Core/Render/Shadow/ShadowCameras.h), from its
	 * Feature::sourceHooks (called with the source, in ShadowSource order: the Sun's, the Spot's with Features/LocalLights' light
	 * selection, then the others').
	 *
	 * - lightKept (Spot and Point, required): render thread, at the end of the light selection, once per kept light of the source's kind,
	 *   in the selection's order.
	 * - fullFrustumWindowEnd (Sun, required; none for the others): render thread, once a frame, after the main camera's full-frustum cull call site
	 *   (PrimaryCull::AfterFullFrustum); whether this frame's full-frustum cull applied the source's entry exclusion.
	 * - frameStart (optional): render thread, once a frame, in source order (Extensions::ShadowSourcesFrameStart): the frame driver's,
	 *   after the frame requests are served, before the main renderers are refreshed; a_frame is the frame starting.
	 * - frameEnd (optional): render thread, at Present, in source order (Extensions::ShadowSourcesFrameEnd), first in the primary cull's
	 *   end of the frame.
	 */
	struct ShadowSourceContribution
	{
		ShadowSource source = ShadowSource::Count;
		void (*lightKept)(const ShadowLightKept& a_light) = nullptr;
		bool (*fullFrustumWindowEnd)() = nullptr;
		void (*frameStart)(std::uint32_t a_frame) = nullptr;
		void (*frameEnd)() = nullptr;
	};

	/**
	 * @brief What Feature::sourceHooks is handed: the source whose hooks install now, and the call that says its engine culls' hooks are
	 * in (Features/Shadows/Local's point culls take a cut entry's lights' bits from the Sun's registration hooks, so they install only
	 * after those).
	 */
	class ShadowSourceHooks
	{
	public:
		ShadowSource source = ShadowSource::Count;
		/** @brief Main thread, in the call: the source's cull hooks are installed (Extensions::SourceCullsInstalled). */
		void CullsInstalled();
	};
}
