#pragma once

// Image Based Lighting (Features/IBL.h): its environment and sky SH (t76, t77, IBL.hlsli's EnvIBLTexture and SkyIBLTexture), which CS
// binds in its prepass (IBL::Prepass) and in the water reflection's (IBL::ReflectionsPrepass), and its static diffuse and specular cubemaps
// (t78, t79, StaticDiffuseIBLTexture and StaticSpecularIBLTexture), which only the reflection's prepass binds; every colour draw after them
// reads them. The integration registers the four frame textures (Api::FrameTexture, with capturedWhenNull), so the views with frame
// constants of their own (the reflection's faces), drawn before the main pass captures its frame, read them too. Included by the
// integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::IBL
{
	// t76-t79: the environment SH, the sky SH, the static diffuse and specular cubemaps, frame textures (Features/Manifest.cpp).
	inline constexpr std::uint32_t kEnvironmentRegister = 76;
	inline constexpr std::uint32_t kSkyRegister = 77;
	inline constexpr std::uint32_t kStaticDiffuseRegister = 78;
	inline constexpr std::uint32_t kStaticSpecularRegister = 79;

	// IBLIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
