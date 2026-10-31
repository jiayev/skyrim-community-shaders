#pragma once

// Landscape (CS_DCLF_MTLAND): the terrain, the MTLand and MTLandLODBlend Lighting techniques (8, 19), DCLF's while the terrain toggle is
// on. The feature registers its switch and live toggle and the two techniques' contributions (Api::TechniqueContribution: the Lighting
// port's eligibility reads the toggle through them, as data; their objects take Core's land-blend extras, Api::ObjectExtras::LandBlend).
// While Terrain Blending holds the terrain, Integrations/TerrainBlending gates both techniques on its own toggle too (Api::TechniqueGate).
//
// Core keeps what every land quad and its draw share: the MTLand material handling (the Lighting port's SetupMaterial replica, the
// Landscape and LODLandscape vtables, the source signature's landscape bit, the PS names), LandSetupProperties, the landscape lights
// (kObjectLandscapeLights) and the land-blend extras (kObjectLandBlend, its rows shared with the projected UV, the LandBlendParams hook, the
// capture and its parity). LOD terrain is Features/Lod's (LODLand, LODLandNoise); its flag rule runs before these techniques' eligibility.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Landscape
{
	// LandscapeFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// Diagnostics/LandTrace.cpp: CS_DCLF_LAND_TRACE=1, main thread at the scene frame's start (FrameBegin), with or without DCLF: every
	// loaded land quad's engine state (node, geometry, property, material, texture paths), logged when first seen and when it changes.
	void SubscribeLandTrace();
}
