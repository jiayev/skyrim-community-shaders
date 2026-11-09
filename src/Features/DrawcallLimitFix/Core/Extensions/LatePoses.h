#pragma once

// The late pose contract's Core side (Api/LatePoses.h): the contributions, resolved once at InstallFeatures.

#include "Features/DrawcallLimitFix/Api/LatePoses.h"

#include <span>
#include <string>
#include <vector>

namespace DCLF::Extensions
{
	/** @brief A registered late pose (its name owned). */
	struct LatePoseEntry
	{
		std::string name;
		decltype(Api::LatePoseContribution::root) root = nullptr;
	};

	/** @brief Main thread, the end of InstallFeatures: the entries from the registrations. Once. */
	void ResolveLatePoses(std::span<const Api::LatePoseContribution> a_registered);
	/** @brief Any thread, after InstallFeatures (empty before): the entries, in registration order. */
	std::span<const LatePoseEntry> LatePoses();
}
