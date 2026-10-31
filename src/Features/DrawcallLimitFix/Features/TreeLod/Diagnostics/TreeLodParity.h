#pragma once

// CS_DCLF_PERSISTENT_PARITY: tree LOD's mirror against the LOD trees root (TreeLodParity.cpp), reported with the mirror's line
// (TreeLodReport.cpp). Included by the feature's sources only.

#include <cstdint>
#include <string>

namespace DCLF::Features::TreeLod
{
	/** @brief The parity's state (render thread; reported with the mirror's line). */
	struct TreeLodParityState
	{
		std::uint64_t checks = 0, shapes = 0, stale = 0, missing = 0, differ = 0, late = 0, unsettled = 0;
		std::string first;
		ankerl::unordered_dense::map<const void*, std::string> suspects;  // differed at the last check, with what
		std::uint32_t frame = ~0u;                                       // the frame the last check ran in
	};

	/** @brief The parity's state (its counters zero while it does not run). */
	TreeLodParityState& ParityState();
}
