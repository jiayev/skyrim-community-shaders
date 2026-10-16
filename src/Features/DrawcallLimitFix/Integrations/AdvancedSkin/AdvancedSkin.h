#pragma once

// Advanced Skin (Features/Skin.h): its SkinPerGeometry (PS b7, Skin::GetWetness: the owning actor's sweat, wetness, height, water depth)
// as a shading field (Api::ShadingRowField::SkinPerGeometry) of Features/Actors' published actor-owned class: the actors' membership kept
// from the publications' writes (OwnerValues), each actor's value captured once a frame at the frame's start and fanned out to its meshes
// whose value changed or that joined; zero for every other slot (Core). Its parities are in Diagnostics/ (WetnessParity.h).
// Its material textures (t71 RFAOS, t74 wetness; Skin::MaterialTexturesOf, outermost on SetupMaterial) are an After material contributor
// of the Lighting port (SkinTextures.cpp: Skin::skinExtraTextures sampled at the frame's start, held for its evaluations), declared with
// SkinPerGeometry's per-draw buffer; their parity is in Diagnostics/TextureParity.h.
// Included by the integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>

namespace DCLF::Integrations::AdvancedSkin
{
	// Lighting.hlsl's t71 (RFAOS) and t74 (wetness), its material contributor's registers in that order (strictly ascending; its local
	// indices 0 and 1). Core assigns their feature texture slots (DCLFObjects.hlsli's decode, Features/Manifest.cpp).
	inline constexpr std::uint32_t kRfaosRegister = 71;
	inline constexpr std::uint32_t kWetnessRegister = 74;
	inline constexpr std::array<std::uint32_t, 2> kMaterialTextureRegisters{ kRfaosRegister, kWetnessRegister };
	// SkinPerGeometry (PS b7, DCLFObjects.hlsli's): bound per draw by its SetupGeometry hook, for the actor the engine drew last.
	inline constexpr std::uint32_t kSkinPerGeometryRegister = 7;

	// SkinTextures.cpp: the material contributor ("Advanced Skin", After) and the per-draw buffer, from the integration's install.
	void InstallMaterialTextures(Api::InstallContext& a_context);

	// AdvancedSkinIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
