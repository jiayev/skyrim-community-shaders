#pragma once

// Improved Camera (an SKSE plugin): DCLF's integration (ImprovedCameraIntegration.cpp).

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Integrations::ImprovedCamera
{
	/** @brief The integration: the player's first-person pose, a late pose. */
	Api::Feature Integration();
}
