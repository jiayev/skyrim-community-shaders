#pragma once

// The decal draw order (DecalOrder.cpp, the feature's coordinator component; Diagnostics/DecalOrderParity.cpp, its parity): the order's
// key, the component's state and the observers' comparison. Included by the feature's sources only.

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Api
{
	class CoordinatorComponentContext;
}

namespace DCLF::Features::Decals
{
	/**
	 * @brief Where the main registration reaches a geometry (skyrim-engine-notes.md, "The main registration's order"):
	 * its scene list, whether a BSOrderedNode groups it (registered after the list's plain appends), its root's
	 * position in the list, and the positions the cull visits on the way down.
	 */
	struct OrderKey
	{
		std::uint32_t list = 0;
		bool ordered = false;
		std::uint32_t position = 0;
		std::vector<std::uint32_t> path;
	};

	inline bool Before(const OrderKey& a_a, const OrderKey& a_b)
	{
		if (a_a.list != a_b.list)
			return a_a.list < a_b.list;
		if (a_a.ordered != a_b.ordered)
			return !a_a.ordered;
		if (a_a.position != a_b.position)
			return a_a.position < a_b.position;
		return std::lexicographical_compare(a_a.path.begin(), a_a.path.end(), a_b.path.begin(), a_b.path.end());
	}

	struct OrderedDecal
	{
		std::uint64_t chain;
		OrderKey key;
		std::uint32_t object;
		// The BGSDecalNode on its chain, if any: its decals' places count from the end of the node's decal array
		// (VisitIndex), which an added or removed decal shifts for all of them, so they are keyed again together.
		const RE::NiAVObject* decalNode = nullptr;
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY: the kept order against one made whole, summed until the report's order parity line takes it. */
	struct DecalOrderParity
	{
		std::uint64_t checks = 0, decals = 0, differ = 0, staleKeys = 0, unordered = 0;
		std::string first, staleFirst;
	};

	/**
	 * @brief The member decals (the component's state, scene lane): each member decal's key, the changes since the last order, the kept
	 * order and the ordered set; with the walk's candidates and the parity's sums, for the report.
	 */
	struct DecalOrder
	{
		// Decal candidates this walk, by group (the patch's chain >> 60, less one): the accumulate phase's patches since the walk began.
		std::array<std::uint32_t, 3> candidates{};
		DecalOrderParity parity;
		// Member decals: object -> its chain, group, technique and sub-pass; joined and left with the membership.
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> memberDecals;
		// Decals changed since the last OrderDecals, and whether to rebuild the order whole.
		std::vector<std::uint32_t> decalsChanged;
		bool decalsRebuild = true;
		// The member decals in draw order with their keys, kept so a change re-keys only the decals it names.
		std::vector<OrderedDecal> kept;
		// The decals the last OrderDecals ordered, whose ordinals the next one resets.
		std::vector<std::uint32_t> decalOrdered;
	};

	/** @brief The kept decal order against one rebuilt whole (keys retaken): the observers' reference (Ordered). */
	struct DecalOrderComparison
	{
		std::uint64_t decals = 0, staleKeys = 0, unordered = 0;
		bool differ = false;
		std::string first, staleFirst;  // the first difference, the first stale key
	};

	/** @brief The kept order against one made whole now (a key that went stale: a parent's children re-indexed under a kept decal). */
	DecalOrderComparison CompareDecalOrder(const DecalOrder& a_order, const Api::CoordinatorComponentContext& a_context);

	// The order's hook points (Diagnostics::Notify; the scene lane, in the accumulate phase's statistics), for the parity.

	/** @brief The order's upkeep: whether an observer checks the order this pass (`checked`), which runs the upkeep even with no change. */
	struct OrderDue
	{
		std::uint64_t passSerial;
		bool& checked;
	};

	/** @brief The kept order brought up to date, on a pass an observer checks it (OrderDue): the parity sums into `parity`. */
	struct Ordered
	{
		const DecalOrder& order;
		const Api::CoordinatorComponentContext& context;  // the tables and the mirror, as the order read them
		DecalOrderParity& parity;
	};
}
