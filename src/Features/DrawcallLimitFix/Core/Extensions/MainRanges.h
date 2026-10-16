#pragma once

// The main range contract's Core side (Api/MainRanges.h): the second pass's ranges in draw order, resolved once at InstallFeatures
// (ResolveMainRanges) from the registrations (the features' main ranges and held groups); a slot nobody registers is never drawn. The
// colour segment's passes (Features/MainView/MainViewPasses.cpp) walk the table; each slot's arguments, count word and capacity are Core's (GpuLayouts.h). And the
// held groups (HeldGroups), which the Lighting port's derive reads, and the near twin (NearTwin).

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

	/** @brief Main thread, the end of InstallFeatures: the table from the registrations by slot (slot Count: none). */
	void ResolveMainRanges(const std::array<Api::MainRangeContribution, kMainRangeSlots>& a_registered);
	/** @brief Any thread, after InstallFeatures (empty before): the second pass's ranges. */
	const MainRangeTable& MainRanges();

	/** @brief The most held groups there may be: one an appended slot. */
	inline constexpr std::size_t kHeldGroupSlots = kMainRangeSlots - Api::kFixedMainRanges;
	/**
	 * @brief A registered held group (Api::HeldGroupContribution, its range in the main ranges): group is its slot's terrain group bits
	 * (Appended0: 1, Appended1: 2; Records.h).
	 */
	struct HeldGroupEntry
	{
		Api::ClassifyInputId heldWhile;
		std::uint32_t group = 0;
		std::uint64_t propertyFlags = 0;
		Api::ToggleId toggle;
	};
	/** @brief Any thread, after InstallFeatures (empty before): the held groups, in registration order (the first whose flags match takes). */
	std::span<const HeldGroupEntry> HeldGroups();

	/** @brief The registered near twin (Api::NearTwinContribution; target null: none), its strings owned and its NotReady texts built. */
	struct NearTwinEntry
	{
		std::string name, debugName, resource;
		std::string cannotDescribe, cannotImport;  // "<name> cannot be described", "<name> could not be imported"
		decltype(Api::NearTwinContribution::target) target = nullptr;
		decltype(Api::NearTwinContribution::nearTest) nearTest = nullptr;
	};
	/** @brief Any thread, after InstallFeatures: the near twin (target null: none registered). */
	const NearTwinEntry& NearTwin();
}
