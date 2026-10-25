#include "Draws.h"

#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

// The CPU-driven pipeline's report line ([CPU]).

namespace DCLF::Reports
{
	std::string CpuPipelineLine()
	{
		const auto w = Direct::TakeWindowCounters();
		if (!w.frames)
			return {};
		const double n = static_cast<double>(w.frames);
		const auto mode = ModeOf(FrameToggles());
		std::string views, cadences;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			if (!w.sum.batches[v])
				continue;
			views += fmt::format("{}{} {:.0f} us, {:.1f} batches, {:.0f} drawn", views.empty() ? "" : "; ", CpuViewName(v), w.sum.cullUs[v] / n, w.sum.batches[v] / n,
				w.sum.drawn[v] / n);
		}
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v)
			if (mode.cadence[v] == Direct::Cadence::PerSnapshot)
				cadences += fmt::format("{}{}", cadences.empty() ? "" : ",", CpuViewName(v));
		static constexpr const char* kSiteNames[] = { "shadow", "depth", "colour", "forward", "first person" };
		std::string sites, empty;
		for (std::size_t site = 0; site < std::size(kSiteNames); ++site) {
			if (!w.siteRuns[site])
				continue;
			sites += fmt::format("{}{} {:.0f} us ({} callbacks, {} drew nothing)", sites.empty() ? "" : ", ", kSiteNames[site], w.sum.recordUs[site] / n, w.siteRuns[site],
				w.siteEmpty[site]);
			if (w.siteEmpty[site] == w.siteRuns[site])
				empty += fmt::format(" <- {} DREW NOTHING", kSiteNames[site]);
		}
		if (!cadences.empty())
			cadences += fmt::format(" ({} recordings made, {:.1f} executed a frame)", w.sum.snapshotRecorded, w.sum.snapshotExecuted / n);
		return fmt::format("[DCLF] [CPU] {} frames, record {}, per-snapshot views: {}; a frame's cull (pool) by view: {}; record (DXVK's CS thread) by site: {}; "
						   "{:.1f} secondaries a frame, {} callback waits for a batch, {} frame slot waits ({:.2f} ms), {} forward row waits, {:.1f} geometry slots a frame changed since their set (not drawn){}{}{}",
			w.frames, mode.record == Direct::RecordMode::Parallel ? "parallel" : "serial", cadences.empty() ? "none" : cadences, views.empty() ? "none" : views,
			sites.empty() ? "none" : sites, w.sum.secondaries / n, w.sum.waits, w.slotWaits, w.slotWaitMs, w.rowWaits,
			w.sum.staleGeometry / n, w.notQueued ? fmt::format(", {} callbacks not queued <- NOT QUEUED", w.notQueued) : std::string(),
			w.recordedUploadLists ? fmt::format(", {} recorded upload lists not taken <- UPLOADS NOT TAKEN", w.recordedUploadLists) : std::string(), empty);
	}
}
