#pragma once

// Skylighting (Features/Skylighting.h): its probe array (t50, Skylighting.hlsli's SkylightingProbeArray) and shadow visibility (t53,
// ShadowVisibilityProbeArray), which CS binds in its prepass (Skylighting::Prepass) and every colour draw reads. The integration registers
// them frame textures (Api::FrameTexture, with capturedWhenNull), so the views with frame constants of their own (the reflection's faces),
// drawn before the main pass captures its frame, read them too. (Its occlusion map is Features/OcclusionMaps'.) Included by the
// integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::Skylighting
{
	// t50 and t53: the probe array and the shadow visibility, frame textures (Features/Manifest.cpp).
	inline constexpr std::uint32_t kProbeArrayRegister = 50;
	inline constexpr std::uint32_t kShadowVisibilityRegister = 53;

	// SkylightingIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
