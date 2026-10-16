#pragma once

// Advanced Skin (Features/Skin.h): its SkinPerGeometry (PS b7, Skin::GetWetness: the owning actor's sweat, wetness, height, water depth)
// as a shading field (Api::ShadingRowField::SkinPerGeometry) of Features/Actors' published actor-owned class: the actors' membership kept
// from the publications' writes (OwnerValues), each actor's value captured once a frame at the frame's start and fanned out to its meshes
// whose value changed or that joined; zero for every other slot (Core). Its parities are in Diagnostics/ (WetnessParity.h).
// Its material textures (t71, t74) are still the Lighting port's (MaterialPortFeature's SkinTextures).
// Included by the integration's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Integrations::AdvancedSkin
{
	// AdvancedSkinIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
