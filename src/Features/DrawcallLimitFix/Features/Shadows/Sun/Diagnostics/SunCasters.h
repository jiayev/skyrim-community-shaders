#pragma once

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunAccumulation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Features::Shadows::Sun::SunCasters
{
	/**
	 * @brief CS_DCLF_PERSISTENT_PARITY, at the sun's cascade registration (SunAccumulation's hooks, render thread): on the frames the
	 * cascades skip nothing (ExcludeSun, SunAccumulation.cpp), an unclaimed caster registered under an excluded entry is one the skip would lose
	 * (<- SUN EXCLUSION, in the sun entry exclusion's line).
	 */
	void Install();

	/** @brief The exclusion watch since the last Take: the frames it watched, the casters lost, the first named. */
	struct Watch
	{
		std::uint64_t frames = 0, lost = 0;
		std::string first;
		const void* firstGeometry = nullptr;  // described at the report (Coordinator::DescribeSunCandidate)
	};
	Watch TakeWatch();
}
