#pragma once

// Effects (CS_DCLF_EFFECTS, CS_DCLF_OPAQUE_EFFECTS; dclf-architecture.md, "The Effect shader in the forward epoch", "Opaque effects"): the
// BSEffectShaderProperty objects DCLF draws through the Effect port - blended meshes in the forward epoch's sort, decals in the decal
// groups, skinned ones through the skin palette, and the opaque ones (hint 0) through the opaque decal group without its bias. The feature
// registers its switches and live toggle, the effect and opaque effect object classes' contributions (Api::ObjectClass::Effect: the
// eligibility, the primary cull's membership passes, the scene capture's and the effect controllers' material captures and the census read
// the toggle through it, as data; Api::ObjectClass::OpaqueEffect, taken while CS_DCLF_OPAQUE_EFFECTS is not 0: the Effect port's opaque
// rule reads whether it is taken), its report lines among the forward epoch's (Diagnostics/EffectsReport.cpp) and the parity of the members'
// Lighting-variant passes (Diagnostics/EffectLight.cpp).
//
// And the Effect port itself, which Core classifies every effect-shader item by before a feature sees it (the particle systems and the
// billboards' effect shapes too), on Core's contracts: the material port (Api::MaterialPortContribution, EffectMaterial.cpp: its capture
// and shading, EffectProperty.cpp; its descriptors and variants, EffectDescriptors.cpp; its material records), the program family
// (Api::ProgramFamilyContribution, EffectPrograms.cpp: Effect.hlsl's programs and pipelines on Core's Effect slot, its record bits), and
// the frame inputs (EffectFrame.cpp: the camera, light and blood rows, Api::FrameConstantRow; the soft depth, Api::FrameTexture). Its
// constants are EffectConstants.h's. Core keeps what it decides by: the Ineligible verdicts (EffectOpaque, EffectVariant), the effect key
// bit (Records.h's kRasterEffect, the family's slot), the particle item class and the billboard capture. The effect controllers' and the
// effect material's write hooks are the feature's (EffectWrites.cpp), naming each write through the material-write sink
// (Api/MaterialWrites.h). Included by the feature's sources only.

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
	// (EffectFrame.cpp, SampleEffectLight), which holds while FUN_1414fcf80 gives them ShadowSceneNode +0x200.
	void SubscribeLight();
	/** @brief The Effect Lighting passes checked and those with another light 0, since the last call. Render thread, once a report. */
	std::pair<std::uint32_t, std::uint32_t> TakeLight();

	// Diagnostics/EffectsReport.cpp: the report lines (among the forward epoch's).
	void RegisterReport(Api::InstallContext& a_context);

	// EffectMaterial.cpp: the material port's registration (kind 1: kMaterialKindEffect, checked).
	void RegisterPort(Api::InstallContext& a_context);
	// EffectProperty.cpp: the port's capture (in the read window), shading (per shading item) and emissive (per external emittance
	// re-point) callbacks.
	void CaptureEffectProperty(const Api::MaterialPortCapture& a_capture);
	void SampleEffectShading(Api::MaterialPortShading& a_shading);
	const void* EffectEmissive(const RE::BSShaderProperty& a_property);
	// EffectPrograms.cpp: the program family's registration.
	void RegisterPrograms(Api::InstallContext& a_context);
	// EffectFrame.cpp: the frame constant rows' and the frame texture's registrations.
	void RegisterFrame(Api::InstallContext& a_context);
	// EffectWrites.cpp: the effect controllers' and the effect material's write hooks (main thread, the feature's install, whatever the
	// toggle; the controllers capture nothing while it is off).
	void InstallWrites();
}
