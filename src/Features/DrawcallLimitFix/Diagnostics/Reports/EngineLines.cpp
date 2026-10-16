#include "Engine.h"

#include "Schedule.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/LightSelectionParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Registrations.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingConstants.h"

// The engine area's report lines (Engine.h), but the primary cull's (EnginePrimaryCull.cpp).

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

	std::string EngineReadWindowLine()
	{
		// The scene work's accesses without a lease (any is a defect).
		const auto c = EngineReadWindow::Take();
		if (!c.any)
			return {};
		return fmt::format("[DCLF] engine-read window: {} closes, {} of them waited for items in flight ({:.1f} us in all, max {:.1f} us); {} leases refused (work left for the next window); "
						   "scene work engine accesses without a lease {}{}{}",
			c.closes, c.closesWaited, c.waitedUs, c.waitedMaxUs, c.refused, c.unleased, c.unleased ? " <- LANE ENGINE ACCESS; first: " : " <- OK",
			c.unleasedFirst ? c.unleasedFirst : "");
	}

	std::string ReleaseGuardLine()
	{
		if (!ReleaseGuard::Installed())
			return {};
		const auto c = ReleaseGuard::Take();
		return fmt::format("[DCLF] release guard (T6b3e, CS_DCLF_RELEASE_GUARD): engine objects' last releases on DCLF's threads: scene lane {}, executor/pool {} "
						   "({} without a class name){}{}{}{}",
			c.lane, c.pool, c.unnamed, c.lane || c.pool ? " <- LANE ENGINE RELEASE; first: " : " <- OK", c.first ? c.first : "", c.lane || c.pool ? ", last: " : "",
			(c.lane || c.pool) && c.last ? c.last : "");
	}

	std::string AnimRateLine()
	{
		if (!Diagnostics::Observe().accumulated)
			return {};
		const auto r = Diagnostics::Accumulated::TakeAnimRate();
		std::string sites;
		std::uint64_t differ = 0;
		std::string first;
		for (std::size_t s = 0; s < r.sites.size(); ++s) {
			const auto& c = r.sites[s];
			differ += c.differ;
			if (c.firstDiffer && first.empty())
				first = fmt::format("; first: {:p} ({})", c.firstDiffer, Diagnostics::Accumulated::kAnimGateCallers[s]);
			sites += fmt::format("; {}: {} stood-in calls ({} never fading, {} fading), {} rejected ({} never fading), {} written out of view ({} of them updated by "
								 "their fade step), {} written visible but faded out, {} differ",
				Diagnostics::Accumulated::kAnimGateCallers[s], c.calls, c.neverFade, c.fading, c.rejected, c.rejectedNeverFade, c.predictedOut, c.acceptedOut, c.fadedOut, c.differ);
		}
		return fmt::format("[DCLF] anim rate (T7b2): {} predictions, {:.1f} stood-in roots a frame ({:.1f} written visible){}{}{}\n", r.predictions,
			r.predictions ? double(r.roots) / double(r.predictions) : 0.0, r.predictions ? double(r.set) / double(r.predictions) : 0.0, sites,
			!r.predictions ? " <- NOT COMPARED" : differ ? " <- ANIM RATE" : " <- OK", first);
	}
}
