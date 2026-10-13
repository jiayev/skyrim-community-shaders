#pragma once

// IndirectDraws::Impl: the state of IndirectDraws' threads (the render thread's frame, the revision producer, the snapshot builder),
// and the functions over it. Part of IndirectDraws' implementation: included by Internal.h, last.

namespace DCLF
{
	// The implementation's names resolve here as they do in DCLF::Draws.
	using namespace Draws;

	struct IndirectDraws::Impl
	{
		std::shared_ptr<Resources> resources;
		// The main rows the last main commit sent (its payload's kept views): what a deferred growth of their tables is filled with,
		// posted for the revision code (ReserveMainSequences reads its copy, RevisionProducer::committedRows).
		struct CommittedRows
		{
			KeptView<MaterialRow> materials;
			KeptView<PipelineRow> pipelines;
		};
		LatestSlot<CommittedRows> committedRowsSlot;
		// What the resources were created for; a change rebuilds them (and the graph).
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::optional<Capture> pending;  // this frame's main-pass bindings, until the epoch runs

		// The shadow views (CS_DCLF_SHADOWS). Their own resources and CPU staging, so the main path's are
		// untouched; the frame's shared uploads happen in the first view's epoch.
		std::shared_ptr<ShadowResources> shadow;
		bool shadowSetupFailed = false;
		// One view's culling counters, sampled every so many epochs and mapped a few frames later.
		struct ShadowCullReadback
		{
			winrt::com_ptr<ID3D11Buffer> count;
			std::uint32_t copiedFrame = 0;
			std::uint32_t view = 0, mode = 0;
		};
		std::optional<ShadowCullReadback> shadowCullReadback;
		std::uint32_t shadowCullEpochs = 0;
		std::uint32_t shadowLoggedTargets = 0;
		void ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats);
		using PendingView = Draws::PendingView;
		std::vector<PendingView> pendingViews;
		// Per occlusion view (ExecuteOcclusion): the view as DCLF computes it from the map's camera (OcclusionView), the state and format
		// its pipelines are built for, and the frame whose shadow commit uploaded its occluders.
		struct OcclusionState
		{
			PendingView view;
			std::uint32_t capturedFrame = ~0u;
			std::uint32_t rasterState = 0;
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			std::uint32_t committedFrame = ~0u;
			std::uint32_t inputs = 0, skipped = 0;
		};
		std::array<OcclusionState, kOcclusionViews> occlusion;
		/** @brief The occlusion views' formats, for the scene lane's shadow pipelines (PostLookupInputs). */
		std::array<DXGI_FORMAT, kOcclusionViews> OcclusionFormats() const
		{
			std::array<DXGI_FORMAT, kOcclusionViews> viewFormats{};
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				viewFormats[v] = occlusion[v].dsvFormat;
			return viewFormats;
		}
		/**
		 * @brief A shadow view of a captured phase this frame's epoch cannot draw: the casters the engine withheld from it are a hole
		 * this frame (counted, flagged). The capability stays: the set does not change with a frame.
		 */
		void ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats);
		/**
		 * The shadow capability (IndirectDraws::ShadowCapability): the modes DCLF draws and the rasterizer state catalog their
		 * pipelines are built for (PhaseReady's), set up once (UpdateShadowCapability) and changed only by a toggle, a failure, an
		 * occlusion map's first capture, or a defect: a view of a mode or under a state the catalog does not hold (NoteCapability,
		 * flagged "<- PHASES"). Render thread; the set reads the published phases.
		 */
		std::array<bool, kShadowModeCount> readyModes{};
		std::array<ModeRasterStates, kShadowModeCount> readyStates{};
		std::atomic<std::uint8_t> shadowCapability{ 0 };
		bool shadowCatalogBuilt = false;
		DXGI_FORMAT shadowTargetFormat = DXGI_FORMAT_UNKNOWN;
		/** @brief The catalog: every solid-fill state of the engine's table its shadow views can draw with, and the cascades' clones. */
		void BuildShadowCatalog();
		/**
		 * @brief The capability's modes or catalog changed: the readiness serial moves and every caster is decided again. a_defect: a
		 * view the capability did not foresee (counted and named, "<- PHASES").
		 */
		void NoteCapability(bool a_defect, std::string a_cause);
		struct CapabilityStats
		{
			std::uint32_t changes = 0;  // every change of the modes or the catalog, setup's included
			std::uint32_t defects = 0;  // of them, a view the capability did not foresee
			std::string lastCause;
		} capabilityStats;
		/**
		 * @brief Step 6e S1: the shadow payload the frame's epoch committed (the installed publication's, built ahead with it), which the
		 * occlusion epoch and the revision shapes read until the next commit. It holds no publication (the builds ahead drop their
		 * streams: the publication holds them).
		 */
		std::shared_ptr<ShadowPayload> committedShadow;
		std::uint64_t shadowUnbuilt = 0;  // covered frames whose installed publication had no shadow payload (<- UNBUILT), since the last report
		std::vector<std::shared_ptr<ShadowPayload>> shadowPayloadPool;  // the builds task's
		std::shared_ptr<ShadowPayload> AcquireShadowPayload();
		std::shared_ptr<const void> shadowExecutionOwner;  // reused by the sky epoch's copy of the shadow records
		/**
		 * @brief The capability's modes (occlusion maps included) with the catalog's states, and the shadow targets' format, which the
		 * builds ahead and the scene lane's shadow lookups are for (UpdateShadowCapability, PostLookupInputs).
		 */
		struct LastShadow
		{
			std::array<bool, kShadowModeCount> modes{};
			std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			bool known = false;
		} lastShadow;
		// What the scene lane's shadow pipelines are resolved for (T6b2c step 5): posted by the frame's start when it changes (PostLookupInputs:
		// postedShadowLookups, the render thread's), taken by the scene lane (ResolveLookups), latest wins.
		std::atomic<std::shared_ptr<const ShadowLookupInputs>> shadowLookupInputs;
		ShadowLookupInputs postedShadowLookups;
		/**
		 * @brief The shadow views' placements (BuildShadowPlacements, at setup): every place the engine's shadow maps can take a view -
		 * the sun's cascade slices and their volumetric copies, and each slice of the lights' map whole (a spot light) or as two halves
		 * (a point light's paraboloid pair) - each with its mode and rasterizer state, at a slot of its own (kFirstShadowViewSlot + its
		 * index). The shadow epoch's shape is all of them, whichever views a frame has: a view draws at its placement's slot, and a slot
		 * without one does no work. A view at no placement, under another state, or twice in a frame is a defect
		 * (ShadowNotReady::Placement): the casters withheld from it are a hole, flagged.
		 */
		std::vector<ShadowViewLayout> shadowPlacements;
		bool BuildShadowPlacements();
		/** @brief The placement a captured view drew at (its target, slice and viewport), or SIZE_MAX. */
		std::size_t PlacementOf(const PendingView& a_view) const;
		std::uint32_t placementDefectsLogged = 0;
		// Since the last report: frames whose views were left to the engine as the selected revision had no shape for the placements, or
		// the installed publication no shadow payload (startup, a toggle).
		std::uint64_t shadowUnrecorded = 0;
		/**
		 * @brief The occlusion maps' layout as their last captures drew (CaptureOcclusion, taken whether DCLF draws a map or not): what
		 * a revision makes the occlusion epoch's shapes for (RevisionProducer::recentOcclusionLayouts, newest first; posted with the
		 * revision inputs), and what OcclusionReady asks the selected revision to have a shape for.
		 */
		std::vector<ShadowViewLayout> PredictedOcclusion() const;
		static constexpr std::size_t kRecentOcclusionLayouts = 4;
		std::uint64_t occlusionUnrecorded = 0;  // maps left to the engine for want of the revision's shape, since the last report
		/**
		 * @brief The sequence buffers' reserves (render thread, before an epoch): each grown to hold every draw the scene's tracked
		 * objects can produce (SceneDrawBound, kept in drawBound). A bound over the device's max sequence count, or over the sort's rank field,
		 * is a hard failure.
		 */
		/** @brief A view rasterizer state's map row's buckets, from a_cache (the revision producer's). */
		static const RowBuckets& ShadowRowBuckets(RowBucketsCache& a_cache, std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups,
			const ShadowIndirectState& a_indirect);
		/** @brief The producer's draw bound brought up to date with a_tables' change log (the revision code: the reserves). */
		void UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation);
		/** @brief The revision code: a_resources' sequences, bucket layout and rows grown to a_tables (a growth, Growths::Change). */
		void ReserveMainSequences(Resources& a_resources, const SceneTables& a_tables, std::uint32_t a_generation);
		/** @brief The revision code: the index pool's room for every geometry the tables have (a growth, Growths::Change). */
		void ReserveIndexPool(SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_generation);
		void ReserveShadowSequences(ShadowResources& a_shadow, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_slots, std::uint32_t a_first = 0);
		/** @brief The revision code: grows the shadow material rows' table to a_wanted (the last build's, posted) and a quarter more. */
		void ReserveShadowRows(ShadowResources& a_shadow, std::uint32_t a_wanted);
		/**
		 * @brief The revision code: view slots for Skylighting's map and a_views views, and pipeline map rows of a_keys key slots: more
		 * slots are more buffers, which the shadow passes declare, so the graph is built again; either growth is a new latch block
		 * (the producer's, RevisionLatches).
		 */
		void ReserveShadowLatch(ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys, std::uint32_t a_rasterStates, std::uint32_t a_sunProcesses);
		// The revision code: the producer's main latch (RevisionLatches::main): a new block when it holds fewer cascades, shadow volumes
		// or buckets than asked for.
		void ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets = 0);
		std::uint32_t shadowRowsWanted = 0;  // the last shadow build's (ShadowPayload::rowsWanted)
		ShadowInputs PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const;
		std::uint32_t shadowLoggedReasons = 0;
		bool ShadowNotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((shadowLoggedReasons >> a_reason) & 1)) {
				shadowLoggedReasons |= 1u << a_reason;
				logger::info("[DCLF] shadow views not ready: {}", a_what);
			}
			return false;
		}
		bool SetupShadow();
		bool ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target);
		/** @brief ImportShadowDepth's import of a_texture, through the depth-stencil view a_view, as depth target a_index (a_label: for the log). */
		bool ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label);

		// The volatile t16+ inputs retain a live import only while their register
		// keeps naming that SRV. This bounds reuse without an age-based import cache.
		struct FrameTextureBinding
		{
			ID3D11ShaderResourceView* view = nullptr;
			GpuTextures::Binding binding;
		};
		std::array<FrameTextureBinding, kTextureRegisters> frameTextureBindings{};
		std::uint32_t frameTextureGeneration = ~0u;
		// The commits' own uploads on the render thread (CommitUploads).
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> commitStagedPool;
		BuildCache mainCache;  // the main list's region, both segments' (BuildMainPayloads)
		/**
		 * @brief Step 6e E3b, the builds ahead: the coordinator builds the main payloads with the publication they draw (BuildAhead),
		 * from what the frame's start posted (AheadContext: the resources, the inputs' frame part, prepared by PrepareMainInputs). Its
		 * journals keep what the payload ring's entries lack (ringHolders, step 6e E5): the frame's producer sends each entry that. The
		 * frames that install the publication commit them (installedDraws); only CS_DCLF_BINDLESS_PARITY builds at the epoch.
		 */
		struct DrawPublication
		{
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const StreamViews> streams;
			std::array<std::shared_ptr<MainPayload>, 2> payloads;  // kAsyncColour, kAsyncZPrepass
			std::shared_ptr<ShadowPayload> shadow;                 // with its exclusions (step 6e S1)
		};
		struct AheadContext
		{
			bool valid = false;
			std::array<MainInputs, 2> inputs;
			std::array<bool, 2> build{};
			std::shared_ptr<Resources> target;
			// The shadow payload's (step 6e S1): its inputs' frame part (PrepareShadowInputs for the last epoch's views) and resources.
			bool shadow = false;
			ShadowInputs shadowInputs;
			std::shared_ptr<ShadowResources> shadowTarget;
		} aheadContext;  // the render thread's, made at the frame's start (PostAheadContext)
		// The fits the builds ahead are made against (the main builds', the shadow build's), posted with aheadContext; changes counted.
		// The render thread's (PostFit).
		std::array<SceneFit, 2> postedFit{};
		std::array<std::uint32_t, 2> postedRows{};  // the main material and pipeline rows' capacities (MainBuild::ResolvePair's rowsFit)
		std::uint64_t postedFitSerial = 0;
		void PostFit();
		/**
		 * @brief T6b3a: what the frame's start posts for the coordinator, immutable once posted (latest wins): the ahead context and the
		 * fits with their serial. The coordinator takes it at each pass's start (TakeAheadContext) and reads only its copy (BuildAhead,
		 * FitsScene, SceneFitSerial).
		 */
		struct AheadPost
		{
			AheadContext context;
			std::array<SceneFit, 2> fit{};
			std::array<std::uint32_t, 2> rows{};
			std::uint64_t fitSerial = 0;
		};
		LatestSlot<AheadPost> aheadSlot;
		AheadPost aheadTaken;  // the coordinator's
		/** @brief Render thread: the context and the fits as they are now, posted (a_fit: PostFit first, the resources there). */
		void PostAhead(bool a_fit = true);
		std::vector<std::shared_ptr<MainPayload>> payloadPool;  // the builds' task's
		// The builds ahead posted (PostSnapshotWork, the coordinator: each work item's number) and done (the snapshot builder: the number
		// of the item it built, which covers the ones it skipped). The frame's fallback streams are refused while one is outstanding.
		std::atomic<std::uint64_t> aheadKicked{ 0 };
		std::atomic<std::uint64_t> aheadDone{ 0 };
		std::atomic<bool> aheadRunning{ false };  // a build ahead running now (WaitAhead)

		/**
		 * @brief T6b3b: what the coordinator hands the snapshot builder (PostSnapshotWork): a publication and what its draws are built from
		 * as the coordinator had them (its copy of the frame's ahead context, its frame, the candidates). Latest wins (snapshotWorkSlot).
		 */
		struct SnapshotWork
		{
			std::shared_ptr<const ScenePublication> publication;
			AheadContext context;
			std::uint32_t frame = 0;
			std::shared_ptr<const SunCandidates> sun, light;
			std::uint64_t aheadNumber = 0;  // aheadKicked when posted
		};
		/**
		 * @brief T6b3b: a complete scene snapshot (the builder's, posted to snapshotSlot, adopted whole by the frame's start: AdoptSnapshot):
		 * a publication, its draws built ahead, and its revision whole - versions, each epoch's shape and recordings, the growths it names -
		 * with the generations it was built for, which the adoption compares, and what its build took.
		 */
		struct SceneSnapshot
		{
			std::uint64_t serial = 0;
			std::shared_ptr<const ScenePublication> publication;
			std::shared_ptr<const DrawPublication> draws;
			org::async::RevisionAssembler::Lease revision;
			// The builder's verdict on its draws: both main payloads, built for the main resources it was built for (or the epochs build
			// their own: CS_DCLF_BINDLESS_PARITY). Without, its frames have no claims (DecideCoverage).
			bool drawsComplete = false;
			// The generations: the graph's build its recordings are of, the targets and shadow format its pipeline sets are for, the main
			// resources' (RevisionInputs::mainGeneration). Its commit's toggles are its publication's.
			std::uint64_t buildGeneration = 0, mainGeneration = 0;
			std::uint32_t targetsGeneration = 0;
			DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		};
		/** @brief One snapshot build's times (the builder's, drained by the report): its draws (0 when they were kept), shapes, recordings. */
		struct SnapshotTiming
		{
			double aheadMs = 0.0, shapesMs = 0.0, recordingsMs = 0.0;
			bool ahead = false;
		};
		LatestSlot<SnapshotWork> snapshotWorkSlot;  // the coordinator -> the builder
		LatestSlot<SceneSnapshot> snapshotSlot;     // the builder -> the frame's start
		// The frame's: the adopted snapshot, and whether the newest it found was stale (its frames have no claims until a current one).
		std::shared_ptr<const SceneSnapshot> adoptedSnapshot;
		bool snapshotStale = false;
		// Snapshots the frame replaced, back to the builder, which drops them (nothing of a snapshot is released on the render thread).
		Retired<SceneSnapshot> retiredSnapshots;
		/**
		 * @brief The builder's state (SnapshotPass: one pass at a time, on the pool, woken by every post it takes, a revision's completion,
		 * a growth settled, the frame's inputs and a retired snapshot): the work item it builds, its draws, the revision it waits for.
		 */
		struct SnapshotBuilder
		{
			enum class Stage : std::uint8_t
			{
				Idle,        // a snapshot of work stands (or none yet): made again when the frame's inputs move
				Make,        // its revision to make (again), then its draws for the versions the revision names
				Waiting,     // made, none sealed (a growth or a build pending): made again when woken
				Recordings,  // sealed, its draws built: its recordings awaited (the assembler's completion wakes the builder)
			};
			Stage stage = Stage::Idle;
			std::unique_ptr<SnapshotWork> work;
			std::shared_ptr<const DrawPublication> draws;
			std::uint64_t sealed = 0;  // the revision sealed for work
			std::uint64_t serial = 0, lastPublication = 0;
			std::chrono::steady_clock::time_point sealedAt{};
			SnapshotTiming timing;
		} snapshotBuilder;
		std::atomic<bool> snapshotInputsMoved{ false };  // the frame posted revision inputs since the builder's last make
		std::atomic<bool> producerReportWanted{ false };
		LatestSlot<std::string> producerReportSlot;      // the builder's report lines, for the frame's report
		std::string producerReport;                      // the frame's: the newest taken
		// The builder's counters (atomic: the report's exchange), and its builds' times (drained by the report).
		std::atomic<std::uint64_t> snapshotsBuilt{ 0 }, snapshotsCoalesced{ 0 }, snapshotsWithoutDraws{ 0 };
		EventQueue<SnapshotTiming, 256> snapshotTimings;
		// The frame's counters since the last report: adopted, frames adopting nothing new, stale by kind, the frames from a publication's
		// commit to its adoption.
		enum SnapshotStale : std::size_t
		{
			kStaleBuild,
			kStaleTargets,
			kStaleShadowFormat,
			kStaleToggles,
			kStaleMain,
			kStaleKinds
		};
		std::uint64_t snapshotsAdopted = 0, snapshotFramesKept = 0;
		std::array<std::uint64_t, kStaleKinds> snapshotsStale{};
		std::vector<std::uint32_t> adoptionLatency;
		// The guard (DecideCoverage, unconditional): frames whose adopted snapshot's draws embed a face positions address that is not
		// the current version's (withdrawn, <- STALE DRAWS; 0 by construction: BuildSnapshotDraws). And from a growth's adoption (its
		// frame, ~0u: none outstanding) to the first frame whose draws embed the versions current then, in frames.
		std::uint64_t staleDraws = 0;
		std::uint32_t growthAdoptedFrame = ~0u;
		std::vector<std::uint32_t> growthToDraws;
		// T6b3d: from the oldest event its publication was the first to carry (ScenePublication::eventNs, eventFrame) to its adoption,
		// ms and the render thread's frames (the frame's; the report's).
		std::vector<double> eventAdoptionMs;
		std::vector<std::uint32_t> eventAdoptionFrames;
		/** @brief The snapshot builder's pump (configured once, never destroyed: a function-local static). */
		org::async::SerializedTaskPump& SnapshotPump();
		/** @brief The builder's pass (SnapshotPump's drain): see SnapshotBuilder. */
		void SnapshotPass();
		/**
		 * @brief The builder: the work item's draws, built ahead (RunAhead), and the builds ahead done up to its number. Every address
		 * they embed of a versioned buffer is of the version a_versions names (the revision sealed for them; none: the context's, for a
		 * snapshot without a revision): the face positions, the only one (the rows' tables, the objects and extras are the payload
		 * ring's, the frame's own; the constants buffers are the resources', never versioned).
		 */
		void BuildSnapshotDraws(const VersionSet* a_versions);
		/** @brief The device address of the version of a_buffer a_versions names (its current one where it names none, or without a set). */
		static std::uint64_t NamedVersionAddress(const VersionSet* a_versions, const Versioned& a_buffer);
		/** @brief Whether a_draws embed the face positions at a_address wherever they embed them at all (0 in a payload: none embedded). */
		static bool DrawsEmbedFacePositions(const DrawPublication& a_draws, std::uint64_t a_address);
		/** @brief The builder: the snapshot of its work item, draws and the completed revision a_revision, posted for the frame. */
		void PostSnapshot(org::async::RevisionAssembler::Lease a_revision);
		/** @brief The builder's report lines (its counters reset): the revisions, the growths, the shapes made, the draw bound. */
		std::string ProducerReport();
		/** @brief The builder's report lines of the stores its builds write (main rows, object records, shadow state, extras, geometry, exclusions), reset. */
		std::string StoresReport();
		/** @brief The frame's report lines of the snapshots, and the builder's last. */
		std::string SnapshotReport();
		/** @brief The builds' task: a publication's stream views and main payloads (on the pool, in order). */
		std::shared_ptr<const DrawPublication> RunAhead(std::shared_ptr<const SceneTables> a_tables, const AheadContext& a_context, const Lookups& a_lookups,
			std::uint32_t a_generation, std::uint32_t a_frame, std::shared_ptr<const SunCandidates> a_sunCandidates, std::shared_ptr<const SunCandidates> a_lightCandidates);
		/** @brief Every builds' task kicked done (teardown, the toggle, a load screen). */
		void WaitAhead();
		/** @brief Teardown, the toggle, a load screen: the fade write-back task done (never per frame). */
		void WaitFadeWriteBack();
		/**
		 * @brief T7b6, the depth commit (render thread, inside the epoch: the host has waited for the frame slot's previous execution):
		 * that execution's fade events harvested from the slot's host buffer (a batch for the write-back task), the event list's header
		 * written (count 0, this frame), and the frame slot armed for this frame (FadeWriteBack).
		 */
		void ArmFadeReadback(SceneBuffers& a_scene, CommitUploads& a_uploads);
		/**
		 * @brief T7b1 (CS_DCLF_ACCUMULATED_PARITY), the depth commit (render thread, inside the epoch: the host has waited for the frame
		 * slot's previous execution): that execution's events harvested from the slot's host buffer, the event list's count zeroed, the
		 * reported words zeroed once per backing, and the frame slot armed for this frame (VisibilityReadback).
		 */
		void ArmVisibilityReadback(SceneBuffers& a_scene, CommitUploads& a_uploads);
		/** @brief Teardown, the toggle, a load screen: the visibility decode task done (never per frame). */
		void WaitVisibilityReadback();
		std::shared_ptr<const DrawPublication> installedDraws;  // the frame's
		/**
		 * @brief The pipeline slots the installed publication's members can draw with: its tables'. A revision that covers it was made at or
		 * after its commit, so its latch holds them (ReserveMainLatch, ReserveReflection); the newest tables may name more, which no member
		 * of the frame uses.
		 */
		std::uint32_t InstalledPipelineSlots() const
		{
			return installedDraws && installedDraws->tables ? static_cast<std::uint32_t>(installedDraws->tables->pipelines.size()) : 0u;
		}
		/**
		 * @brief Step 6e E4: the payload ring. Each frame the epochs read one entry - the installed publication's payload buffers
		 * (object records, extras rows, geometry table, the rows' tables, each segment's inputs) - which the frame's producer
		 * (FrameValues' job, its other uploads) brings up to that publication on the dedicated uploader before it signals the frame's
		 * wait: each buffer is sent what changed since the version it holds. An entry is reused once the frame that last read it is
		 * done on the GPU. The epochs name it by values (the latches, the frame record): no recording binds it.
		 */
		static constexpr std::uint32_t kPayloadRing = 4;
		struct RingPart
		{
			std::shared_ptr<org::Buffer> buffer;
			std::uint64_t capacity = 0;  // elements
			std::uint32_t srvIndex = 0;
			std::uint64_t address = 0;
			std::uint64_t held = 0;  // the version it holds (the producer's)
		};
		struct RingEntry
		{
			RingPart objects;
			RingPart extras;
			RingPart geometries, materialRows, pipelineRows;
			RingPart inputs;  // the main list, both segments' (U4a)
			// The shadow payload's (step 6e S2): its material rows and each mode's inputs (the occlusion maps' included).
			RingPart shadowRows;
			org::PersistentGraphHost::GpuPoint reuse;  // the last frame that read it
		};
		std::array<RingEntry, kPayloadRing> payloadRing;
		std::uint64_t payloadRingSeq = 0;
		/** @brief What a frame's epochs read of the ring: its entry's values, for the payloads of its publication. */
		struct RingFrame
		{
			bool valid = false;
			std::uint32_t entry = 0;
			std::shared_ptr<const DrawPublication> draws;
			std::uint32_t objectsIndex = 0, extrasIndex = 0, geometriesIndex = 0;
			std::uint32_t inputsIndex = 0;  // the main list's
			std::uint64_t materialRows = 0, pipelineRows = 0;
			bool shadow = false;  // the entry holds the publication's shadow payload
			std::uint64_t shadowRows = 0;
			std::uint32_t shadowInputsIndex = 0;
		};
		RingFrame ringFrame;
		struct RingStats
		{
			std::uint64_t frames = 0, grown = 0, bytes = 0, runs = 0, committed = 0, shadowCommitted = 0;
		} ringStats;
		std::atomic<std::uint64_t> ringBytes{ 0 }, ringRuns{ 0 };
		// By buffer (RingPartIndex), for the report.
		enum RingPartIndex : std::size_t
		{
			kRingObjects,
			kRingExtras,
			kRingGeometries,
			kRingMaterialRows,
			kRingPipelineRows,
			kRingResident,
			kRingFrameInputs,
			kRingShadowRows,
			kRingShadowInputs,
			kRingParts
		};
		std::array<std::atomic<std::uint64_t>, kRingParts> ringPartBytes{};
		/**
		 * @brief Step 6e E5: the versions the ring's entries hold of each journal (their producer sets them as it queues an entry's
		 * uploads), the oldest of which the builds keep changes back to (ChangeJournal::BeginBuild): each entry is sent what changed
		 * since it was last filled, a few frames back, never everything. The scene buffers (the shadow commits', a fallback's) are
		 * holders too, uncounted: they are a frame behind at most in steady play, and one that falls below is sent everything.
		 */
		struct RingHolders
		{
			KeptHolders<kPayloadRing> objects, extras, geometries, materialRows, pipelineRows;
			KeptHolders<kPayloadRing> resident;  // the main list's region
			KeptHolders<kPayloadRing> shadowRows;
		} ringHolders;
		// The entry the frame's shadow commit read (none: its own buffers), which the occlusion epoch's latches name too.
		RingFrame ringShadow;
		/** @brief The ring's values into a shadow or occlusion view's latch, for its mode (none: the views' own buffers). */
		static void ShadowRingLatch(const RingFrame& a_ring, std::uint32_t a_mode, BuildDrawsLatch& a_latch)
		{
			if (!a_ring.valid || !a_ring.shadow || a_mode >= kShadowModeCount)
				return;
			a_latch.payloadValid = 1;
			a_latch.inputsIndex = a_ring.shadowInputsIndex;
			a_latch.geometriesIndex = a_ring.geometriesIndex;
			a_latch.materialRowsLo = static_cast<std::uint32_t>(a_ring.shadowRows);
			a_latch.materialRowsHi = static_cast<std::uint32_t>(a_ring.shadowRows >> 32);
			a_latch.pipelineRowsLo = a_latch.pipelineRowsHi = 0;
		}
		/** @brief Whether a commit of a_payload reads the frame's ring entry (an installed payload, the entry filled for it). */
		bool RingFor(const MainPayload& a_payload, std::size_t a_job) const
		{
			return ringFrame.valid && !a_payload.foreignRows && ringFrame.draws && ringFrame.draws->payloads[a_job].get() == &a_payload;
		}
		/** @brief The ring's values into a culling latch (none: the pass's own buffers). */
		static void RingLatch(const RingFrame& a_ring, BuildDrawsLatch& a_latch)
		{
			if (!a_ring.valid)
				return;
			a_latch.payloadValid = 1;
			a_latch.inputsIndex = a_ring.inputsIndex;
			a_latch.geometriesIndex = a_ring.geometriesIndex;
			a_latch.materialRowsLo = static_cast<std::uint32_t>(a_ring.materialRows);
			a_latch.materialRowsHi = static_cast<std::uint32_t>(a_ring.materialRows >> 32);
			a_latch.pipelineRowsLo = static_cast<std::uint32_t>(a_ring.pipelineRows);
			a_latch.pipelineRowsHi = static_cast<std::uint32_t>(a_ring.pipelineRows >> 32);
		}
		MainRows fallbackRows;
		// The scene tables (SceneBuffers) and their stores, every epoch's: the builds ahead run one at a time, in publication order.
		std::shared_ptr<SceneBuffers> scene;
		// The frame's tree LOD decision (DecideTreeLod), which the depth commit draws on; and the commits that could not.
		bool treeLodOwned = false;
		std::uint32_t treeLodMissed = 0;
		/**
		 * @brief A view's frame state (ViewEpoch.cpp), the render thread's: its targets (and their copy published for the scene lane's set
		 * commit, Drawable and SlotReady, which never read `targets`), each pipeline slot's forward pipeline as the frame's catalog has it,
		 * its resources, and whether the frame's epochs draw its members (DecideCoverage, at BeginSceneFrame: before the engine draws it).
		 */
		struct ViewEpochState
		{
			ForwardTargets targets;
			std::atomic<std::uint64_t> laneColours{ 0 }, laneDepth{ 0 };  // colour in the low 32 bits, colour2 in the high
			void PublishTargets()
			{
				const std::uint64_t colours = std::uint64_t(static_cast<std::uint32_t>(targets.colour)) | (std::uint64_t(static_cast<std::uint32_t>(targets.colour2)) << 32);
				if (laneColours.load(std::memory_order_relaxed) != colours)
					laneColours.store(colours, std::memory_order_release);
				if (laneDepth.load(std::memory_order_relaxed) != static_cast<std::uint64_t>(targets.depth))
					laneDepth.store(static_cast<std::uint64_t>(targets.depth), std::memory_order_release);
			}
			ForwardTargets LaneTargets() const
			{
				const std::uint64_t colours = laneColours.load(std::memory_order_acquire);
				ForwardTargets out;
				out.colour = static_cast<DXGI_FORMAT>(colours & 0xFFFFFFFFu);
				out.colour2 = static_cast<DXGI_FORMAT>(colours >> 32);
				out.depth = static_cast<DXGI_FORMAT>(laneDepth.load(std::memory_order_acquire));
				return out;
			}
			std::vector<rhi::PipelineHandle> slotPipelines;
			std::shared_ptr<ViewResources> resources;
			bool setupFailed = false;
			bool covered = false;
		};
		// The water reflection's faces (Reflection.cpp; dclf-lod.md, "Water reflections"): the forward programs and pipelines of the LOD
		// they draw (PrepareReflection), this update's captured faces, and the counters since the last report.
		struct ReflectionState : ViewEpochState
		{
			ReflectionState() { covered = true; }
			std::uint32_t lodSlots = 0, programsReady = 0, pipelinesReady = 0;
			bool treeReady = false;
			rhi::PipelineHandle treePipeline{};
			bool treeOwned = false;  // the faces' tree LOD is DCLF's this frame (PassCapture::SetReflectionTreeLodOwned)
			// This update's faces (ReflectionFaceCamera), until ExecuteReflection takes them.
			struct Face
			{
				bool captured = false;
				std::array<std::byte, kViewBlockBytes> perFrame{};
				std::uint32_t perFrameBytes = 0;
				std::array<float, 16> viewProj{};
				RE::NiPoint3 eye;
			};
			std::array<Face, kReflectionFaces> faces;
			// The face the engine last oriented the camera to (ReflectionFaceCamera), for the observers at its draws; the updates
			// (Policy::KeepReflectionRoots).
			std::uint32_t orientedFace = ~0u;
			std::uint32_t residueUpdates = 0;
			winrt::com_ptr<ID3D11Texture2D> cube;  // the cube target the faces render into
			std::uint32_t width = 0, height = 0;
			// Since the last report: faces captured, updates, epochs and faces drawn, and the updates not drawn by cause.
			std::uint32_t facesCaptured = 0, updates = 0, epochs = 0, facesDrawn = 0;
			std::array<std::uint32_t, 6> skipped{};  // not drawable, not covered (DecideCoverage), no faces, stale inputs, no resources, the epoch failed
			// The scene frame of the last ExecuteReflection, and the faces captured after it in the same frame (none: every update
			// of a frame runs before BeforeShadowMaps).
			std::uint32_t executedFrame = ~0u, lateFaces = 0;
		} reflection;
		// The first-person view (FirstPerson.cpp): its colour epoch's targets are kMAIN's format, the motion vectors' (colour2) and
		// kPOST_ZPREPASS_COPY's.
		struct FirstPersonState : ViewEpochState
		{
			std::uint32_t slots = 0, pipelinesReady = 0;
			// The slots the skeleton's geometry draws with (BeginFirstPersonFrame's walk), and the frame they were found.
			std::vector<std::uint8_t> slotsWanted;
			std::uint32_t slotsFrame = 0;
			// The frame's view: Main::Draw's cVar23 recomputed at the frame's start (BeginFirstPersonFrame), the frame it is of; the
			// depth range the engine sets (RenderDepth's second argument: the control-driven range).
			bool latch = false;
			std::uint32_t latchFrame = ~0u;
			float depthRange = 0.1f;
			// The first-person camera as the engine set it up for the frame (FUN_1414cb020), in world space (at the depth epoch): its
			// VS_PerFrame block, its view-projection (eye-relative) and eye; last frame's (the motion vectors' previous camera).
			std::array<std::byte, kViewBlockBytes> perFrame{};
			std::array<float, 16> viewProj{};
			RE::NiPoint3 eye;
			PreviousCamera previous;
			bool havePrevious = false;
			// The frame whose depth epoch built the sequences the colour epoch draws.
			std::uint32_t depthFrame = ~0u;
			// Since the last report: frames reached, frames latched, epochs submitted, and the epochs not submitted by cause.
			std::uint64_t frames = 0, latched = 0, depthEpochs = 0, colourEpochs = 0;
			std::array<std::uint64_t, 7> skipped{};  // not drawn (latch, claims), not covered, no camera, stale inputs, no resources, failed, no depth epoch
			// The forward range: whether the main set's forward pipelines can draw into the first-person pass (kPOST_ZPREPASS_COPY's depth
			// format the main pass's: BeginFirstPersonFrame), published for the scene lane (FirstPersonSlotReady); the forward members the
			// installed tables hold (BeginFirstPersonFrame's walk), summed over the frames since the last report.
			std::atomic<bool> forwardCompatible{ false };
			std::uint64_t forwardMembers = 0;
			std::uint32_t forwardMembersNow = 0;  // the last walk's (every 16 frames)
		} firstPerson;
		ViewEpochState& View(ViewKind a_kind) { return a_kind == ViewKind::Reflection ? static_cast<ViewEpochState&>(reflection) : firstPerson; }
		const ViewEpochState& View(ViewKind a_kind) const { return a_kind == ViewKind::Reflection ? static_cast<const ViewEpochState&>(reflection) : firstPerson; }
		// The revision code's, per view: each pipeline slot's forward pipeline as its request's catalog has it (MakeRevision), the
		// reflection's tree LOD pipeline, first person's forward slots.
		std::array<std::vector<rhi::PipelineHandle>, kViews> viewRevisionPipelines;
		rhi::PipelineHandle viewRevisionTree{};
		std::vector<std::uint8_t> viewForwardSlots;
		/** @brief A view's graph resources, created once its targets are known (render thread). */
		bool SetupView(ViewKind a_kind);
		/**
		 * @brief The revision code: a view's latch for a_slots pipeline slots and a_buckets buckets, its buffers for a_draws a face, its
		 * tree lists for the scene's shape slots.
		 */
		void ReserveView(ViewResources& a_view, SceneBuffers* a_scene, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws);
		/** @brief The engine's kPOST_ZPREPASS_COPY imported (again when it changes), and the extension added. */
		bool ImportFirstPersonDepth();
		/** @brief The reflection phase's readiness of an object (PhaseReady): its pipeline slot's forward pipeline is built. */
		bool ReflectionPhaseReady(const SceneTables& a_tables, std::uint32_t a_slot) const;
		/** @brief The engine's cube target imported (again when it changes). */
		bool ImportReflectionCube(ID3D11Texture2D* a_texture);
		/**
		 * @brief A view's commit (render thread, inside its epoch): the shape's map (none for the slots gained since), each face's bucket
		 * table, culling latch (a_inputs of the frame's scene list for a drawn face) and block, and with a_treeRow its tree LOD rows, into
		 * the shape's latch. Returns the faces drawn.
		 */
		std::uint32_t CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const TreeLod::DrawRow* a_treeRow);
		/**
		 * @brief What an epoch's execution holds besides its shape: a_payloads' binding owners, the frame textures' bindings, the ring
		 * entry's draws and the adopted snapshot whole (its versions and what its draws embed). Null without the host's cleanup.
		 */
		std::shared_ptr<std::vector<std::shared_ptr<const void>>> EpochOwners(std::span<const MainPayload* const> a_payloads);
		/** @brief The scene tables, created once (render thread). False when they have no device address. */
		bool EnsureSceneBuffers(rhi::Device a_device);
		/**
		 * @brief The scene tables grown to hold what a_tables does, and the per-object buffers with them (ReserveObjectBuffers).
		 * Render thread, before a build's inputs are taken: the tables are final for the frame from the scene phase on, so the
		 * first call of a frame does any growing and the builds after it see the capacities they are built against.
		 */
		void ReserveSceneTables(SceneBuffers& a_scene, const SceneTables& a_tables);
		/** @brief The main and shadow resources' per-object buffers grown to the scene's object capacity. */
		void ReserveObjectBuffers();
		ObjectRecordStore objectStore;
		ExtrasStore extrasStore;
		GeometryStore geometryStore;
		/**
		 * @brief The per-frame streams (drawcall-limit-fix.md, "The streams leave the builds"): the object records and the extras
		 * rows the tables hold now, updated from the change log and uploaded as the changes since what the scene buffers hold.
		 * Render thread, at every epoch's commit, before its passes: a placement, a palette or a shading value reaches the
		 * epoch that draws it, whichever build it ran with, and no build waits for them. Returns the rows it sent.
		 */
		struct SceneStreams
		{
			std::size_t objects = 0, extraRows = 0, objectBytes = 0, extraRowsSent = 0;
		};
		/**
		 * @brief Render thread: the frame's stream views - the installed publication's (the builds task made them with it), or, with
		 * none installed for the frame's tables, made here while the coordinator and the builds task are idle (they own the stores;
		 * none is made while they run, counted). What the epochs' own builds and a commit that reads no ring entry take (step 6e S3).
		 */
		std::shared_ptr<const StreamViews> StreamsNow();
		std::shared_ptr<const StreamViews> streamViews;  // made here, for the tables of streamViewsKey (released at the frame's end)
		StreamsKey streamViewsKey;
		std::uint64_t streamsRefused = 0;  // views wanted while the coordinator or the builds task ran (since the last report)
		std::shared_ptr<MainPayload> AcquirePayload()
		{
			// An idle payload holds nothing: what it held (its stream views and their tables, its owners) would keep the retirement
			// chain from that publication on (step 6e E3).
			std::shared_ptr<MainPayload> free;
			for (auto& payload : payloadPool)
				if (payload.use_count() == 1) {
					payload->Reset();
					if (!free)
						free = payload;
				}
			return free ? free : payloadPool.emplace_back(std::make_shared<MainPayload>());
		}
		// The payloads the frame's main epochs committed, by job (the forward and first-person epochs draw the colour one's).
		std::array<const MainPayload*, 2> committedPayload{};
		std::array<std::uint32_t, 2> committedFrame{};
		// T7d: the forward epoch's (ExecuteForward), since the last report: frames it was reached for the main camera, epochs submitted, and
		// why the rest were not (no colour epoch this frame, no forward recording in the revision, the epoch failed).
		struct ForwardEpochStats
		{
			std::uint64_t reached = 0, epochs = 0, noColour = 0, noRecording = 0, failed = 0;
			std::uint32_t submittedFrame = ~0u;
			// T7d's refraction (ExecuteForward): epochs whose recording drew the refraction normals (the normals marked begun for the engine),
			// and epochs with refraction members whose draw could not (no pipeline yet, no refraction normals, a format the pipeline is not of).
			std::uint64_t refractionDrawn = 0, refractionNotReady = 0;
			std::uint32_t refractionFrame = ~0u;  // the frame whose forward epoch drew the refraction normals
		} forwardEpoch;
		// CS_DCLF_BINDLESS_PARITY's payloads, built at the epoch (the parity's builds read the eye the Z-prepass captures).
		std::array<MainPayload, 2> fallbackPayloads;
		// Per main epoch since the last report: covered frames whose installed publication had no payload for the main resources
		// (not submitted, <- UNBUILT).
		std::array<std::uint64_t, 2> unbuilt{};
		SceneStreams CommitSceneStreams(SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_frame, std::uint32_t a_generation,
			CommitUploads& a_uploads);
		MainRows mainRows;  // both main segments' (MainRows)
		ShadowKept shadowKept;  // the shadow epoch's inputs and records (Step 6)
		ShadowKept* ShadowKeptState() { return &shadowKept; }
		SunExclusionCache sunExclusionCache;  // the shadow builds', in frame order
		SunExclusionCache parabolicExclusionCache;  // likewise, the paraboloid mode's (LocalLightCull)
		std::array<std::uint32_t, 5> decalWords{};  // the count buffer's decal words (19-22, then 27), uploaded per colour epoch

		/** @brief The per-frame constant blocks of an epoch from the capture's mirrors (render thread; records the Z-prepass's bytes for the replay). */
		void PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out);
		// Per main epoch (Z-prepass, colour): each frame slot's last captured block, for a frame that lacks it (PackFrameBlocks).
		struct CarriedBlocks
		{
			std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
		};
		std::array<CarriedBlocks, 2> carriedBlocks;
		std::uint64_t frameSlotsCarried = 0;  // since the last report
		std::uint32_t carriedLogged = 0;      // the slots logged once (bit: slot, +8 colour, +16 pixel)
		/** @brief The build's inputs, snapshotted on the render thread. Without a capture (a job kicked ahead of the epoch) the eye is the replayed one. */
		MainInputs PrepareMainInputs(const Capture* a_capture, bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask);

		// Per main epoch (kAsyncColour, kAsyncZPrepass): the frame slots it last supplied, which the builds ahead are made for.
		struct EpochMasks
		{
			std::uint32_t vsMask = 0, psMask = 0;
			bool known = false;
		};
		std::array<EpochMasks, 2> epochMasks;
		/** @brief Inside the epoch's preparation: the frame textures and blocks, the uploads, the PassFrame. */
		bool CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
			const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats,
			std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision);

		// The vertex-stage inputs the Z-prepass wrote its depth with. The colour pass tests EQUAL against
		// that depth, so it has to transform the geometry to exactly the same place - and it cannot simply
		// read the buffers again, because the engine rewrites the per-frame constants between the depth
		// pass and the composite, and the camera moves on. Reusing the bytes makes the two epochs agree.
		std::array<std::vector<std::byte>, kConstantBufferRegisters> prepassVS;
		RE::NiPoint3 prepassEye, prepassPreviousEye;
		// The main pass's viewport depth range, kept from its capture. The Z-prepass writes its depth with
		// this rather than with the depth pass's own range, so that the depth in the buffer is what the
		// main pass - DCLF's colour draws and the native draws alike - tests EQUAL against. It is stable
		// frame to frame, so the previous frame's value is right for this frame's prepass.
		float mainMinDepth = 0.0f, mainMaxDepth = 0.0f;
		bool prepassInputs = false;
		std::uint32_t loggedDepthViewport = 0;
		std::uint32_t loggedColourViewport = 0;

		winrt::com_ptr<ID3D11Texture2D> mainPassDepth;  // what the main pass binds, to check the depth pass against
		bool loggedDepthMismatch = false;
		bool loggedNoViewProj = false;
		// The colour commits whose sun test ran without the sun's cascades, over the report window (Reports::ColourSunTestLine).
		Reports::Window sunTestWindow;
		std::uint32_t sunTestEmpty = 0;
		bool loggedFrameConstants = false;

		// The sun cascades the colour commit's BuildDraws tests against (its latch's).
		std::vector<SunAccumulation::GpuCascade> sunCascades;
		// The local shadow lights its BuildDraws selected against (LocalShadowLights), and their volumes as uploaded.
		LocalShadowLights localShadows;
		std::vector<GpuShadowVolume> shadowVolumes;
		// The depth commit's scratch: its buckets (PlanZBuckets) and the zeros their count words take.
		ZBucketPlan zBucketPlan;
		std::vector<std::uint32_t> zBucketZeros;

		/**
		 * @brief What a scene revision's main shapes are made from that only the frame's captures have (MakeRevision), taken at each
		 * main epoch (posted: RevisionInputs): per segment, whether one has run, its viewport and its frame blocks' sizes.
		 */
		struct MainEpochInputs
		{
			std::array<bool, 2> known{};
			std::array<MainViewport, 2> viewport{};
			std::array<FrameBlockSizes, 2> blockSizes{};
		} mainEpochInputs;

		/**
		 * @brief T6b3b b2a: what the scene revision's producer owns (MakeRevision, AssembleRevision, the reserves): nothing of it is read
		 * or written by the frame's epochs, and it reads nothing of theirs - its inputs (RevisionInputs, taken from revisionInputsSlot at
		 * each make) and its request alone. Its report reads the counters.
		 */
		struct RevisionProducer
		{
			RevisionInputs inputs;
			std::uint32_t frame = 0;  // the frame the make is for (MakeRevision's a_frame)
			RevisionLatches latches;
			MadeShapes made;
			// The occlusion maps' layouts drawn lately (newest first, kRecentOcclusionLayouts): a revision makes a shape for each.
			std::vector<std::vector<ShadowViewLayout>> recentOcclusionLayouts;
			// The scene's draw bound (UpdateDrawBound) and the index pool's (ReserveIndexPool), kept from the tables' logs.
			DrawBoundStore drawBound;
			IndexPool::Bound poolBound;
			RowBucketsCache rowBuckets;
			// The last main commit's rows (committedRowsSlot, taken at the make's start).
			CommittedRows committedRows;
			// The graph build a request of the frame's needs (ShadowSlotsRequest: the inputs' build generation it must reach, 0 none): no
			// revision is sealed before it (AssembleRevision), its recordings would be of the graph without what it declares.
			std::uint64_t buildWanted = 0;
		} producer;
		/** @brief Render thread: the newest RevisionInputs posted, latest wins; the producer takes it at each make's start. */
		LatestSlot<RevisionInputs> revisionInputsSlot;
		RevisionInputs postedInputs;  // the render thread's: what it last posted
		std::shared_ptr<const ShadowSlotBuffers> postedShadowSlots;  // the render thread's: the slots' buffers as last posted
		/**
		 * @brief The revision code's requests of the owner thread's (T6b3b b2a): shadow view slots, whose buffers the passes declare
		 * (ShadowSlotsRequest). The frame serves them (ServeRevisionRequests: the buffers made, the extension added, the inputs posted)
		 * after each make while the revision is made on the render thread, and at the build point before its build.
		 */
		LatestSlot<ShadowSlotsRequest> shadowSlotsSlot;
		void ServeRevisionRequests();
		/**
		 * @brief Render thread: the revision inputs as the frame has them now, posted when anything moved (generation and, for the main
		 * resources, mainGeneration with them). Called where each value is captured, and before each make while the revision is made
		 * on the render thread (b2a).
		 */
		void PostRevisionInputs();
		/**
		 * @brief The revision code (T6b3b b2a: on the render thread still, b2b's producer job): a scene revision's shapes for a_request
		 * (its tables, lookups and catalog, and its RevisionRequest counts), reserved and made from it and the producer's inputs
		 * alone, then assembled (AssembleRevision).
		 * a_frame: the frame the make is for (the report's; the publication's commitFrame).
		 */
		void MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame);
		/** @brief The revision code: the producer's latches made at their first sizes once their resources are (RevisionLatches). */
		void EnsureRevisionLatches();

		/**
		 * @brief R3c (b), the scene revisions (Revision/), assembled and selected but not drawn with yet. At the scene work's
		 * join (AssembleRevision) a draft names the versions (VersionSet), each epoch's shape (MakeRevision's: a fragment kept
		 * while the shape is the same) and each epoch's recording, which requires exactly its shape and the versions: an epoch whose
		 * shape and versions are unchanged inherits the last one's, and any other is recorded for the revision on ORG's host thread
		 * (PersistentGraphHost::RequestEpochRecording; a live epoch's recording is made but never admitted). BeginSceneFrame takes the
		 * newest complete revision with its snapshot (AdoptSnapshot; T6b3b: assembled and collected by the snapshot builder). Render thread, but for the recordings' completions (the host's thread).
		 */
		struct SceneRevisions
		{
			// By Epoch: the forward epoch's shape is the colour shape while CS_DCLF_FORWARD is on, the first-person epochs' one first-person
			// shape while CS_DCLF_FIRST_PERSON is on.
			static constexpr std::uint32_t kEpochs = Epoch::Count;
			// The growths slot (T6b3b b2a): the changes the revision names (Growths::NamedChanges), adopted by the frame with its selection.
			static constexpr std::uint32_t kVersionsSlot = 0, kShapeSlot = 1, kRecordingSlot = kShapeSlot + kEpochs, kGrowthsSlot = kRecordingSlot + kEpochs,
										   kSlots = kGrowthsSlot + 1;
			// The snapshot builder's (Begin, Seal, Collect and, as the exchange's only consumer, TrySelect and AcquireActive: T6b3b). A
			// revision's completion (its last recording, on ORG's host thread) wakes the builder.
			org::async::RevisionAssembler assembler{ kSlots, [] { WakeSnapshotBuilder(); } };
			std::uint64_t versionChanges = ~0ull;  // VersionRegistry::changes the versions fragment was made at
			std::uint64_t graphBuilds = ~0ull;     // and the host's graph build (PersistentGraphHost::BuildGeneration)
			std::uint64_t growthStamp = ~0ull;     // and Growths::stamp (the ready growths it names)
			// G2: a growth still pending at this make (Growths::Settle): no revision is sealed (the builder makes it again when woken).
			bool growthPending = false;
			std::uint64_t growthWaits = 0;  // joins that sealed nothing for it, since the last report
			std::uint64_t buildWaits = 0;   // makes that sealed nothing for a graph build asked of the frame (RevisionProducer::buildWanted)
			// The builder's, since the last report: drafts sealed, version sets made; per epoch the shapes that changed, the recordings
			// requested, refused (no async epochs), recorded (a request whose every shape is recorded) and failed, and the shapes recorded.
			std::uint64_t sealed = 0, versionSets = 0, sealFailures = 0;
			// The versions the last revision sealed names (the builder's): what its snapshot's draws embed (BuildSnapshotDraws).
			std::shared_ptr<const VersionSet> sealedVersions;
			struct EpochStats
			{
				std::uint64_t changed = 0, requested = 0, refused = 0;
				// The completions' (ORG's host thread): recorded, failed, dropped by a graph build (or recorded on another: asked again).
				std::atomic<std::uint64_t> recorded = 0, failed = 0, dropped = 0, shapes = 0;
			};
			std::array<EpochStats, kEpochs> epochs;
			std::atomic<std::uint32_t> failuresLogged = 0;
			// The frame's from here on. The adopted snapshot's revision, for the frame's epochs (AdoptSnapshot).
			org::async::RevisionAssembler::Lease active;
			std::uint64_t latchedMisses = 0;  // values a commit writing into a revision's shape staged: its latched copies lacked them
			// Per epoch since the last report: commits that found no shape or recording in the selected revision (not submitted: a
			// coverage decision missed it), values past the revision's latch (clamped: dropped), and commits that submitted the selected
			// revision's recording.
			std::array<std::uint64_t, kEpochs> unrevised{}, latchClamped{}, submitted{};
			// Strict epochs: the frame's coverage (DecideCoverage, BeginSceneFrame): per epoch, whether the selected revision has its
			// recordings, of the graph as built now; the main epochs' decide the frame's claims (uncovered: withdrawn). And the graph
			// builds made at the frame's build point (BuildPoint), and the frames withdrawn, since the last report.
			std::array<bool, kEpochs> covered{};
			bool explicitBuilds = false;
			std::uint64_t builds = 0, withdrawn = 0, ticketsReleased = 0;
			std::array<std::uint64_t, kEpochs> uncovered{};
			// Per epoch, the inputs (versions, shape) whose recording failed: not asked for again until they change.
			std::array<std::pair<std::shared_ptr<const org::async::RevisionFragment>, std::shared_ptr<const org::async::RevisionFragment>>, kEpochs> failedFor;
		} revisions;
		/** @brief The builder: the revision of MakeRevision's shapes, sealed (SceneRevisions); none while a growth or a build it needs is pending. */
		void AssembleRevision(std::uint32_t a_frame);
		/**
		 * @brief The recording requests not completed yet (AssembleRevision counts each up, its completion down, on ORG's host thread or
		 * a build's): what the build point gives unsubmitted tickets back for (BuildPoint), never the assembler's pending list.
		 */
		std::atomic<std::uint32_t> recordingsOutstanding{ 0 };
		/**
		 * @brief The selected revision's shape and recordings for epoch a_epoch, trusted (invariant 5): nothing of the frame is checked
		 * against them. Empty (counted, <- UNREVISED) only when a coverage decision let a frame through without them: the epoch is
		 * then not submitted.
		 */
		EpochRevision RevisionOf(std::uint32_t a_epoch);
		/**
		 * @brief Strict epochs: whether the frame's selected revision covers epoch a_epoch (DecideCoverage) and the graph it was recorded
		 * on still runs (no rebuild requested since: explicit builds keep the graph as built until the next frame's build point). An
		 * epoch that is not covered is the engine's, decided before the engine draws its work, and is not submitted. True without
		 * revisions.
		 */
		bool EpochCovered(std::uint32_t a_epoch) const;
		void SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index);
		std::string RevisionReport();
		/**
		 * @brief What the revisions' shapes are made from (MakeRevision), hashed: the growths, the pipeline sets, the heaps, the
		 * toggles, the lookups, the latches and their layouts, the main commits' viewport and frame blocks, the shadow placements and the
		 * occlusion maps' layouts, the scene's casting bound, the reflection's resources and pipelines. The shapes are made again only
		 * when it moves.
		 */
		enum ShapesKeyGroup : std::size_t
		{
			kKeyGrowths,
			kKeyPipelines,
			kKeyLookups,
			kKeyMain,
			kKeyShadow,
			kKeyBound,
			kKeyReflection,
			kKeyFirstPerson,
			kKeyGroups
		};
		static constexpr std::array<const char*, kKeyGroups> kKeyGroupNames = { "growths", "pipelines", "lookups", "main latch and captures", "shadow latch and layouts",
			"casting bound outgrown", "reflection", "first person" };
		using ShapesKey = std::array<std::uint64_t, kKeyGroups>;
		ShapesKey RevisionShapesKey(const IndirectState& a_indirect, const ShadowIndirectState& a_shadowIndirect, const Lookups& a_lookups, std::size_t a_pipelines,
			const TreeLodPipelines* a_treeLod) const;
		ShapesKey revisionShapesKey{};
		std::array<std::uint64_t, kKeyGroups> shapesMadeBy{};  // since the last report: makes by the group whose inputs moved
		// Since the last report: joins that made the shapes, and that kept the last ones.
		std::uint64_t shapesMade = 0, shapesKept = 0;
		/**
		 * @brief Whether the last made shadow and occlusion shapes hold a_bounds: every view's capacity and buckets at least what the
		 * bound needs of them, and no row at the bucket words (TrimmedRow orders such a row by the draws). Their capacities only grow
		 * (SizeShadowBuckets, GrowCapacity from the last made), so a shape made again from a bound they hold is the same shape.
		 */
		bool ShadowShapesHold(const ShadowBounds& a_bounds) const;
		// The casting bound's version last checked against the shapes (drawBound.shadowVersion), and the times it outgrew them: what the
		// shapes key names of the bound (kKeyBound), so a move of the bound alone keeps the shapes.
		std::uint64_t shadowBoundChecked = ~0ull, shadowBoundOutgrown = 0;

		std::uint32_t loggedReasons = 0;

		bool NotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((loggedReasons >> a_reason) & 1)) {
				loggedReasons |= 1u << a_reason;
				logger::info("[DCLF] Main-pass draws not ready: {}", a_what);
			}
			return false;
		}

		bool Setup(const Capture& a_capture, bool a_depthOnly = false);
	};
}
