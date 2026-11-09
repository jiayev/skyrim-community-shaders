// The late pose contributions (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/LatePoses.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveLatePoses (InstallFeatures, before DCLF's threads exist), read-only after.
		std::vector<LatePoseEntry> resolved;
	}

	void ResolveLatePoses(std::span<const Api::LatePoseContribution> a_registered)
	{
		resolved.clear();
		for (const auto& contribution : a_registered)
			resolved.push_back({ std::string(contribution.name), contribution.root });
	}

	std::span<const LatePoseEntry> LatePoses() { return resolved; }
}
