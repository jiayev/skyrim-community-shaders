#include "Engine.h"

#include "Features/DrawcallLimitFix/Engine/Cull/LodGates.h"

namespace DCLF::Reports
{
	std::string LodGatesLine(std::uint32_t a_frames)
	{
		const auto t = LodGates::Take();
		if (!t.installed)
			return {};
		std::string flushes;
		for (std::size_t i = 0; i < t.flushes.size(); ++i)
			if (const auto count = t.flushes[i])
				flushes += fmt::format("{}{} {}", flushes.empty() ? "" : ", ", LodGates::FlushName(i), count);
		const auto ms = LodGates::TicksToMilliseconds;
		return fmt::format(
			"[DCLF] LOD gates (T6b5, over {} frames): {} opened ({} initial loads; {} initial loads under a load screen passed through), {} flipped ({} flips "
			"missed), {} forced ({} superseded; flushes: {}); {} blocks held, {} released ({} reused, {} handed off, {} deferred past a flip), {} reattached "
			"before their flip, {} kept notices to the scene, {} reused still hidden by the flip with no gate pending, {} orphans shown, {} shows vetoed; {} "
			"open, the oldest {} frames {:.1f} ms; request to flip {:.1f} frames {:.2f} ms average, {:.2f} ms max; flip to release {:.2f} ms average, {:.2f} ms max{}",
			a_frames, t.opened, t.initialOpened, t.initialLoadScreen, t.flipped, t.flipMisses, t.forced, t.superseded, flushes.empty() ? "none" : flushes, t.held,
			t.released, t.reused, t.handedOff, t.deferred, t.reattached, t.kept, t.reusedHidden, t.orphansShown, t.vetoes, t.open, t.oldestFrames, ms(t.oldestTicks),
			t.flipped ? static_cast<double>(t.flipFrames) / static_cast<double>(t.flipped) : 0.0, t.flipped ? ms(t.flipTicks) / static_cast<double>(t.flipped) : 0.0,
			ms(t.flipTicksMax), t.releases ? ms(t.releaseTicks) / static_cast<double>(t.releases) : 0.0, ms(t.releaseTicksMax),
			t.coverage ? fmt::format("; coverage: both levels shown {}, neither {}, neither with every incoming hidden again by the engine (LOD under the "
									 "loaded cells) {}; incoming hidden by the engine between flip and release {}",
							 t.coverageBoth, t.coverageNeither, t.coverageEngineHid, t.engineRehides) :
						 std::string());
	}
}
