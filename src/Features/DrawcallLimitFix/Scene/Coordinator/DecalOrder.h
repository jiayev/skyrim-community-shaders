#pragma once

// The decal draw order's key (DecalOrder.cpp's, and the decal order parity's: Diagnostics/Scene).

#include <algorithm>
#include <cstdint>
#include <vector>

namespace DCLF::Scene
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
}
