#pragma once

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunAccumulation.h"

namespace DCLF::Features::Shadows::Sun
{
	/** @brief The sun observers' read-only view of SunAccumulation's frame state (SunAccumulation befriends it). Render thread. */
	struct SunAccumulationView
	{
		/** @brief The cascade culls skip the excluded entries this frame (else nothing: ExcludeSun, SunAccumulation.cpp). */
		static bool SkipExcluded() { return SunAccumulation::Get().frameState.skipExcluded; }
		static bool ExclusionLive() { return SunAccumulation::Get().exclusionLive.load(std::memory_order_relaxed); }
		/** @brief a_geometry under an entry this frame's exclusion excludes. */
		static bool UnderExcludedEntry(const RE::BSGeometry* a_geometry) { return SunAccumulation::Get().UnderExcludedEntry(a_geometry); }
		/** @brief Cascade a_index of this frame's Accumulate: DCLF's planes (planes, planeMask, customPlanes, customMask) and whether they are known (captured). */
		static const auto& Cascade(std::uint32_t a_index) { return SunAccumulation::Get().frameState.cascades[a_index]; }
	};
}
