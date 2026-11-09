#pragma once

// Advanced Skin's actor wetness and per-mesh material snapshots feed the shading rows at frame start.
// Profiles, editor previews and the three texture bindings are independent even for meshes sharing
// an engine material. Diagnostics compare those snapshots against native b7/t71/t74/t72 bindings.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct ID3D11ShaderResourceView;

namespace DCLF::Integrations::AdvancedSkin
{
	// Native texture registers in the profile texture order: controls, wetness, detail normal.
	inline constexpr std::uint32_t kRfaosRegister = 71;
	inline constexpr std::uint32_t kWetnessRegister = 74;
	inline constexpr std::array<std::uint32_t, 3> kMaterialTextureRegisters{ kRfaosRegister, kWetnessRegister, 72 };
	/** @brief The views captured for a shading row, for native/DCLF parity. Render thread. */
	std::array<ID3D11ShaderResourceView*, 3> CapturedTextures(std::uint32_t a_slot);
	// SkinPerGeometry (PS b7, DCLFObjects.hlsli's): bound per draw by its SetupGeometry hook, for the actor the engine drew last.
	inline constexpr std::uint32_t kSkinPerGeometryRegister = 7;

	// SkinTextures.cpp: the per-mesh shading field and native per-draw buffer.
	void InstallMaterialTextures(Api::InstallContext& a_context);

	// AdvancedSkinIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();

	/**
	 * @brief Render thread (the diagnostic capture): the wetness index's sizes and the last capture's counts, one "<name> <value>" line
	 * each. AdvancedSkinIntegration.cpp.
	 */
	std::vector<std::string> GrowthLines();
}
