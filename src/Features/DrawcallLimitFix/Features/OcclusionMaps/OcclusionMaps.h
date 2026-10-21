#pragma once

// OcclusionMaps (CS_DCLF_SKYLIGHT, CS_DCLF_PRECIPITATION): the precipitation accumulator's two views (render
// mode 0x1C), Skylighting's sky occlusion map and the precipitation occlusion mask, drawn in the shadow epoch (Features/Shadows/Engine's,
// DEPENDS: listed after it) in the engine's place
// while Skylighting is loaded (it drives both: the engine's mask, then its own map, set up before the engine's shadow maps when DCLF
// draws them, RenderOcclusionAtShadowMaps, else rendered whole at the engine's occlusion point, RenderOcclusion) and the map's toggle is
// on. The feature registers its switches and live toggles (which need the shadows) and each map's contribution
// (Api::OcclusionMapContribution, Api/ShadowViews.h): its set phase and view bit (kSetOccluderSky / kViewSkyOccluder,
// kSetOccluderPrecipitation / kViewPrecipOccluder), its occluder rule (OccluderRules.cpp: Skylighting's, the precipitation mask's) and
// cull size test (both maps') and floor (the sky map's: Skylighting's probe grid), its target (the sky map imports Skylighting's own
// occlusion texture; the mask is the engine's), its labels,
// its camera and accumulator (the precipitation accumulator's) and its reference parity (CS_DCLF_SKYLIGHT_PARITY's,
// Diagnostics/SkylightParity.cpp, with its switches and compare). Its report lines (Diagnostics/OcclusionReport.cpp) go at the shadow views'.
//
// Core keeps the maps' part of the shadow commit (ExecuteShadowFrame: their views latched with the shadow views'; Features/Shadows/Engine
// submits it), the maps' modes, view slots, depth targets and technique
// columns, the walk that hands each object to the rules (Api::OccluderLeaf, with the nearest fade node's BSX flags), the set decisions
// and the reports' other lines (Core/Extensions/ShadowViews.h: the maps' entries, resolved once). Skylighting calls the shell's
// occlusion methods (DrawcallLimitFix::OcclusionReady and its siblings), whose view and 0x1C capture ask the maps' camera and
// accumulator.
// Included by the feature's sources only (and Features/Manifest.cpp, for the constants).

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Api/ShadowViews.h"

#include <cstdint>

namespace DCLF::Features::OcclusionMaps
{
	// The maps' set phases (SceneSet.h's SetPhase bits) and view bits at the GPU cull (GpuLayouts.h's kView*; no HLSL mirror: the
	// shadow builds take a map's view bit as data, from its entry): Features/Manifest.cpp checks them against Core's.
	inline constexpr std::uint8_t kSetOccluderSky = 1u << 2;            // Skylighting's occlusion map
	inline constexpr std::uint8_t kSetOccluderPrecipitation = 1u << 3;  // the precipitation occlusion mask
	inline constexpr std::uint32_t kViewSkyOccluder = 1u << 5;
	inline constexpr std::uint32_t kViewPrecipOccluder = 1u << 6;

	// OcclusionMapsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// OccluderRules.cpp: the maps' occluder rules (their technique; the scene work, per walked entry, pure): Skylighting::
	// OcclusionTechnique's (skinned only with tree animation, no object under a fade node whose BSX flags reject it, depth-writing,
	// none of refraction, LOD landscape, eye reflection, decals), the precipitation mask's (never skinned, depth-writing, none of
	// refraction, landscape blends, LOD landscape, eye reflection, decals).
	std::uint32_t SkylightingTechnique(const Api::OccluderLeaf& a_leaf);
	std::uint32_t PrecipitationTechnique(const Api::OccluderLeaf& a_leaf);
	// Render thread: both maps' camera and accumulator, the precipitation accumulator's (Precipitation::occlusionData); it draws
	// Skylighting's map while Skylighting draws it (inOcclusion), else the precipitation mask.
	void PrecipitationCamera(Api::OcclusionCamera& a_camera);
	bool SkyOwnsAccumulator(const Api::OcclusionAccumulator& a_accumulator);
	bool PrecipitationOwnsAccumulator(const Api::OcclusionAccumulator& a_accumulator);

	// Diagnostics/OcclusionReport.cpp: the maps' report lines (ReportPoint::ShadowViews).
	void RegisterReport(Api::InstallContext& a_context);

	// Diagnostics/SkylightParity.cpp: CS_DCLF_SKYLIGHT_PARITY (CS_DCLF_SKYLIGHT_DUMP_DIR: where it writes the maps it compares): every
	// 120th frame a map (0 the sky map, 1 the precipitation mask) is rendered both ways, the engine's first, and the two compared texel
	// by texel. Its switches, and its observers of the maps' events when it is on.
	void RegisterParity(Api::InstallContext& a_context);
	// Render thread (the maps' referenceDue): whether this frame renders a_map's reference. Off: false.
	bool ReferenceDue(std::uint32_t a_map);

	/** @brief The occlusion views' check (T4: a capture against its map's own camera), with the claims or scene observers. */
	void SubscribeOcclusionViewCheck();
}
