#pragma once

// Effects (CS_DCLF_EFFECTS, CS_DCLF_OPAQUE_EFFECTS; drawcall-limit-fix.md, "The Effect shader in the forward epoch", "Opaque effects"): the
// BSEffectShaderProperty objects DCLF draws through the Effect port - blended meshes in the forward epoch's sort, decals in the decal
// groups, skinned ones through the skin palette, and the opaque ones (hint 0) through the opaque decal group without its bias. The feature
// registers its switches and live toggle, the effect and opaque effect object classes' contributions (Api::ObjectClass::Effect: the
// eligibility, the primary cull's membership passes, the scene capture's and the effect controllers' material captures and the census read
// the toggle through it, as data; Api::ObjectClass::OpaqueEffect, taken while CS_DCLF_OPAQUE_EFFECTS is not 0: the Effect port's opaque
// rule reads whether it is taken), its report lines among the forward epoch's (Diagnostics/EffectsReport.cpp) and the parity of the members'
// Lighting-variant passes (Diagnostics/EffectLight.cpp).
//
// Core keeps the Effect port itself, which Core classifies every effect-shader item by before a feature sees it (the particle systems and
// the billboards' effect shapes too): its descriptors and variants (DeriveEffectDescriptors, Ineligible::EffectOpaque and EffectVariant,
// EffectConstants.h's EffectFlag), its material records and constants (MaterialPortEffect, kMaterialKindEffect, the effect frame slots
// and SampleEffectLight), the effect keys (Records.h's kRasterEffect) and their programs and pipelines (ShaderPrograms' FindEffect,
// DrawPipelines' effect pipelines), and the builds and passes that draw them. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <utility>

namespace DCLF::Features::Effects
{
	// EffectsFeature.cpp: the feature's entry (Features/FeatureList.cpp), and its toggle (the report's).
	Api::Feature Feature();
	Api::ToggleId Toggle();

	// Diagnostics/EffectLight.cpp: CS_DCLF_SET_PARITY's observer of the members' blended passes the main camera's registration withholds
	// (PassCapture's hooks, on whatever thread the engine registers): <- EFFECT LIGHT, of those the Effect shader's Lighting variant's
	// (checked), the ones whose light 0 is not the scene's sun light (other): the port draws them with the sun light's DLightColor
	// (LightingConstants.h, SampleEffectLight), which holds while FUN_1414fcf80 gives them ShadowSceneNode +0x200.
	void SubscribeLight();
	/** @brief The Effect Lighting passes checked and those with another light 0, since the last call. Render thread, once a report. */
	std::pair<std::uint32_t, std::uint32_t> TakeLight();

	// Diagnostics/EffectsReport.cpp: the report lines (among the forward epoch's).
	void RegisterReport(Api::InstallContext& a_context);
}
