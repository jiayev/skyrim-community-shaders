#pragma once

// IndirectDraws::Impl's revision producer and scene revisions state (ImplProducer, a base of Impl).
// Part of IndirectDraws' implementation: included by Impl.h, before the struct.

namespace DCLF
{
	// The scene revision producer and the scene revisions (Revision/): the producer's state, the render thread's posts to it,
	// the assembler and the shapes key.
	struct ImplProducer
	{
		/** @brief What a revision's main shapes need from the frame's captures, per segment (posted in RevisionInputs). */
		struct MainEpochInputs
		{
			std::array<bool, 2> known{};
			std::array<MainViewport, 2> viewport{};
			std::array<FrameBlockSizes, 2> blockSizes{};
		} mainEpochInputs;
		/**
		 * @brief The scene revision producer's state (the snapshot builder's). Shares nothing with the frame's epochs: its inputs are
		 * RevisionInputs (revisionInputsSlot) and its request.
		 */
		struct RevisionProducer
		{
			RevisionInputs inputs;
			// The toggles the make's publication was committed under (MakeRevision): its view, family and cull mode choices are the
			// frames' that install it (never the frame's toggles when the inputs were posted).
			ToggleSet toggles{};
			std::uint32_t frame = 0;  // the frame the make is for (MakeRevision's a_frame)
			// The latches' layouts, and their blocks' handles in the scene's set (the sealed set names the blocks).
			RevisionLatchLayouts latches;
			RevisionLatchHandles latchHandles;
			MadeShapes made;
			// The occlusion maps' recent layouts (newest first, ShadowEngine::kRecentOcclusionLayouts): a revision makes a shape for each.
			std::vector<std::vector<ShadowViewLayout>> recentOcclusionLayouts;
			// The shadow epochs' recent sets of placements in use (newest first, ShadowEngine::kRecentActiveViews): a variant each.
			std::vector<std::uint64_t> recentActiveViews;
			// The scene's draw bound and the index pool's, kept from the tables' logs.
			DrawBoundStore drawBound;
			IndexPool::Bound poolBound;
			RowBucketsCache rowBuckets;
			// How the main epochs lay out the sequences (RequireMain; its first sizes Setup's), and the layout of the last sealed set.
			MainLayout mainLayout;
			bool mainLayoutMade = false;
			// The bucket count words the last shadow shapes were made for (ShadowShapesHold).
			std::uint32_t shadowBucketWords = 0;
			// The graph build a frame request needs (0 none): nothing is sealed before it, or its recordings would lack its declarations.
			std::uint64_t buildWanted = 0;
		} producer;
		/** @brief Revision code: the producer's draw bound brought up to date with a_tables' change log (Resources/Resources.cpp). */
		void UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation);
		/** @brief Render thread: the newest RevisionInputs posted, latest wins; the producer takes it at each make's start. */
		LatestSlot<RevisionInputs> revisionInputsSlot;
		RevisionInputs postedInputs;  // the render thread's: what it last posted (the shadow slots' buffers: ShadowEngine::postedShadowSlots)
		/** @brief The revision code's requests of the render thread (shadow view slots), served at the build point before its build. */
		LatestSlot<ShadowSlotsRequest> shadowSlotsSlot;
		/**
		 * @brief The scene revisions (Revision/), assembled by the snapshot builder: a draft names the versions, each epoch's shape and
		 * its recording, which needs exactly that shape and those versions: an unchanged epoch inherits the last recording, any other
		 * is recorded on ORG's host thread (a live epoch's is made but never admitted). Members are the builder's, the frame's or the
		 * recording completions', as each says.
		 */
		struct SceneRevisions
		{
			// By epoch (the schedule's rows, Revision/Epochs.h; kMaxEpochs of them, EpochCount() used): an epoch sharing a shape
			// (EpochTraits::sharesWith) holds its source's. The versions slot: the set the revision names (NamedVersions), adopted by the
			// frame with it.
			static constexpr std::uint32_t kVersionsSlot = 0, kShapeSlot = 1, kRecordingSlot = kShapeSlot + kMaxEpochs, kSlots = kRecordingSlot + kMaxEpochs;
			// The builder's alone (the only consumer); a revision's last recording completing wakes the builder.
			org::async::RevisionAssembler assembler{ kSlots, [] { WakeSnapshotBuilder(); } };
			// This make's sealed set (MakeRevision) and the layout its shapes were made from.
			SealedSet sealed;
			std::shared_ptr<const MainLayout> layout;
			// The versions fragment the revisions carry: the set's own, or one of the same set made for a graph build since (a
			// recording is of the build it was made on, so every epoch is recorded again).
			std::shared_ptr<const org::async::RevisionFragment> versionsFragment;
			std::shared_ptr<const NamedVersions> versionsFragmentSet;  // the sealed set versionsFragment was made for
			std::uint64_t graphBuilds = ~0ull;  // the build versionsFragment was made for (RevisionInputs::buildGeneration)
			std::uint64_t growthWaits = 0;  // makes whose set was pending (a growth batch in flight), since the last report
			std::uint64_t buildWaits = 0;   // makes that sealed nothing for a graph build asked of the frame (RevisionProducer::buildWanted)
			// The builder's report counters.
			std::uint64_t seals = 0, versionSets = 0, sealFailures = 0;
			// The versions and layout the last sealed revision names (the builder's): what its snapshot's draws embed.
			std::shared_ptr<const NamedVersions> revisionVersions;
			std::shared_ptr<const MainLayout> revisionLayout;
			struct EpochStats
			{
				std::uint64_t changed = 0, requested = 0, refused = 0;
				// The completions' (ORG's host thread): recorded, failed, dropped by a graph build (or recorded on another: asked again).
				std::atomic<std::uint64_t> recorded = 0, failed = 0, dropped = 0, shapes = 0;
			};
			std::array<EpochStats, kMaxEpochs> epochs;
			std::atomic<std::uint32_t> failuresLogged = 0;
			// The frame's from here on. The adopted snapshot's revision, for the frame's epochs.
			org::async::RevisionAssembler::Lease active;
			std::uint64_t latchedMisses = 0;  // values a commit staged because the revision's latched copies lacked them
			// Per epoch since the last report: commits with no shape or recording, values past the latch (dropped), submissions (a hosted
			// epoch's: its host's submissions with its work), and those of a recording other than the first (a shadow epoch's narrower
			// variant, the depth epoch's without the index pool or without the hosted reflection's work).
			std::array<std::uint64_t, kMaxEpochs> unrevised{}, latchClamped{}, submitted{}, variants{};
			// Per epoch, whether the selected revision has its recordings of the graph as built now (DecideCoverage).
			std::array<bool, kMaxEpochs> covered{};
			bool explicitBuilds = false;
			// Graph builds at the build point (and in a frame's wait for a current snapshot), frames whose main epochs were not covered
			// (DCLF not drawing yet, or a load screen up: the only uncovered frames), tickets given back for recordings.
			std::uint64_t builds = 0, uncoveredFrames = 0, ticketsReleased = 0;
			std::array<std::uint64_t, kMaxEpochs> uncovered{};
			// Per epoch, the inputs (versions, shape, the hosted epoch's shape: EpochTraits::hosts) whose recording failed: not asked again
			// until they change.
			std::array<std::array<std::shared_ptr<const org::async::RevisionFragment>, 3>, kMaxEpochs> failedFor;
		} revisions;
		/** @brief Recording requests not yet completed: what the build point releases unsubmitted tickets for. */
		std::atomic<std::uint32_t> recordingsOutstanding{ 0 };
		/** @brief What the revisions' shapes are made from, hashed by group; the shapes are made again only when it moves. */
		enum ShapesKeyGroup : std::size_t
		{
			kKeyVersions,
			kKeyPipelines,
			kKeyLookups,
			kKeyMain,
			kKeyShadow,
			kKeyBound,
			kKeyReflection,
			kKeyFirstPerson,
			kKeyGroups
		};
		static constexpr std::array<const char*, kKeyGroups> kKeyGroupNames = { "named set", "pipelines", "lookups", "main latch and captures", "shadow latch and layouts",
			"casting bound outgrown", "reflection", "first person" };
		using ShapesKey = std::array<std::uint64_t, kKeyGroups>;
		ShapesKey revisionShapesKey{};
		std::array<std::uint64_t, kKeyGroups> shapesMadeBy{};  // since the last report: makes by the group whose inputs moved
		// Since the last report: makes that made the shapes, and that kept the last ones.
		std::uint64_t shapesMade = 0, shapesKept = 0;
		// The casting bound's version last checked (drawBound.shadowVersion) and its outgrowths (kKeyBound: a move alone keeps the shapes).
		std::uint64_t shadowBoundChecked = ~0ull, shadowBoundOutgrown = 0;
	};
}
