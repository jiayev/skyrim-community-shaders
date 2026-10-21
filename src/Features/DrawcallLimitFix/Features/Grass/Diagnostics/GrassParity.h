#pragma once

// CS_DCLF_PERSISTENT_PARITY: grass's mirror against the capture's live set (GrassParity.cpp), reported with the mirror's line
// (GrassReport.cpp). Included by the feature's sources only.

#include <cstdint>
#include <string>

namespace DCLF::Features::Grass
{
	/** @brief The parity's state (render thread; reported with the mirror's line). */
	struct GrassParityState
	{
		std::uint64_t checks = 0, shapes = 0, missing = 0, stale = 0, differ = 0, late = 0;
		std::uint64_t liveInstances = 0, mirroredInstances = 0;  // the last check's totals
		std::string first;
		ankerl::unordered_dense::map<const void*, std::string> suspects;  // differed at the last check, with what
		std::uint32_t frame = ~0u;                                       // the frame the last check ran in
	};

	/** @brief The parity's state (its counters zero while it does not run). */
	GrassParityState& ParityState();
}
