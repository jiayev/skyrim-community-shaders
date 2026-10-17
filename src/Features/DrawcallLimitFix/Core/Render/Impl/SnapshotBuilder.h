#pragma once

// IndirectDraws::Impl's builds ahead and snapshot builder state (ImplSnapshotBuilder, a base of Impl).
// Part of IndirectDraws' implementation: included by Impl.h, before the struct.

namespace DCLF
{
	// The builds ahead and the snapshot builder: the publication's draws and the ahead context, the coordinator's post,
	// the builder's work, its snapshots and their adoption by the frame.
	struct ImplSnapshotBuilder
	{
		/** @brief A publication's draws, built ahead by the snapshot builder (RunAhead); committed by the frames that install it. */
		struct DrawPublication
		{
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const StreamViews> streams;
			std::array<std::shared_ptr<MainPayload>, 2> payloads;  // kAsyncColour, kAsyncZPrepass
			std::shared_ptr<ShadowPayload> shadow;                 // with its exclusions
		};
		/**
		 * @brief The main payload whose rows, geometry table and frame inputs a publication's payload holds: the colour build's (it ran
		 * after the Z-prepass's), else the Z-prepass's; null: none. Both main commits read that payload's parts.
		 */
		static const MainPayload* NewestMainPayload(const DrawPublication& a_draws)
		{
			return a_draws.payloads[kAsyncColour] ? a_draws.payloads[kAsyncColour].get() : a_draws.payloads[kAsyncZPrepass].get();
		}
		struct AheadContext
		{
			bool valid = false;
			std::array<MainInputs, 2> inputs;
			std::array<bool, 2> build{};
			std::shared_ptr<Resources> target;
			// The shadow payload's: its inputs' frame part (for the last epoch's views) and resources.
			bool shadow = false;
			ShadowInputs shadowInputs;
			std::shared_ptr<ShadowResources> shadowTarget;
		} aheadContext;  // the render thread's, made at the frame's start (PostAheadContext)
		// Render thread (PostFit): the fits the builds ahead are made against (main, shadow), posted with aheadContext; changes counted.
		std::array<SceneFit, 2> postedFit{};
		std::uint64_t postedFitSerial = 0;
		/** @brief The frame's start's post for the coordinator, immutable, latest wins; it reads only its copy (aheadTaken). */
		struct AheadPost
		{
			AheadContext context;
			std::array<SceneFit, 2> fit{};
			std::uint64_t fitSerial = 0;
		};
		LatestSlot<AheadPost> aheadSlot;
		AheadPost aheadTaken;  // the coordinator's
		std::vector<std::shared_ptr<MainPayload>> payloadPool;  // the snapshot builder's
		// Builds ahead posted (coordinator) and done (builder, covering skipped ones); fallback streams refused while one is pending.
		std::atomic<std::uint64_t> aheadKicked{ 0 };
		std::atomic<std::uint64_t> aheadDone{ 0 };
		std::atomic<bool> aheadRunning{ false };  // a build ahead running now (WaitAhead)
		/** @brief The coordinator's hand-off to the snapshot builder (PostSnapshotWork), latest wins. */
		struct SnapshotWork
		{
			std::shared_ptr<const ScenePublication> publication;
			AheadContext context;
			std::uint32_t frame = 0;
			std::shared_ptr<const EntrySpace> sun, light;
			std::uint64_t aheadNumber = 0;  // aheadKicked when posted
		};
		/** @brief A complete scene snapshot (builder -> AdoptSnapshot): publication, draws, revision, and the generations compared. */
		struct SceneSnapshot
		{
			std::uint64_t serial = 0;
			std::shared_ptr<const ScenePublication> publication;
			std::shared_ptr<const DrawPublication> draws;
			// Its draws' payload (PayloadVersions, ready: the builder posts a snapshot only once its uploads completed); null without
			// draws. A snapshot without one draws nothing (DecideCoverage).
			std::shared_ptr<const PayloadVersions> payload;
			org::async::RevisionAssembler::Lease revision;
			// Both main payloads built for the main resources; without, its frames have no claims (DecideCoverage).
			bool drawsComplete = false;
			// The graph build its recordings are of, the targets and shadow format its pipeline sets are for, the main resources'
			// generation. Its commit's toggles are its publication's.
			std::uint64_t buildGeneration = 0, mainGeneration = 0;
			std::uint32_t targetsGeneration = 0;
			DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		};
		/** @brief One snapshot build's times (drained by the report): its draws (0 when kept), shapes, recordings. */
		struct SnapshotTiming
		{
			double aheadMs = 0.0, shapesMs = 0.0, recordingsMs = 0.0;
			bool ahead = false;
		};
		LatestSlot<SnapshotWork> snapshotWorkSlot;  // the coordinator -> the builder
		LatestSlot<SceneSnapshot> snapshotSlot;     // the builder -> the frame's start
		// Publication pacing (SnapshotWorkTaken): the sequence of the newest publication the builder took from snapshotWorkSlot (the
		// builder's store, release; the coordinator's load, acquire).
		std::atomic<std::uint64_t> snapshotWorkTaken{ 0 };
		// What gates a payload's publication (PublishPayload: only for a snapshot the frame will adopt). The frame's (AdoptSnapshot):
		// the serial of the last snapshot it took from the slot (adopted or stale) and the frame that took it (the frame first, then the
		// serial, both release). The coordinator's (NoteScenePassDone): the frame of the last scene pass that ended.
		std::atomic<std::uint32_t> snapshotTakenFrame{ 0 };
		std::atomic<std::uint64_t> snapshotTaken{ 0 };
		std::atomic<std::uint32_t> scenePassDoneFrame{ 0 };
		// The builder's: a sealed snapshot's publication, or work held for its build, waits on the gate above, so the scene pass's end wakes it; else that end wakes
		// nobody (a pass ends several times a frame). Set before the builder reads the gate again, and the pass's frame stored before
		// it is read (both seq_cst): one of them sees the other, so no end is missed. The wakes it skipped (the report's).
		std::atomic<bool> publicationGated{ false };
		std::atomic<std::uint64_t> passDoneWakesSkipped{ 0 };
		/**
		 * @brief The builder: the gate above is closed - the frame has yet to take the last snapshot posted, or the scene pass of the
		 * frame that took it has not ended. A payload waits on it (PublishPayload), and no build starts while it is closed (SnapshotPass).
		 */
		bool PublicationGateClosed() const
		{
			const std::uint64_t taken = snapshotTaken.load(std::memory_order_acquire);
			return taken < snapshotBuilder.serial || scenePassDoneFrame.load(std::memory_order_seq_cst) < snapshotTakenFrame.load(std::memory_order_acquire);
		}
		// The frame's: the adopted snapshot, and whether the newest found was stale (no claims until a current one).
		std::shared_ptr<const SceneSnapshot> adoptedSnapshot;
		// The frame's: the adopted snapshot's payload, installed with its draws (the snapshot holds it too, so the builder releases it),
		// bound after the frame's last submission (BindAdopted). The epochs read its parts by value.
		std::shared_ptr<const PayloadVersions> installedPayload;
		/** @brief The frame: the installed payload (null: none; a covered epoch always has one, ready: DecideCoverage). */
		const PayloadVersions* InstalledPayload() const { return installedPayload.get(); }
		// The snapshot builder's: the payloads' pools (D5), on DCLF's retirement clock; their single acquirer is the builder. Census is
		// any thread's.
		PayloadBuilder payloadBuilder{ FrameRetirement() };
		bool snapshotStale = false;
		// The adopted snapshot's revision's versions (held by it): what commits and latch writes take every versioned buffer's
		// address, SRV index and capacity from (Adopted).
		const NamedVersions* adoptedSet = nullptr;
		/** @brief The frame: the adopted set; it names nothing before the first adoption of a revision. */
		const NamedVersions& Adopted() const
		{
			static const NamedVersions kNone;
			return adoptedSet ? *adoptedSet : kNone;
		}
		// Snapshots the frame replaced, back to the builder, which drops them (nothing of a snapshot is released on the render thread).
		Retired<SceneSnapshot> retiredSnapshots;
		/** @brief The builder's state (SnapshotPass: one pass at a time on the pool, woken by posts, completions and growths). */
		struct SnapshotBuilder
		{
			enum class Stage : std::uint8_t
			{
				Idle,        // a snapshot of work stands (or none yet): made again when the frame's inputs move
				Make,        // its revision to make (again), then its draws for the versions the revision names
				Waiting,     // made, none sealed (a growth or a build pending): made again when woken
				// Sealed, draws built: its recordings awaited, its payload published once the frame will adopt it (PublishPayload; no
				// newer work replaces it: a build starts only while the gate is open) and its uploads awaited (their completions and the
				// frame's take wake the builder).
				Recordings,
			};
			Stage stage = Stage::Idle;
			std::unique_ptr<SnapshotWork> work;
			// Work taken from the slot while the publication gate is closed, not started (latest wins): taken once the gate opens, unless
			// it passes through at once (PassesThrough). The coordinator is not told it was taken (SnapshotWorkTaken), so it publishes no
			// more meanwhile.
			std::unique_ptr<SnapshotWork> held;
			std::shared_ptr<const DrawPublication> draws;
			// draws' payload (PublishPayload), published once per draws: a snapshot made again for the same draws keeps it.
			std::shared_ptr<const PayloadVersions> payload;
			std::uint64_t drawsSerial = 0;     // the serial of the sealed set draws embed the addresses of (0: none)
			std::uint64_t sealedSequence = 0;  // the assembler sequence of the revision sealed for work
			std::uint64_t serial = 0, lastPublication = 0;
			std::chrono::steady_clock::time_point sealedAt{};
			SnapshotTiming timing;
			// The report's (the builder's, reset by StoresReport): sealed draws whose publication waited for the frame (counted once per
			// draws), and draws newer work replaced before their payload was published.
			std::uint64_t publicationsWaited = 0, replacedUnpublished = 0;
			bool publicationWaited = false;  // the draws' publication has waited (publicationsWaited counted)
			// The report's (ProducerReport): the builder's passes, and those that found nothing to do (no work, or a standing snapshot
			// whose inputs had not moved); the publications passed through (PassThrough: only deltas moved).
			std::uint64_t passes = 0, idlePasses = 0, passedThrough = 0;
			// The report's (ProducerReport): draws built (BuildSnapshotDraws) and those replaced before a snapshot posted them; the passes
			// that held work for the gate; the builds whose stages ran on this thread for a small change (RunAhead) and on the pool.
			std::uint64_t buildsStarted = 0, buildsDiscarded = 0, heldPasses = 0, stagesInline = 0, stagesPooled = 0;
			bool drawsPosted = false;  // draws were posted in a snapshot (buildsDiscarded counts those that were not)
			bool fresh = false;  // work taken and not yet made (PassThrough is tried once, for it)
			/**
			 * @brief The last snapshot posted with a revision and complete draws, and what it was built from: a publication whose
			 * tables, lookups, catalog, claims and candidates are the same objects (the coordinator publishes them again only when they
			 * changed), with the same ahead context's shape and the frame's revision inputs unmoved, would be built into exactly this
			 * snapshot; it is posted again for that publication instead (PassThrough), which carries the publication's deltas to the frame.
			 */
			struct Posted
			{
				std::shared_ptr<const ScenePublication> publication;
				std::shared_ptr<const EntrySpace> sun, light;
				std::shared_ptr<const DrawPublication> draws;
				std::shared_ptr<const PayloadVersions> payload;
				org::async::RevisionAssembler::Lease revision;
				std::uint64_t drawsSerial = 0;
				// The ahead context's shape (which builds ran, for which resources and frame slots).
				bool valid = false, shadow = false;
				std::array<bool, 2> build{};
				const void* target = nullptr;
				const void* shadowTarget = nullptr;
				std::array<std::uint32_t, 2> vsMask{}, psMask{};
			} posted;
		} snapshotBuilder;
		/**
		 * @brief The builder's (MakeRevision): what the requirement pass (Begin, the participants' requirements, Seal, AssembleRevision)
		 * of the last make that sealed a revision read. A make with the same key would seal the same set and assemble the same revision,
		 * so it keeps that one (reusedNow); CS_DCLF_PERSISTENT_PARITY runs the pass anyway and compares.
		 */
		struct RevisionReuse
		{
			struct Key
			{
				// What the pass reads of the publication's tables, by value (TablesWitness, Producer.cpp), never by the tables' identity:
				// the coordinator pools them, so the same object comes back with other rows and a new one may hold the same rows. The
				// catalog (immutable: the same object holds the same contents; held weakly, so a freed one is never taken for a new one at
				// its address), the tables' generation and the request.
				std::uint64_t tables = 0;
				std::weak_ptr<const PipelineCatalog> catalog;
				std::uint32_t tablesGeneration = 0;
				RevisionRequest request;
				std::uint64_t inputs = 0;     // RevisionInputs::generation
				std::uint64_t lookups = 0;    // the lookups' entries the pass reads (RevisionMake::lookupsWitness)
				std::uint64_t shadowRows = 0; // and the shadow map rows the shapes read (ShadowRowsWitness)
				std::uint64_t volatiles = 0;  // what the pass reads besides the inputs and the publication (the event lists' asks, ...)
			} key;
			bool valid = false;      // key is the last sealed revision's, which stands (not failed since)
			bool reusedNow = false;  // this make kept the last revision (SnapshotPass takes it as sealed)
			std::uint64_t setSerial = 0;  // the set's serial the last seal named (the parity's)
			// The report's (ProducerReport): the makes that kept the last revision and that ran the pass; the parities (the pass run
			// anyway for an unchanged key; the shapes made anyway for an unchanged shapes key).
			std::uint64_t kept = 0, ran = 0;
			// The report's: the makes that ran it with no last revision to keep (none sealed yet, or one failed since), and the others by
			// what of the key moved (each part that did: Moved).
			enum Moved : std::size_t
			{
				kMovedTables,
				kMovedCatalog,
				kMovedGeneration,
				kMovedRequest,
				kMovedInputs,
				kMovedLookups,
				kMovedShadowRows,
				kMovedVolatiles,
				kMovedParts
			};
			static constexpr std::array<const char*, kMovedParts> kMovedNames{ "tables", "catalog", "tables generation", "request", "inputs", "lookups", "shadow rows",
				"volatiles" };
			std::uint64_t ranUnkept = 0;
			std::array<std::uint64_t, kMovedParts> movedBy{};
			/** @brief A make that ran the pass: counted, by the parts of the key that moved (a_moved, a bit by Moved; ~0u: none to keep). */
			void NoteRan(std::uint32_t a_moved)
			{
				++ran;
				if (!valid) {
					++ranUnkept;
					return;
				}
				for (std::size_t part = 0; part < movedBy.size(); ++part)
					movedBy[part] += (a_moved >> part) & 1u;
			}
			Diagnostics::ParityCounter parity, shapesParity;
		} revisionReuse;
		std::atomic<bool> snapshotInputsMoved{ false };  // the frame posted revision inputs since the builder's last make
		/**
		 * @brief The builder (SnapshotPass, Snapshot.cpp): the newest work posted, once a build may start from it - the publication gate
		 * open, or the work passing through the last snapshot posted (PassesThrough); else null, the work held (SnapshotBuilder::held)
		 * and the gate's opening armed to wake the builder (publicationGated).
		 */
		std::unique_ptr<SnapshotWork> WorkToBuild();
		std::atomic<bool> producerReportWanted{ false };
		LatestSlot<std::string> producerReportSlot;      // the builder's report lines, for the frame's report
		std::string producerReport;                      // the frame's: the newest taken
	};
}
