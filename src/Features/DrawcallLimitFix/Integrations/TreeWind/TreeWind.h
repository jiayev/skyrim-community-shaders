#pragma once

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Integrations::TreeWind
{
	/** @brief Registers per-mesh tree LOD bend data for GPU-driven draws. */
	Api::Feature Integration();
}
