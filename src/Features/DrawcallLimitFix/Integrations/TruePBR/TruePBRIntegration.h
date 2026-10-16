#pragma once

// TruePBR (src/TruePBR.h): while it is loaded its hooks on the Lighting shader replace SetupMaterial for the PBR passes and change the
// passes themselves. The integration registers what DCLF needs to draw them as it does:
// - its material contributor (Api::MaterialContributor, Replace, order 0; TruePBRMaterial.cpp): BSLightingShader_SetupMaterial for the
//   passes its hook takes (PBRTakesPass: TruePbr, not LODLand or LODLandNoise), handing back to vanilla what it does not draw
//   (CHECK_PBR_TEXTURE); its own classes captured as Lighting's (BSLightingShaderMaterialPBR, ...PBRLandscape: a vtable in this DLL's
//   image); its landscape displacement and RMAOS layers at t80-t91 (extendedRendererState); the TruePbr bit in the material signature;
// - its property bits ("glints": a PBR material's glintParameters.enabled, which its GetRenderPasses hook reads);
// - its Lighting pass rules: GetRenderPasses' swap (PBR swaps Specular for TruePbr; glint adds AnisoLighting), GetPixelTechnique's split
//   (keeps bits 3-5) and SetupGeometry's raw technique (a TruePbr pass gains AmbientSpecular and flips AnisoLighting);
// - its parity (Diagnostics/TextureParity.h): CaptureParity's t80-t91 check.
//
// The rules apply whether or not TruePBR is loaded, as its hooks' ports always did; the contributor is active only while it is loaded
// (sampled at the frame's start). Its writes to materials reach DCLF through the shell (Api::NoteMaterialWritten). Core keeps the vanilla
// SetupMaterial replica it hands back to, the hook frame's engine globals and MaterialRecord's frozen feature texture slots. Included by
// the integration's sources, Diagnostics/Scene/CaptureParity and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF::Integrations::TruePBR
{
	// t80-t91: its landscape displacement (t80-t85) and RMAOS (t86-t91) layers, TruePBR::FirstExtendedTexture on (Lighting.hlsl's), the
	// contributor's textureRegisters (static storage; Features/Manifest.cpp checks them, with Advanced Skin's, against DCLFObjects.hlsli's
	// decode).
	inline constexpr std::array<std::uint32_t, 12> kTextureRegisters{ 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91 };
	// Its contributor bit in the Lighting property record (Api::PropertyBits): the material's glint is enabled.
	inline constexpr std::uint8_t kGlints = 1u << 0;

	// TruePBRMaterial.cpp: the material contributor's callbacks (registered by TruePBRIntegration.cpp).
	/** @brief Whether a vtable is this DLL's: CS's own material classes (BSLightingShaderMaterialPBR, ...PBRLandscape). */
	bool OwnClass(const void* a_vtable);
	/** @brief Bytes Capture takes of CS's PBR classes (sizeof BSLightingShaderMaterialPBR, or ...PBRLandscape for feature 19). */
	std::uint32_t PBRMaterialBytes(const RE::BSShaderMaterial& a_material);
	/** @brief The PBR classes' texture fields (landscape: 4 per tile, the overlay and noise; PBR: the base four and five maps). */
	void PBRTextureFields(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count);
	/** @brief Render thread, the frame's start: active while TruePBR is loaded (its hooks are installed only then). */
	void SampleFrame(Api::MaterialContributorFrame& a_frame);
	/** @brief Whether TruePBR's hook takes the pass (TruePbr set and the technique +0x94 holds is not LODLand or LODLandNoise). */
	bool PBRTakesPass(const Api::MaterialContributorEvaluation& a_evaluation);
	/** @brief TruePBR::BSLightingShader_SetupMaterial for a pass it takes: Made, Uncovered, or HandBack (CHECK_PBR_TEXTURE). */
	Api::MaterialOutcome EvaluatePBR(Api::MaterialContributorEvaluation& a_evaluation);

	// TruePBRIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
