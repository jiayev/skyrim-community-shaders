// The readers of an entry space's published snapshots and change log (any thread).

#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"

#include <algorithm>

namespace DCLF
{
	void ChangedEntries(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out)
	{
		ChangedEntriesByScan(a_from, a_to, a_out);
	}

	void ChangedEntriesByScan(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out)
	{
		if (a_from == &a_to)
			return;
		const std::uint32_t to = a_to.Capacity(), from = a_from ? a_from->Capacity() : 0u;
		for (std::uint32_t e = 0, end = std::max(to, from); e < end; ++e) {
			const bool inFrom = e < from && a_from->entryNodes[e];
			const bool inTo = e < to && a_to.entryNodes[e];
			if ((inFrom || inTo) && (inFrom != inTo || a_from->entryVersion[e] != a_to.entryVersion[e]))
				a_out.push_back(e);
		}
	}

	bool ChangedEntriesByLog(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out)
	{
		if (a_from == &a_to)
			return true;
		if (!a_from || !a_from->changes || !a_to.changes || !a_to.changes->Reaches(a_from->changes->log, a_from->generation)) {
			ChangedEntriesByScan(a_from, a_to, a_out);
			return false;
		}
		// The runs after a_from's generation, merged; then the indices whose versions differ (an index written and freed again
		// between them is not a change).
		const std::size_t first = a_out.size();
		std::size_t runs = 0;
		for (const auto& run : a_to.changes->runs)
			if (run->generation > a_from->generation) {
				a_out.insert(a_out.end(), run->entries.begin(), run->entries.end());
				++runs;
			}
		if (runs > 1) {
			std::sort(a_out.begin() + std::ptrdiff_t(first), a_out.end());
			a_out.erase(std::unique(a_out.begin() + std::ptrdiff_t(first), a_out.end()), a_out.end());
		}
		const std::uint32_t to = a_to.Capacity(), from = a_from->Capacity();
		a_out.erase(std::remove_if(a_out.begin() + std::ptrdiff_t(first), a_out.end(),
						[&](std::uint32_t e) {
							const bool inFrom = e < from && a_from->entryNodes[e];
							const bool inTo = e < to && a_to.entryNodes[e];
							return !((inFrom || inTo) && (inFrom != inTo || a_from->entryVersion[e] != a_to.entryVersion[e]));
						}),
			a_out.end());
		return true;
	}
}
