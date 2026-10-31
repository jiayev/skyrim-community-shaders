// Both view families' phases and view bits (see the header): checked against Core's and each other's, and the table the per-object
// paths read.

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ViewCommon.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"

#include <bit>

namespace DCLF::Extensions
{
	namespace
	{
		/** @brief One feature view's bits: one bit each, neither Core's nor an earlier view's; then taken. */
		void TakeViewBits(const char* a_family, std::size_t a_index, std::uint8_t a_phase, std::uint32_t a_viewBit, std::uint32_t& a_phases, std::uint32_t& a_viewBits)
		{
			if (!std::has_single_bit(static_cast<std::uint32_t>(a_phase)) || (a_phase & a_phases) || !std::has_single_bit(a_viewBit) || (a_viewBit & a_viewBits))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} {} has a set phase ({:#x}) or view bit ({:#x}) that is none, more than one, or Core's or another view's",
					a_family, a_index, a_phase, a_viewBit));
			a_phases |= a_phase;
			a_viewBits |= a_viewBit;
		}

		/**
		 * @brief Each registered view's and occlusion map's phase and view bit: one bit, none Core's (its phases and view bits) nor another
		 * view's, of either family.
		 */
		void CheckViewBits(const ViewSet& a_views, const ShadowViewSet& a_shadow)
		{
			std::uint32_t phases = kCoreSetPhases;
			std::uint32_t viewBits = Draws::kCoreViewBits;
			for (std::size_t v = 0; v < a_views.views.size(); ++v)
				if (const auto& entry = a_views.views[v]; entry.registered)
					TakeViewBits("view", v, entry.phase, entry.viewBit, phases, viewBits);
			for (std::size_t m = 0; m < a_shadow.maps.size(); ++m)
				if (const auto& map = a_shadow.maps[m]; map.registered)
					TakeViewBits("occlusion map", m, map.phase, map.viewBit, phases, viewBits);
		}

		void AddPhaseBits(ViewBitTable& a_table, std::uint8_t a_phase, std::uint32_t a_viewBits)
		{
			for (std::uint32_t b = 0; b < a_table.viewBitsOfPhaseBit.size(); ++b)
				if (a_phase & (1u << b))
					a_table.viewBitsOfPhaseBit[b] |= a_viewBits;
		}

		/**
		 * @brief The view bits by phase (Core's main and caster phases', then the maps', then the views'), both families' phases and bits,
		 * and the lacking counts' phases.
		 */
		void DeriveViewBits(const ViewSet& a_views, const ShadowViewSet& a_shadow, ViewBitTable& a_table)
		{
			AddPhaseBits(a_table, kSetMain, Draws::kViewMain);
			AddPhaseBits(a_table, kSetCaster, Draws::kViewSunCaster | Draws::kViewSpotCaster);
			AddPhaseBits(a_table, kSetCasterPoint, Draws::kViewPointCaster);
			for (const auto& map : a_shadow.maps) {
				a_table.phases |= map.phase;
				a_table.viewBits |= map.viewBit;
				AddPhaseBits(a_table, map.phase, map.viewBit);
			}
			for (const auto& entry : a_views.views) {
				a_table.phases |= entry.phase;
				a_table.viewBits |= entry.viewBit;
				AddPhaseBits(a_table, entry.phase, entry.viewBit);
			}
			for (std::uint32_t mask = 0; mask < a_table.viewBitsOfPhases.size(); ++mask)
				for (std::uint32_t b = 0; b < a_table.viewBitsOfPhaseBit.size(); ++b)
					if (mask & (1u << b))
						a_table.viewBitsOfPhases[mask] |= a_table.viewBitsOfPhaseBit[b];
			a_table.lackingPhases = { a_shadow.maps[kOcclusionSky].phase, a_shadow.maps[kOcclusionPrecipitation].phase, a_views.lodPhases };
			for (const auto phase : a_table.lackingPhases)
				a_table.lackingMask |= phase;
		}
	}

	void ResolveViewBits()
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the view bits were resolved twice");
		CheckViewBits(Views(), ShadowViewsSet());
		ViewBitTable table;
		DeriveViewBits(Views(), ShadowViewsSet(), table);
		Detail::resolvedViewBits = table;
	}
}
