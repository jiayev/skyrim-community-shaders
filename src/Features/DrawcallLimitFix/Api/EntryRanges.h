#pragma once

// The entry range contract (Api::GeometryTypeContribution::ranges): the index ranges an entry of a ranged geometry type draws instead of
// its whole index list (object LOD's visible segments' runs: Features/Lod). The capture takes them into the mirror's record, the
// coordinator keeps them by tracked entry (Api::CoordinatorComponentContext::EntryRanges), and the walk reads them as data: whole, the
// entry's own geometry slot; none, nothing drawn; else a chain of range slots.

#include <cstdint>
#include <vector>

namespace DCLF::Api
{
	/** @brief One drawn index range of an entry: DrawIndexed(indexCount, firstIndex, 0) over the entry's renderer data. */
	struct EntryRange
	{
		std::uint32_t firstIndex = 0;
		std::uint32_t indexCount = 0;
		// The range's geometry slot key (Coordinator::ResolveEntryRangeSlots): its first segment's record, unique while the shape lives
		// and never a TriShape's address; null for a non-segmented shape's whole range.
		const void* key = nullptr;
		// The triangles alone: a run's start moves to an empty segment before it when the engine rebuilds the runs (FUN_140e31180),
		// so the same range can come with another key.
		bool operator==(const EntryRange& a_other) const { return firstIndex == a_other.firstIndex && indexCount == a_other.indexCount; }
	};

	/** @brief Whether a_ranges are the entry's whole index list in one range: drawn as the TriShape's own geometry slot. */
	inline bool EntryRangesWhole(std::uint32_t a_triangles, const std::vector<EntryRange>& a_ranges)
	{
		return a_ranges.size() == 1 && a_ranges[0].firstIndex == 0 && a_ranges[0].indexCount == a_triangles * 3u;
	}
}
