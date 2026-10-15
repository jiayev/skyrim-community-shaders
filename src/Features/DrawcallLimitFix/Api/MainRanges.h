#pragma once

// The main range contract (Api::InstallContext::RegisterMainRange): a feature's ordered range of the colour segment's second pass, drawn
// after every opaque draw at Core's fixed slot for it in the main sequence buffer. The slots, their count words and the object flag bits
// that select them are BuildDrawsCS's compile-time layout (GpuLayouts.h: MainRangeCountWord; Features/Manifest.cpp checks a feature's
// slots against it); what a registration names is which slot is drawn, where in the pass, under which nvperf range, and whether the slot
// also draws a depth prepass before the main colour draws.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief Core's fixed slot per range (GpuLayouts.h). A fixed slot's objects take an ordinal each (SceneTables::rangeOrdinal, written
	 * by the slot's feature, with SceneTables::rangeCount its slots): the colour epoch writes each one's draw, or a zero-count one, to its
	 * ordinal, so the range keeps the feature's order. An appended slot's draws are appended by the culling (no order within it; it reuses
	 * phase 2's range once the depth segment is done with it).
	 */
	enum class MainRangeSlot : std::uint8_t
	{
		Fixed0,     // the objects of decal group 1 (kObjectDecal, group bits 1)
		Fixed1,     // decal group 2
		Fixed2,     // decal group 3
		Appended0,  // terrain group 1 (no kObjectDecal, group bits 1)
		Appended1,  // terrain group 2
		Count,
	};
	inline constexpr std::size_t kFixedMainRanges = static_cast<std::size_t>(MainRangeSlot::Appended0);

	/**
	 * @brief A main range: drawn in the second pass by ascending order (every registration's order its own), each from its slot's range
	 * and count word, under the nvperf range "cs.dclf.colour.<name>" (nested in "cs.dclf.colour.decals"). Read as data per frame (no call).
	 *
	 * - slot: the slot, one registration's.
	 * - name: its nvperf range's suffix.
	 * - order: its place in the second pass.
	 * - depthPrepass: a fixed slot only: its draws also go first, depth only (the depth variants), in a pass of their own before the main
	 *   colour draws ("cs.dclf.colour.decal-depth"), so a host fragment under an opaque texel fails the colour pass's EQUAL test.
	 * - objectClass: the class whose objects fill it (registered before); Count for Core's built-ins (Extensions::CoreMainRanges).
	 */
	struct MainRangeContribution
	{
		MainRangeSlot slot = MainRangeSlot::Count;
		std::string_view name;
		std::uint8_t order = 0;
		bool depthPrepass = false;
		ObjectClass objectClass = ObjectClass::Count;
	};
}
