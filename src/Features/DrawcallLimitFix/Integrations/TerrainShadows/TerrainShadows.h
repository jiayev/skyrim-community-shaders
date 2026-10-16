#pragma once

// Terrain Shadows (Features/TerrainShadows.h): its shadow height map (t60, TerrainShadows.hlsli's ShadowHeightTexture), which CS binds in its
// early prepass (TerrainShadows::EarlyPrepass, after the shadow maps) and every colour draw reads. The integration registers it a frame
// texture (Api::FrameTexture, with capturedWhenNull), so the views with frame constants of their own (the reflection's faces), drawn before
// the main pass captures its frame, read it too. Included by the integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::TerrainShadows
{
	// t60: the shadow height map, a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kShadowHeightRegister = 60;

	// TerrainShadowsIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
