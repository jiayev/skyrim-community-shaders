#pragma once

// Volumetric Shadows (Features/VolumetricShadows.h): its downsampled sun shadow map (t18, VolumetricShadows.hlsli's SharedShadowMap),
// which CS binds at the engine's shadow mask draw (State::Draw's RenderShadowmask Utility pass, CopyShadowLightData) and every colour draw
// reads. The integration registers it a frame texture (Api::FrameTexture, with capturedWhenNull), so the views with frame constants of
// their own (the reflection's faces), drawn before the main pass captures its frame, read it too. Included by the integration's sources
// and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::VolumetricShadows
{
	// t18: the shared shadow map (VolumetricShadows::kSharedShadowMapShaderSlot), a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kSharedShadowMapRegister = 18;

	// VolumetricShadowsIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
