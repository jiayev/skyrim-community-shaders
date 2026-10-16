#pragma once

// Water Effects (Features/WaterEffects.h): its caustics texture (t65, WaterCaustics.hlsli's WaterCaustics), which CS binds in its prepass
// (WaterEffects::Prepass) and every colour draw reads. The integration registers it a frame texture (Api::FrameTexture, with
// capturedWhenNull), so the views with frame constants of their own (the reflection's faces), drawn before the main pass captures its frame,
// read it too. Included by the integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::WaterEffects
{
	// t65: the caustics texture, a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kCausticsRegister = 65;

	// WaterEffectsIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
