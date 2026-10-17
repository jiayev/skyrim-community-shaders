// The sun accumulation's report lines (Api::ReportPoint::RenderThread: Reports::RenderThread, at Present, before the primary cull's):
// the accumulation's and the sun entry exclusion's, with the exclusion watch's verdict (SunCasters).

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Sun.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/Diagnostics/SunCasters.h"

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"

#include <algorithm>

#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

namespace DCLF::Features::Shadows::Sun
{
	void SunAccumulationLines(std::uint32_t a_frame)
	{
		auto& sun = SunAccumulation::Get();
		if (!sun.Installed() || !Reports::Due(a_frame))
			return;
		const auto stats = sun.TakeStats();
		const auto watch = SunCasters::TakeWatch();
		if (!stats.frames)
			return;
		const double toMs = Reports::MsPerTick();
		const double frames = stats.frames;
		const auto timing = fmt::format("; per frame, render thread: full-frustum cull {:.3f} ms (max {:.3f}), Accumulate {:.3f} ms (max {:.3f})",
			stats.fullFrustumTicks * toMs / frames, stats.fullFrustumMax * toMs, stats.accumulateTicks * toMs / frames, stats.accumulateMax * toMs);
		logger::info("[DCLF] sun accumulation: skipping registration on {} of {} frames; {:.0f} claimed geometries a frame not registered, {:.0f} registered by the engine, {} sun registrations outside Accumulate{}",
			stats.activeFrames, stats.frames, stats.skipped / frames, stats.registered / frames, stats.offThread, timing);
		const auto c = sun.TakeExclusionCounts();
		if (stats.wholeFrames || stats.wholeDeclined) {
			const double whole = std::max<double>(stats.wholeFrames, 1.0);
			logger::info("[DCLF] sun whole skip (no engine sun cull: DCLF draws every cascade): applied on {} of {} frames ({} declined: a cascade not DCLF's, or a parity "
						 "frame); per frame {:.1f} full-frustum processes culled into the cascades ({:.1f} empty ones left out), {:.0f} main registrations took DCLF's bits "
						 "({:.0f} with a cascade), {} written without a stamp{}",
				stats.wholeFrames, stats.frames, stats.wholeDeclined, stats.wholeProcessesCulled / whole, stats.wholeProcessesLeft / whole, c.wholeWritten / whole,
				c.wholeWithBits / whole, c.wholeUnstamped, c.wholeUnstamped ? " <- STAMPS" : "");
		}
		if (stats.exclusionFrames || stats.exclusionStale || stats.exclusionMissing || stats.exclusionUnforwarded) {
			const double applied = std::max<double>(stats.exclusionFrames, 1.0);
			logger::info("[DCLF] sun entry exclusion: applied on {} frames ({} stale, {} with none; the builder's translation taken at {} frame starts, none for the frame's candidates at {}); per frame {:.0f} of {:.0f} objectArray entries removed, "
						 "{:.0f} of {:.0f} candidates excluded, filter {:.3f} ms; {:.0f} registrations took DCLF's bits ({:.0f} with a cascade), {} before the cascades, "
						 "{} cascade registrations under a removed entry; excluded entries the cascades reached anyway and skipped {:.0f} a frame, parity {} frames: {} unclaimed casters under an excluded entry{}",
				stats.exclusionFrames, stats.exclusionStale, stats.exclusionMissing, stats.exclusionForwarded, stats.exclusionUnforwarded,
				stats.entriesRemoved / applied, stats.entriesSeen / applied, stats.excluded / applied, stats.candidates / applied,
				stats.filterTicks * toMs / applied, c.written / applied, c.withBits / applied, c.notReady, stats.cascadeRegistrationsUnderRemoved,
				c.skipped / applied, watch.frames, watch.lost,
				watch.frames ? (watch.lost ? " <- SUN EXCLUSION; first " + watch.first + " (" + Scene::Coordinator::Get().DescribeSunCandidate(watch.firstGeometry) + ")" : std::string(" <- OK")) : std::string());
		}
	}
}
