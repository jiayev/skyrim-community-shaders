#pragma once

// The main range contract's Core side (Api/MainRanges.h): the second pass's ranges in draw order, resolved once at InstallFeatures
// (ResolveMainRanges) from Core's built-ins (CoreMainRanges) and the features' registrations; a slot nobody registers is never drawn. The
// colour segment's passes (MainOpaque.cpp) walk the table; each slot's arguments, count word and capacity are Core's (GpuLayouts.h).

#include "Features/DrawcallLimitFix/Api/MainRanges.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Extensions
{
	inline constexpr std::size_t kMainRangeSlots = static_cast<std::size_t>(Api::MainRangeSlot::Count);

	/** @brief A range's entry: its slot (as GpuLayouts.h's index), its nvperf range's full name, whether it draws a depth prepass. */
	struct MainRangeEntry
	{
		std::uint32_t slot = 0;
		std::string colourRange;
		bool depthPrepass = false;
		Api::ObjectClass objectClass = Api::ObjectClass::Count;
	};

	/** @brief The second pass's ranges in draw order, and whether any draws a depth prepass. */
	struct MainRangeTable
	{
		std::vector<MainRangeEntry> drawn;
		bool anyDepthPrepass = false;
	};

	/**
	 * @brief Core's own ranges, registered ahead of the features' (Registry's InstallFeatures): Terrain Blending's terrain and the
	 * meshes it redraws, until an integration takes them.
	 */
	std::span<const Api::MainRangeContribution> CoreMainRanges();
	/** @brief Main thread, the end of InstallFeatures: the table from the registrations by slot (slot Count: none). */
	void ResolveMainRanges(const std::array<Api::MainRangeContribution, kMainRangeSlots>& a_registered);
	/** @brief Any thread, after InstallFeatures (empty before): the second pass's ranges. */
	const MainRangeTable& MainRanges();
}
