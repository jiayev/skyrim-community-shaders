#pragma once

// The features DCLF installs (Extensions::InstallFeatures, from the shell), in the order they install and run.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <span>

namespace DCLF::Features
{
	/** @brief Every feature, in order (any thread; built on first call). */
	std::span<const Api::Feature> List();
}
