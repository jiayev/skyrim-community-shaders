#include "Engine.h"

#include "Schedule.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/CutObservers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/LightCulls.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/PrimaryCullView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/SunExclusion.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include <ranges>

namespace DCLF::Reports
{
	namespace
	{
		/** @brief The scene lists' line's filter part: why entries stay in the lists, and the filter's deltas. */
		std::string ListReasons(const PrimaryCull::ListCounters& a_l)
		{
			const double frames = std::max<double>(static_cast<double>(a_l.reasonFrames), 1.0);
			std::string reasons;
			for (std::size_t r = 1; r < a_l.keptBy.size(); ++r)
				reasons += fmt::format("{}{:.0f} {}", reasons.empty() ? "" : ", ", a_l.keptBy[r] / frames, PrimaryCull::kListReasonNames[r]);
			return fmt::format("entries per filtered frame: {:.0f} left out, kept in the lists: {}; filter deltas: {} entries judged, {} roots inserted, {} erased; kept frames applying "
							   "a delta {} ({} roots taken out, {} put back), {} rebuilt instead",
				a_l.keptBy[0] / frames, reasons, a_l.filterJudged, a_l.filterInserted, a_l.filterErased, a_l.deltaKept, a_l.deltaOut, a_l.deltaIn, a_l.deltaRebuilt);
		}

		/** @brief The scene lists' line (a_diagnostics: the dry runs' stand-in counts and the engine's lists against DCLF's). */
		void SceneListsLine(const Diagnostics::CutObservers::Counts& a_diagnostics)
		{
			const auto l = PrimaryCull::Get().TakeListCounters();
			const double toMs = MsPerTick();
			const auto& d = a_diagnostics;
			const bool bad = l.unexcluded || d.filterMissed || d.listMissing || d.listExtra || d.keptMissing || d.keptExtra;
			logger::info("[DCLF] scene lists: {} frames: {} kept, {} built by DCLF ({} late; by cause: {} structure or hidden ({}), {} filter, {} globals, {} not DCLF's), {} the engine's ({} not keepable: {}; {} filtered); "
						 "per DCLF build {:.0f} entries ({:.3f} ms), {:.0f} DCLF roots left out a build or filtered frame; filter sets built {} (not published: {} toggled off, {} occlusion maps native, {} snapshot not current, {} no exclusion); "
						 "dry runs {}: {} roots, {} stand-in disagreements; DCLF's lists against the engine's: {} checks, {} of the engine's missing, {} of DCLF's not hidden extra; "
						 "put back for an occlusion map the engine drew on {} frames ({} detached roots left out); filtered frames without the sun's exclusion {}, members lost while out {}; {}{}{}",
				l.frames, l.kept, l.rebuilt, l.lateRebuilds, l.rebuildStructure,
				fmt::format("events: {} object root children, {} category nodes, {} roots ({} applied: {} added, {} removed, {} rebuilt), {} hidden bits", l.structureBy[0],
					l.structureBy[1], l.structureBy[2], l.eventsApplied, l.eventsAdded, l.eventsRemoved, l.eventsRebuilt, l.structureBy[3]),
				l.rebuildFilter, l.rebuildWitness, l.rebuildDirty, l.engineFrames, l.notKeepable,
				fmt::format("{} no hidden events or call site, {} scene, {} not unbound space, {} interior, {} scene children", l.notKeepableBy[0], l.notKeepableBy[1],
					l.notKeepableBy[2], l.notKeepableBy[3], l.notKeepableBy[4]),
				l.filtered, l.rebuilt ? double(l.rebuildEntries) / l.rebuilt : 0.0, l.rebuilt ? l.rebuildTicks * toMs / l.rebuilt : 0.0,
				(l.rebuilt + l.filtered) ? double(l.removed) / double(l.rebuilt + l.filtered) : 0.0, l.built, l.notToggled, l.noOcclusion, l.notCurrent,
				l.noExclusion, l.dryRuns, d.filterChecked, d.filterMissed, d.listChecks, d.listMissing, d.listExtra, l.restored, l.restoreDetached, l.unexcluded,
				l.lostWhileOut,
				ListReasons(l) + (d.keptChecks ? fmt::format("; kept lists against a rebuild: {} checks, {} missing, {} extra{}", d.keptChecks, d.keptMissing, d.keptExtra,
														  d.keptFirst.empty() ? "" : " (first: " + d.keptFirst + ")") :
				                                   ""),
				bad ? " <- LIST FILTER" : " <- OK", d.listFirst.empty() ? "" : "; first: " + d.listFirst);
		}

		/**
		 * @brief The culls off line: what the engine's culls still visit of the entries DCLF lists, by the cut's verdict on each candidate
		 * entry of its snapshot. Whole (admitted, nothing engine-drawn, nothing to hand over): no cull of the engine's reaches it (the lists
		 * leave it out or the list jobs stand in, the sun's exclusion, the point lights' filter). Every other entry keeps the engine's culls,
		 * each class named: mixed (the first engine-drawn geometry), waiting for admission, walked for an unbound member, rejected (why).
		 */
		void CullsOffLine()
		{
			using View = Diagnostics::PrimaryCullView;
			const auto& cut = View::Cut();
			if (!cut.candidates)
				return;
			std::uint64_t whole = 0, mixed = 0, waiting = 0, walked = 0, rejected = 0, sunKept = 0;
			ankerl::unordered_dense::map<const char*, std::pair<std::uint64_t, const RE::NiAVObject*>> reasons;
			std::string mixedFirst;
			std::uint32_t mixedNamed = 0;
			const auto exclusion = SunExclusionSlot::Get().Pending();
			const bool exclusionCurrent = exclusion && exclusion->candidates == cut.candidates && exclusion->excluded.size() >= cut.plans.size();
			const auto nameOf = [](const RE::NiAVObject* a_node) { return a_node && a_node->name.c_str() ? a_node->name.c_str() : ""; };
			for (std::uint32_t e = 0; e < cut.plans.size(); ++e) {
				if (!cut.roots[e])
					continue;
				if (View::Rejected(e)) {
					++rejected;
					auto& reason = reasons[cut.rejectReason[e] ? cut.rejectReason[e] : "?"];
					if (!reason.first++)
						reason.second = cut.roots[e];
					continue;
				}
				if (cut.mixed[e]) {
					++mixed;
					if (mixedNamed++ < 6)
						mixedFirst += fmt::format("{}'{}' by {}", mixedFirst.empty() ? "" : ", ", nameOf(cut.roots[e]), cut.mixedBy[e]);
				} else if (!cut.admitted[e]) {
					++waiting;
				} else if (cut.walk[e]) {
					++walked;
				} else {
					++whole;
					sunKept += exclusionCurrent && !exclusion->excluded[e] ? 1 : 0;
				}
			}
			std::vector<std::pair<const char*, std::pair<std::uint64_t, const RE::NiAVObject*>>> sorted(reasons.begin(), reasons.end());
			std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
			std::string reasonText;
			for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 8); ++i)
				reasonText += fmt::format("{}{} {} (e.g. '{}')", i ? ", " : "", sorted[i].second.first, sorted[i].first, nameOf(sorted[i].second.second));
			logger::info("[DCLF] culls off (T7f item 8): of {} candidate entries {} whole (no engine cull visits them; {} the sun's exclusion "
						 "leaves in the cascades), the engine's culls keep {} mixed, {} waiting for admission, {} walked for an unbound member, {} rejected; rejected by "
						 "the OnVisible the cut cannot stand for: {}; mixed first: {}",
				cut.candidates->Count(), whole, exclusionCurrent ? fmt::format("{}", sunKept) : std::string("?"), mixed, waiting,
				walked, rejected, reasonText.empty() ? "none" : reasonText, mixedFirst.empty() ? "none" : mixedFirst);
		}

		/** @brief The point lights' Accumulate timing and, with the skip installed, its culls', category filter's and bits' lines. */
		std::string LocalLightLines()
		{
			auto c = LocalLightCull::Take();
			const auto p = Diagnostics::LightCulls::Take();
			const auto timing = fmt::format("[DCLF] point lights' Accumulate: {} calls, {:.3f} ms in all", c.accumulateCalls, c.accumulateTicks * MsPerTick());
			if (!c.installed)
				return timing;
			const auto lost = p.lost;
			const auto text = fmt::format("[DCLF] point lights' shadow culls: {} frames, {} with the exclusion ({} none built, {} the mode's claims not live, {} without candidates, {} translated to newer candidates); {} entries visited, {} skipped; parity {} frames: {} paraboloid passes not withheld, {} of them under an excluded entry{}",
				c.frames, c.live, c.noExclusion, c.noClaims, c.stale, c.masked, c.visited, c.skipped,
				p.frames, p.passes, lost, p.frames ? (lost ? " <- LIGHT EXCLUSION" : " <- OK") : "");
			std::string by;
			for (std::size_t r = 0; r < p.lostByReject.size(); ++r)
				if (p.lostByReject[r])
					by += fmt::format(" reject {}={}", r, p.lostByReject[r]);
			const auto withFirst = p.firstLost.empty() ? text : text + "; lost by" + by + "; first lost " + p.firstLost;
			std::string list;
			if (c.filterInstalled) {
				const double filtered = static_cast<double>(c.filtered);
				list = fmt::format("\n[DCLF] point lights' category filter: {} lights, {} filtered, {} walked whole (frames without it); {:.1f} filtered nodes a light ({} now), {:.0f} children culled and {:.0f} cut a light, {} dirty nodes walked; {} builds on the pool ({} nodes, {:.3f} ms each; {} for an older verdict, {} refused a lease); parity {} casters under a filtered node checked, {} not under a child it keeps{}{}",
					c.lights, c.filtered, c.walkedNotLive, c.filtered ? static_cast<double>(c.nodes) / filtered : 0.0, c.filterNodes,
					c.filtered ? static_cast<double>(c.children) / filtered : 0.0, c.filtered ? static_cast<double>(c.cut) / filtered : 0.0, c.dirtyNodes, c.builds,
					c.nodesRebuilt, c.builds ? static_cast<double>(c.buildTicks) * MsPerTick() / static_cast<double>(c.builds) : 0.0, c.buildsStale, c.buildsRefused,
					p.filterChecked, p.filterMissed, p.filterChecked || p.filterMissed ? (p.filterMissed ? " <- LIGHT FILTER" : " <- OK") : "",
					p.filterFirstMissed.empty() ? "" : "; first:" + p.filterFirstMissed);
			}
			auto byReach = [](const auto& a_counts) {
				std::string text;
				for (std::uint32_t k = 0; k <= LocalLightCull::kReachKindCount; ++k)
					if (const auto count = a_counts[k])
						text += fmt::format(" {} {}", LocalLightCull::kReachNames[k], count);
				return text.empty() ? std::string() : " (" + text.substr(1) + ")";
			};
			list += fmt::format("\n[DCLF] point lights' bits for engine main passes under cut entries: {} lights ({} past the mask's 32), {} geometries given them ({} any); parity {} compared, {} agree, {} with engine-only bits{}, {} with DCLF-only bits{}, {} cut but not an excluded entry's{}",
				c.bitLights, c.overflow, c.given, c.withBits, p.compared, p.agree, p.engineOnly, byReach(p.engineOnlyBy), p.dclfOnly, byReach(p.dclfOnlyBy), p.unhandled,
				p.compared || p.unhandled ? (p.engineOnly || p.unhandled ? " <- LIGHT BITS" : " <- OK") : "");
			return timing + "\n" + withFirst + list;
		}
	}

	void PrimaryCullLines(std::uint32_t a_frame)
	{
		using View = Diagnostics::PrimaryCullView;
		auto& cull = PrimaryCull::Get();
		if (!cull.Installed() || !Due(a_frame))
			return;
		// The counters stand until a window with frames reports them.
		if (!View::CutFrames())
			return;
		const auto s = cull.TakeCutCounters();
		const auto d = Diagnostics::CutObservers::Take();
		const double toMs = MsPerTick();
		const double applied = std::max<double>(static_cast<double>(s.appliedFrames), 1.0);
		logger::info("[DCLF] primary exclusion: applied on {} of {} frames ({} without candidates, {} preconditions), {:.1f} entries planned again a frame as their candidates moved ({} pool gatherings; parity {} checks, {} differ, {} entries differing once); per frame {:.0f} eligible entries reached, "
					 "{:.0f} stood in for ({:.0f} walked, {:.0f} of them in view), {:.1f} cross-fading LOD (stood in), {:.1f} not yet admitted ({:.1f} admitted); {:.0f} members in view, {:.1f} not bound yet (the engine's), "
					 "{:.1f} bound but not in the set (the engine's), {:.1f} hidden;  {:.1f} owned geometries left out of the engine's registration; "
					 "{:.0f} of the engine's members in view registered by it; switches: {:.1f} entries culled by the engine (stale child), {:.0f} members unselected, selection read from every switch on {} frames and from {} events' entries; render thread: prepare {:.3f} ms, after the jobs {:.3f} ms; plans on the pool: {} kicked, {} spliced, {} stale, {} refused a lease, {:.1f} entries waiting a frame",
			s.appliedFrames, s.frames, s.skippedStale, s.skippedPreconditions, double(s.entriesPlanned) / applied, s.compactions, d.cutChecks, d.cutMismatches, d.cutLagging, s.seen / applied, s.skipped / applied, s.walked / applied, s.visibleEntries / applied,
			s.notSettled / applied, s.notAdmitted / applied, s.admittedNow / applied, s.members / applied, s.unbound / applied,
			s.undrawable / applied, s.hiddenSkipped / applied, s.excluded / applied,
			s.engineMembers / applied, s.switchStale / applied, s.unselected / applied, s.liveAll, s.liveEntries, s.prepareTicks * toMs / applied, s.afterTicks * toMs / applied,
			s.plansKicked, s.plansSpliced, s.plansStale, s.plansRefused, s.awaiting / applied);
		// The scene membership, registration and resident lines are the coordinator's (from its pass: the joins' statistics are its own).
		SceneListsLine(d);
		CullsOffLine();
		// The features' lines here (Features/TreeWind: the tree animation list's).
		Extensions::ReportLinesAt(Api::ReportPoint::PrimaryCull, a_frame);
		for (const auto& line : std::views::split(LocalLightLines(), '\n'))
			if (const std::string text(line.begin(), line.end()); !text.empty())
				logger::info("{}", text);
		if (View::WalkEverything())
			logger::info("[DCLF] stand-in walk parity: {} geometries handed to the registration from entries the stand-in would not walk{}", d.walkMissed,
				d.walkMissed ? " <- STAND-IN WALK" : " <- OK");
		if (!d.walkMissedFirst.empty())
			logger::info("[DCLF] stand-in walk parity, missed:{}", d.walkMissedFirst);
		if (d.maskChecked)
			logger::info("[DCLF] light masks (owned members in view, which no main registration clears): {} checked, {} with the sun's bits, {} with other lights' bits{}{}",
				d.maskChecked, d.maskSun, d.maskOther, d.maskSun || d.maskOther ? " <- LIGHT MASKS" : " <- OK", d.maskFirst.empty() ? "" : "; first: " + d.maskFirst);
		const double mixedPerFrame = s.mixed / applied;
		if (d.fadePort.checked)
			logger::info("[DCLF] fade port parity (the port against the engine's functions on node copies): {} roots, {} differ{}{}", d.fadePort.checked, d.fadePort.differ,
				d.fadePort.differ ? " <- FADE PORT" : " <- OK", d.fadePort.first.empty() ? "" : "; first: " + d.fadePort.first);
		// CS_DCLF_ACCUMULATED_PARITY: BeginFrameCamera's main camera against the list processes'.
		if (const auto camera = Diagnostics::Accumulated::TakeFrameCamera(); camera.checked)
			logger::info("[DCLF] frame camera (T7f preparation: FadeEye, the camera cut and the height test from the main camera at BeginSceneFrame): {} frames, {} with "
						 "another camera on the list processes{}",
				camera.checked, camera.differ, camera.differ ? " <- FRAME CAMERA" : " <- OK");
		// LOD skins: records whose partitions BuildDraws picks by their root's level in FadeStateCS's state.
		const auto& lodTables = Scene::FrameView::Get().GetTables();
		const auto lodSkins = std::count_if(lodTables.skinLodPartitions.begin(), lodTables.skinLodPartitions.end(), [](std::uint32_t a_word) { return a_word != 0; });
		logger::info("[DCLF] fade roots: {} stood in; {} LOD skins (partitions by their root's level on the GPU); {:.0f} entries with engine-drawn parts culled by the engine a frame",
			Scene::Coordinator::Get().StoodInFadeRoots(), lodSkins, mixedPerFrame);
	}

	std::string FadeWritersLine()
	{
		const auto c = PrimaryCull::Get().TakeFadeWriterCounts();
		if (!c.gateSkipped && !c.onVisibleSkipped)
			return {};
		return fmt::format("[DCLF] fade writers (T7b4; T7f: every listed root): {} fade updates of the animation gate skipped on roots DCLF writes, {} OnVisible fade steps "
						   "skipped ({} roots in the set)\n",
			c.gateSkipped, c.onVisibleSkipped, c.roots);
	}
}
