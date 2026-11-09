#pragma once

// The CS features DCLF supports (Extensions::InstallFeatures, from the shell, after the features), in the order they install and run.

#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <span>
#include <string>
#include <vector>

namespace DCLF::Integrations
{
	/** @brief Every integration, in order (any thread; built on first call). */
	std::span<const Api::Feature> List();

	/** @brief Any thread (menu or render). One line per integration, "name: status" ("installed" when it has no status of its own). */
	std::vector<std::string> StatusLines();
}
