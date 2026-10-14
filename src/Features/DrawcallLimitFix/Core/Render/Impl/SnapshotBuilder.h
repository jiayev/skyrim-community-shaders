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
			std::shared_ptr<const SunCandidates> sun, light;
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
		// The frame's: the adopted snapshot, and whether the newest found was stale (no claims until a current one).
		std::shared_ptr<const SceneSnapshot> adoptedSnapshot;
		// The frame's: the adopted snapshot's payload, installed with its draws (the snapshot holds it too, so the builder releases it),
		// bound after the frame's last submission (BindAdopted). The epochs read its parts by value.
		std::shared_ptr<const PayloadVersions> installedPayload;
		std::uint32_t installedPayloadFrame = 0;  // the frame it was installed at (a frame record of an earlier frame names another)
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
				// Sealed, draws built: its recordings awaited, its payload published once the frame will adopt it (PublishPayload; newer
				// work replaces it until then, nothing sent for it) and its uploads awaited (their completions and the frame's take wake
				// the builder).
				Recordings,
			};
			Stage stage = Stage::Idle;
			std::unique_ptr<SnapshotWork> work;
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
		} snapshotBuilder;
		std::atomic<bool> snapshotInputsMoved{ false };  // the frame posted revision inputs since the builder's last make
		std::atomic<bool> producerReportWanted{ false };
		LatestSlot<std::string> producerReportSlot;      // the builder's report lines, for the frame's report
		std::string producerReport;                      // the frame's: the newest taken
	};
}
