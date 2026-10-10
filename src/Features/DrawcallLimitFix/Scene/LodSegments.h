#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"

namespace DCLF::LodSegments
{
	/**
	 * @brief Object LOD's segments (AE 1.6.1170; dclf-lod.md, "Object LOD: segments").
	 *
	 * A BSSubIndexTriShape (geometry type 8) of a LOD block has one segment per cell of its terrain node. While the node is in
	 * the active grid, the terrain manager hides the segments of the cells that are attached (FUN_1405110b0, and
	 * FUN_1405112f0 for the large-reference grid): FUN_140e31160 clears a segment's enabled byte, FUN_140e31130 sets it
	 * again (if it has triangles), both setting the shape's dirty byte. FUN_140e310b0 shows every segment as one run over the
	 * whole shape and clears the dirty byte. The draw (FUN_1414f2ad0, type 8) rebuilds the runs when dirty (FUN_140e31180:
	 * consecutive enabled segments merged, each run's start flagged with its triangle count) and draws each run:
	 * DrawIndexed(runTriangles * 3, firstIndex, 0). A non-segmented shape draws whole.
	 */
	struct Segment
	{
		std::uint32_t firstIndex;    // 00: its first index
		std::uint32_t triangles;     // 04: its own triangles
		std::uint8_t enabled;        // 08: shown (FUN_140e31130 / FUN_140e31160)
		std::uint8_t pad09[3];       // 09
		std::uint32_t runTriangles;  // 0C: a run's triangles, from its start (FUN_140e31180)
		std::uint8_t runStart;       // 10: a run starts here
		std::uint8_t pad11[3];       // 11
	};
	static_assert(sizeof(Segment) == 0x14);

	inline constexpr std::size_t kTriangleCount = 0x158;  // BSTriShape's triangle count (uint16)
	inline constexpr std::size_t kSegments = 0x160;       // Segment*
	inline constexpr std::size_t kSegmentCount = 0x168;   // uint32
	inline constexpr std::size_t kDirty = 0x170;          // the runs need rebuilding
	inline constexpr std::size_t kNonSegmented = 0x171;   // drawn whole

	struct Range
	{
		std::uint32_t firstIndex = 0;
		std::uint32_t indexCount = 0;
		// The range's geometry slot key (SceneStore::ResolveLodRanges): its first segment's record, unique while the shape lives
		// and never a TriShape's address; null for a non-segmented shape's whole range.
		const void* key = nullptr;
		// The triangles alone: a run's start moves to an empty segment before it when the engine rebuilds the runs (FUN_140e31180),
		// so the same range can come with another key.
		bool operator==(const Range& a_other) const { return firstIndex == a_other.firstIndex && indexCount == a_other.indexCount; }
	};

	template <class T>
	const T& At(const void* a_object, std::size_t a_offset)
	{
		return *reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(a_object) + a_offset);
	}

	/**
	 * @brief The index ranges the engine's draw of a_shape draws now, contiguous ones merged: the runs, or (dirty) the enabled
	 * segments the rebuild will merge into runs, which cover the same triangles.
	 */
	inline void DrawnRanges(const void* a_shape, std::vector<Range>& a_out)
	{
		EngineReadWindow::Touch("LodSegments::DrawnRanges");
		a_out.clear();
		auto add = [&](std::uint32_t a_first, std::uint32_t a_triangles, const void* a_key) {
			if (!a_triangles)
				return;
			const std::uint32_t count = a_triangles * 3;
			if (!a_out.empty() && a_out.back().firstIndex + a_out.back().indexCount == a_first)
				a_out.back().indexCount += count;
			else
				a_out.push_back({ a_first, count, a_key });
		};
		if (At<std::uint8_t>(a_shape, kNonSegmented)) {
			add(0, At<std::uint16_t>(a_shape, kTriangleCount), nullptr);
			return;
		}
		const auto* segments = At<const Segment*>(a_shape, kSegments);
		if (!segments)
			return;
		const std::uint32_t count = At<std::uint32_t>(a_shape, kSegmentCount);
		const bool dirty = At<std::uint8_t>(a_shape, kDirty) != 0;
		for (std::uint32_t i = 0; i < count; ++i) {
			const auto& segment = segments[i];
			if (dirty) {
				if (segment.enabled)
					add(segment.firstIndex, segment.triangles, &segment);
			} else if (segment.runStart) {
				add(segment.firstIndex, segment.runTriangles, &segment);
			}
		}
	}

	/** @brief Whether a_ranges are the shape's whole index list in one range: drawn as the TriShape's own geometry slot. */
	inline bool Whole(std::uint32_t a_triangles, const std::vector<Range>& a_ranges)
	{
		return a_ranges.size() == 1 && a_ranges[0].firstIndex == 0 && a_ranges[0].indexCount == a_triangles * 3u;
	}
	inline bool Whole(const void* a_shape, const std::vector<Range>& a_ranges)
	{
		return Whole(At<std::uint16_t>(a_shape, kTriangleCount), a_ranges);
	}
}
