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

		// Terrain Blending's groups (CS_DCLF_TB_TERRAIN; Records.h's terrain group bits): its terrain blocks (blend mode 1, LESS_EQUAL with
		// depth writes) and the no-blend meshes after it (EQUAL), as that feature replays them before the blended decals
		// (Deferred::Hooks::Main_RenderWorld_BlendedDecals).
		constexpr Api::MainRangeContribution kCoreRanges[] = {
			{ .slot = Api::MainRangeSlot::Appended0, .name = "terrain", .order = 2 },
			{ .slot = Api::MainRangeSlot::Appended1, .name = "terrain-no-blend", .order = 3 },
		};
	}

	std::span<const Api::MainRangeContribution> CoreMainRanges() { return kCoreRanges; }

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
