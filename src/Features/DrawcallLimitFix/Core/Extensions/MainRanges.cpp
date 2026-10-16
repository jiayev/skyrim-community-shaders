// The main ranges' table (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"

#include <algorithm>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveMainRanges (InstallFeatures, before DCLF's threads exist), read-only after.
		MainRangeTable& Table()
		{
			static MainRangeTable table;
			return table;
		}
	}

	void ResolveMainRanges(const std::array<Api::MainRangeContribution, kMainRangeSlots>& a_registered)
	{
		std::array<const Api::MainRangeContribution*, kMainRangeSlots> ordered{};
		std::size_t count = 0;
		for (const auto& registered : a_registered)
			if (registered.slot != Api::MainRangeSlot::Count)
				ordered[count++] = &registered;
		// The install refused a repeated order, so the draw order is total.
		std::sort(ordered.begin(), ordered.begin() + count, [](const auto* a_left, const auto* a_right) { return a_left->order < a_right->order; });
		auto& table = Table();
		for (std::size_t i = 0; i < count; ++i) {
			const auto& range = *ordered[i];
			table.drawn.push_back({ static_cast<std::uint32_t>(range.slot), "cs.dclf.colour." + std::string(range.name), range.depthPrepass, range.objectClass });
			table.anyDepthPrepass = table.anyDepthPrepass || range.depthPrepass;
		}
	}

	const MainRangeTable& MainRanges() { return Table(); }
}
