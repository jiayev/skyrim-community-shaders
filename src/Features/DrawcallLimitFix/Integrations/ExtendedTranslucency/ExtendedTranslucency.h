#pragma once

// Extended Translucency (Features/ExtendedTranslucency.h): while it is loaded, its BSLightingShader_SetupGeometry hook sets the
// permutation's ExtraFeatureDescriptor to a blended geometry's material model (MaterialModelOf: the geometry's AnisotropicAlphaMaterial
// extra data, else the default when SkinnedOnly allows it). The integration registers what DCLF needs to draw it as that hook does:
// - the geometry extra its model reads (Api::InstallContext::DeclareGeometryExtra: "AnisotropicAlphaMaterial"), captured with the
//   geometry's record;
// - the Lighting key's spare bits (Api::LightingKeyBits): per join, the model XOR DescriptorDisabled (non-zero only for blended geometry,
//   e.g. NPC hairlines), so each model has a pipeline of its own; per new key, the permutation's ExtraFeatureDescriptor from them.
//
// Included by the integration's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Integrations::ExtendedTranslucency
{
	// ExtendedTranslucencyIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
