#pragma once

// IndirectDraws::Impl: the state of IndirectDraws' threads and the functions over it: the render thread's frame (captures, the
// adopted snapshot, the payload ring, the epochs' commits), the coordinator's copy of the ahead context, the snapshot builder
// (builds ahead and the revision producer, on the preparation pool) and the report counters. Each member says whose it is.
// Included by Internal.h, last.

namespace DCLF
{
	// The implementation's names resolve here as they do in DCLF::Draws.
	using namespace Draws;

	struct IndirectDraws::Impl
	{
		std::shared_ptr<Resources> resources;
		// The main rows the last main commit sent, for the revision code to fill a deferred growth with.
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

		// The shadow views (CS_DCLF_SHADOWS): their own resources, apart from the main epochs'.
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
		// Per occlusion view: its view, its pipelines' state and format, and the frame whose commit uploaded its occluders.
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
		/** @brief A shadow view this frame's epoch cannot draw: its withheld casters are a hole (flagged); the capability stays. */
		void ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats);
		/**
		 * The shadow capability: the modes DCLF draws and the state catalog their pipelines are built for; changed only by a toggle,
		 * a failure, an occlusion map's first capture, or a defect ("<- PHASES"). Render thread; the set reads the published phases.
		 */
		std::array<bool, kShadowModeCount> readyModes{};
		std::array<ModeRasterStates, kShadowModeCount> readyStates{};
		std::atomic<std::uint8_t> shadowCapability{ 0 };
		bool shadowCatalogBuilt = false;
		DXGI_FORMAT shadowTargetFormat = DXGI_FORMAT_UNKNOWN;
		/** @brief The catalog: every solid-fill state of the engine's table its shadow views can draw with, and the cascades' clones. */
		void BuildShadowCatalog();
		/** @brief The capability changed: the readiness serial moves and every caster is decided again. a_defect: an unforeseen view. */
		void NoteCapability(bool a_defect, std::string a_cause);
		struct CapabilityStats
		{
			std::uint32_t changes = 0;  // every change of the modes or the catalog, setup's included
			std::uint32_t defects = 0;  // of them, a view the capability did not foresee
			std::string lastCause;
		} capabilityStats;
		/** @brief The shadow payload the frame's epoch committed, read by the occlusion epoch until the next commit. */
		std::shared_ptr<ShadowPayload> committedShadow;
		std::uint64_t shadowUnbuilt = 0;  // covered frames whose installed publication had no shadow payload (<- UNBUILT), since the last report
		std::vector<std::shared_ptr<ShadowPayload>> shadowPayloadPool;  // the snapshot builder's
		std::shared_ptr<ShadowPayload> AcquireShadowPayload();
		std::shared_ptr<const void> shadowExecutionOwner;  // reused by the sky epoch's copy of the shadow records
		/** @brief The capability's modes and states and the shadow targets' format that the builds ahead and shadow lookups are for. */
		struct LastShadow
		{
			std::array<bool, kShadowModeCount> modes{};
			std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			bool known = false;
		} lastShadow;
		// What the scene lane's shadow pipelines are resolved for, latest wins (postedShadowLookups: the render thread's copy).
		std::atomic<std::shared_ptr<const ShadowLookupInputs>> shadowLookupInputs;
		ShadowLookupInputs postedShadowLookups;
		/**
		 * @brief The shadow views' placements (built at setup): every place the engine's shadow maps can take a view (cascade slices
		 * and their volumetric copies, a light map slice whole or as a paraboloid pair's halves), each with its mode, state and slot.
		 * The shadow epoch's shape is all of them; an empty slot does no work. A view at no placement, under another state or twice in
		 * a frame is a defect (ShadowNotReady::Placement), flagged.
		 */
		std::vector<ShadowViewLayout> shadowPlacements;
		bool BuildShadowPlacements();
		/** @brief The placement a captured view drew at (its target, slice and viewport), or SIZE_MAX. */
		std::size_t PlacementOf(const PendingView& a_view) const;
		std::uint32_t placementDefectsLogged = 0;
		// Frames whose views were left to the engine (no shape or payload yet), since the last report.
		std::uint64_t shadowUnrecorded = 0;
		/** @brief The occlusion maps' layouts as last captured: what revisions make occlusion shapes for and OcclusionReady checks. */
		std::vector<ShadowViewLayout> PredictedOcclusion() const;
		static constexpr std::size_t kRecentOcclusionLayouts = 4;
		std::uint64_t occlusionUnrecorded = 0;  // maps left to the engine for want of the revision's shape, since the last report
		/** @brief A view rasterizer state's map row's buckets, from a_cache (the revision producer's). */
		static const RowBuckets& ShadowRowBuckets(RowBucketsCache& a_cache, std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups,
			const ShadowIndirectState& a_indirect);
		/** @brief Revision code: the producer's draw bound brought up to date with a_tables' change log. */
		void UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation);
		/**
		 * @brief Revision code (a growth): a_resources' sequences, buckets and rows grown to the draw bound. A bound over the device's
		 * max sequence count or the sort's rank field is a hard failure.
		 */
		void ReserveMainSequences(Resources& a_resources, const SceneTables& a_tables, std::uint32_t a_generation);
		/** @brief Revision code (a growth): the index pool's room for every geometry the tables have. */
		void ReserveIndexPool(SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_generation);
		void ReserveShadowSequences(ShadowResources& a_shadow, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_slots, std::uint32_t a_first = 0);
		/** @brief Revision code: grows the shadow material rows to a_wanted (the last build's) and a quarter more. */
		void ReserveShadowRows(ShadowResources& a_shadow, std::uint32_t a_wanted);
		/** @brief Revision code: view slots and map rows grown (more slots rebuild the graph; either growth is a new latch block). */
		void ReserveShadowLatch(ShadowResources& a_shadow, std::uint32_t a_views, std::uint32_t a_keys, std::uint32_t a_rasterStates, std::uint32_t a_sunProcesses);
		// Revision code: a new main latch block (RevisionLatches::main) when it holds fewer cascades, volumes or buckets than asked.
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
		/** @brief Imports a_texture through a_view as depth target a_index (a_label: for the log). */
		bool ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label);

		// The volatile t16+ inputs keep a live import only while their register keeps naming that SRV: reuse is bounded without an
		// age-based import cache.
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
		/** @brief A publication's draws, built ahead by the snapshot builder (RunAhead); committed by the frames that install it. */
		struct DrawPublication
		{
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const StreamViews> streams;
			std::array<std::shared_ptr<MainPayload>, 2> payloads;  // kAsyncColour, kAsyncZPrepass
			std::shared_ptr<ShadowPayload> shadow;                 // with its exclusions
		};
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
		std::array<std::uint32_t, 2> postedRows{};  // the main material and pipeline rows' capacities (MainBuild::ResolvePair's rowsFit)
		std::uint64_t postedFitSerial = 0;
		void PostFit();
		/** @brief The frame's start's post for the coordinator, immutable, latest wins; it reads only its copy (aheadTaken). */
		struct AheadPost
		{
			AheadContext context;
			std::array<SceneFit, 2> fit{};
			std::array<std::uint32_t, 2> rows{};
			std::uint64_t fitSerial = 0;
		};
		LatestSlot<AheadPost> aheadSlot;
		AheadPost aheadTaken;  // the coordinator's
		/** @brief Render thread: posts the context and the fits as they are now (a_fit: PostFit first). */
		void PostAhead(bool a_fit = true);
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
		// The frame's: the adopted snapshot, and whether the newest found was stale (no claims until a current one).
		std::shared_ptr<const SceneSnapshot> adoptedSnapshot;
		bool snapshotStale = false;
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
				Recordings,  // sealed, draws built: its recordings awaited (the assembler's completion wakes the builder)
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
		// The builder's counters (atomic: the report's exchange), and its builds' times.
		std::atomic<std::uint64_t> snapshotsBuilt{ 0 }, snapshotsCoalesced{ 0 }, snapshotsWithoutDraws{ 0 };
		EventQueue<SnapshotTiming, 256> snapshotTimings;
		// The frame's, since the last report: adopted, frames adopting nothing new, stale by kind, frames from commit to adoption.
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
		// The guard (DecideCoverage): frames whose draws embed a stale face positions address (withdrawn, <- STALE DRAWS; 0 by
		// construction); frames from a growth's adoption (~0u: none) to draws embedding its versions.
		std::uint64_t staleDraws = 0;
		std::uint32_t growthAdoptedFrame = ~0u;
		std::vector<std::uint32_t> growthToDraws;
		// From the oldest event its publication first carried (ScenePublication::eventNs, eventFrame) to its adoption, ms and frames.
		std::vector<double> eventAdoptionMs;
		std::vector<std::uint32_t> eventAdoptionFrames;
		/** @brief The snapshot builder's pump (configured once, never destroyed: a function-local static). */
		org::async::SerializedTaskPump& SnapshotPump();
		/** @brief The builder's pass (SnapshotPump's drain): see SnapshotBuilder. */
		void SnapshotPass();
		/**
		 * @brief The builder: the work item's draws (RunAhead). The only versioned address they embed, the face positions, is of the
		 * version a_versions names (none: the context's).
		 */
		void BuildSnapshotDraws(const VersionSet* a_versions);
		/** @brief The device address of the version of a_buffer a_versions names (its current one where it names none, or without a set). */
		static std::uint64_t NamedVersionAddress(const VersionSet* a_versions, const Versioned& a_buffer);
		/** @brief Whether a_draws embed the face positions at a_address wherever they embed them at all (0 in a payload: none embedded). */
		static bool DrawsEmbedFacePositions(const DrawPublication& a_draws, std::uint64_t a_address);
		/** @brief The builder: posts the snapshot of its work item, draws and the completed revision a_revision. */
		void PostSnapshot(org::async::RevisionAssembler::Lease a_revision);
		/** @brief The builder's report lines (counters reset): revisions, growths, shapes made, the draw bound. */
		std::string ProducerReport();
		/** @brief The builder's report lines of the stores its builds write (main rows, object records, shadow state, extras, geometry, exclusions), reset. */
		std::string StoresReport();
		/** @brief The frame's report lines of the snapshots, and the builder's last. */
		std::string SnapshotReport();
		/** @brief The snapshot builder: a publication's stream views and payloads (on the pool, in publication order). */
		std::shared_ptr<const DrawPublication> RunAhead(std::shared_ptr<const SceneTables> a_tables, const AheadContext& a_context, const Lookups& a_lookups,
			std::uint32_t a_generation, std::uint32_t a_frame, std::shared_ptr<const SunCandidates> a_sunCandidates, std::shared_ptr<const SunCandidates> a_lightCandidates);
		/** @brief Waits until no build ahead runs (DrainAsync; never per frame). */
		void WaitAhead();
		/** @brief Waits for the fade write-back task (DrainAsync; never per frame). */
		void WaitFadeWriteBack();
		/** @brief The depth commit: harvests the frame slot's last fade events for the write-back task, resets the list, arms the slot. */
		void ArmFadeReadback(SceneBuffers& a_scene, CommitUploads& a_uploads);
		/** @brief As ArmFadeReadback for the visibility events (the reported words zeroed once per backing). */
		void ArmVisibilityReadback(SceneBuffers& a_scene, CommitUploads& a_uploads);
		/** @brief Waits for the visibility decode task (DrainAsync; never per frame). */
		void WaitVisibilityReadback();
		std::shared_ptr<const DrawPublication> installedDraws;  // the frame's
		/** @brief The installed tables' pipeline slots; a covering revision was made at or after its commit, so its latch holds them. */
		std::uint32_t InstalledPipelineSlots() const
		{
			return installedDraws && installedDraws->tables ? static_cast<std::uint32_t>(installedDraws->tables->pipelines.size()) : 0u;
		}
		/**
		 * @brief The payload ring. Each frame's epochs read one entry (the installed publication's payload buffers), brought up to that
		 * publication by FrameValues' job on the dedicated uploader before the frame's wait is signalled, each buffer sent what changed
		 * since the version it holds. Reused once its last reader is done on the GPU. Epochs name it by values, so no recording binds it.
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
			RingPart inputs;  // the main list, both segments'
			// The shadow payload's: its material rows and each mode's inputs (the occlusion maps' included).
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
		// By buffer, for the report.
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
		 * @brief The versions the ring's entries hold of each journal; the builds keep changes back to the oldest
		 * (ChangeJournal::BeginBuild). The scene buffers are uncounted holders: one that falls below is sent everything.
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
			return ringFrame.valid && ringFrame.draws && ringFrame.draws->payloads[a_job].get() == &a_payload;
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
		// The scene buffers and their stores, every epoch's: the builds ahead run one at a time, in publication order.
		std::shared_ptr<SceneBuffers> scene;
		// The frame's tree LOD decision (DecideTreeLod), which the depth commit draws on; and the commits that could not.
		bool treeLodOwned = false;
		std::uint32_t treeLodMissed = 0;
		/**
		 * @brief A view's frame state (ViewEpoch.cpp, render thread). The scene lane reads only the published LaneTargets, never
		 * `targets`. `covered` is decided at the frame's start, before the engine draws the view.
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
		// The water reflection's faces (Reflection.cpp; dclf-lod.md, "Water reflections"): the LOD forward programs and pipelines,
		// this update's captured faces, and the report counters.
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
			// The face the engine last oriented the camera to (observers); Policy::KeepReflectionRoots' updates.
			std::uint32_t orientedFace = ~0u;
			std::uint32_t residueUpdates = 0;
			winrt::com_ptr<ID3D11Texture2D> cube;  // the cube target the faces render into
			std::uint32_t width = 0, height = 0;
			// Since the last report.
			std::uint32_t facesCaptured = 0, updates = 0, epochs = 0, facesDrawn = 0;
			std::array<std::uint32_t, 6> skipped{};  // not drawable, not covered (DecideCoverage), no faces, stale inputs, no resources, the epoch failed
			// The last ExecuteReflection's frame, and faces captured after it that frame (expected none).
			std::uint32_t executedFrame = ~0u, lateFaces = 0;
		} reflection;
		// The first-person view (FirstPerson.cpp): its colour targets are kMAIN's format, the motion vectors' (colour2) and
		// kPOST_ZPREPASS_COPY's.
		struct FirstPersonState : ViewEpochState
		{
			std::uint32_t slots = 0, pipelinesReady = 0;
			// The slots the skeleton's geometry draws with (BeginFirstPersonFrame's walk), and the frame they were found.
			std::vector<std::uint8_t> slotsWanted;
			std::uint32_t slotsFrame = 0;
			// Main::Draw's cVar23 recomputed, its frame; the engine's depth range (RenderDepth's second argument).
			bool latch = false;
			std::uint32_t latchFrame = ~0u;
			float depthRange = 0.1f;
			// The first-person camera as the engine set it up (FUN_1414cb020), in world space; last frame's for the motion vectors.
			std::array<std::byte, kViewBlockBytes> perFrame{};
			std::array<float, 16> viewProj{};
			RE::NiPoint3 eye;
			PreviousCamera previous;
			bool havePrevious = false;
			// The frame whose depth epoch built the sequences the colour epoch draws.
			std::uint32_t depthFrame = ~0u;
			// Since the last report.
			std::uint64_t frames = 0, latched = 0, depthEpochs = 0, colourEpochs = 0;
			std::array<std::uint64_t, 7> skipped{};  // not drawn (latch, claims), not covered, no camera, stale inputs, no resources, failed, no depth epoch
			// Whether the main set's forward pipelines can draw into the first-person pass (kPOST_ZPREPASS_COPY's depth format the
			// main pass's), published for FirstPersonSlotReady; the installed tables' forward members, summed since the last report.
			std::atomic<bool> forwardCompatible{ false };
			std::uint64_t forwardMembers = 0;
			std::uint32_t forwardMembersNow = 0;  // the last walk's (every 16 frames)
		} firstPerson;
		ViewEpochState& View(ViewKind a_kind) { return a_kind == ViewKind::Reflection ? static_cast<ViewEpochState&>(reflection) : firstPerson; }
		const ViewEpochState& View(ViewKind a_kind) const { return a_kind == ViewKind::Reflection ? static_cast<const ViewEpochState&>(reflection) : firstPerson; }
		// Revision code, per view: each pipeline slot's forward pipeline from its request's catalog, the reflection's tree LOD
		// pipeline, first person's forward slots.
		std::array<std::vector<rhi::PipelineHandle>, kViews> viewRevisionPipelines;
		rhi::PipelineHandle viewRevisionTree{};
		std::vector<std::uint8_t> viewForwardSlots;
		/** @brief A view's graph resources, created once its targets are known (render thread). */
		bool SetupView(ViewKind a_kind);
		/** @brief Revision code: a view's latch for a_slots slots and a_buckets buckets, buffers for a_draws per face, tree lists. */
		void ReserveView(ViewResources& a_view, SceneBuffers* a_scene, std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws);
		/** @brief The engine's kPOST_ZPREPASS_COPY imported (again when it changes), and the extension added. */
		bool ImportFirstPersonDepth();
		/** @brief The reflection phase's readiness of an object: its pipeline slot's forward pipeline is built. */
		bool ReflectionPhaseReady(const SceneTables& a_tables, std::uint32_t a_slot) const;
		/** @brief The engine's cube target imported (again when it changes). */
		bool ImportReflectionCube(ID3D11Texture2D* a_texture);
		/** @brief A view's commit (render thread): its map, faces and tree LOD rows into the shape's latch; returns faces drawn. */
		std::uint32_t CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const TreeLod::DrawRow* a_treeRow);
		/** @brief What an epoch's execution holds besides its shape (binding owners, ring draws, the adopted snapshot). */
		std::shared_ptr<std::vector<std::shared_ptr<const void>>> EpochOwners(std::span<const MainPayload* const> a_payloads);
		/** @brief The scene buffers, created once (render thread). False when they have no device address. */
		bool EnsureSceneBuffers(rhi::Device a_device);
		/** @brief The scene buffers and per-object buffers grown to hold a_tables (render thread, before a build's inputs are taken). */
		void ReserveSceneTables(SceneBuffers& a_scene, const SceneTables& a_tables);
		/** @brief The main and shadow resources' per-object buffers grown to the scene's object capacity. */
		void ReserveObjectBuffers();
		ObjectRecordStore objectStore;
		ExtrasStore extrasStore;
		GeometryStore geometryStore;
		/**
		 * @brief The per-frame streams (drawcall-limit-fix.md, "The streams leave the builds"): object records and extras rows,
		 * uploaded as changes at every epoch's commit (render thread), so no build waits for them.
		 */
		struct SceneStreams
		{
			std::size_t objects = 0, extraRows = 0, objectBytes = 0, extraRowsSent = 0;
		};
		/**
		 * @brief Render thread: the installed publication's stream views, or made here when the coordinator and builder are idle
		 * (they own the stores; refused and counted otherwise).
		 */
		std::shared_ptr<const StreamViews> StreamsNow();
		std::shared_ptr<const StreamViews> streamViews;  // made here, for the tables of streamViewsKey (released at the frame's end)
		StreamsKey streamViewsKey;
		std::uint64_t streamsRefused = 0;  // views wanted while the coordinator or the snapshot builder ran (since the last report)
		std::shared_ptr<MainPayload> AcquirePayload()
		{
			// An idle payload holds nothing: its stream views, tables and owners would keep the retirement chain from that
			// publication on.
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
		// The forward epoch's report counters.
		struct ForwardEpochStats
		{
			std::uint64_t reached = 0, epochs = 0, noColour = 0, noRecording = 0, failed = 0;
			std::uint32_t submittedFrame = ~0u;
			// Refraction: epochs that drew the normals (marked begun for the engine), and those whose members could not.
			std::uint64_t refractionDrawn = 0, refractionNotReady = 0;
			std::uint32_t refractionFrame = ~0u;  // the frame whose forward epoch drew the refraction normals
		} forwardEpoch;
		// Per main epoch since the last report: covered frames with no payload for the main resources (not submitted, <- UNBUILT).
		std::array<std::uint64_t, 2> unbuilt{};
		SceneStreams CommitSceneStreams(SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_frame, std::uint32_t a_generation,
			CommitUploads& a_uploads);
		MainRows mainRows;  // both main segments' (MainRows)
		ShadowKept shadowKept;  // the shadow epoch's inputs and records
		ShadowKept* ShadowKeptState() { return &shadowKept; }
		SunExclusionCache sunExclusionCache;  // the shadow builds', in frame order
		SunExclusionCache parabolicExclusionCache;  // likewise, the paraboloid mode's (LocalLightCull)
		std::array<std::uint32_t, 5> decalWords{};  // the count buffer's decal words (19-22, then 27), uploaded per colour epoch

		/** @brief An epoch's per-frame constant blocks from the capture's mirrors (render thread; keeps the Z-prepass's bytes). */
		void PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out);
		// Per main epoch (Z-prepass, colour): each frame slot's last captured block, for a frame that lacks it.
		struct CarriedBlocks
		{
			std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
		};
		std::array<CarriedBlocks, 2> carriedBlocks;
		std::uint64_t frameSlotsCarried = 0;  // since the last report
		std::uint32_t carriedLogged = 0;      // the slots logged once (bit: slot, +8 colour, +16 pixel)
		/** @brief The build's inputs, snapshotted on the render thread. Without a capture (a build ahead) the eye is the replayed one. */
		MainInputs PrepareMainInputs(bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask);

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

		// The vertex-stage inputs the Z-prepass wrote its depth with: the colour pass tests EQUAL against it, and the engine rewrites
		// the per-frame constants between the depth pass and the composite, so the colour pass reuses these bytes.
		std::array<std::vector<std::byte>, kConstantBufferRegisters> prepassVS;
		// The main pass's viewport depth range (from the last capture; stable frame to frame): the Z-prepass writes depth with it so
		// the main pass's EQUAL test matches.
		float mainMinDepth = 0.0f, mainMaxDepth = 0.0f;
		bool prepassInputs = false;
		std::uint32_t loggedDepthViewport = 0;
		std::uint32_t loggedColourViewport = 0;

		winrt::com_ptr<ID3D11Texture2D> mainPassDepth;  // what the main pass binds, to check the depth pass against
		bool loggedDepthMismatch = false;
		bool loggedNoViewProj = false;
		// Colour commits whose sun test ran without the sun's cascades, over the report window (Reports::ColourSunTestLine).
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
			std::uint32_t frame = 0;  // the frame the make is for (MakeRevision's a_frame)
			RevisionLatches latches;
			MadeShapes made;
			// The occlusion maps' recent layouts (newest first, kRecentOcclusionLayouts): a revision makes a shape for each.
			std::vector<std::vector<ShadowViewLayout>> recentOcclusionLayouts;
			// The scene's draw bound and the index pool's, kept from the tables' logs.
			DrawBoundStore drawBound;
			IndexPool::Bound poolBound;
			RowBucketsCache rowBuckets;
			// The last main commit's rows (committedRowsSlot, taken at the make's start).
			CommittedRows committedRows;
			// The graph build a frame request needs (0 none): nothing is sealed before it, or its recordings would lack its declarations.
			std::uint64_t buildWanted = 0;
		} producer;
		/** @brief Render thread: the newest RevisionInputs posted, latest wins; the producer takes it at each make's start. */
		LatestSlot<RevisionInputs> revisionInputsSlot;
		RevisionInputs postedInputs;  // the render thread's: what it last posted
		std::shared_ptr<const ShadowSlotBuffers> postedShadowSlots;  // the render thread's: the slots' buffers as last posted
		/** @brief The revision code's requests of the render thread (shadow view slots), served at the build point before its build. */
		LatestSlot<ShadowSlotsRequest> shadowSlotsSlot;
		void ServeRevisionRequests();
		/** @brief Render thread: posts the revision inputs when anything moved. Called where each value is captured. */
		void PostRevisionInputs();
		/**
		 * @brief The snapshot builder: a scene revision's shapes, reserved and made from a_publication and the producer's inputs
		 * alone, then assembled. a_frame: the publication's commitFrame.
		 */
		void MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame);
		/** @brief Revision code: the producer's latches made at their first sizes once their resources are. */
		void EnsureRevisionLatches();

		/**
		 * @brief The scene revisions (Revision/), assembled by the snapshot builder: a draft names the versions, each epoch's shape and
		 * its recording, which needs exactly that shape and those versions: an unchanged epoch inherits the last recording, any other
		 * is recorded on ORG's host thread (a live epoch's is made but never admitted). Members are the builder's, the frame's or the
		 * recording completions', as each says.
		 */
		struct SceneRevisions
		{
			// By Epoch: the forward epoch uses the colour shape, the first-person epochs share one.
			static constexpr std::uint32_t kEpochs = Epoch::Count;
			// The growths slot: the changes the revision names (Growths::NamedChanges), adopted by the frame with it.
			static constexpr std::uint32_t kVersionsSlot = 0, kShapeSlot = 1, kRecordingSlot = kShapeSlot + kEpochs, kGrowthsSlot = kRecordingSlot + kEpochs,
										   kSlots = kGrowthsSlot + 1;
			// The builder's alone (the only consumer); a revision's last recording completing wakes the builder.
			org::async::RevisionAssembler assembler{ kSlots, [] { WakeSnapshotBuilder(); } };
			std::uint64_t versionChanges = ~0ull;  // VersionRegistry::changes the versions fragment was made at
			std::uint64_t graphBuilds = ~0ull;     // and the host's graph build (PersistentGraphHost::BuildGeneration)
			std::uint64_t growthStamp = ~0ull;     // and Growths::stamp (the ready growths it names)
			// A growth still pending at this make (Growths::Settle): nothing is sealed until woken again.
			bool growthPending = false;
			std::uint64_t growthWaits = 0;  // makes that sealed nothing for it, since the last report
			std::uint64_t buildWaits = 0;   // makes that sealed nothing for a graph build asked of the frame (RevisionProducer::buildWanted)
			// The builder's report counters.
			std::uint64_t sealed = 0, versionSets = 0, sealFailures = 0;
			// The versions the last sealed revision names (the builder's): what its snapshot's draws embed.
			std::shared_ptr<const VersionSet> sealedVersions;
			struct EpochStats
			{
				std::uint64_t changed = 0, requested = 0, refused = 0;
				// The completions' (ORG's host thread): recorded, failed, dropped by a graph build (or recorded on another: asked again).
				std::atomic<std::uint64_t> recorded = 0, failed = 0, dropped = 0, shapes = 0;
			};
			std::array<EpochStats, kEpochs> epochs;
			std::atomic<std::uint32_t> failuresLogged = 0;
			// The frame's from here on. The adopted snapshot's revision, for the frame's epochs.
			org::async::RevisionAssembler::Lease active;
			std::uint64_t latchedMisses = 0;  // values a commit staged because the revision's latched copies lacked them
			// Per epoch since the last report: commits with no shape or recording, values past the latch (dropped), submissions.
			std::array<std::uint64_t, kEpochs> unrevised{}, latchClamped{}, submitted{};
			// Per epoch, whether the selected revision has its recordings of the graph as built now (DecideCoverage).
			std::array<bool, kEpochs> covered{};
			bool explicitBuilds = false;
			std::uint64_t builds = 0, withdrawn = 0, ticketsReleased = 0;
			std::array<std::uint64_t, kEpochs> uncovered{};
			// Per epoch, the inputs (versions, shape) whose recording failed: not asked again until they change.
			std::array<std::pair<std::shared_ptr<const org::async::RevisionFragment>, std::shared_ptr<const org::async::RevisionFragment>>, kEpochs> failedFor;
		} revisions;
		/** @brief The builder: seals the revision of MakeRevision's shapes; none while a growth or a needed build is pending. */
		void AssembleRevision(std::uint32_t a_frame);
		/** @brief Recording requests not yet completed: what the build point releases unsubmitted tickets for. */
		std::atomic<std::uint32_t> recordingsOutstanding{ 0 };
		/**
		 * @brief The selected revision's shape and recordings for a_epoch, trusted. Empty (counted, <- UNREVISED) only when a coverage
		 * decision let a frame through without them; the epoch is then not submitted.
		 */
		EpochRevision RevisionOf(std::uint32_t a_epoch);
		/**
		 * @brief Whether the selected revision covers a_epoch and its graph still runs. An uncovered epoch is the engine's, decided
		 * before the engine draws, and not submitted. True without revisions.
		 */
		bool EpochCovered(std::uint32_t a_epoch) const;
		void SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index);
		std::string RevisionReport();
		/** @brief What the revisions' shapes are made from, hashed by group; the shapes are made again only when it moves. */
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
		// Since the last report: makes that made the shapes, and that kept the last ones.
		std::uint64_t shapesMade = 0, shapesKept = 0;
		/**
		 * @brief Whether the last shadow and occlusion shapes hold a_bounds (capacities and buckets enough, no row at the bucket words).
		 * Capacities only grow, so a shape remade from a bound they hold is the same shape.
		 */
		bool ShadowShapesHold(const ShadowBounds& a_bounds) const;
		// The casting bound's version last checked (drawBound.shadowVersion) and its outgrowths (kKeyBound: a move alone keeps the shapes).
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
