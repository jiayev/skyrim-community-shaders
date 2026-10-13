#include "Engine.h"

#include "Schedule.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/SunCasters.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"

namespace DCLF::Reports
{
	void SunAccumulationLines(std::uint32_t a_frame)
	{
		auto& sun = SunAccumulation::Get();
		if (!sun.Installed() || !Due(a_frame))
			return;
		const auto stats = sun.TakeStats();
		const auto watch = Diagnostics::SunCasters::TakeWatch();
		if (!stats.frames)
			return;
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
		const double frames = stats.frames;
		const auto timing = fmt::format("; per frame, render thread: full-frustum cull {:.3f} ms (max {:.3f}), Accumulate {:.3f} ms (max {:.3f})",
			stats.fullFrustumTicks * toMs / frames, stats.fullFrustumMax * toMs, stats.accumulateTicks * toMs / frames, stats.accumulateMax * toMs);
		logger::info("[DCLF] sun accumulation: skipping registration on {} of {} frames; {:.0f} claimed geometries a frame not registered, {:.0f} registered by the engine, {} sun registrations outside Accumulate{}",
			stats.activeFrames, stats.frames, stats.skipped / frames, stats.registered / frames, stats.offThread, timing);
		if (stats.exclusionFrames || stats.exclusionStale || stats.exclusionMissing) {
			const double applied = std::max<double>(stats.exclusionFrames, 1.0);
			const auto c = sun.TakeExclusionCounts();
			logger::info("[DCLF] sun entry exclusion: applied on {} frames ({} stale, {} with none); per frame {:.0f} of {:.0f} objectArray entries removed, "
						 "{:.0f} of {:.0f} candidates excluded, filter {:.3f} ms; {:.0f} registrations took DCLF's bits ({:.0f} with a cascade), {} before the cascades, "
						 "{} cascade registrations under a removed entry; excluded entries the cascades reached anyway and skipped {:.0f} a frame, parity {} frames: {} unclaimed casters under an excluded entry{}",
				stats.exclusionFrames, stats.exclusionStale, stats.exclusionMissing,
				stats.entriesRemoved / applied, stats.entriesSeen / applied, stats.excluded / applied, stats.candidates / applied,
				stats.filterTicks * toMs / applied, c.written / applied, c.withBits / applied, c.notReady, stats.cascadeRegistrationsUnderRemoved,
				c.skipped / applied, watch.frames, watch.lost,
				watch.frames ? (watch.lost ? " <- SUN EXCLUSION; first " + watch.first + " (" + Scene::Coordinator::Get().DescribeSunCandidate(watch.firstGeometry) + ")" : std::string(" <- OK")) : std::string());
		}
	}
}
