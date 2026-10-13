#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics::LightSelectionParity
{
	/**
	 * @brief The light selection's parity (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY): DCLF's choice of shadow lights, slots, mask
	 * indices and focus hosts against CalculateActiveShadowCasterLights' (LightSelectionMade), and each tested light's room verdict and
	 * walked rooms against the engine's entries sampled at the function's entry (LightSelectionBegin), a light moved since the
	 * engine's walk tallied apart. The lines every 300 selections (Reports::LightSelectionLines).
	 */
	void Install();

	struct Tally
	{
		std::uint64_t frames = 0, kept = 0, slotsDiffer = 0, masksDiffer = 0, countsDiffer = 0, focusDiffer = 0, focusHosts = 0, framesDiffer = 0;
		std::uint64_t roomTests = 0, roomEntriesApart = 0, roomsDiffer = 0, roomsMovedDiffer = 0, roomSetsDiffer = 0, roomSetsMovedDiffer = 0;
		std::uint64_t roomWalks[3]{};          // by PortalViews::LightWalk
		std::uint64_t roomProcessesOther = 0;  // walked lights whose room process is not as WalkLight assumes
		std::string first, roomSetsFirst, roomProcessFirst;
	};
	/** @brief The tally since the last Take, reset. */
	Tally Take();
}
