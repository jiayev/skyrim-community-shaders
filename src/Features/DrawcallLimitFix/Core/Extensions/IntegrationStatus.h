#pragma once

// The integrations' status lines (Api::Feature::status): whether the CS feature an integration supports is loaded and switched on.

#include "Feature.h"
#include "State.h"

#include <string>

namespace DCLF::Integrations
{
	/** @brief Any thread: "loaded", "loaded, disabled in the menu" or "not loaded", for the CS feature a_feature. */
	inline std::string FeatureStatus(::Feature& a_feature)
	{
		if (!a_feature.loaded)
			return "not loaded";
		return globals::state->IsFeatureDisabled(a_feature.GetShortName()) ? "loaded, disabled in the menu" : "loaded";
	}
}
