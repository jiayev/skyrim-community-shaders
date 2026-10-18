#pragma once

// The shadow-view contract (Api::InstallContext::RegisterOcclusionMap, RegisterShadowSource, RegisterShadowExclusion), the view
// contract's sibling (Api/Views.h): the occlusion maps and the shadow sources that Core's one shadow engine draws (one payload, one set
// of resources, one latch block and one key group; the occlusion epoch shares the shadow commit), taken by features as data and event
// callbacks, and the entry exclusions its payload carries (the contribution's own type, held by Core opaquely). Casters are keyed by the
// engine's render mode, not by source: the sources own no set phase or view bit.

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Core-defined (Core/Extensions/ShadowViews.h): what a map's and a source's callbacks are handed.
	class OcclusionTargetImport;
	class OccluderLeaf;
	class OcclusionCamera;
	class OcclusionAccumulator;
	class ShadowLightKept;
	class ShadowSourceCamera;
	// Core-defined (Core/Render/Shadow/ShadowExclusionBuild.h): what the exclusion contribution's build is handed.
	class ShadowExclusionBuild;

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
	 * - floorHeight (optional): render thread, as the map's epoch latches its view: a world height; the map's cull draws nothing of an
	 *   object whose bound lies wholly below it (Skylighting::OcclusionTechnique's probe grid test: an occluder below every probe is on
	 *   no probe's ray to the sky). None: no test.
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
		float (*floorHeight)() = nullptr;
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
	 * the engine's). Its cameras are the feature's to compute (with Core's camera math, Core/Render/Shadow/ShadowCameras.h), into its own
	 * boards, from its Feature::sourceHooks (called with the source, in ShadowSource order: the Sun's, the Spot's with Features/LocalLights'
	 * light selection, then the others'), and to hand Core through camera.
	 *
	 * - camera (required): render thread, per view of the source the shadow views' capture takes (and per descriptor of a kept light the
	 *   light volumes' sample reads, Extensions::SourceCamera): DCLF's camera for a_camera's view (its light, descriptor and scene frame),
	 *   null when it has none this frame; a Focus view's also its target's node (the casters' root).
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
		void (*camera)(ShadowSourceCamera& a_camera) = nullptr;
		void (*lightKept)(const ShadowLightKept& a_light) = nullptr;
		bool (*fullFrustumWindowEnd)() = nullptr;
		void (*frameStart)(std::uint32_t a_frame) = nullptr;
		void (*frameEnd)() = nullptr;
	};

	/**
	 * @brief What the exclusion contribution's listExclusion answers: the sun's pending exclusion as the primary cull's scene-list filter
	 * reads it (a root out of the lists is out of the sun's cascades only when it takes the root's entry out too). A plain view the
	 * contribution fills once a call, read by Core per entry directly; held keeps it alive (none: no exclusion pending).
	 *
	 * - candidates: the identity of the candidates (Core's EntrySpace) it is a verdict on.
	 * - excluded: per candidate index, nonzero for an entry it takes out of the cascade culls (its size the entry count).
	 * - version: changes only when excluded does.
	 */
	struct ShadowListExclusion
	{
		std::shared_ptr<const void> held;
		const void* candidates = nullptr;
		std::span<const std::uint8_t> excluded;
		std::uint64_t version = 0;
	};

	/**
	 * @brief The shadow payload's entry exclusions (one contribution: a second fails the install): the next frame's verdicts on the
	 * entries DCLF can take out of the engine's culls (the sun's cascade culls, the point lights'), from the candidates and the mode's
	 * inputs, which Core's shadow epoch hands back to it (publish) and the contribution keeps for its culls (Features/Shadows/Local's
	 * SunExclusionSlot, ParabolicExclusionSlot). The exclusions are the contribution's own type: Core holds them opaquely. Without one the
	 * payload carries no exclusion, and nothing is taken out.
	 *
	 * - name: the contribution's (the install's failures).
	 * - build (required): the snapshot builder, in the shadow participant's payload (ShadowEngine::BuildPayload) while the shadow
	 *   withholding is on, once per mode its views used, the Sun source's then the Point source's (a pool task each when both): a_build's
	 *   exclusion into a_build.result. Per build, never per object.
	 * - publish (required): render thread, once a frame, after the shadow epoch drew (ShadowEngine::NoteShadowEpochDrawn) while the
	 *   shadow withholding is on: the Sun source's exclusion when its mode drew (the payload's, null: none), then the Point source's while
	 *   its culls are installed (Extensions::SourceCullsInstalled; null when its mode drew nothing). None is published before DCLF's
	 *   first covered frame (Scene::FrameView::NotDrawingYet), and claims once installed are never withdrawn.
	 * - frameStart (required): render thread, once a frame, in the frame driver after the views' FrameStartBeforeCoverage work and before
	 *   the lookups are installed: the pending exclusions brought to the frame's candidates.
	 * - listExclusion (required): render thread, once a frame where the primary cull's scene-list filter is judged (and on its report
	 *   frames): the Sun source's pending exclusion into a_view (left empty: none).
	 */
	struct ShadowExclusionContribution
	{
		std::string_view name;
		void (*build)(ShadowExclusionBuild& a_build) = nullptr;
		void (*publish)(ShadowSource a_source, std::shared_ptr<void> a_exclusion) = nullptr;
		void (*frameStart)() = nullptr;
		void (*listExclusion)(ShadowListExclusion& a_view) = nullptr;
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
