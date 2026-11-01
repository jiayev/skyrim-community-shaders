#pragma once

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Integrations::PhysicalSky
{
	inline constexpr std::uint32_t kTransmittance = 61, kCloudShadowMean = 69, kCloudShadowVolume = 112;
	Api::Feature Integration();
}
