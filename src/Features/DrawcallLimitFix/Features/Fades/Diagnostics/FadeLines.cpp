// Fades' report lines (CS_DCLF_STATS): among the primary cull's, the fade port parity (PrimaryCullParity) and the fade roots' line
// (PrimaryCullLast); first in the draws' async work, the fade write-back and the fade writers (AsyncWorkWriteBack).

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Features/Fades/Diagnostics/FadeDiagnostics.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Engine.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <algorithm>
#include <string>

namespace DCLF::Features::Fades
{
	namespace
	{
		/** @brief CS_DCLF_FADE_PARITY: the port check's line, when it checked a root since the last report. */
		void PortLines(std::uint32_t)
		{
			const auto fadePort = TakePortCheck();
			if (fadePort.checked)
				logger::info("[DCLF] fade port parity (the port against the engine's functions on node copies): {} roots, {} differ{}{}", fadePort.checked, fadePort.differ,
					fadePort.differ ? " <- FADE PORT" : " <- OK", fadePort.first.empty() ? "" : "; first: " + fadePort.first);
		}

		/** @brief The stood-in roots, the LOD skins that read their levels, and the cut's entries with engine-drawn parts. */
		void RootsLines(std::uint32_t)
		{
			// LOD skins: records whose partitions BuildDraws picks by their root's level in FadeStateCS's state.
			const auto& lodTables = Scene::FrameView::Get().GetTables();
			const auto lodSkins = std::count_if(lodTables.skinLodPartitions.begin(), lodTables.skinLodPartitions.end(), [](std::uint32_t a_word) { return a_word != 0; });
			logger::info("[DCLF] fade roots: {} stood in; {} LOD skins (partitions by their root's level on the GPU); {:.0f} entries with engine-drawn parts culled by the engine a frame",
				Scene::FadeRootsStoodIn(), lodSkins, Reports::CutMixedPerFrame());
		}

		/** @brief The write-back's readback and stores since the last report (nothing without Fades' scene resources or their ring). */
		void WriteBackText(std::string& a_text)
		{
			const auto* scene = IndirectDraws::Get().Scene();
			if (const auto* fades = scene ? SceneOf(*scene) : nullptr; fades && fades->fadeWriteBack) {
				auto& writeBack = *fades->fadeWriteBack;
				const auto applied = writeBack.applied.exchange(0), stale = writeBack.stale.exchange(0), busy = writeBack.busy.exchange(0);
				const auto storeNs = writeBack.storeNs.exchange(0), notWritten = writeBack.notWritten.exchange(0), dropped = writeBack.dropped.exchange(0);
				const auto harvested = writeBack.harvested.exchange(0), withEvents = writeBack.harvestedWithEvents.exchange(0), events = writeBack.events.exchange(0);
				const auto overflowed = writeBack.overflowed.exchange(0), missed = writeBack.missed.exchange(0), setsAdded = writeBack.setsAdded.exchange(0);
				std::uint32_t sets = 0, listEvents = 0;
				for (const auto* set = writeBack.sets.load(std::memory_order_acquire); set; set = set->next) {
					++sets;
					listEvents = std::max(listEvents, set->events);
				}
				// The readback (a fixed per-frame-slot copy harvested on re-arm: a frame read whether it recorded or replayed), and the
				// stores (Main::Update's entry, on the roots DCLF alone writes).
				if (applied || stale || busy || harvested || missed)
					a_text += fmt::format("[DCLF] fade write-back (T7b4/T7b6): {} milestones written onto stood-in roots' nodes by the main thread at Main::Update's entry "
										  "({:.1f} us in all), {} left (no longer DCLF's to write), {} dropped (DCLF off), {} stale (the root listed again or no longer stood "
										  "in), {} frames found the last task still running; readback {} frames read ({} with events), {} events, {} past their list, {} arms "
										  "missed, {} staging sets (+{}), event list {}\n",
						applied, static_cast<double>(storeNs) / 1000.0, notWritten, dropped, stale, busy, harvested, withEvents, events, overflowed, missed, sets, setsAdded,
						listEvents);
			}
		}

		/** @brief The fade writers' line; nothing while nothing was skipped. */
		void WritersText(std::string& a_text)
		{
			const auto c = TakeFadeWriterCounts();
			if (!c.gateSkipped && !c.onVisibleSkipped)
				return;
			a_text += fmt::format("[DCLF] fade writers (T7b4; T7f: every listed root): {} fade updates of the animation gate skipped on roots DCLF writes, {} OnVisible fade steps "
								  "skipped ({} roots in the set)\n",
				c.gateSkipped, c.onVisibleSkipped, c.roots);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "fade port", .point = Api::ReportPoint::PrimaryCullParity, .lines = &PortLines });
		a_context.RegisterReportLines({ .name = "fade roots", .point = Api::ReportPoint::PrimaryCullLast, .lines = &RootsLines });
		a_context.RegisterReportLines({ .name = "fade write-back", .point = Api::ReportPoint::AsyncWorkWriteBack, .text = &WriteBackText });
		a_context.RegisterReportLines({ .name = "fade writers", .point = Api::ReportPoint::AsyncWorkWriteBack, .text = &WritersText });
	}
}
