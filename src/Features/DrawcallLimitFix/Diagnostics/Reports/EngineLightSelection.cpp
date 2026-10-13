#include "Engine.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/LightSelectionParity.h"

namespace DCLF::Reports
{
	void LightSelectionLines()
	{
		const auto p = Diagnostics::LightSelectionParity::Take();
		logger::info("[DCLF] light rooms (T5a3: DCLF's walk of each tested light against the engine's entry): {} tests ({} not walked, {} by bounds, {} walked), {} on an "
					 "entry apart from the room process's, {} room processes not as assumed{}; verdicts differ {} (+{} lights moved since the engine's walk), room sets "
					 "differ {} (+{} moved){}{}",
			p.roomTests, p.roomWalks[0], p.roomWalks[1], p.roomWalks[2], p.roomEntriesApart, p.roomProcessesOther,
			p.roomProcessFirst.empty() ? "" : " (" + p.roomProcessFirst + ")", p.roomsDiffer, p.roomsMovedDiffer, p.roomSetsDiffer, p.roomSetsMovedDiffer,
			p.roomsDiffer ? " <- LIGHT ROOMS" : " <- OK", p.roomSetsFirst.empty() ? "" : "; first set: " + p.roomSetsFirst);
		logger::info("[DCLF] light selection (T3b, T3c: DCLF's choice of shadow lights and focus hosts against CalculateActiveShadowCasterLights'): {} frames, {} local "
					 "lights kept, {} focus hosts; {} frames differ ({} slots, {} mask indices, {} counts, {} focus){}{}",
			p.frames, p.kept, p.focusHosts, p.framesDiffer, p.slotsDiffer, p.masksDiffer, p.countsDiffer, p.focusDiffer, p.framesDiffer ? " <- LIGHT SELECTION" : " <- OK",
			p.first.empty() ? "" : "; first: " + p.first);
	}
}
