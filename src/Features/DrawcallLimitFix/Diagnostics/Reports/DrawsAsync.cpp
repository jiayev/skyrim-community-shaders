#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Draws.h"
#include "Engine.h"

// IndirectDraws' report lines at the render thread's report (Reports::RenderThread): the async work, the views' epochs, the snapshots and
// revisions (the payload's pools are the snapshot builder's: Impl::ProducerReport).

namespace DCLF
{
	std::string IndirectDraws::AsyncReport()
	{
		std::string text;
		if (auto window = Reports::EngineReadWindowLine(); !window.empty())
			text += window + "\n";
		if (impl->scene && impl->scene->fadeWriteBack) {
			auto& writeBack = *impl->scene->fadeWriteBack;
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
				text += fmt::format("[DCLF] fade write-back (T7b4/T7b6): {} milestones written onto stood-in roots' nodes by the main thread at Main::Update's entry "
									"({:.1f} us in all), {} left (no longer DCLF's to write), {} dropped (DCLF off), {} stale (the root listed again or no longer stood "
									"in), {} frames found the last task still running; readback {} frames read ({} with events), {} events, {} past their list, {} arms "
									"missed, {} staging sets (+{}), event list {}\n",
					applied, static_cast<double>(storeNs) / 1000.0, notWritten, dropped, stale, busy, harvested, withEvents, events, overflowed, missed, sets, setsAdded,
					listEvents);
		}
		text += Reports::FadeWritersLine();
		text += VisibilityReport();
		text += ReflectionReport();
		text += ForwardReport();
		// The features' lines here (Features/Particles, Features/Billboards).
		Extensions::ReportTextAt(Api::ReportPoint::AsyncWork, text);
		text += FirstPersonReport();
		if (impl->scene && impl->scene->treeLodCull && impl->scene->treeLodUploads) {
			auto& scene = *impl->scene;
			text += fmt::format("[DCLF] tree LOD draws: {} ({} depth commits, {} shape slots sent; tables {} shape and {} mesh slots; {} commits that could not draw a "
								"withheld frame){}\n",
				impl->treeLodOwned ? "DCLF's" : "the engine's", scene.treeLodUploads, scene.treeLodSlotsSent, NamedUnits(impl->Adopted(), scene.treeLodShapes),
				NamedUnits(impl->Adopted(), scene.treeLodMeshes),
				Diagnostics::DrawsState::Get().treeLodMissed, Diagnostics::DrawsState::Get().treeLodMissed ? " <- TREE LOD HOLES" : "");
			scene.treeLodUploads = scene.treeLodSlotsSent = 0;
			// Its two-phase cull: in view, drawn by phase 1, occluded by the last frame's HZB, brought back by the rebuilt one.
			if (auto& counts = scene.treeLodCounts) {
				if (const auto frames = counts->frames.exchange(0, std::memory_order_relaxed)) {
					const double phaseOne = double(counts->phaseOne.exchange(0, std::memory_order_relaxed)) / frames;
					const double retests = double(counts->retests.exchange(0, std::memory_order_relaxed)) / frames;
					const double phaseTwo = double(counts->phaseTwo.exchange(0, std::memory_order_relaxed)) / frames;
					text += fmt::format("[DCLF] tree LOD cull over {} frames: per frame {:.0f} instances in view, {:.0f} drawn by phase 1, {:.0f} occluded by the last "
										"frame's HZB, {:.0f} of them brought back by phase 2 ({:.0f} drawn, {:.1f}% occluded)\n",
						frames, phaseOne + retests, phaseOne, retests, phaseTwo, phaseOne + phaseTwo,
						phaseOne + retests > 0.0 ? 100.0 * (retests - phaseTwo) / (phaseOne + retests) : 0.0);
				}
			}
		}
		// The builds' stores (main rows, object records, shadow state, extras rows, geometry table, exclusions) are the snapshot builder's:
		// their lines are composed in its pass (Impl::StoresReport, with the producer's report) and printed with it, a report behind.
		if (auto line = Reports::FrameValuesLine(); !line.empty())
			text += line + "\n";
		if (auto line = Reports::FrameDataLine(); !line.empty())
			text += line + "\n";
		{
			text += impl->RevisionReport();
			// The snapshots (the frame's counters, the builder's times) and the builder's own lines (the revisions, the growths,
			// the shapes made, the draw bound), composed in its pass and taken here.
			text += impl->SnapshotReport();
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.used)
				continue;
			text += fmt::format("[DCLF] {} epochs: {} committed the payload built ahead with their publication\n", kNames[i], a.used);
			if (i == kAsyncZPrepass)
				text += fmt::format("[DCLF] frame slots supplied from an earlier capture (6e E5): {}\n", std::exchange(impl->frameSlotsCarried, 0));
			a = {};
		}
		if (auto& p = Diagnostics::DrawsState::Get().viewMasks; p.checks.load(std::memory_order_relaxed)) {
			static constexpr std::array<const char*, 2 + kShadowModeCount> kLists{ "main", "main drawable", "plain", "clamped", "paraboloid", "sky occlusion", "precipitation" };
			const auto first = p.first.exchange(~0ull);
			const auto differ = p.differ.exchange(0);
			text += fmt::format("[DCLF] view mask parity (U3): {} publications, {} inputs, {} with a mask other than their object's or outside their list's views{}{}\n",
				p.checks.exchange(0), p.inputs.exchange(0), differ, differ ? " <- VIEW MASK" : " <- OK",
				first == ~0ull ? std::string() :
								 fmt::format("; first: {} input of object {}, mask {:#x}, expected {:#x}", kLists[std::min<std::size_t>(first >> 56, kLists.size() - 1)],
									 (first >> 24) & 0xFFFFFFFFu, p.firstMask.load(std::memory_order_relaxed), first & 0xFFFFFFu));
		}
		return text;
	}
}
