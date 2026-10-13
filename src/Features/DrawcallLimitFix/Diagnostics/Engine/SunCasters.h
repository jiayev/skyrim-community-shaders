#pragma once

#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Diagnostics::SunCasters
{
	/**
	 * @brief The sun's casters as the engine's cascade culls reach them, observed at the cascade registration (SunAccumulation's hooks,
	 * render thread):
	 * - CS_DCLF_SET_PARITY (every 30th frame): the set's casters the cascade culls of the frame's Accumulate reached, each with its
	 *   cascade's descriptor index - what DCLF's views of those cascades must draw (Claimed);
	 * - CS_DCLF_PERSISTENT_PARITY: on the frames the cascades skip nothing (Policy::ExcludeSun), an unclaimed caster registered under an
	 *   excluded entry is one the skip would lose (<- SUN EXCLUSION, in the sun entry exclusion's line).
	 */
	void Install(bool a_claims, bool a_watch);

	/** @brief The last recorded frame's claimed casters (a_frame: that frame). Render thread. */
	const std::vector<SunAccumulation::CascadeRegistration>& Claimed(std::uint32_t& a_frame);

	/** @brief The exclusion watch since the last Take: the frames it watched, the casters lost, the first named. */
	struct Watch
	{
		std::uint64_t frames = 0, lost = 0;
		std::string first;
		const void* firstGeometry = nullptr;  // described at the report (Coordinator::DescribeSunCandidate)
	};
	Watch TakeWatch();
}
