#pragma once

// Linear Lighting (Features/LinearLighting.h): a native Lighting draw reads its LLPerGeometry (PS b8, Color.hlsli's emissiveMult), which
// the engine's SetupGeometry binds per draw. The integration declares it a per-draw buffer (Api::PerDrawBuffer), so the main pass's frame
// blocks and the capture checks leave b8 out (Extensions::PerDrawMask). Declaration-only: nothing is called.
//
// Core keeps the value DCLF's draws read: the property's emissiveMult, sampled with emitColor into the shading row's EmissiveMult
// (LightingConstants). Included by the integration's sources and Features/Manifest.cpp.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::LinearLighting
{
	// PS b8: LLPerGeometry, Linear Lighting's per-object emissive multiplier (Color.hlsli's; Features/Manifest.cpp).
	inline constexpr std::uint32_t kLLPerGeometryRegister = 8;

	// LinearLightingIntegration.cpp: the integration's entry (Integrations/IntegrationList.cpp).
	Api::Feature Integration();
}
