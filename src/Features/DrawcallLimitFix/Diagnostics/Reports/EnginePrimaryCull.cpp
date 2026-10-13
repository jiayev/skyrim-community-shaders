#include "Engine.h"

#include "Schedule.h"
#include "Features/DrawcallLimitFix/Runtime/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Accumulated.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/CutObservers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/LightCulls.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/PrimaryCullView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Engine/Cull/TreeAnimation.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include <ranges>

namespace DCLF::Reports
{
	namespace
	{
		double TicksToMs()
		{
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			return 1000.0 / static_cast<double>(frequency.QuadPart);
		}

		/** @brief The scene lists' line (a_diagnostics: the dry runs' stand-in counts and the engine's lists against DCLF's). */
		void SceneListsLine(const Diagnostics::CutObservers::Counts& a_diagnostics)
		{
			const auto l = PrimaryCull::Get().TakeListCounters();
			const double toMs = TicksToMs();
			const auto& d = a_diagnostics;
			const bool bad = l.unexcluded || d.filterMissed || d.listMissing || d.listExtra;
			logger::info("[DCLF] scene lists: {} frames: {} kept, {} built by DCLF ({} late; by cause: {} structure or hidden ({}), {} filter, {} globals, {} not DCLF's), {} the engine's ({} not keepable: {}; {} filtered); "
						 "per DCLF build {:.0f} entries ({:.3f} ms), {:.0f} DCLF roots left out a build or filtered frame; filter sets built {} (not published: {} toggled off, {} decal order, {} occlusion maps native, {} snapshot not current, {} no exclusion); "
						 "dry runs {}: {} roots, {} stand-in disagreements; DCLF's lists against the engine's: {} checks, {} of the engine's missing, {} of DCLF's not hidden extra; "
						 "put back for an occlusion map the engine drew on {} frames ({} detached roots left out); filtered frames without the sun's exclusion {}, members lost while out {}{}{}",
				l.frames, l.kept, l.rebuilt, l.lateRebuilds, l.rebuildStructure,
				fmt::format("events: {} object root children, {} category nodes, {} roots ({} applied: {} added, {} removed, {} rebuilt), {} hidden bits", l.structureBy[0],
					l.structureBy[1], l.structureBy[2], l.eventsApplied, l.eventsAdded, l.eventsRemoved, l.eventsRebuilt, l.structureBy[3]),
				l.rebuildFilter, l.rebuildWitness, l.rebuildDirty, l.engineFrames, l.notKeepable,
				fmt::format("{} no hidden events or call site, {} scene, {} not unbound space, {} interior, {} scene children", l.notKeepableBy[0], l.notKeepableBy[1],
					l.notKeepableBy[2], l.notKeepableBy[3], l.notKeepableBy[4]),
				l.filtered, l.rebuilt ? double(l.rebuildEntries) / l.rebuilt : 0.0, l.rebuilt ? l.rebuildTicks * toMs / l.rebuilt : 0.0,
				(l.rebuilt + l.filtered) ? double(l.removed) / double(l.rebuilt + l.filtered) : 0.0, l.built, l.notToggled, l.decalOrder, l.noOcclusion, l.notCurrent,
				l.noExclusion, l.dryRuns, d.filterChecked, d.filterMissed, d.listChecks, d.listMissing, d.listExtra, l.restored, l.restoreDetached, l.unexcluded,
				l.lostWhileOut, bad ? " <- LIST FILTER" : " <- OK", d.listFirst.empty() ? "" : "; first: " + d.listFirst);
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
			const auto exclusion = SunAccumulation::Get().PendingExclusion();
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

		/** @brief The tree animation list's line (a_checks, a_onList: the persistent parity's owned trees still on the list). */
		std::string TreeAnimationLine(std::uint64_t a_checks, std::uint64_t a_onList)
		{
			const auto s = TreeAnimation::Take();
			if (!s.installed)
				return fmt::format("[DCLF] tree animation list: off (CS_DCLF_TREE_LIST=0): {} on the list (the manager's last update {:.3f} ms)", s.listed, s.updateMs);
			const auto text = fmt::format("[DCLF] tree animation list: {} on the list, {} owned trees off it (the manager's last update {:.3f} ms); {} syncs: {} taken off, {} put back; engine: {} adds ({} taken off again), {} removes of trees off the list, {} clears; parity {} checks, {} owned trees on the list{}; "
										  "tree list (T7b5 fix): {} stood-in BSTreeNodes, {} loops given DCLF's verdicts: {} in / {} out (replacing the engine's {} stale / {} in / {} out), {} without a multibound",
				s.listed, s.held, s.updateMs, s.syncs, s.takenOff, s.putBack, s.engineAdds, s.takenOffAgain, s.engineRemoves, s.clears, a_checks, a_onList,
				a_checks ? (a_onList ? " <- TREE LIST" : " <- OK") : "", s.ownedTrees, s.treeLoops, s.treesIn, s.treesOut, s.replaced[0], s.replaced[1], s.replaced[2],
				s.noMultiBound);
			// With CS_DCLF_TREE_LIST_ALL: every BSTreeNode DCLF lists.
			const auto wideText = !SwitchEnabled(Switch::TreeListAll) ? std::string() : fmt::format(
				"; every BSTreeNode (T7f; CS_DCLF_TREE_LIST_ALL): {} in / {} out written (replacing the engine's {} stale / {} in / {} out); against the engine's {} same, {} DCLF's in "
				"and the engine's out, {} DCLF's out and the engine's in, {} the engine's stale; aside {} without DCLF's verdict, {} without a multibound",
				s.wideIn, s.wideOut, s.wideReplaced[0], s.wideReplaced[1], s.wideReplaced[2], s.drySame, s.dryInOut, s.dryOutIn,
				s.dryStale, s.wideNoVerdict, s.wideNoMultiBound);
			return text + wideText;
		}

		/** @brief The point lights' Accumulate timing and, with the skip installed, its culls', category filter's and bits' lines. */
		std::string LocalLightLines()
		{
			auto c = LocalLightCull::Take();
			const auto p = Diagnostics::LightCulls::Take();
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			const auto timing = fmt::format("[DCLF] point lights' Accumulate: {} calls, {:.3f} ms in all", c.accumulateCalls, c.accumulateTicks * 1000.0 / static_cast<double>(frequency.QuadPart));
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
				list = fmt::format("\n[DCLF] point lights' category filter: {} lights, {} filtered, {} walked whole (frames without it); {:.1f} filtered nodes a light ({} now), {:.0f} children culled and {:.0f} cut a light, {} dirty nodes walked; {} builds ({} nodes, {:.3f} ms each); parity {} casters under a filtered node checked, {} not under a child it keeps{}{}",
					c.lights, c.filtered, c.walkedNotLive, c.filtered ? static_cast<double>(c.nodes) / filtered : 0.0, c.filterNodes,
					c.filtered ? static_cast<double>(c.children) / filtered : 0.0, c.filtered ? static_cast<double>(c.cut) / filtered : 0.0, c.dirtyNodes, c.builds,
					c.nodesRebuilt, c.builds ? static_cast<double>(c.buildTicks) * 1000.0 / static_cast<double>(frequency.QuadPart) / static_cast<double>(c.builds) : 0.0,
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
		const double toMs = TicksToMs();
		const double applied = std::max<double>(static_cast<double>(s.appliedFrames), 1.0);
		logger::info("[DCLF] primary exclusion: applied on {} of {} frames ({} without candidates, {} preconditions), {:.1f} entries planned again a frame as their candidates moved ({} pool gatherings; parity {} checks, {} differ); per frame {:.0f} eligible entries reached, "
					 "{:.0f} stood in for ({:.0f} walked, {:.0f} of them in view), {:.1f} cross-fading LOD (stood in), {:.1f} not yet admitted ({:.1f} admitted); {:.0f} members in view, {:.1f} not bound yet (the engine's), "
					 "{:.1f} bound but not in the set (the engine's), {:.1f} hidden;  {:.1f} owned geometries left out of the engine's registration; "
					 "{:.0f} of the engine's members in view registered by it; switches: {:.1f} entries culled by the engine (stale child), {:.0f} members unselected, selection read from every switch on {} frames and from {} events' entries; render thread: prepare {:.3f} ms, after the jobs {:.3f} ms",
			s.appliedFrames, s.frames, s.skippedStale, s.skippedPreconditions, double(s.entriesPlanned) / applied, s.compactions, d.cutChecks, d.cutMismatches, s.seen / applied, s.skipped / applied, s.walked / applied, s.visibleEntries / applied,
			s.notSettled / applied, s.notAdmitted / applied, s.admittedNow / applied, s.members / applied, s.unbound / applied,
			s.undrawable / applied, s.hiddenSkipped / applied, s.excluded / applied,
			s.engineMembers / applied, s.switchStale / applied, s.unselected / applied, s.liveAll, s.liveEntries, s.prepareTicks * toMs / applied, s.afterTicks * toMs / applied);
		// The scene membership, registration and resident lines are the coordinator's (from its pass: the joins' statistics are its own).
		SceneListsLine(d);
		CullsOffLine();
		std::uint64_t treeChecks = 0, treeOnList = 0;
		if (TreeAnimation::Installed() && Diagnostics::Observe().scene)
			treeChecks = TreeAnimation::OwnedOnList(treeOnList) ? 1 : 0;
		logger::info("{}", TreeAnimationLine(treeChecks, treeOnList));
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
		if (d.visibilityChecked)
			logger::info("[DCLF] fade visibility parity (the engine's cull of entries with engine-drawn parts against FadeStateCS's test): {} checked, {} differ{}{}",
				d.visibilityChecked, d.visibilityDiffer, d.visibilityDiffer ? " <- FADE VISIBILITY" : " <- OK", d.visibilityFirst.empty() ? "" : "; first: " + d.visibilityFirst);
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
