#include "Engine.h"

#include "Schedule.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Registrations.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ReleaseGuard.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/WindowWriters.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

// The engine area's report lines (Engine.h), but the primary cull's (EnginePrimaryCull.cpp).

namespace DCLF::Reports
{
	std::string EngineReadWindowLine()
	{
		// The scene work's accesses without a lease (any is a defect).
		const auto c = EngineReadWindow::Take();
		if (!c.any)
			return {};
		// The cell animations job's update inside the window (WindowWriters.h): its waits for the frame's sampling, its suspensions'.
		std::string writers;
		if (WindowWriters::Installed()) {
			const auto w = WindowWriters::Take();
			const auto job = [](const char* a_name, const WindowWriters::Counters::Job& a_job) {
				return fmt::format("{} {} updates, {} waited for the frame values' sampling ({} us in all, max {} us)", a_name, a_job.runs, a_job.waited, a_job.waitedUs,
					a_job.waitedMaxUs);
			};
			writers = fmt::format("; {}; {}; {} suspensions, {} of them waited for items in flight ({:.0f} us in all, max {:.0f} us)", job("cell animations", w.cellAnimations),
				job("animated cell objects", w.animatedObjects), c.suspends, c.suspendsWaited, c.suspendWaitedUs, c.suspendWaitedMaxUs);
		}
		return fmt::format("[DCLF] engine-read window: {} closes, {} of them waited for items in flight ({:.1f} us in all, max {:.1f} us){}; {} leases refused (work left for the "
						   "next window); scene work engine accesses without a lease {}{}{}",
			c.closes, c.closesWaited, c.waitedUs, c.waitedMaxUs, writers, c.refused, c.unleased, c.unleased ? " <- LANE ENGINE ACCESS; first: " : " <- OK",
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
