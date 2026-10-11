#include "Internal.h"

#include "Features/DrawcallLimitFix/Common/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Common/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Common/SceneWake.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Engine/ImportTimings.h"
#include "Features/DrawcallLimitFix/Engine/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Scene/FrameGlobals.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <sstream>
#include <thread>
#include <type_traits>

// T6b3d: the coordinator's pump (dclf-async-publication.md, "T6b3d"). The scene passes run on the scene lane whenever a producer wakes
// the pump - never kicked or joined by the frame - and the render thread serves what they ask of it at the frame's start.

namespace DCLF
{
	namespace
	{
		// The pump, once StartScenePump configured it (never destroyed: a wake may come while the process tears down).
		std::atomic<org::async::SerializedTaskPump*> scenePump{ nullptr };
		// The sources woken since the running pass took them at its start (a bit per SceneWake): a producer whose bit is set leaves the
		// Notify to the one that set it, and one finding any bit set knows a pass is queued (the pump queues one pass while one runs).
		std::atomic<std::uint32_t> scenePassReasons{ 0 };
		// The parities' mode (SetScenePassMode): the wakes are dropped and the frame's start runs the pass.
		std::atomic<bool> scenePassesInline{ false };
		// A load screen is up (SetScenePassLoading): the engine events' wakes are held (NoteLoadingScreen wakes once a Present instead).
		std::atomic<bool> scenePassesLoading{ false };
		// The pushes each source made inside the frame's render, not woken (a line each: the job threads count them).
		struct alignas(64) DeferredCount
		{
			std::atomic<std::uint64_t> count{ 0 };
			std::byte padding[64 - sizeof(std::atomic<std::uint64_t>)]{};  // a line each, explicit (C4324)
		};
		std::array<DeferredCount, static_cast<std::size_t>(SceneWake::Count)> scenePassDeferred{};
		// The executor refused the pump's task: the pump is closed for good (logged once).
		std::atomic<bool> scenePumpFailed{ false };
		// This thread's ScenePassWakeBatch depth and the sources folded into it.
		thread_local std::uint32_t wakeBatchDepth = 0;
		thread_local std::uint32_t wakeBatched = 0;

		/** @brief a_bits woken (the batch's or one wake's): the pump notified when no pass was queued yet. */
		void WakeScenePassBits(std::uint32_t a_bits)
		{
			auto* pump = scenePump.load(std::memory_order_acquire);
			if (!pump || !a_bits || scenePassesInline.load(std::memory_order_relaxed))
				return;
			// The push this wake announces, before the reasons are read: with the fence ScenePass makes after taking them, either that
			// pass's drains see the push, or this read sees the reasons taken (and this wake queues the next pass).
			std::atomic_thread_fence(std::memory_order_seq_cst);
			if ((scenePassReasons.load(std::memory_order_relaxed) & a_bits) == a_bits)
				return;
			if (scenePassReasons.fetch_or(a_bits, std::memory_order_acq_rel) != 0)
				return;
			(void)pump->Notify();
		}
		// The render thread inside RunScenePassInline: the pump's drain runs the pass inline.
		thread_local bool passInline = false;

		/** @brief The frame's globals before the first frame inputs (a pass that applies the events before any frame). */
		const std::shared_ptr<const FrameGlobals>& NoFrameGlobals()
		{
			static const auto none = std::make_shared<const FrameGlobals>();
			return none;
		}

		/** @brief avg / p95 / max of a_values (sorted here), "-" without any. */
		std::string Spread(std::vector<double>& a_values)
		{
			if (a_values.empty())
				return "-";
			std::sort(a_values.begin(), a_values.end());
			double sum = 0.0;
			for (const double value : a_values)
				sum += value;
			const std::size_t p95 = (std::min)(a_values.size() - 1, a_values.size() * 95 / 100);
			return fmt::format("{:.3f}/{:.3f}/{:.3f}", sum / double(a_values.size()), a_values[p95], a_values.back());
		}

		/** @brief p50 / p95 / max of a_values (sorted here), "-" without any. */
		template <class T>
		std::string Percentiles(std::vector<T>& a_values)
		{
			if (a_values.empty())
				return "-";
			std::sort(a_values.begin(), a_values.end());
			const auto at = [&a_values](std::size_t a_percent) { return a_values[(std::min)(a_values.size() - 1, a_values.size() * a_percent / 100)]; };
			if constexpr (std::is_floating_point_v<T>)
				return fmt::format("{:.2f}/{:.2f}/{:.2f}", at(50), at(95), a_values.back());
			else
				return fmt::format("{}/{}/{}", at(50), at(95), a_values.back());
		}
	}

	void WakeScenePass(SceneWake a_source)
	{
		static_assert(static_cast<std::uint32_t>(SceneWake::Count) <= 32, "a bit per source");
		// While a load screen is up the engine's events are drained once a Present (NoteLoadingScreen's LoadDrain wake), not once a push:
		// a load pushes thousands a frame, every one for the mirror's carry alone.
		const bool engineEvent = a_source >= SceneWake::Attach && a_source <= SceneWake::SwitchEvent;
		if (engineEvent && scenePassesLoading.load(std::memory_order_relaxed))
			return;
		// Inside the frame's render (Main::Draw to Present: EngineReadWindow) the engine sets and undoes these for its own views - each
		// camera's registration leaves its alpha on the property (GetRenderPasses), the reflection faces hide the water around them, Main::Draw
		// hides the first-person skeleton - so the frame's next pass takes them (w130: 70-80% of the passes they woke changed nothing). An
		// attach, a detach or a switch still wakes.
		const bool renderToggle = a_source == SceneWake::Hidden || a_source == SceneWake::Leaf || a_source == SceneWake::PropertyUpdate ||
		                          a_source == SceneWake::AlphaUpdate || a_source == SceneWake::PropertyEvent;
		if (renderToggle && EngineReadWindow::IsOpen()) {
			scenePassDeferred[static_cast<std::size_t>(a_source)].count.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const std::uint32_t bit = 1u << static_cast<std::uint32_t>(a_source);
		if (wakeBatchDepth) {
			wakeBatched |= bit;
			return;
		}
		WakeScenePassBits(bit);
	}

	void SetScenePassLoading(bool a_loading)
	{
		scenePassesLoading.store(a_loading, std::memory_order_relaxed);
	}

	std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count)> TakeScenePassDeferred()
	{
		std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count)> taken{};
		for (std::size_t s = 0; s < taken.size(); ++s)
			taken[s] = scenePassDeferred[s].count.exchange(0, std::memory_order_relaxed);
		return taken;
	}

	ScenePassWakeBatch::ScenePassWakeBatch()
	{
		++wakeBatchDepth;
	}

	ScenePassWakeBatch::~ScenePassWakeBatch()
	{
		if (--wakeBatchDepth == 0)
			WakeScenePassBits(std::exchange(wakeBatched, 0u));
	}

	void SceneStore::StartScenePump()
	{
		// Configured once, never destroyed (as the snapshot builder's pump). Its passes run on the scene lane, one at a time
		// (level-triggered, lock-free: SerializedTaskPump); the lane is the scene work's alone (step 6c), so a long pass never holds up the
		// coordinator's jobs (AsyncWorker's, the frame's builds), and it is above the engine's job threads as the scene work was.
		static auto* pump = [this] {
			auto* created = new org::async::SerializedTaskPump;
			created->Configure(
				[](org::async::SerializedTaskPump::Task a_task) {
					return SceneScheduler::SceneLane().Dispatch(SceneScheduler::SceneLaneScope(), PublishedSceneExecutor::Coordinator, org::async::TaskDispatch::Controlled,
						"DCLF scene pass", [task = std::move(a_task)](const org::async::TaskContext&) { task(); });
				},
				[this] { ScenePass(passInline); },
				[] {
					if (!scenePumpFailed.exchange(true, std::memory_order_acq_rel))
						logger::error("[DCLF] the scene pump was refused by the scene lane; no scene pass runs from here on");
				});
			return created;
		}();
		// No wake on a retirement returned (T6b3d, w124: a return woke a pass whose publication's drop returned the next): the next pass
		// with an input recycles it.
		scenePump.store(pump, std::memory_order_release);
		// What was pushed before the pump existed (the session's first events).
		WakeScenePass(SceneWake::Attach);
	}

	void SceneStore::SetScenePassMode(bool a_full, bool a_inline)
	{
		passesFull.store(a_full, std::memory_order_release);
		scenePassesInline.store(a_inline, std::memory_order_relaxed);
		// A frame-side read of the coordinator's state is a defect whenever a pass may run beside the frame (GuardFrameAccess).
		sceneTaskInFlight.store(!a_inline && scenePump.load(std::memory_order_acquire), std::memory_order_relaxed);
	}

	bool SceneStore::SceneTaskInFlight() const
	{
		const auto* pump = scenePump.load(std::memory_order_acquire);
		return pump && !pump->IsIdle();
	}

	void SceneStore::RunScenePassInline()
	{
		auto* pump = scenePump.load(std::memory_order_acquire);
		if (!pump)
			return;
		// The parities only: a pass the lane started (a wake from before the mode was set) ends first, then this frame's runs here.
		passInline = true;
		while (!pump->TryRunInline()) {
			if (scenePumpFailed.load(std::memory_order_acquire))
				break;
			std::this_thread::yield();
		}
		passInline = false;
		// What the pass held for PrimaryCull, now (the join delivered them before T6b3d).
		DeliverPrimaryNotes();
	}

	void SceneStore::ApplyEventsInline()
	{
		const bool full = passesFull.exchange(false, std::memory_order_acq_rel);
		RunScenePassInline();
		passesFull.store(full, std::memory_order_release);
	}

	void SceneStore::ScenePass(bool a_inline)
	{
		ZoneScopedN("CS.DCLF.ScenePass");
		// A wake from here on is the next pass's: the sources taken before anything is (WakeScenePassBits' fence pairs with this one).
		const std::uint32_t sources = scenePassReasons.exchange(0, std::memory_order_acq_rel);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		const std::uint64_t publishedBefore = pumpStats.published;
		const auto start = std::chrono::steady_clock::now();
		const std::uint64_t eventsOnlyBefore = pumpStats.eventsOnly;
		// On the lane: the scene work's thread (no frame-side access to guard; GetFrame is the coordinator's), and no engine memory
		// without a lease (EngineReadWindow::Touch counts the rest). Inline (the parities) the render thread stays itself.
		if (!a_inline) {
			sceneLaneThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
			sceneWorkThread = true;
		}
		const bool marked = EngineReadWindow::sceneWork;
		if (!a_inline)
			EngineReadWindow::sceneWork = true;
		// The frame's engine globals, never the engine's (step 6e F2): the newest frame inputs' (the pass takes them again and finds none
		// newer), or none before the first frame.
		TakeFrameInputs();
		try {
			// T6b3e: kept for the pass's evaluations on the pool (ShardScope binds the same capture there).
			passGlobals = sceneInputs.globals ? sceneInputs.globals : NoFrameGlobals();
			FrameGlobals::Scope scope(passGlobals);
			RunSceneWork(!a_inline);
		} catch (const std::exception& e) {
			// Never out of the drain (the pump would close for good): the pass's flags put back, the next wake runs another.
			if (pumpStats.failed++ < 4)
				logger::error("[DCLF] a scene pass threw: {}; the tables are whatever it left, and the next wake runs another", e.what());
			holdPrimaryNotes = false;
			holdLostMembers = false;
			inSceneTask = false;
		}
		EngineReadWindow::sceneWork = marked;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		auto& ps = pumpStats;
		ps.passMs.push_back(ms);
		++ps.passes;
		ps.inlinePasses += a_inline ? 1 : 0;
		// The passes each source woke, and those of them that published nothing (a pass counts under every source it took).
		const bool changed = ps.published != publishedBefore;
		for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(SceneWake::Count); ++s)
			if ((sources >> s) & 1u) {
				++ps.bySource[s];
				ps.unchangedBySource[s] += changed ? 0u : 1u;
			}
		if (!sources) {
			++ps.bySource[static_cast<std::size_t>(SceneWake::Count)];
			ps.unchangedBySource[static_cast<std::size_t>(SceneWake::Count)] += changed ? 0u : 1u;
		}
		if (ps.eventsOnly == eventsOnlyBefore) {
			ps.fullMs += ms;
			ps.fullMaxMs = (std::max)(ps.fullMaxMs, ms);
		}
		if (!ps.any || sceneFrame != ps.lastFrame) {
			if (!ps.any)
				ps.firstFrame = sceneFrame;
			ps.any = true;
			ps.lastFrame = sceneFrame;
			++ps.framesWithPass;
			ps.passesThisFrame = 0;
		}
		ps.maxPassesInFrame = (std::max)(ps.maxPassesInFrame, ++ps.passesThisFrame);
		TracyPlot("CS.DCLF.ScenePassMs", ms);
		// The coordinator's report, when its frame crossed the interval (a pass in that frame, or the first after it).
		if (const std::uint32_t interval = sceneFrame / kReportInterval; interval != reportedInterval) {
			reportedInterval = interval;
			ReportCoordinator();
		}
	}

	bool SceneStore::DeltasPending() const
	{
		return !publicationJoined.empty() || !publicationLeft.empty() || placementPlanReady || !placementPlansHeld.empty() || !shadingNamed.empty() ||
		       !fadeSeedRequests.empty() || !treeSeedRequests.empty() || !switchesApplied.empty() || switchResync || !retiredImports.empty() ||
		       !tables.actorWetnessChanges.empty() || !lightEntryChanges.empty() || !gateFlipsMade.empty();
	}

	SceneStore::PublishKey SceneStore::CurrentPublishKey() const
	{
		PublishKey key;
		key.words = { tablesGeneration, tables.changeLog.End(), tables.geometryLog.End(), tables.materialLog.End(), tables.extrasBlockLog.End(),
			tables.versionCounter, tables.constantsStamp, tables.treesStamp, tables.fadeRootsStamp, tables.objects.size(), tables.geometries.size(),
			tables.pipelines.size(), tables.materials.size(), sunCandidatesGeneration, lightCandidatesGeneration, lightEntriesAppeared, setCommitToggles,
			constantsPostStats.pipelinesApplied };
		key.lookups = lookups.ChangeKey();
		key.catalog = lookupsCatalog.get();
		return key;
	}

	bool SceneStore::PublicationNeeded()
	{
		// T6b3d (w124: a publication, and a snapshot built, every pass): a pass publishes only what changed since the last publication - the
		// tables (their logs, stamps and sizes), the lookups and their catalog, the candidates, the claims (the commit's applications and its
		// snapshot), the deltas the frame takes (plans, shading, seeds, switches, imports, wetness, light entries), the category nodes, the
		// toggles its commit was made under, or an input the coordinator took for the frame. A pass that changed none publishes nothing: the
		// snapshot of the last publication stands.
		// T6b5: a LOD gate's forced release the last publication was not built for publishes too (the frames withdraw until one is).
		const bool needed = !publishedKeyValid || publishForced || !setApply.empty() || setSnapshotDirty || nodeSetsDirty || DeltasPending() ||
		                    sceneInputs.lodForcedGeneration != publishedLodForced || !(CurrentPublishKey() == publishedKey);
		if (!needed)
			++pumpStats.unchanged;
		return needed;
	}

	void SceneStore::NotePublished()
	{
		publishedKey = CurrentPublishKey();
		publishedKeyValid = true;
		publishForced = false;
		publishedLodForced = sceneInputs.lodForcedGeneration;
	}

	void SceneStore::ServeFrameRequests(bool a_running)
	{
		ZoneScopedN("CS.DCLF.FrameRequests");
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::SceneCaptures);
		// The switch catch-ups the passes asked for (engine code that writes, before the culls: CatchUpSwitches), and every switch in the
		// world on the first frame after a load. The requests' references dropped here, after.
		{
			std::vector<std::shared_ptr<SwitchCatchUp>> requests;
			switchCatchUps.Drain([&requests](std::shared_ptr<SwitchCatchUp>&& a_request) {
				if (a_request)
					requests.push_back(std::move(a_request));
			});
			for (const auto& request : requests) {
				attachedRoots.clear();
				for (const auto& root : request->attached)
					attachedRoots.push_back(root.get());
				CatchUpSwitches(attachedRoots, request->switches);
			}
			attachedRoots.clear();
			if (worldCatchUpPending)
				CatchUpSwitches({}, {});
		}
		if (a_running) {
			// The category nodes as the passes will diff them (step 6e F2): made again when the signature moved, a pass saw a detach (a
			// detach can take a category node with it before the signature sees its cell go), or a load ended (every cell is new).
			const bool detached = categoryDetachSeen.exchange(false, std::memory_order_acq_rel);
			CaptureCategories(std::exchange(categoryCapturePending, false) || detached);
			// What the passes found the mirror lacking, and the mirror parity's slice (CS_DCLF_MIRROR_PARITY).
			CaptureMirrorRequests();
			ProbeMirror();
			// T6b3e: the fade nodes the coordinator holds no reference to, read from the properties where the engine reads them.
			ServeFadeNodeRequests();
		}
		// Tree LOD's mirror is the render thread's (DecideTreeLod reads it at the frame's start).
		treeLod.Drain(frame);
	}

	void SceneStore::ServeFadeNodeRequests()
	{
		// Render thread, the frame's start (T6b3e): each request's geometry's property read live, as GetRenderPasses reads its fade node to
		// draw it. A geometry in the world whose property still names the requested node is drawn with that node, so the node is alive here:
		// its reference is taken and posted back. Otherwise the answer is none (the coordinator asks again for the node its mirror names
		// next). The requests' geometry references are dropped here, on the render thread.
		bool answered = false;
		fadeNodeRequests.Drain([&](FadeNodeRequest&& a_request) {
			FadeNodeAnswer answer;
			answer.geometry = a_request.geometry.get();
			answer.fadeNode = a_request.fadeNode;
			if (auto* geometry = a_request.geometry.get(); geometry && SceneCapture::InWorld(geometry))
				if (auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get(); property && property->fadeNode && property->fadeNode == a_request.fadeNode)
					answer.node.reset(property->fadeNode);
			fadeNodeAnswers.Push(std::move(answer));
			answered = true;
		});
		if (answered)
			WakeScenePass(SceneWake::Capture);
	}

	void SceneStore::ReportCoordinator()
	{
		// T6b3d (T6b3a's item 11): the coordinator's own lines, composed in its pass from its own state as its frame crosses the report
		// interval; the frame's report reads only the frame's (DrawcallLimitFix::ReportStats). "A frame" in the lines below that the
		// walk counts is a walk (a pass) since T6b3d.
		auto& ps = pumpStats;
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
			for (std::size_t s = 0; s <= static_cast<std::size_t>(SceneWake::Count); ++s)
				if (ps.bySource[s])
					bySource += fmt::format("{}{} {}/{}", bySource.empty() ? "" : ", ", s < kSceneWakeNames.size() ? kSceneWakeNames[s] : "none", ps.bySource[s],
						ps.unchangedBySource[s]);
			logger::info("[DCLF] scene pump wakes by source (T6b3d: passes woken / of them changed nothing): {}", bySource.empty() ? "-" : bySource);
			std::string deferred;
			const auto held = TakeScenePassDeferred();
			for (std::size_t s = 0; s < held.size(); ++s)
				if (held[s])
					deferred += fmt::format("{}{} {}", deferred.empty() ? "" : ", ", kSceneWakeNames[s], held[s]);
			logger::info("[DCLF] scene pump pushes inside the frame's render, not woken (T6b3d: the frame's next pass takes them): {}",
				deferred.empty() ? "-" : deferred);
			// T6b3e: the walk's rounds and the joins evaluated on the pool, and CS_DCLF_FANOUT_PARITY's comparison.
			if (const auto fanout = FanoutReport(); !fanout.empty())
				logger::info("{}", fanout);
			// T6b3e, CS_DCLF_RELEASE_GUARD: the engine's last releases on DCLF's threads (any is a defect).
			if (const auto releases = ReleaseGuard::Report(); !releases.empty())
				logger::info("{}", releases);
			// T6b5: the LOD gates (opened, flipped, retired; the open ones' age and what they wait for).
			if (const auto gates = GateReport(); !gates.empty())
				logger::info("{}", gates);
		}
		const std::uint64_t wholePasses = ps.passes - ps.eventsOnly;
		const double fullPasses = std::max<double>(1.0, static_cast<double>(wholePasses));
		const double fullMs = ps.fullMs, fullMaxMs = ps.fullMaxMs;
		ps = PumpStats{};
		// The set (its commits are the passes'), and the claims its passes took back.
		{
			const auto set = TakeSetStats();
			const double commits = std::max<double>(static_cast<double>(set.commits), 1.0);
			logger::info("[DCLF] DCLF set: {:.0f} members and {:.1f} bound objects waiting a frame ({:.1f} with some of their phases ready: whole-object claims) over {} commits; {} joined, {} left, {} evaluated ({} readiness events taking {} waiting ones again, {} "
						 "resyncs), {} members bound again by the joins before the commit (T6b3c: kept when ready at it), {} publications; waiting for: pipeline {}, material {}, shadow mask {}, shared lookups {}, "
						 "geometry {}, decal slot {}, layer partner {}, shadow pipelines {}, reflection (forward pipeline) {}, constants {}, scene buffers' growth {}, past the scene buffers (shadow) {}, a shadow diffuse not imported {}{}{}; members patched by the accumulate phase {}{}; {} left the last commit's decision (LeaveSet: a rebind before this commit, a revocation after); commit parity {} checks, {} slots differ{}",
				set.members / commits, set.waiting / commits, set.partial / commits, set.commits, set.joined, set.left, set.evaluated, set.readinessEvents, set.waitingRequeued, set.resyncs, set.rebinding,
				set.publications, set.waitingBy[0], set.waitingBy[1], set.waitingBy[2], set.waitingBy[3], set.waitingBy[4], set.waitingBy[5], set.waitingBy[6], set.waitingBy[7], set.waitingBy[8],
				set.waitingBy[9], set.waitingBy[10], set.waitingBy[12], set.waitingBy[13], set.firstWaiting.empty() ? "" : "; first: ", set.firstWaiting, set.patchedMember, set.patchedMember ? " <- SET PATCHED" : "", set.leftAfterCommit, set.commitParityChecks, set.commitParityDiffer,
				set.commitParityChecks ? (set.commitParityDiffer ? " <- COMMIT" : " <- OK") : "");
			// T6b3d: why the members that left did (each leave once, by its commit's verdict).
			std::string waitingText;
			for (std::size_t why = 0; why < set.leftWaiting.size(); ++why)
				if (set.leftWaiting[why])
					waitingText += fmt::format("{}{} {}", waitingText.empty() ? "" : ", ", why, set.leftWaiting[why]);
			logger::info("[DCLF] set leaves by cause (T6b3d): {} left; scene not built {}, no phase to take part in (freed, ineligible, phases off) {}, not bound (the joins "
						 "dropped its membership) {}, waiting {} (by the waiting-for index: {}), its layer partner or a phase of the whole object {}",
				set.left, set.leftBy[0], set.leftBy[1], set.leftBy[2], set.leftBy[3], waitingText.empty() ? "-" : waitingText, set.leftBy[4]);
			const auto [revoked, revokedMainPhase] = TakeRevokedClaims();
			const auto structural = TakeStructureRevocations();
			logger::info("[DCLF] claims revoked mid-frame (the engine drew them that frame): {} geometries, {} of them in the main phase; {} for a structural change "
						 "after the selected revision's join",
				revoked, revokedMainPhase, structural);
		}
		// The joins' membership (PrimaryCull's report printed it from the frame before T6b3d).
		if (residentStats.frames) {
			const auto r = TakeResidentStats();
			const double rf = std::max<double>(static_cast<double>(r.frames), 1.0);
			logger::info("[DCLF] scene membership: {:.0f} objects bound a frame ({} frames); {} records queued, {} joined, {} failed ({} the engine's pass, {} no record, {} a frame verdict, {} material or extras; {} waited for a material record, {} served, {} stale), {} rewritten ({} kept their binding), {} released, {} layers or bases unpaired; {} registrations of eligible objects not bound{}{}",
				r.resident / rf, r.frames, r.membershipQueued, r.joined, r.failed, r.failedBy[0], r.failedBy[1], r.failedBy[2], r.failedBy[3], r.materialWaits, r.materialsServed, r.materialsStale, r.rewritten, r.membershipKept, r.released,
				r.layerUnpaired, r.registeredUnbound, r.registeredUnboundFirst.empty() ? "" : ", first ", r.registeredUnboundFirst);
			// The capture drain is an observer (T6b2c step 8): the registrations above are counted only on the frames it observed.
			if (r.registrationFrames)
				logger::info("[DCLF] registration parity (the capture drain, an observer): {} frames observed, {} main-camera registrations checked, {} of eligible objects DCLF has not bound, {} Lighting passes of another shader{}",
					r.registrationFrames, r.registrationsChecked, r.registeredUnbound, r.lightingShaderDiffers,
					r.lightingShaderDiffers ? " <- LIGHTING SHADER" : (r.registeredUnbound ? "" : " <- OK"));
			else
				logger::info("[DCLF] registration parity: not observed (CS_DCLF_PERSISTENT_PARITY off); the normal path reads no registration");
			if (r.parityChecks)
				logger::info("[DCLF] resident parity: {} checks, {} records compared, {} passes differ, {} records differ ({} not compared: the root fading, leaving at the next decode){}",
					r.parityChecks, r.parityChecked, r.parityPass, r.parityRecord, r.parityPending, r.parityPass || r.parityRecord ? " <- RESIDENT PARITY" : " <- OK");
		}
		if (!SwitchEnabled(Switch::Stats)) {
			ResetTimes();
			return;
		}
		std::string ineligibleText;
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i)
			if (stats.ineligible[i])
				ineligibleText += fmt::format(" {}={}", kIneligibleNames[i], stats.ineligible[i]);
		// The per-part breakdown appears only under CS_DCLF_PROFILE=1, because that is the only time it is measured (per whole pass).
		std::string partText;
		if (ProfileEnabled()) {
			for (std::size_t i = 0; i < stats.partMs.size(); ++i)
				partText += fmt::format("{}{} {:.3f}", partText.empty() ? "; by part (ms a pass): " : ", ", kBuildPartNames[i], stats.partMs[i] / fullPasses);
			for (std::size_t i = 0; i < stats.accumulatePartMs.size(); ++i)
				partText += fmt::format("{}{} {:.3f}", i ? ", " : "; accumulate by part: ", kBuildPartNames[i], stats.accumulatePartMs[i] / fullPasses);
		}
		std::istringstream sceneLines(SceneReport());
		for (std::string line; std::getline(sceneLines, line);)
			logger::info("{}", line);
		if (stats.projectedUV || stats.landBlend)
			logger::info("[DCLF] projected UV / terrain (the last pass): {} projected candidates, {} terrain candidates, projected textures {}", stats.projectedUV, stats.landBlend,
				projectedTextures.valid ? "captured" : "not seen yet");
		if (const auto [fading, fadingFrames] = TakeFadingDrawn(); fading)
			logger::info("[DCLF] fading: {} screen-door fading objects drawn by DCLF over {} frames", fading, fadingFrames);
		if (stats.skinned)
			logger::info("[DCLF] skinned (the last pass): {} candidates, {} palette rows (FrameValues')", stats.skinned, stats.boneRows);
		if (stats.decals[0] || stats.decals[1] || stats.decals[2])
			logger::info("[DCLF] decals (the last pass): {} candidates ({} in the opaque group, {} multi-index layers, {} in the blended group)",
				stats.decals[0] + stats.decals[1] + stats.decals[2], stats.decals[0], stats.decals[2], stats.decals[1]);
		if (SwitchEnabled(Switch::DeriveProbe)) {
			std::string bitBreakdown;
			for (std::uint32_t bit = 0; bit < 32; ++bit) {
				if (!stats.derivationBitCounts[bit])
					continue;
				bitBreakdown += std::format("{} bit {}{}={}", bitBreakdown.empty() ? "" : ",", bit, ((1u << bit) & kRuntimePassBits) ? "*" : "", stats.derivationBitCounts[bit]);
			}
			logger::info("[DCLF] derivation (the last pass): {} objects compared, {} would stay native; property bits: {} differ ({:08X}), {} only where the engine's LOD fades ran out; runtime bits: {} differ ({:08X}); per bit (* = runtime):{}",
				stats.derivationChecked, stats.derivationNative, stats.derivationDiffers, stats.derivationBits, stats.derivationFadeBits,
				stats.derivationRuntimeDiffers, stats.derivationRuntimeBits, bitBreakdown.empty() ? std::string(" none") : bitBreakdown);
			logger::info("[DCLF] LOD fades (the last pass): {} objects with a fade node compared; the metric differs from the engine's on {}, the draw's fades on {}{}{}",
				stats.lodFadeChecked, stats.lodMetricDiffers, stats.lodFadeDiffers, stats.lodFadeFirst.empty() ? "" : "; first: ", stats.lodFadeFirst,
				stats.lodFadeDiffers ? " <- LOD FADE" : "");
		}
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
		if (SwitchEnabled(Switch::CoverageProbe))
			for (const auto& line : std::views::split(CoverageCensus(), '\n'))
				logger::info("{}", std::string_view(line.begin(), line.end()));
		if (!stats.propertyRejects.empty()) {
			std::vector<std::pair<const RE::NiRTTI*, std::uint32_t>> sorted(stats.propertyRejects.begin(), stats.propertyRejects.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string byProperty;
			for (const auto& [rtti, count] : sorted)
				byProperty += fmt::format(" {}={}", rtti && rtti->name ? rtti->name : "?", count);
			logger::info("[DCLF] left native by property type:{}; {} alpha blended, {} opaque ({} alpha tested)",
				byProperty, stats.rejectedBlended, stats.rejectedOpaque, stats.rejectedOpaqueAlphaTest);
		}
		// The material cache and its standing alarm. materialCacheStale must be 0: it is the count of entries that were re-evaluated live
		// and disagreed with what the cache would have served.
		logger::info("[DCLF] materials (the last pass): {} evaluated; the scene work's last pass (T6b2c step 7): {} captured ({} records rewritten, {} materials held for another evaluation), {} signature samples; slots {} alive (+{} retired on last reference, {} of them a member's); validated {}, stale {}{}",
			stats.materialsEvaluated, stats.materialWrites, stats.materialsRewritten, stats.materialsHeld,
			stats.frameMaterialSamples,
			stats.materialCacheEntries, stats.materialCacheEvicted, stats.materialEvictedMember, stats.materialsValidated, stats.materialCacheStale,
			stats.materialCacheStale ? " <- STALE MATERIAL" : "");
		if (stats.materialCacheStale)
			logger::warn("[DCLF] material cache staleness is in: {}{}{}{}{}{}",
				(stats.materialDiffMask & 1) ? "vs " : "", (stats.materialDiffMask & 2) ? "ps " : "",
				(stats.materialDiffMask & 4) ? "textures " : "", (stats.materialDiffMask & 8) ? "address " : "",
				(stats.materialDiffMask & 16) ? "filter " : "", (stats.materialDiffMask & 32) ? "written" : "");
		logger::info("[DCLF] tracked {} under {} category nodes: {} objects, {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{} ({} geometries moved), validation drops {}{}{}",
			stats.tracked, stats.categoryNodes, stats.objects, stats.geometries, stats.pipelines, stats.materials, ineligibleText,
			stats.attachedEvents, stats.detachedEvents, stats.detachMoves, stats.validationDrops, stats.validationDrops ? " <- DETACH" : "", partText);
		if (const auto posts = TakeConstantsPostStats(); posts.pipelinesPosted || posts.techniquesEvaluated || posts.stale)
			logger::info("[DCLF] frame evaluations published (step 6e A): {} pipeline blocks posted, {} applied, {} dropped; technique rows (the coordinator's, "
						 "T6b2c): {} evaluated, {} written, the inputs moved {} times; material records: the scene work's (T6b2c step 7), none posted",
				posts.pipelinesPosted, posts.pipelinesApplied, posts.stale, posts.techniquesEvaluated, posts.techniquesWritten, posts.techniqueInputsMoved);
		if (const auto publication = TakeTablesPublication(); publication.published) {
			logger::info("[DCLF] tables published (step 6): {} snapshots ({} written again, {} made, pool {}), {:.3f} ms each on the coordinator (max {:.3f}), one per pass that changed something (T6b3d); lookups with them (T6b2c step 5): {} copied ({} chunks written again since, {:.3f} ms a copy, max {:.3f}), {} the last copy shared again (unchanged)",
				publication.published, publication.reused, publication.made, publication.pool, publication.ms / publication.published, publication.maxMs,
				publication.lookupsCopied, publication.lookupsChunks, publication.lookupsMs / std::max<std::uint64_t>(publication.lookupsCopied, 1), publication.lookupsMaxMs,
				publication.lookupsShared);
			const double n = double(publication.published);
			const std::uint64_t differ = publication.parityDiffer + publication.parityObjectsDiffer + publication.parityLogsDiffer + publication.parityGeometriesDiffer +
			                             publication.parityFamiliesDiffer;
			logger::info("[DCLF] tables replay (6d): {:.1f} of {:.0f} object slots, {:.1f} of {:.0f} geometry slots and {:.1f} of {:.0f} material records written a "
						 "publication, {}/{} copied whole; trees kept {}, fade roots kept {}; the rest copied whole {:.3f} ms; parity {} checks: {} slots ({} differ), {} "
						 "geometries ({} differ), {} records ({} differ), logs {} differ, kept families {} differ{}",
				publication.objectsReplayed / n, publication.objectSlots / n, publication.geometriesReplayed / n, publication.geometrySlots / n,
				publication.materialsReplayed / n, publication.materialSlots / n, publication.wholeCopies, publication.geometryWholeCopies, publication.treesKept,
				publication.fadeRootsKept, publication.restMs / n,
				publication.parityChecks, publication.parityObjects, publication.parityObjectsDiffer, publication.parityGeometries, publication.parityGeometriesDiffer,
				publication.parityMaterials, publication.parityDiffer, publication.parityLogsDiffer, publication.parityFamiliesDiffer,
				publication.parityChecks ? (differ ? " <- REPLAY DIFFERS" : " <- OK") : "");
			logger::info("[DCLF] retirement (6e E3: nothing freed while a publication names it): {}", TakeRetirementReport());
		}
		if (TimelineEnabled()) {
			// T6b0: the frames from an event to the set (the commit to installation is the frame's: its report).
			auto histogram = [](const auto& a_buckets) {
				std::string out;
				for (std::size_t b = 0; b < a_buckets.size(); ++b)
					if (a_buckets[b])
						out += fmt::format("{}{}:{}", out.empty() ? "" : " ", kAgeBucketNames[b], a_buckets[b]);
				return out.empty() ? std::string("-") : out;
			};
			const auto timeline = TakeTimelineStats();
			logger::info("[DCLF] latency (T6b0, frames): {} joins ({} first since tracked), {} bindings; attach to join {}; record to binding {}; record to join {}; "
						 "a show to its join {}",
				timeline.joins, timeline.firstJoins, timeline.bound, histogram(timeline.attachToMember), histogram(timeline.writtenToBound),
				histogram(timeline.writtenToMember), histogram(timeline.showToMember));
			std::string failures;
			for (std::size_t w = 1; w < timeline.bindFailures.size(); ++w)
				if (timeline.bindFailures[w])
					failures += fmt::format("{}{} {}", failures.empty() ? "" : ", ", PrimaryCull::kSyntheticFailNames[w], timeline.bindFailures[w]);
			if (!failures.empty())
				logger::info("[DCLF] membership passes that gave none (T6b0: dropped, bound only when written again): {}; first: {}", failures, timeline.bindFailFirst);
			logger::info("[DCLF] hidden bit stores (T6b0, shown/hidden): {}", TakeHiddenSiteReport());
			if (const auto line = ImportTimings::TakeReport(kReportInterval); !line.empty())
				logger::info("{}", line);
		}
		{
			// The scene pass by sub-zone (ScenePart), per whole pass (T6b3d: the passes leave the frames).
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
				traits.empty() ? "-" : traits, stats.lightPlaced / fullPasses, stats.moveEvents / fullPasses, MoveEventsLive() ? "" : ", not installed",
				stats.lightSkins / fullPasses, stats.perFrameRelookups, wholePasses);
			if (stats.verdictsChecked || stats.verdictsSkipped)
				logger::info("[DCLF] actor frame verdicts per pass: {:.0f} taken again, {:.0f} left for want of a hidden event ({:.0f} hidden events, {:.1f} statics classified again for one{}); {} changed with no event{}{}",
					stats.verdictsChecked / fullPasses, stats.verdictsSkipped / fullPasses, stats.hiddenEvents / fullPasses, stats.hiddenRetaken / fullPasses,
					HiddenEventsLive() ? "" : ", not installed", stats.verdictsMissed, stats.verdictsMissed ? " <- MISSED; first: " : " <- OK", stats.firstVerdictMissed);
			if (stats.inputRereads[0] || stats.inputRereads[1]) {
				std::string components;
				for (std::size_t i = 0; i < stats.inputChanged.size(); ++i)
					if (stats.inputChanged[i])
						components += fmt::format("{}{} {}", components.empty() ? "" : ", ", kInputComponentNames[i], stats.inputChanged[i]);
				logger::info("[DCLF] input watch: classify re-reads {:.0f}/pass ({} changed with no event), shading re-reads {:.0f}/pass ({} changed with no event); by component: {}{}{}{}",
					stats.inputRereads[0] / fullPasses, stats.inputRereadsChanged[0], stats.inputRereads[1] / fullPasses, stats.inputRereadsChanged[1],
					components.empty() ? "none" : components, stats.inputRereadsChanged[0] || stats.inputRereadsChanged[1] ? " <- MISSED" : " <- OK",
					stats.firstInputChange.empty() ? "" : "; first: ", stats.firstInputChange);
			}
			if (const auto line = PlacementReport(); !line.empty())
				logger::info("{}", line);
			if (ProfileEnabled()) {
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
		ResetTimes();
	}
}
