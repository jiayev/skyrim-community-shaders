#pragma once

// The dependents lists (CoordinatorDependents): the entries under each property, root and category node, their positions
// (DependentsAt), and the root owners (references to every listed root). Listed by the walk (Walk.cpp) and the attaches
// (Classification.cpp), drained by the events (ApplyEvents.cpp) and the placements (Placements.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorDependents
	{
		// Sun entry nodes something was attached under or detached from since the last walk (keys).
		std::vector<const RE::NiAVObject*> dirtyRoots;
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> propertyDependents;
		/**
		 * @brief A dependents list's positions ((key, geometry) to its index in the key's list), for List/Unlist (Internal.h): a removal
		 * swaps the last into its place, no search. One per list whose every insertion goes through List.
		 */
		using DependentsAt = ankerl::unordered_dense::map<std::pair<const void*, const RE::BSGeometry*>, std::uint32_t>;
		DependentsAt propertyDependentsAt;
		// rootDependents', lightDependents' and hiddenDependents': used once their insertions (Classification.cpp, ApplyEvents.cpp) go
		// through List (requests-w1.md R1, R2); until then those lists' Unlist searches.
		DependentsAt rootDependentsAt, lightDependentsAt, hiddenDependentsAt;
		/**
		 * @brief The tracked entries by category node (Tracked::categoryNode, set by AddGeometry alone; off at EraseTracked): a cell that
		 * goes away, or a category node's hidden event, takes its members, not a walk over every tracked entry.
		 */
		ankerl::unordered_dense::map<const RE::NiNode*, std::vector<RE::BSGeometry*>> categoryMembers;
		DependentsAt categoryMembersAt;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> rootDependents;
		/**
		 * @brief A reference to every root a dependents list names, taken when first listed, handed back when none does. What the scene
		 * work hands out of a root is a copy: no reference is made from a key, which may name a dead node.
		 */
		ankerl::unordered_dense::map<const RE::NiAVObject*, RE::NiPointer<RE::NiAVObject>> rootOwners;
		std::uint64_t unownedRoots = 0;  // since the last report: roots handed out unlisted (a defect)
		// A listed root's reference, and back (once its last dependent leaves, the root is a key that may be gone).
		ankerl::unordered_dense::map<const RE::NiAVObject*, const void*> rootReference;
		ankerl::unordered_dense::map<const void*, const RE::NiAVObject*> referenceRoot;
	};
}
