// The coordinator's scene pump: the scene passes run on the scene lane, one at a time, whenever a producer wakes the pump
// (WakeScenePass, any thread); the frame never kicks or joins them, and the render thread serves what they ask of it at the frame's
// start. Also the pass's bookkeeping, whether a pass publishes (PublicationNeeded) and when (PublicationDue: paced by the snapshot builder's
// take, and the frame's own pass).

#include "Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"

#include "Features/DrawcallLimitFix/Runtime/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Runtime/SceneWake.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <algorithm>
#include <format>
#include <ranges>
#include <sstream>
#include <thread>
#include <type_traits>

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
		// camera's registration leaves its alpha on the property (GetRenderPasses), the reflection faces hide the water around them,
		// Main::Draw hides the first-person skeleton - so the frame's next pass takes them (most passes they woke changed nothing). An
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

}

namespace DCLF::Scene
{
	void Coordinator::StartScenePump()
	{
		// Configured once, never destroyed (as the snapshot builder's pump). Its passes run on the scene lane, one at a time
		// (level-triggered, lock-free: SerializedTaskPump); the lane is the scene work's alone, so a long pass never holds up the
		// coordinator's jobs (the frame's builds), and it is above the engine's job threads.
		static auto* pump = SceneScheduler::MakeLanePump(
			SceneScheduler::Executor(), SceneScheduler::SceneLaneScope(), SceneExecutor::Scene, org::async::TaskDispatch::Controlled, "DCLF scene pass",
			[this] { ScenePass(passInline); },
			[] {
				if (!scenePumpFailed.exchange(true, std::memory_order_acq_rel))
					logger::error("[DCLF] the scene pump was refused by the scene lane; no scene pass runs from here on");
			});
		// No wake on a retirement returned (a return would wake a pass whose publication's drop returns the next): the next pass
		// with an input recycles it.
		scenePump.store(pump, std::memory_order_release);
		// What was pushed before the pump existed (the session's first events).
		WakeScenePass(SceneWake::Attach);
	}

	void Coordinator::SetScenePassMode(bool a_full, bool a_inline)
	{
		passesFull.store(a_full, std::memory_order_release);
		scenePassesInline.store(a_inline, std::memory_order_relaxed);
	}

	bool Coordinator::SceneTaskInFlight() const
	{
		const auto* pump = scenePump.load(std::memory_order_acquire);
		return pump && !pump->IsIdle();
	}

	void Coordinator::RunScenePassInline()
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
		// What the pass held for PrimaryCull, delivered here.
		FrameView::Get().DeliverPrimaryNotes();
	}

	void Coordinator::ApplyEventsInline()
	{
		const bool full = passesFull.exchange(false, std::memory_order_acq_rel);
		RunScenePassInline();
		passesFull.store(full, std::memory_order_release);
	}

	void Coordinator::ScenePass(bool a_inline)
	{
		ZoneScopedN("CS.DCLF.ScenePass");
		// A wake from here on is the next pass's: the sources taken before anything is (WakeScenePassBits' fence pairs with this one).
		const std::uint32_t sources = scenePassReasons.exchange(0, std::memory_order_acq_rel);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		// What the pass's publication pacing reads (PublicationDue): its sources, and whether it takes new frame inputs.
		passSources = sources;
		passTookFrameInputs = false;
		passDeferred = false;
		const std::uint64_t publishedBefore = pumpStats.published;
		const auto start = std::chrono::steady_clock::now();
		const std::uint64_t eventsOnlyBefore = pumpStats.eventsOnly;
		// On the lane: the scene work's thread (no frame-side access to guard; GetFrame is the coordinator's), and no engine memory
		// without a lease (EngineReadWindow::Touch counts the rest). Inline (the parities) the render thread stays itself.
		if (!a_inline)
			sceneWorkThread = true;
		const bool marked = EngineReadWindow::sceneWork;
		if (!a_inline)
			EngineReadWindow::sceneWork = true;
		// The frame's engine globals, never the engine's: the newest frame inputs', taken once for the pass (inputs posted
		// after this are the next pass's: the post wakes it), or none before the first frame.
		TakeFrameInputs();
		try {
			// Kept for the pass's evaluations on the pool (ShardScope binds the same capture there).
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
		const bool changed = ps.published != publishedBefore || passDeferred;
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
		// The pass of sceneFrame's inputs is done, its publication (if any) posted: the snapshot builder may publish for the frame now.
		IndirectDraws::Get().NoteScenePassDone(sceneFrame);
		// The coordinator's report lines, when its frame crossed into a new window.
		Reports::ScenePassEnd(*this, sceneFrame);
		ReportMoverGate();
		ReportPublicationPacing();
	}

	void Coordinator::ReportPublicationPacing()
	{
		// Once the pass's frame is in a later report window, as the mover gate's.
		const std::uint32_t window = sceneFrame / Reports::kInterval;
		if (window == paceWindow)
			return;
		paceWindow = window;
		auto& p = paceStats;
		if (p.passes) {
			const double frames = static_cast<double>((std::max)(p.frames, 1u));
			const double publications = static_cast<double>((std::max)(p.published, std::uint64_t(1)));
			const auto reason = [&](PublishReason a_why) { return p.byReason[static_cast<std::size_t>(a_why)]; };
			logger::info("[DCLF] publication pacing (W3: a publication only once the snapshot builder took the last, or for the frame's pass): {} full passes over {} frames "
						 "({:.2f} a frame, max {}), {} publications ({:.2f} a frame, max {}; {} the frame's pass, {} the last taken, {} inline, {} the first), {} passes deferred "
						 "({} folded into a later publication); claims {} replayed ({:.1f} keys each), {} copied whole, {} shared again, {} revoked; tables' whole-copied rest "
						 "{:.3f} ms a publication; paced parity {} checks, {} tables differ, {} claims differ{}{}",
				p.passes, p.frames, static_cast<double>(p.passes) / frames, p.maxPasses, p.published, static_cast<double>(p.published) / frames, p.maxPublished,
				reason(PublishReason::Frame), reason(PublishReason::Taken), reason(PublishReason::Inline), reason(PublishReason::First), p.deferred, p.folded,
				p.claimsReplayed, static_cast<double>(p.claimsKeys) / static_cast<double>((std::max)(p.claimsReplayed, std::uint64_t(1))), p.claimsWhole,
				p.claimsShared, p.claimsRevoked, p.restMs / publications, p.parityChecks, p.parityTables, p.parityClaims,
				!p.parityChecks ? "" : (p.parityTables || p.parityClaims) ? " <- PACED PUBLICATION DIFFERS; first: " : " <- OK", p.parityFirst);
		}
		const std::uint32_t lastFrame = p.lastFrame;
		p = {};
		p.lastFrame = lastFrame;
	}

	bool Coordinator::PublicationDue(bool a_task, PublishReason& a_why) const
	{
		// The first publication; the parities' inline pass (once a frame, where its observers read); the frame's own pass, whose
		// publication the builder waits for (NoteScenePassDone: PublishPayload holds a sealed snapshot until the pass of the frame that
		// took the last one has ended); or the builder took the last one. Otherwise one posted now would only replace the last in the work
		// slot: the pass defers, and everything it changed stays pending (the set's applications, the deltas, the logs the snapshot replays).
		if (!publishedKeyValid || !publicationSequence)
			return a_why = PublishReason::First, true;
		if (!a_task)
			return a_why = PublishReason::Inline, true;
		if (passTookFrameInputs || (passSources & (1u << static_cast<std::uint32_t>(SceneWake::FrameInput))))
			return a_why = PublishReason::Frame, true;
		if (IndirectDraws::Get().SnapshotWorkTaken() >= publicationSequence)
			return a_why = PublishReason::Taken, true;
		return false;
	}

	void Coordinator::ReportMoverGate()
	{
		// Once the pass's frame is in a later report window (the coordinator's clock may skip frame numbers).
		const std::uint32_t window = sceneFrame / Reports::kInterval;
		if (window == moverGateWindow)
			return;
		moverGateWindow = window;
		auto& g = moverGateStats;
		if (g.perFrameWalks) {
			const double walks = static_cast<double>(g.perFrameWalks);
			const bool witnessed = g.witnessChecks != 0;
			logger::info("[DCLF] mover gate (A1: the light path by event, the plan as a delta): {} per-frame walks ({} ungated); per walk {:.1f} per-frame entries taken "
						 "({:.1f} moved, {:.1f} hidden event owed, {:.1f} first person, {:.1f} with no light path), {:.1f} left for want of an event; placement set {} movers, "
						 "{} roots ({} still); {} plans ({} with written slots, {} with the set changed; {} candidates the same as the last), {} per-frame walks made none; "
						 "mirror updates classifying again {} nodes, {} geometries; gate parity {} checks, {} would differ{}{}",
				g.perFrameWalks, g.ungatedWalks, static_cast<double>(g.due) / walks, static_cast<double>(g.moved) / walks, static_cast<double>(g.owed) / walks,
				static_cast<double>(g.firstPerson) / walks, static_cast<double>(g.fullEveryFrame) / walks, static_cast<double>(g.skipped) / walks, moverSet.size(),
				movingRoots.size() + stillRoots.size(), stillRoots.size(), g.plans, g.plansWritten, g.plansChanged, g.candidatesSame, g.perFrameWithoutPlan, g.mirrorNodes,
				g.mirrorGeometries, g.witnessChecks, g.witnessDiffer, !witnessed ? "" : g.witnessDiffer ? " <- GATE MISSED; first: " : " <- OK", g.firstDiffer);
		}
		g = {};
	}

	bool Coordinator::DeltasPending() const
	{
		return !publicationJoined.empty() || !publicationLeft.empty() || placementPlanReady || !placementPlansHeld.empty() || !shadingNamed.empty() ||
		       !fadeSeedRequests.empty() || !treeSeedRequests.empty() || !switchesApplied.empty() || switchResync || !retiredImports.empty() ||
		       !tables.actorWetnessChanges.empty() || !lightEntryChanges.empty() || !gateFlipsMade.empty();
	}

	Coordinator::PublishKey Coordinator::CurrentPublishKey() const
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

	bool Coordinator::PublicationNeeded()
	{
		// A pass publishes only what changed since the last publication - the tables (their logs, stamps and sizes), the lookups and their
		// catalog, the candidates, the claims (the commit's applications and its snapshot), the deltas the frame takes (plans, shading,
		// seeds, switches, imports, wetness, light entries), the category nodes, the toggles its commit was made under, an input the
		// coordinator took for the frame, or a LOD gate's forced release the last publication was not built for (the frames withdraw until
		// one is). A pass that changed none publishes nothing: the snapshot of the last publication stands.
		const bool needed = !publishedKeyValid || publishForced || !setApply.empty() || setSnapshotDirty || nodeSetsDirty || DeltasPending() ||
		                    sceneInputs.lodForcedGeneration != publishedLodForced || !(CurrentPublishKey() == publishedKey);
		if (!needed)
			++pumpStats.unchanged;
		return needed;
	}

	void Coordinator::NotePublished()
	{
		publishedKey = CurrentPublishKey();
		publishedKeyValid = true;
		publishForced = false;
		publishedLodForced = sceneInputs.lodForcedGeneration;
	}

}
