#pragma once

// LocalLights: the selection and receiver side of the engine's local shadow lights. LightSelection is the CalculateActiveShadowCasterLights
// port (installed from the Spot source's hooks, so the install log keeps its place: the sun's, the selection's, the point culls', the
// focus cameras'): it hands the Spot and Point sources their kept lights (lightKept), publishes the frame's slots to Core's views list
// (ShadowViews::LightSlots) and tests each light's rooms through Portals (PortalViews::WalkLight, SharesRoom: static calls, DEPENDS
// {Portals}). LocalShadowLights is each DCLF object's main-pass shadow mask: the kept lights' volumes, sampled per colour commit into Core's
// light volumes (Api::LightVolumesContribution, Api/LightVolumes.h), with its landscape rule as data (kLandscapeFlags). Its parity
// (Diagnostics/LightSelectionParity, under CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY) and report lines (self-scheduled,
// Diagnostics/LightSelectionLines.cpp) are its own.
//
// Core keeps the shadow engine: the light candidates, the views list, the latch's volumes and BuildDraws' receiver test. The caster side
// (the point culls) and the kept lights' views are Shadows/Local's (LocalShadowLights reads them through the sources' cameras,
// Extensions::SourceCamera). LightContextResources publishes immutable selected-light atlases from a GPU retirement pool.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Features::LocalLights
{
	// LocalLightsFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// The property flags whose objects take only the lights that affect land (+0x61): kLandscape 14, kNoLODLandFade 46
	// (BSLightingShaderProperty's FUN_14147c340).
	inline constexpr std::uint64_t kLandscapeFlags = (1ull << 14) | (1ull << 46);

	// Diagnostics/LightSelectionLines.cpp, self-scheduled: the light rooms' and light selection's parity lines, logged every 300
	// selections with CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY (LightSelectionParity's window).
	void LightSelectionLines();
}
