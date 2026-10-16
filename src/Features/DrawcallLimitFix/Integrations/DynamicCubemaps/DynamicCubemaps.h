#pragma once

// Dynamic Cubemaps (Features/DynamicCubemaps.h): its reflections and environment cubemaps (t30, t31, DynamicCubemaps.hlsli's
// EnvReflectionsTexture and EnvTexture), which CS binds after the deferred pass (DynamicCubemaps::PostDeferred), so every colour draw of the
// next frame reads them. The integration registers them frame textures (Api::FrameTexture, with capturedWhenNull), so the views with frame
// constants of their own (the reflection's faces), drawn before the main pass captures its frame, read them too. t30's view is the one
// PostDeferred bound (DynamicCubemaps::boundReflectionsView): it picks it by a reflection state Reset overwrites at every Present.
// Included by the integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::DynamicCubemaps
{
	// t31: the environment cubemap, a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kReflectionsRegister = 30;
	inline constexpr std::uint32_t kEnvironmentRegister = 31;

	// DynamicCubemapsIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
