#pragma once

// Cloud Shadows (Features/CloudShadows.h): its cloud occlusion cubemap (t25, CloudShadows.hlsli's CloudShadowsTexture), which CS binds in
// the water reflection's prepass (CloudShadows::ReflectionsPrepass, a copy of its last cloud layer) and every colour draw reads after it.
// The integration registers it a frame texture (Api::FrameTexture, with capturedWhenNull), so the views with frame constants of their own
// (the reflection's faces), drawn before the main pass captures its frame, read it too. Included by the integration's sources and
// Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::CloudShadows
{
	// t25: the cloud occlusion cubemap, a frame texture (Features/Manifest.cpp).
	inline constexpr std::uint32_t kCloudOcclusionRegister = 25;

	// CloudShadowsIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
