#pragma once

// The CS features DCLF supports (Extensions::InstallFeatures, from the shell, after the features), in the order they install and run.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <span>

namespace DCLF::Integrations
{
	/** @brief Every integration, in order (any thread; built on first call). */
	std::span<const Api::Feature> List();
}
