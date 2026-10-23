#include "Scene.h"

#include "Engine.h"
#include "Schedule.h"
#include "Spread.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneObservers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneState.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

#include <ranges>
#include <sstream>

// The coordinator's report lines (Coordinator's scene pass): the pump, the set, the membership, and under CS_DCLF_STATS the scene walk's
// statistics and the observers' lines that ride with them.

namespace DCLF::Reports
{
	namespace
	{
		using V = Diagnostics::SceneView;

		/**
		 * @brief The table families' kept counts and the publications that replayed their marked rows instead of copying them whole ("<name>
		 * kept N (M replayed)", joined by ", ": Features/TreeWind's trees, then the fade roots).
		 */
		std::string FamiliesKept(const std::array<std::uint64_t, Extensions::kTableFamilySlots>& a_kept)
		{
			std::string line;
			const auto families = Extensions::RegisteredTableFamilies();
			for (std::size_t f = 0; f < families.size(); ++f)
				line += fmt::format("{}{} kept {} ({} replayed)", f ? ", " : "", families[f].name, a_kept[f], Extensions::TakeTableFamilyReplays(static_cast<std::uint32_t>(f)));
			return line;
		}

		std::string FanoutLine(Scene::Coordinator& s)
		{
			auto& f = V::fanoutStats(s);
			if (!f.rounds && !f.joinRounds && !f.subtreeRounds)
				return {};
			auto line = fmt::format(
				"[DCLF] scene fan-out (T6b3e, CS_DCLF_FANOUT={}, {} pool workers): walk rounds {} ({} on the pool), entries {} ({} on the pool, {} chunks), "
				"light-path entries written in full at the merge {}; join rounds {} ({} on the pool), joins {}",
				ConfiguredFanout(), SceneScheduler::PreparationWorkers(), f.rounds, f.parallelRounds, f.entries, f.parallelEntries, f.chunks, f.lightMissed, f.joinRounds,
				f.joinParallelRounds, f.joinEntries);
			// AddSubtrees' prepares (the pool from kSubtreeFanoutMin roots) and merges; their times in modes 1 and 2.
			line += fmt::format("; subtree rounds {} ({} on the pool), roots {} ({} on the pool), geometries {}, prepare {:.2f} ms, merge {:.2f} ms", f.subtreeRounds,
				f.subtreeParallelRounds, f.subtreeRoots, f.subtreeParallelRoots, f.subtreeGeometries, f.subtreePrepareMs, f.subtreeMergeMs);
			f = {};
			return line;
		}

		std::string ParallelForLine()
		{
			// Cumulative per name in the executor: the differences since the last report (this one's alone), the peaks taken.
			static std::unordered_map<std::string, SceneExecutor::ParallelForStatistics> last;
			std::string line;
			for (const auto& now : SceneScheduler::Executor().GetParallelForStatistics(true)) {
				auto& before = last[now.name];
				if (now.calls != before.calls)
					line += fmt::format("{}{}: {} calls, {} chunks ({} by helpers), {} blocked {:.3f} ms/frame (peak {:.0f} us), {} boosts, {} idle helpers",
						line.empty() ? "" : "; ", now.name, now.calls - before.calls, now.chunks - before.chunks, now.helperChunks - before.helperChunks,
						now.blocked - before.blocked, double(now.blockedNanoseconds - before.blockedNanoseconds) / 1e6 / kInterval,
						double(now.maxBlockedNanoseconds) / 1e3, now.boosts - before.boosts, now.idleHelpers - before.idleHelpers);
				before = now;
			}
			return line.empty() ? line : "[DCLF] parallel-for waits (W7: callers blocked on claimed chunks): " + line;
		}

		std::string PlacementLine(Scene::Coordinator& s)
		{
			auto& p = V::placementStats(s);
			std::string line;
			std::string writers;
			for (std::size_t i = 0; i < Scene::moveWriterCalls.size(); ++i)
				if (const auto calls = Scene::moveWriterCalls[i].exchange(0, std::memory_order_relaxed))
					writers += fmt::format("{}{} {}", writers.empty() ? "" : ", ", Scene::moveWriterNames[i], calls);
			if (p.roots || p.rootsGated)
				line = fmt::format("[DCLF] placement plan: roots: {} listed ({} of still roots on a move event), {} left for want of a move event", p.roots, p.stillRoots,
					p.rootsGated);
			if (!writers.empty())
				line += fmt::format("{}move writers' calls: {}", line.empty() ? "[DCLF] placement plan: " : "; ", writers);
			if (p.fadeRootsUnmirrored)
				line += fmt::format("{}{} fade roots listed with no mirror record yet (their seeds hold the node's values)", line.empty() ? "[DCLF] placement plan: " : "; ", p.fadeRootsUnmirrored);
			p = {};
			return line;
		}

		std::string MirrorLines(Scene::Coordinator& s)
		{
			auto& m = V::mirror(s);
			auto& applied = V::mirrorApplied(m);
			auto& detached = V::mirrorDetached(m);
			auto& updates = V::mirrorUpdates(m);
			auto& updatesUnheld = V::mirrorUpdatesUnheld(m);
			auto& lightEvictions = V::mirrorLightEvictions(m);
			auto& evictions = V::mirrorEvictions(m);
			auto& evictedRecords = V::mirrorEvictedRecords(m);
			auto& firstEviction = V::mirrorFirstEviction(m);
			auto& replayed = V::mirrorReplayed(m);
			auto& stale = V::mirrorStale(m);
			auto& superseded = V::mirrorSuperseded(m);
			auto& updated = V::mirrorUpdated(m);
			std::string text = fmt::format(
				"[DCLF] scene mirror (6e F3): {} nodes, {} geometries, {} properties, {} alphas; {} captures applied, {} detaches, {} updates ({} to objects it holds "
				"no record of)\n",
				V::mirrorNodes(m).size(), V::mirrorGeometries(m).size(), V::mirrorProperties(m).size(), V::mirrorAlphas(m).size(), std::exchange(applied, 0), std::exchange(detached, 0), std::exchange(updates, 0),
				std::exchange(updatesUnheld, 0));
			if (lightEvictions)
				text += fmt::format("[DCLF] scene mirror evictions (T6b1b): {} lights and cameras named by an attach out of the world (detached by no hook)\n", std::exchange(lightEvictions, 0));
			if (evictions)
				text += fmt::format("[DCLF] scene mirror evictions (T6b1b): {} records named by an attach out of the world ({} records with them: left the world by no detach a hook saw) <- MIRROR EVICTED; first: {}\n",
					std::exchange(evictions, 0), std::exchange(evictedRecords, 0), std::exchange(firstEviction, {}));
			text += fmt::format(
				"[DCLF] scene mirror order (6e F3c): {} updates replayed after a capture numbered before them, {} skipped as older than their record's capture, {} "
				"records a capture left to a newer one\n",
				std::exchange(replayed, 0), std::exchange(stale, 0), std::exchange(superseded, 0));
			auto line = [&](const char* a_label, const auto& a_counts) {
					std::string out;
					for (std::size_t type = 0; type < SceneMirror::kTypes; ++type) {
						for (std::uint32_t f = 0; f < SceneMirror::kMaxFields; ++f) {
							if (!a_counts[type][f])
								continue;
							const char* name = type == 0 ? (f < SceneCapture::NodeRecord::kFieldCount ? SceneCapture::NodeRecord::kFieldNames[f] : "?") :
							                   type == 1 ? (f < SceneCapture::GeometryRecord::kFieldCount ? SceneCapture::GeometryRecord::kFieldNames[f] : "?") :
							                   type == 2 ? (f < SceneCapture::PropertyRecord::kFieldCount ? SceneCapture::PropertyRecord::kFieldNames[f] : "?") :
							                               (f < SceneCapture::AlphaRecord::kFieldCount ? SceneCapture::AlphaRecord::kFieldNames[f] : "?");
							out += fmt::format("{}{} {} {}", out.empty() ? "" : ", ", SceneMirror::kTypeNames[type], name, a_counts[type][f]);
						}
					}
					return fmt::format("{} {}", a_label, out.empty() ? "0" : out);
				};
			text += fmt::format("[DCLF] scene mirror, {}\n", line("updated fields:", updated));
			updated = {};
			text += Diagnostics::TakeMirrorParityLines();
			return text;
		}

		std::string MirrorReadLines(Scene::Coordinator& s)
		{
			auto& m = V::mirrorReads(s);
			std::string text;
			for (std::size_t r = 0; r < m.reads.size(); ++r) {
				if (!m.reads[r])
					continue;
				text += fmt::format("{}{} {} reads ({} with a record missing){}{}", text.empty() ? "" : "; ", V::kMirrorReadNames[r], m.reads[r], m.unmirrored[r],
					m.checked[r] ? fmt::format(", {} checked, {} lagged a batch, {} differ{}{}", m.checked[r], m.lagged[r], m.differ[r], m.differ[r] ? ": " : "", m.first[r]) : std::string(),
					m.refused[r] ? fmt::format(", {} checks skipped outside the read window", m.refused[r]) : std::string());
			}
			bool differ = false;
			for (const auto n : m.differ)
				differ |= n != 0;
			const bool checked = std::ranges::any_of(m.checked, [](auto n) { return n != 0; });
			m = {};
			if (text.empty())
				return {};
			return fmt::format("[DCLF] mirror reads (T6b1b): {}{}\n", text, checked ? (differ ? " <- MIRROR READ" : " <- OK") : "");
		}

		std::string SceneLines(Scene::Coordinator& s)
		{
			using Tables = SceneTables;
			const auto& tables = V::tables(s);
			auto& delta = V::delta(s);
			auto& reportedChangeCounts = V::reportedChangeCounts(s);
			std::string text;
			// The change log (Tables::changeLog): notes per frame by cause, and its completeness check.
			if (const double n = delta.walks) {
				std::uint64_t total = 0;
				std::string causes;
				for (std::uint32_t i = 0; i < kChangeCauseCount; ++i) {
					const auto notes = tables.changeCounts[i] - reportedChangeCounts[i];
					total += notes;
					if (notes)
						causes += fmt::format("{}{} {:.1f}", causes.empty() ? "" : ", ", kChangeCauseNames[i], notes / n);
				}
				reportedChangeCounts = tables.changeCounts;
				text += fmt::format("[DCLF] change log: {:.1f} changes a frame, by column: {} ({} entries held)\n", static_cast<double>(total) / n,
					causes.empty() ? "-" : causes, tables.changeLog.Size());
			}
			// The material records kept by the scene work (RefreshMaterialRecords), per scene pass.
			if (auto& r = V::materialRecordStats(s); r.passes) {
				const double n = static_cast<double>(r.passes);
				text += fmt::format("[DCLF] material records (the scene work's, from captures and the frame's sources): {:.1f} captures applied, {:.2f} records "
									"rewritten, {} rewrites waiting on a later frame's sources, {} uncovered; {:.1f} signature samples, {:.2f} applications to "
									"{:.1f} slots; {:.1f} watched transforms, {:.2f} written a pass{}\n",
					r.captured / n, r.rewritten / n, r.retried, r.uncovered, r.samples / n, r.applications / n, r.slotsApplied / n, r.transformsWatched / n,
					r.transformsWritten / n, r.uncovered ? " <- MATERIAL PORT" : "");
				r = {};
			}
			// The features' lines here (Features/Decals' order parity), the components' with their context.
			V::componentsReportText(s, Api::ReportPoint::SceneCoordinatorMaterials, text);
			if (auto& ms = V::materialSnapshotStats(s); ms.captured || ms.made || ms.requested) {
				text += fmt::format("[DCLF] material captures (T6b2a: the scene work's records): {} captured ({} replacing an older one, {} older than the held one), {} "
									"let go unused, {} held; {} records made by the scene work, {} uncovered, {} asked of the render thread{}\n",
					ms.captured, ms.replaced, ms.stale, ms.aged, V::materialSnapshots(s).size(), ms.made, ms.uncovered, ms.requested, ms.uncovered ? " <- MATERIAL PORT" : "");
				if (ms.fieldsMoved) {
					// The words that moved most (offset: count), the reference count's (+0x8) aside.
					std::vector<std::pair<std::uint32_t, std::uint32_t>> words;
					for (std::uint32_t w = 0; w < ms.wordMoved.size(); ++w)
						if (ms.wordMoved[w] && w != 2)
							words.emplace_back(ms.wordMoved[w], w * 4);
					std::sort(words.begin(), words.end(), std::greater<>());
					std::string classes;
					for (const auto& [vtable, count] : ms.fieldsMovedBy)
						if (vtable)
							classes += fmt::format("{}image+{:#x}: {}", classes.empty() ? "" : ", ", reinterpret_cast<std::uintptr_t>(vtable) - REL::Module::get().base(), count);
					std::string list;
					for (std::size_t k = 0; k < words.size() && k < 12; ++k)
						list += fmt::format("{}{:#x}: {}", k ? ", " : "", words[k].second, words[k].first);
					text += fmt::format("[DCLF] material captures whose fields moved: {} (first vtable image+{:#x}); words moved {}; by vtable {}\n", ms.fieldsMoved,
						reinterpret_cast<std::uintptr_t>(ms.fieldsMovedVtable) - REL::Module::get().base(), list, classes);
				}
				ms = {};
			}
			// The material bindings the scene work makes.
			if (auto& mb = V::materialBindings(s); mb.stats.requested || mb.stats.answered || mb.stats.versioned || !mb.inFlight.empty()) {
				std::size_t waiting = 0, holding = 0;
				Tables::ForEachBit(tables.usedMaterialBits, [&](std::uint32_t a_slot) {
					if (a_slot < V::lookups(s).materials.size() && V::lookups(s).materials[a_slot].key.first) {
						waiting += V::lookups(s).materials[a_slot].resolved ? 0 : 1;
						holding += a_slot < mb.held.size() && mb.held[a_slot] ? 1 : 0;
					}
				});
				const auto& bs = mb.stats;
				text += fmt::format("[DCLF] material bindings (T6b2c: the scene work's): {} views requested, {} answered ({} rejected, {} stale), {} served from the "
									"answers kept ({} kept); {} entries resolved, {} retired, {} changes versioned into the scene lane's lookups; pending now: {} views "
									"requested and not answered, {} used slots waiting, {} holding their previous view\n",
					bs.requested, bs.answered, bs.rejected, bs.stale, bs.cached, mb.cache.size(), bs.resolved, bs.retired, bs.versioned, mb.inFlight.size(), waiting, holding);
				mb.stats = {};
			}
			// The shared, projected, mask and shadow texture bindings the scene work makes.
			if (auto& sb = V::sharedBindings(s); sb.stats.fixedPublished || sb.stats.projectedCaptures || sb.stats.shadowAdded || sb.stats.shadowRemoved ||
											 sb.stats.writtenShared || sb.stats.writtenMasks || sb.stats.writtenShadow || !sb.inFlight.empty()) {
				// What is still asked for now, by the kinds waiting on each view in flight; the projected textures and masks waiting, by entry.
				std::array<std::size_t, SharedBindings::kKinds> waitingByKind{};
				for (const auto& waitingView : sb.inFlight)
					for (std::uint32_t kind = 0; kind < SharedBindings::kKinds; ++kind)
						waitingByKind[kind] += (waitingView.second >> kind) & 1;
				std::size_t masksWaiting = 0;
				Tables::ForEachBit(tables.usedPipelineBits, [&](std::uint32_t a_slot) {
					if (a_slot < sb.masks.size() && sb.masks[a_slot].wanted && !sb.masks[a_slot].settled)
						++masksWaiting;
				});
				const auto& ss = sb.stats;
				std::string byKind;
				for (std::uint32_t kind = 0; kind < SharedBindings::kKinds; ++kind)
					byKind += fmt::format("; {}: {} views requested, {} answered ({} rejected), {} served from the answers kept, {} in flight now", SharedBindings::kKindNames[kind],
						ss.requested[kind], ss.answered[kind], ss.rejected[kind], ss.cached[kind], waitingByKind[kind]);
				text += fmt::format("[DCLF] shared bindings (T6b2c: the scene work's): fixed (null, samplers) {} ({} taken); projected: {} captures taken, {} textures "
									"pending; masks: {} used waiting, {} passes holding the previous view, {} rows naming a view the frame does not hold{}; shadow "
									"textures: {} added, {} removed, {} in the index, {} pending; {} stale answers; changes written into the scene lane's lookups: "
									"shared {}, masks {}, shadow textures {}{}\n",
					sb.fixed ? "published" : "not published yet", ss.fixedPublished, ss.projectedCaptures, std::popcount(sb.projectedPending), masksWaiting,
					ss.masksHeld, ss.masksUnheld, ss.masksUnheld ? " <- MASK UNHELD" : "", ss.shadowAdded, ss.shadowRemoved, sb.shadowTextures.size(),
					sb.shadowPending, ss.stale, ss.writtenShared, ss.writtenMasks, ss.writtenShadow, byKind);
				sb.stats = {};
			}
			// The features' lines here (Features/Lod's object LOD segments), the components' with their context.
			V::componentsReportText(s, Api::ReportPoint::SceneCoordinatorSegments, text);
			if (auto& c = Diagnostics::SceneDiagnostics().changeLog; c.checks || c.skipped) {
				text += fmt::format("[DCLF] change log parity: {} checks ({} skipped), {} slots compared, {} changed, {} changed with no log entry{}{}\n", c.checks, c.skipped, c.slots,
					c.changed, c.missing, c.missing ? " <- MISSING; first: " : " <- OK", c.first);
				c = { std::move(c.snapshot), c.cursor };
			}
			text += MirrorLines(s);
			text += MirrorReadLines(s);
			if (auto& r = V::referenceStats(s); r.pins || r.live || r.refused || r.properties || r.propertiesRefused) {
				text += fmt::format("[DCLF] references (T6b1c): {} pins in the batches' captures; {} references made from pins, {} from a live walk's pointers, {} refused (no pin){}; "
									"{} tracked geometries' properties held anew, {} with no pin{}{}\n",
					r.pins, r.pinned, r.live, r.refused, " (a refusal is checked after the next batch: the mirror reads' \"unpinned geometry\")", r.properties, r.propertiesRefused,
					r.propertiesRefused ? " <- UNPINNED PROPERTY; first: " : "", r.firstPropertyRefused);
				// The fade nodes the tree and fade-root owners are copied from: pins, root owners, the render thread's answers.
				if (r.fadeNodes || r.fadeNodesRefused || r.fadeRootsUnowned || r.fadeNodesAnswered || r.fadeNodesAnsweredNone)
					text += fmt::format("[DCLF] fade node references (T6b3e): {} held anew from pins, {} with no pin ({} copied from a root owner, {} asked of the render "
										"thread: {} answered, {} answered none), {} tree or fade roots left unlisted for want of one (until an answer){}{}{}\n",
						r.fadeNodes, r.fadeNodesRefused, r.fadeNodesFromRoots, r.fadeNodesRequested, r.fadeNodesAnswered, r.fadeNodesAnsweredNone, r.fadeRootsUnowned,
						r.fadeNodesRequested > r.fadeNodesAnswered + r.fadeNodesAnsweredNone || r.fadeNodesAnsweredNone ? " <- UNPINNED FADE NODE" : " <- OK",
						r.fadeNodeFirst.empty() ? "" : "; first without a pin or a root owner: ", r.fadeNodeFirst);
				r = {};
			}
			if (auto& l = V::leafStats(s); l.missing || l.rescheduled) {
				text += fmt::format("[DCLF] leaves (T6b1b): {} records written while the mirror held no records of the geometry, {} evaluated again when a capture named it\n",
					l.missing, l.rescheduled);
				l = {};
			}
			if (const auto c = SceneCapture::TakeCounters(); c.attaches || c.outOfWorld)
				text += fmt::format("[DCLF] scene capture (6e F3): {} attaches in the world captured ({} records, {:.1f} us in all; {} on the main thread, {:.1f} us), {} out of the "
									"world left to their world attach\n",
					c.attaches, c.records, static_cast<double>(c.ns) / 1000.0, c.mainThread, static_cast<double>(c.mainThreadNs) / 1000.0, c.outOfWorld);
			// The coordinator's half of the frame capture's line (the subtrees its passes waited for, the category nodes it took).
			if (auto &subtreesPended = V::subtreesPended(s), &subtreesDropped = V::subtreesDropped(s), &categoryChildrenMissing = V::categoryChildrenMissing(s);
				subtreesPended || subtreesDropped || categoryChildrenMissing) {
				text += fmt::format("[DCLF] subtrees (T6b1c, the coordinator's): {} waited for the mirror, {} dropped with no chain after; {} new category nodes without the "
									"capture's children{}\n",
					subtreesPended, subtreesDropped, categoryChildrenMissing, categoryChildrenMissing ? " <- CAPTURE" : " <- OK");
				subtreesPended = subtreesDropped = categoryChildrenMissing = 0;
			}
			if (auto& t = delta; t.walks) {
				const double n = t.walks;
				// The catch-ups are the render thread's, at the frame's start (FrameView's CatchUpSwitches).
				t.switchCatchUps = Scene::Channels::Get().catchUpsBySwitch.exchange(0, std::memory_order_relaxed);
				t.attachCatchUps = Scene::Channels::Get().catchUpsByAttach.exchange(0, std::memory_order_relaxed);
				const double catchUpUs = static_cast<double>(Scene::Channels::Get().catchUpNs.exchange(0, std::memory_order_relaxed)) / 1000.0 / n;
				text += fmt::format("[DCLF] candidates: sun {} entries ({} indices, {} geometry rows), {} written in {} snapshots ({} again for a geometry moved "
									"from another entry); light {} entries ({} indices), {} written in {} snapshots ({} again); parity {} checks, {} differ{}\n",
					V::sunTable(s).index.size(), V::sunTable(s).nodes.size(), V::sunTable(s).geometryIndex.size(), std::exchange(V::sunTable(s).entriesWritten, 0), std::exchange(V::sunTable(s).snapshots, 0),
					std::exchange(V::sunTable(s).rowsTakenBack, 0), V::lightTable(s).index.size(), V::lightTable(s).nodes.size(), std::exchange(V::lightTable(s).entriesWritten, 0),
					std::exchange(V::lightTable(s).snapshots, 0), std::exchange(V::lightTable(s).rowsTakenBack, 0),
					Diagnostics::SceneDiagnostics().candidates.checks, Diagnostics::SceneDiagnostics().candidates.mismatches, Diagnostics::SceneDiagnostics().candidates.checks ? (Diagnostics::SceneDiagnostics().candidates.mismatches ? " <- CANDIDATES" : " <- OK") : "");
				Diagnostics::SceneDiagnostics().candidates = {};
				text += fmt::format("[DCLF] scene delta: {} walks ({} full), evaluated {:.0f}/frame (max {}; per-frame {:.0f}, of them {:.0f} kept by the light path and {:.0f} moved by it; pending {:.0f}, property {:.1f}, node {:.1f}, sun entry node {:.1f}, geometry {:.1f}), lapsed {:.0f}, {:.0f} live slots; events per frame: {:.1f} property, {:.1f} node; switch events {:.2f}/frame: {:.2f} changed, {:.2f} caught up by the render thread at ingestion ({:.2f} under attached subtrees or the world after a load; {:.2f} us/frame), {:.1f} entries classified again",
					t.walks, t.full, t.evaluated / n, t.evaluatedMax, t.perFrame / n, t.kept / n, t.moved / n, t.pending / n, t.property / n, t.node / n, t.roots / n,
					t.geometryDirty / n, t.restored / n, t.live / n, t.propertyEvents / n, t.nodeEvents / n,
					t.switchEvents / n, t.switchChanges / n, t.switchCatchUps / n, t.attachCatchUps / n, catchUpUs, t.switchReclassified / n);
				// The features' fragments of the line (Features/Faces' publications), the components' with their context.
				V::componentsReportText(s, Api::ReportPoint::SceneCoordinatorDelta, text);
				text += '\n';
				if (auto& rootMotion = V::rootMotion(s); rootMotion.size() > (1u << 16))
					rootMotion.clear();
				t = {};
			}
			return text;
		}

		void Coordinator(Scene::Coordinator& s, std::uint32_t a_sceneFrame)
		{
			// The coordinator's own lines, composed in its pass from its own state as its frame crosses the report window; the frame's report
			// reads only the frame's. "A frame" in the lines below that the walk counts is a walk (a pass).
			auto& ps = V::pumpStats(s);
			auto& stats = V::stats(s);
			{
				const std::uint32_t span = ps.any ? ps.lastFrame - ps.firstFrame + 1 : 0u;
				logger::info("[DCLF] scene pump (T6b3d): {} passes over {} frames ({:.2f} a frame, at most {} in one, {} frames without one); {} whole ({} walks took the "
							 "per-frame entries, {} their events alone), {} applying the events alone (DCLF not running), {} under a load screen, {} inline (the parities), "
							 "{} threw; pass ms (avg/p95/max) {}; events a pass (p50/p95/max) {}; {} publications, {} passes changed nothing (none published); event to "
							 "publication: ms (p50/p95/max) {}, frames (p50/p95/max) {}",
					ps.passes, span, span ? double(ps.passes) / double(span) : 0.0, ps.maxPassesInFrame, span > ps.framesWithPass ? span - ps.framesWithPass : 0u,
					ps.passes - ps.eventsOnly, ps.perFrameWalks, ps.eventWalks, ps.eventsOnly, ps.loading, ps.inlinePasses, ps.failed, Spread(ps.passMs),
					Percentiles(ps.passEvents), ps.published, ps.unchanged, Percentiles(ps.publishMs), Percentiles(ps.publishFrames));
				// By source: the passes it woke / those of them that changed nothing (a pass counts under every source it took; "none": a pass
				// woken by no source, inline or a burst's continuation).
				std::string bySource;
				for (std::size_t source = 0; source <= static_cast<std::size_t>(SceneWake::Count); ++source)
					if (ps.bySource[source])
						bySource += fmt::format("{}{} {}/{}", bySource.empty() ? "" : ", ", source < kSceneWakeNames.size() ? kSceneWakeNames[source] : "none", ps.bySource[source],
							ps.unchangedBySource[source]);
				logger::info("[DCLF] scene pump wakes by source (T6b3d: passes woken / of them changed nothing): {}", bySource.empty() ? "-" : bySource);
				std::string deferred;
				const auto held = TakeScenePassDeferred();
				for (std::size_t source = 0; source < held.size(); ++source)
					if (held[source])
						deferred += fmt::format("{}{} {}", deferred.empty() ? "" : ", ", kSceneWakeNames[source], held[source]);
				logger::info("[DCLF] scene pump pushes inside the frame's render, not woken (T6b3d: the frame's next pass takes them): {}",
					deferred.empty() ? "-" : deferred);
				// The walk's rounds and the joins evaluated on the pool.
				if (const auto fanout = FanoutLine(s); !fanout.empty())
					logger::info("{}", fanout);
				// The attach burst's mirror prepares, subtree geometries and category index (W8).
				if (const auto attach = V::attachLine(s); !attach.empty())
					logger::info("{}", attach);
				// The executor's ParallelFors: what their callers waited for chunks a helper had claimed.
				if (const auto parallel = ParallelForLine(); !parallel.empty())
					logger::info("{}", parallel);
				// CS_DCLF_RELEASE_GUARD: the engine's last releases on DCLF's threads (any is a defect).
				if (const auto releases = ReleaseGuardLine(); !releases.empty())
					logger::info("{}", releases);
				// The LOD gates' line (opened, flipped, retired; the open ones' age and what they wait for: their component's, the first registered here)
				// and the features' lines here.
				V::componentsReportLines(s, Api::ReportPoint::SceneCoordinatorGates, a_sceneFrame);
			}
			const std::uint64_t wholePasses = ps.passes - ps.eventsOnly;
			const double fullPasses = std::max<double>(1.0, static_cast<double>(wholePasses));
			const double fullMs = ps.fullMs, fullMaxMs = ps.fullMaxMs;
			ps = {};
			// The set (its commits are the passes'), and the claims its passes took back.
			{
				const auto set = s.TakeSetStats();
				const auto commit = std::exchange(Diagnostics::SceneDiagnostics().commit, {});
				const double commits = std::max<double>(static_cast<double>(set.commits), 1.0);
				logger::info("[DCLF] DCLF set: {:.0f} members and {:.1f} bound objects waiting a frame ({:.1f} with some of their phases ready: whole-object claims) over {} commits; {} joined, {} left, {} evaluated ({} readiness events taking {} waiting ones again, {} members past a shrunk fit, {} "
							 "resyncs), {} members bound again by the joins before the commit (T6b3c: kept when ready at it), {} publications; waiting for: pipeline {}, material {}, shadow mask {}, shared lookups {}, "
							 "geometry {}, decal slot {}, layer partner {}, shadow pipelines {}, reflection (forward pipeline) {}, constants {}, scene buffers' growth {}, past the scene buffers (shadow) {}, a shadow diffuse not imported {}{}{}; members patched by the accumulate phase {}{}; {} left the last commit's decision (LeaveSet: a rebind before this commit, a revocation after); commit parity {} checks, {} slots differ{}",
					set.members / commits, set.waiting / commits, set.partial / commits, set.commits, set.joined, set.left, set.evaluated, set.readinessEvents, set.waitingRequeued, set.fitRequeued, set.resyncs, set.rebinding,
					set.publications, set.waitingBy[0], set.waitingBy[1], set.waitingBy[2], set.waitingBy[3], set.waitingBy[4], set.waitingBy[5], set.waitingBy[6], set.waitingBy[7], set.waitingBy[8],
					set.waitingBy[9], set.waitingBy[10], set.waitingBy[12], set.waitingBy[13], set.firstWaiting.empty() ? "" : "; first: ", set.firstWaiting, set.patchedMember, set.patchedMember ? " <- SET PATCHED" : "", set.leftAfterCommit, commit.commitParityChecks, commit.commitParityDiffer,
					commit.commitParityChecks ? (commit.commitParityDiffer ? " <- COMMIT" : " <- OK") : "");
				// The features' set lines (Features/FirstPerson's root view filing and the slots its decisions leave out).
				V::componentsReportLines(s, Api::ReportPoint::SceneCoordinatorSet, a_sceneFrame);
				// Why the members that left did (each leave once, by its commit's verdict).
				std::string waitingText;
				for (std::size_t why = 0; why < set.leftWaiting.size(); ++why)
					if (set.leftWaiting[why])
						waitingText += fmt::format("{}{} {}", waitingText.empty() ? "" : ", ", why, set.leftWaiting[why]);
				logger::info("[DCLF] set leaves by cause (T6b3d): {} left; scene not built {}, no phase to take part in (freed, ineligible, phases off) {}, not bound (the joins "
							 "dropped its membership) {}, waiting {} (by the waiting-for index: {}), its layer partner or a phase of the whole object {}",
					set.left, set.leftBy[0], set.leftBy[1], set.leftBy[2], set.leftBy[3], waitingText.empty() ? "-" : waitingText, set.leftBy[4]);
				const auto [revoked, revokedMainPhase] = s.TakeRevokedClaims();
				const auto structural = s.TakeStructureRevocations();
				logger::info("[DCLF] claims revoked mid-frame (the engine drew them that frame): {} geometries, {} of them in the main phase; {} for a structural change "
							 "after the selected revision's join",
					revoked, revokedMainPhase, structural);
			}
			// The joins' membership.
			if (V::residentStats(s).frames) {
				const auto r = s.TakeResidentStats();
				auto& reg = Diagnostics::SceneDiagnostics().registrations;
				const double rf = std::max<double>(static_cast<double>(r.frames), 1.0);
				logger::info("[DCLF] scene membership: {:.0f} objects bound a frame ({} frames); {} records queued, {} joined, {} failed ({} the engine's pass, {} no record, {} a frame verdict, {} material or extras; {} waited for a material record, {} served, {} stale), {} rewritten ({} kept their binding), {} released, {} layers or bases unpaired; {} registrations of eligible objects not bound{}{}",
					r.resident / rf, r.frames, r.membershipQueued, r.joined, r.failed, r.failedBy[0], r.failedBy[1], r.failedBy[2], r.failedBy[3], r.materialWaits, r.materialsServed, r.materialsStale, r.rewritten, r.membershipKept, r.released,
					r.layerUnpaired, reg.registeredUnbound, reg.registeredUnboundFirst.empty() ? "" : ", first ", reg.registeredUnboundFirst);
				// The capture drain is an observer: the registrations above are counted only on the frames it observed.
				if (reg.registrationFrames)
					logger::info("[DCLF] registration parity (the capture drain, an observer): {} frames observed, {} main-camera registrations checked, {} of eligible objects DCLF has not bound, {} Lighting passes of another shader{}",
						reg.registrationFrames, reg.registrationsChecked, reg.registeredUnbound, reg.lightingShaderDiffers,
						reg.lightingShaderDiffers ? " <- LIGHTING SHADER" : (reg.registeredUnbound ? "" : " <- OK"));
				else
					logger::info("[DCLF] registration parity: not observed (CS_DCLF_PERSISTENT_PARITY off); the normal path reads no registration");
				auto& rp = Diagnostics::SceneDiagnostics().residents;
				if (rp.parityChecks)
					logger::info("[DCLF] resident parity: {} checks, {} records compared, {} passes differ, {} records differ ({} not compared: the root fading, leaving at the next decode){}",
						rp.parityChecks, rp.parityChecked, rp.parityPass, rp.parityRecord, rp.parityPending, rp.parityPass || rp.parityRecord ? " <- RESIDENT PARITY" : " <- OK");
				reg = {};
				rp = {};
			}
			if (!Diagnostics::Observe().stats) {
				s.ResetTimes();
				return;
			}
			std::string ineligibleText;
			for (std::size_t i = 1; i < stats.ineligible.size(); ++i)
				if (stats.ineligible[i])
					ineligibleText += fmt::format(" {}={}", kIneligibleNames[i], stats.ineligible[i]);
			// The per-part breakdown appears only under CS_DCLF_PROFILE=1, because that is the only time it is measured (per whole pass).
			std::string partText;
			if (Diagnostics::Observe().profile) {
				for (std::size_t i = 0; i < stats.partMs.size(); ++i)
					partText += fmt::format("{}{} {:.3f}", partText.empty() ? "; by part (ms a pass): " : ", ", kBuildPartNames[i], stats.partMs[i] / fullPasses);
				for (std::size_t i = 0; i < stats.accumulatePartMs.size(); ++i)
					partText += fmt::format("{}{} {:.3f}", i ? ", " : "; accumulate by part: ", kBuildPartNames[i], stats.accumulatePartMs[i] / fullPasses);
			}
			std::istringstream sceneLines(SceneLines(s));
			for (std::string line; std::getline(sceneLines, line);)
				logger::info("{}", line);
			if (stats.projectedUV || stats.landBlend)
				logger::info("[DCLF] projected UV / terrain (the last pass): {} projected candidates, {} terrain candidates, projected textures {}", stats.projectedUV, stats.landBlend,
					V::projectedTextures().valid ? "captured" : "not seen yet");
			if (const auto [fading, fadingFrames] = s.TakeFadingDrawn(); fading)
				logger::info("[DCLF] fading: {} screen-door fading objects drawn by DCLF over {} frames", fading, fadingFrames);
			// The features' lines here (Features/Skins' skinned line, then Features/Decals' candidates), the components' with their context.
			V::componentsReportLines(s, Api::ReportPoint::SceneCoordinatorLastPass, a_sceneFrame);
			logger::info("[DCLF] derived cache (the last pass): {} served, {} recomputed and compared, {} differ{}; slots alive {} geometries / {} pipelines / {} materials, {} swept, {} geometries refreshed in place, {} slot violations{}",
				stats.derivedHits, stats.derivedChecked, stats.derivedDiffers, stats.derivedDiffers ? " <- STALE" : "",
				stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive, stats.slotsSwept, stats.geometriesRefreshed,
				stats.slotViolations, stats.slotViolations ? " <- SLOT VIOLATION" : "");
			if (stats.classifyHits || stats.classifyChecked)
				logger::info("[DCLF] classification cache (the last pass): {} served from the cache, {} recomputed and compared, {} differ{}",
					stats.classifyHits, stats.classifyChecked, stats.classifyDiffers, stats.classifyDiffers ? " <- STALE" : "");
			{
				std::string techniques;
				for (std::size_t t = 0; t < stats.techniqueRejects.size(); ++t) {
					if (!stats.techniqueRejects[t])
						continue;
					techniques += fmt::format("{}{}({})={}", techniques.empty() ? "" : " ", LightingTechniqueName(static_cast<std::uint32_t>(t)), t, stats.techniqueRejects[t]);
				}
				if (!techniques.empty())
					logger::info("[DCLF] left native by technique: {}", techniques);
			}
			logger::info("[DCLF] materials (the last pass): {} evaluated; the scene work's last pass (T6b2c step 7): {} captured ({} records rewritten, {} materials held for another evaluation), {} signature samples; slots {} alive (+{} retired on last reference, {} of them a member's)",
				stats.materialsEvaluated, stats.materialWrites, stats.materialsRewritten, stats.materialsHeld,
				stats.frameMaterialSamples,
				stats.materialCacheEntries, stats.materialCacheEvicted, stats.materialEvictedMember);
			logger::info("[DCLF] tracked {} under {} category nodes: {} objects, {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{} ({} geometries moved), validation drops {}{}{}",
				stats.tracked, stats.categoryNodes, stats.objects, stats.geometries, stats.pipelines, stats.materials, ineligibleText,
				stats.attachedEvents, stats.detachedEvents, stats.detachMoves, stats.validationDrops, stats.validationDrops ? " <- DETACH" : "", partText);
			const auto posted = V::pipelinesPosted().exchange(0, std::memory_order_relaxed);
			if (const auto posts = s.TakeConstantsPostStats(); posted || posts.techniquesEvaluated || posts.stale)
				logger::info("[DCLF] frame evaluations published (step 6e A): {} pipeline blocks posted, {} applied, {} dropped; technique rows (the coordinator's, "
							 "T6b2c): {} evaluated, {} written, the inputs moved {} times; material records: the scene work's (T6b2c step 7), none posted",
					posted, posts.pipelinesApplied, posts.stale, posts.techniquesEvaluated, posts.techniquesWritten, posts.techniqueInputsMoved);
			const auto replay = std::exchange(Diagnostics::SceneDiagnostics().replay, {});
			if (const auto publication = s.TakeTablesPublication(); publication.published) {
				logger::info("[DCLF] tables published (step 6): {} snapshots ({} written again, {} made, pool {}), {:.3f} ms each on the coordinator (max {:.3f}), one per pass that changed something (T6b3d); lookups with them (T6b2c step 5): {} copied ({} chunks written again since, {:.3f} ms a copy, max {:.3f}), {} the last copy shared again (unchanged)",
					publication.published, publication.reused, publication.made, publication.pool, publication.ms / publication.published, publication.maxMs,
					publication.lookupsCopied, publication.lookupsChunks, publication.lookupsMs / std::max<std::uint64_t>(publication.lookupsCopied, 1), publication.lookupsMaxMs,
					publication.lookupsShared);
				const double n = double(publication.published);
				const std::uint64_t differ = replay.parityDiffer + replay.parityObjectsDiffer + replay.parityLogsDiffer + replay.parityGeometriesDiffer +
				                             replay.parityFamiliesDiffer;
				logger::info("[DCLF] tables replay (6d): {:.1f} of {:.0f} object slots, {:.1f} of {:.0f} geometry slots and {:.1f} of {:.0f} material records written a "
							 "publication, {}/{} copied whole; {}; the rest copied whole {:.3f} ms; parity {} checks: {} slots ({} differ), {} "
							 "geometries ({} differ), {} records ({} differ), logs {} differ, kept families {} differ{}",
					publication.objectsReplayed / n, publication.objectSlots / n, publication.geometriesReplayed / n, publication.geometrySlots / n,
					publication.materialsReplayed / n, publication.materialSlots / n, publication.wholeCopies, publication.geometryWholeCopies, FamiliesKept(publication.familiesKept),
					publication.restMs / n,
					replay.parityChecks, replay.parityObjects, replay.parityObjectsDiffer, replay.parityGeometries, replay.parityGeometriesDiffer,
					replay.parityMaterials, replay.parityDiffer, replay.parityLogsDiffer, replay.parityFamiliesDiffer,
					replay.parityChecks ? (differ ? " <- REPLAY DIFFERS" : " <- OK") : "");
				logger::info("[DCLF] retirement (6e E3: nothing freed while a publication names it): {}", s.TakeRetirementReport());
			}
			{
				// The scene pass by sub-zone (ScenePart), per whole pass.
				std::string sceneParts;
				double covered = 0.0;
				for (std::size_t i = 0; i < stats.scenePartMs.size(); ++i) {
					covered += ScenePartNested(i) ? 0.0 : stats.scenePartMs[i];
					if (stats.scenePartMs[i] > 0.0)
						sceneParts += fmt::format("{}{} {:.3f} (max {:.2f})", sceneParts.empty() ? "" : ", ", kScenePartNames[i], stats.scenePartMs[i] / fullPasses, stats.scenePartMaxMs[i]);
				}
				logger::info("[DCLF] scene pass CPU (T6b3d: the coordinator's, per whole pass): {:.3f} ms (max {:.3f}) over {} passes; by part: {}; other {:.3f}", fullMs / fullPasses,
					fullMaxMs, wholePasses, sceneParts, (fullMs - covered) / fullPasses);
				static constexpr std::array<const char*, 8> kTraitNames{ "face", "actor", "switch", "skin", "animated shading", "moves", "root moves", "?" };
				std::string traits;
				for (std::size_t i = 0; i < stats.lightByTrait.size(); ++i)
					if (stats.lightByTrait[i])
						traits += fmt::format("{}{} {:.0f}", traits.empty() ? "" : ", ", kTraitNames[i], stats.lightByTrait[i] / fullPasses);
				logger::info("[DCLF] scene tables' light path per pass: kept by trait: {}; movers listed for the frame values {:.0f} ({:.0f} move events{}), kept skins {:.0f}; "
							 "the per-frame set looked up again on {} of {} passes",
					traits.empty() ? "-" : traits, stats.lightPlaced / fullPasses, stats.moveEvents / fullPasses, Scene::MoveEventsLive() ? "" : ", not installed",
					stats.lightSkins / fullPasses, stats.perFrameRelookups, wholePasses);
				if (stats.verdictsChecked || stats.verdictsSkipped)
					logger::info("[DCLF] actor frame verdicts per pass: {:.0f} taken again, {:.0f} left for want of a hidden event ({:.0f} hidden events, {:.1f} statics classified again for one, "
								 "{} announced again at a newer frame capture{}); {} changed with no event{}{}",
						stats.verdictsChecked / fullPasses, stats.verdictsSkipped / fullPasses, stats.hiddenEvents / fullPasses, stats.hiddenRetaken / fullPasses,
						stats.hiddenAheadAnnounced, Scene::HiddenStoresLive() ? "" : ", not installed", stats.verdictsMissed, stats.verdictsMissed ? " <- MISSED; first: " : " <- OK", stats.firstVerdictMissed);
				if (const auto line = PlacementLine(s); !line.empty())
					logger::info("{}", line);
				if (Diagnostics::Observe().profile) {
					std::string kinds;
					for (std::size_t i = 0; i < stats.evaluateKindMs.size(); ++i)
						kinds += fmt::format("{}{} {:.0f} entries {:.3f} ms", kinds.empty() ? "" : ", ", kEvaluateKindNames[i], stats.evaluateKindCount[i] / fullPasses,
							stats.evaluateKindMs[i] / fullPasses);
					logger::info("[DCLF] scene tables' first evaluation round per pass, by entry: {}", kinds);
				}
			}
			{
				std::string rejects;
				for (std::size_t r = 1; r < stats.shadowRejects.size(); ++r)
					if (stats.shadowRejects[r])
						rejects += fmt::format(" {}={}", ShadowRejectName(static_cast<ShadowReject>(r)), stats.shadowRejects[r]);
				logger::info("[DCLF] shadow casters (the last pass): {} of {} records would be drawn into a shadow map; not casters:{}", stats.shadowCasters, stats.objects,
					rejects.empty() ? " none" : rejects);
			}
			s.ResetTimes();
		}
	}

	void ScenePassEnd(Scene::Coordinator& a_store, std::uint32_t a_sceneFrame)
	{
		// When its frame crossed the window (a pass in that frame, or the first after it). The passes run one at a time (the pump, or the
		// render thread inline), so the clock needs no lock.
		static Crossing crossing;
		if (crossing.Due(a_sceneFrame))
			Coordinator(a_store, a_sceneFrame);
	}
}
