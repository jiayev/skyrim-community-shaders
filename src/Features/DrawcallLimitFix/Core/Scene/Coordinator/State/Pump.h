#pragma once

// The scene pump (CoordinatorPump): the pass mode, the publication key and its pacing, what a pass holds for the render
// thread, and the pump's counters. ScenePump.cpp, with the publication (SceneSet.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorPump
	{
		/**
		 * @brief The scene pump: passesFull (SetScenePassMode, render thread); the oldest unpublished stamp and the last publication's key
		 * (unchanged passes publish nothing); report samples.
		 */
		std::atomic<bool> passesFull{ false };
		std::uint64_t unpublishedNs = 0;
		std::uint32_t unpublishedFrame = 0;
		using PublishKey = DCLF::PublishKey;
		PublishKey publishedKey;
		bool publishedKeyValid = false;
		Extensions::AdoptionGenerations publishedAdoptionGuards{};  // the adoption guards' generations of the last publication (NotePublished)
		bool publishForced = false;  // an input TakeCoordinatorInputs took publishes
		// Publication pacing: a pass that needs a publication makes it only when the snapshot builder took the last one, for the frame's
		// own pass (new frame inputs or a FrameInput wake: NoteScenePassDone's gate), inline, or for the first; else it defers, and its
		// commit's applications, deltas and log entries ride the next one (PublicationDue).
		std::uint32_t passSources = 0;      // the SceneWake bits the running pass took
		bool passTookFrameInputs = false;   // the running pass took new frame inputs (TakeFrameInputs)
		bool passDeferred = false;          // the running pass needed a publication and deferred it
		std::uint32_t passesDeferred = 0;   // passes deferred since the last publication (folded into the next)
		std::uint64_t pacedPublications = 0;  // publications that folded deferred passes in (the paced parity's schedule)
		bool pacedCheckDue = false;           // this publication is the paced parity's (Policy::CheckPacedPublication)
		std::shared_ptr<SetSnapshot> claimsReference;  // its claims made whole (PublishClaims, RevokeFromClaims), compared after
		enum class PublishReason : std::uint8_t
		{
			First,   // nothing published yet
			Inline,  // the parities' inline pass
			Frame,   // the frame's own pass
			Taken,   // the builder took the last publication
			Count
		};
		struct PaceStats
		{
			std::uint64_t passes = 0, published = 0, deferred = 0, folded = 0;  // full passes, publications, deferred passes, of them folded
			std::array<std::uint64_t, static_cast<std::size_t>(PublishReason::Count)> byReason{};
			std::uint32_t frames = 0, lastFrame = ~0u, passesThisFrame = 0, maxPasses = 0, publishedThisFrame = 0, maxPublished = 0;
			// The claims (PublishClaims): replayed from the phases log (keys replayed), copied whole, or the last shared again.
			std::uint64_t claimsReplayed = 0, claimsKeys = 0, claimsWhole = 0, claimsShared = 0, claimsRevoked = 0;
			double restMs = 0.0;  // PublishTables' whole-copied rest, per publication summed
			// The replay (W6): change- and publication-log entries read, material-log entries read, the object slots and material records
			// copied, slot columns copied and skipped (no cause of the slot's covers the column), contiguous runs copied, replays whose
			// columns and records went to the pool, pool entries chosen nearer the tables than the first free one would have been, and the
			// replay's time (objects, materials) per publication summed.
			std::uint64_t replayLogEntries = 0, replayMaterialEntries = 0, replaySlots = 0, replayMaterials = 0, replayColumns = 0, replaySkipped = 0, replayRuns = 0,
						  replayPooled = 0, replayNearer = 0;
			double replayMs = 0.0;
			// The pass's fixed cost (W6): commits whose forward-pipeline count was taken from the last catalog's, waiting lists compacted
			// (a mark moved), and commits that left them as they were.
			std::uint64_t forwardCounted = 0, forwardKept = 0, waitingCompacted = 0, waitingKept = 0;
			// The paced parity (CheckPacedPublication): publications checked, tables and claims that differ.
			std::uint64_t parityChecks = 0, parityTables = 0, parityClaims = 0;
			std::string parityFirst;
		} paceStats;
		std::uint32_t paceWindow = ~0u;
		struct PumpStats
		{
			std::vector<double> passMs;                // each pass's whole time
			std::vector<std::uint32_t> passEvents;     // tracker events and queue entries drained
			std::vector<double> publishMs;             // oldest unpublished event to its publication
			std::vector<std::uint32_t> publishFrames;  // the same in render-thread frames
			std::uint64_t passes = 0, eventsOnly = 0, loading = 0, unchanged = 0, published = 0, inlinePasses = 0, failed = 0;
			// Passes by what woke them (each source it took; last: none, an inline pass or a burst's continuation) and those that published
			// nothing; walks that took the per-frame entries (once a frame) and those that did not.
			std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count) + 1> bySource{}, unchangedBySource{};
			std::uint64_t perFrameWalks = 0, eventWalks = 0;
			double fullMs = 0.0, fullMaxMs = 0.0;  // whole passes (not events alone)
			// The sceneFrames seen since the report (first, last), frames with a pass, the most passes in one frame.
			std::uint32_t firstFrame = 0, lastFrame = 0, framesWithPass = 0, passesThisFrame = 0, maxPassesInFrame = 0;
			bool any = false;
		} pumpStats;
		bool holdLostMembers = false;  // from the joins on: members RunSceneWork drops are held for PrimaryCull
		// What a scene pass holds for the render thread while it runs.
		bool inSceneTask = false;       // on the lane, not inline
		bool holdPrimaryNotes = false;  // PrimaryCull's notes held (primaryNotes)
	};
}
