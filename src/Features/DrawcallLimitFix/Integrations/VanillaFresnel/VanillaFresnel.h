#pragma once

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>

namespace DCLF::Integrations::VanillaFresnel
{
	inline constexpr std::uint32_t kObjectEye = 1u << 16;
	inline constexpr std::uint32_t kRecordEye = 1u << 9;

	Api::Feature Integration();
}
