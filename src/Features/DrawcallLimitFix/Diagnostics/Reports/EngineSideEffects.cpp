#include "Engine.h"

#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SideEffects.h"

namespace DCLF::Reports
{
	std::string ActorRateLine()
	{
		const auto c = SideEffects::TakeActorRate();
		if (!c.entries)
			return {};
		return fmt::format(
			"[DCLF] actor rate (T7f checklist item 6): {} entries, {:.1f} high actors a frame ({:.1f} without a skin property or 3D), {:.1f} in the "
			"sun's cascade culls and {:.1f} in a local shadow view a frame, {:.1f} stamped by DCLF a frame\n",
			c.entries, double(c.actors) / double(c.entries), double(c.noProperty) / double(c.entries),
			double(c.inSun) / double(c.entries), double(c.inLocal) / double(c.entries), double(c.stamped) / double(c.entries));
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
