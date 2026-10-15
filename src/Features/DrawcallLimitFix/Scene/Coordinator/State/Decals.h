#pragma once

// The member decals (CoordinatorDecals): each member decal's key, the changes since the last order, the ordered set and the
// observers' comparison. DecalOrder.cpp.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorDecals
	{
		// Member decals (DecalOrder.cpp): object -> its chain, group, technique and sub-pass; joined and left with the membership.
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> memberDecals;
		// Decals changed since the last OrderDecals, and whether to rebuild the order whole.
		std::vector<std::uint32_t> decalsChanged;
		bool decalsRebuild = true;
		/** @brief The kept decal order against one rebuilt whole (keys retaken): the observers' reference (DecalsOrdered). */
		struct DecalOrderComparison
		{
			std::uint64_t decals = 0, staleKeys = 0, unordered = 0;
			bool differ = false;
			std::string first, staleFirst;  // the first difference, the first stale key
		};
		// The decals the last OrderDecals ordered, whose ordinals the next one resets.
		std::vector<std::uint32_t> decalOrdered;
	};
}
