#pragma once

// Subsurface Scattering (Features/SubsurfaceScattering.h): while it is loaded with its beast race keyword, its BSLightingShader_SetupSkin hook
// sets IsBeastRace for a face (kFace or kFaceGenRGBTint) whose actor's race has the keyword. Lighting.hlsl reads it only under SKIN. Per actor,
// so the record's, not the permutation's. The integration registers what DCLF needs to draw it as that hook does:
// - the "beast race" reference fact (Api::ReferenceFact): the actor's race keyword, captured with the node's record (true without an actor
//   or race, or while the feature is unloaded);
// - the IsBeastRace object flag (Api::LightingObjectFlag): per join, a face property's, from the fact and the feature's live state, carried
//   as kObjectBeastRace and the record's kRecordBeastRace.
//
// Included by the integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::SubsurfaceScattering
{
	// Its object flag (Records.h's kFeatureObjectFlags; Features/Manifest.cpp): IsBeastRace for a face (BSLightingShader_SetupSkin: kFace
	// or kFaceGenRGBTint, its actor's race keyword).
	inline constexpr std::uint32_t kObjectBeastRace = 1u << 3;
	// BindlessObject::recordFlags (DCLFObjects.hlsli, DCLFRecordFlags): its IsBeastRace (kObjectBeastRace; Features/Manifest.cpp).
	inline constexpr std::uint32_t kRecordBeastRace = 1u << 0;

	// SubsurfaceScatteringIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
