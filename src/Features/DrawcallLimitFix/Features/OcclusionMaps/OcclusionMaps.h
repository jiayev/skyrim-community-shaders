#pragma once

// OcclusionMaps (CS_DCLF_SKYLIGHT, CS_DCLF_PRECIPITATION): the precipitation accumulator's two views (render
// mode 0x1C), Skylighting's sky occlusion map and the precipitation occlusion mask, drawn by Core's shadow engine in the engine's place
// while Skylighting is loaded (its RenderOcclusion drives both: its hook of the engine's mask, then its own map) and the map's toggle is
// on. The feature registers its switches and live toggles (which need the shadows) and each map's contribution
// (Api::OcclusionMapContribution, Api/ShadowViews.h): its set phase and view bit (kSetOccluderSky / kViewSkyOccluder,
// kSetOccluderPrecipitation / kViewPrecipOccluder), Core's occluder rule (Skylighting's, the precipitation mask's), its target (the sky map
// imports Skylighting's own occlusion texture; the mask is the engine's), its labels and its reference parity (Policy's).
//
// Core keeps the occlusion epoch (ExecuteOcclusion, on the shadow commit), the maps' modes, view slots, depth targets and technique
// columns, the occluder rules' bodies (Coordinator::OcclusionTechniqueOf), the set decisions and the reports (Core/Extensions/ShadowViews.h:
// the maps' entries, read as data). Skylighting calls the shell's occlusion methods (DrawcallLimitFix::OcclusionReady and its siblings),
// and the shell's 0x1C capture; CS_DCLF_SKYLIGHT_PARITY's compare (Diagnostics/Feature/Skylight) stays with the diagnostics.
// Included by the feature's sources only (and Features/Manifest.cpp, for the constants).

#include "Features/DrawcallLimitFix/Api/Feature.h"

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
}
