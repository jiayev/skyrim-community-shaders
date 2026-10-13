#pragma once

#include <cstdint>

namespace DCLF::Diagnostics
{
	/**
	 * @brief What the observers of a main build counted into its payload (Diagnostics/Draws/BuildChecks.cpp), carried with it to its
	 * commits, which hand it to them again (MainPayloadCommitted): the main path only resets it with the payload.
	 */
	struct PayloadChecks
	{
		// CS_DCLF_BINDLESS_PARITY: the record's components compared with the constant groups, and those that differ.
		std::uint32_t bindless = 0, bindlessDiffer = 0;
		// CS_DCLF_RESIDENT_DRAW_PARITY: the region's entries compared, differing and missing; its pairs checked, and those whose witness
		// moved with no event.
		std::uint32_t resident = 0, residentDiffer = 0, residentMissing = 0, pairs = 0, pairsStale = 0;
	};
}
