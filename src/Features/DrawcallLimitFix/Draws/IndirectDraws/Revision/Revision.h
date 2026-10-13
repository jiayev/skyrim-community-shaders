#pragma once

// What a scene revision is made from (RevisionInputs, RevisionLatches) and what it holds (versions, shapes, recordings).
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief The shadow view slots' buffers the frame made (AddShadowViewSlots), as it last posted them (RevisionInputs::shadowSlots):
	 * what the revision code grows and counts the slots by, never the frame's lists.
	 */
	struct ShadowSlotBuffers
	{
		std::vector<Versioned> sequences, bucketCounts;
	};
	/**
	 * @brief The revision code's request for shadow view slots (ReserveShadowLatch): the frame makes their buffers at these sizes and
	 * the graph's build that declares them (Impl::ServeRevisionRequests), latest wins (each holds every slot asked for).
	 */
	struct ShadowSlotsRequest
	{
		std::uint32_t slots = 0, sequenceDraws = 0, bucketCountWords = 0;
	};

	/**
	 * @brief T6b3b b2a: what a scene revision reads of the frame, as the render thread last posted it (Impl::PostRevisionInputs, at
	 * each place a value is captured: BuildPoint, Setup, BeforeShadowMaps, the occlusion views, the shadow placements, the main
	 * epochs' captures, the parity's first commits, PrepareReflection; and, while the revision is made on the render thread, before
	 * each make). Immutable once posted, latest wins (Impl::revisionInputsSlot); the revision code reads its copy and its request
	 * alone, never the frame's state. generation moves with every post; the generations it carries (the graph's build, the targets,
	 * the shadow format, the toggles, the main resources) are what a snapshot's adoption compares (b3).
	 */
	struct RevisionInputs
	{
		std::uint64_t generation = 0;
		// The graph resources (Setup replaces the main ones mid-frame; the shadow views' and the reflection's are made once), and the
		// main ones' generation (moves when Setup makes new ones).
		std::shared_ptr<Resources> main;
		std::uint64_t mainGeneration = 0;
		std::shared_ptr<ShadowResources> shadow;
		std::shared_ptr<SceneBuffers> scene;
		// BuildPoint: the graph's build (PersistentGraphHost::BuildGeneration) and the heaps every recording binds (the graph's own).
		std::uint64_t buildGeneration = 0;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		// The pipeline generations a catalog's sets must be built for (GetIndirectState, GetShadowIndirectState, TreeLodPipelinesOf).
		std::uint32_t targetsGeneration = 0;
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		// The toggles (the cull mode, the reflections and tree LOD, the occlusion maps), their generation, and the switches read.
		ToggleSet toggles{};
		std::uint32_t togglesGeneration = 0;
		std::string gbufferProbe;  // CS_DCLF_GBUFFER_PROBE
		bool claims = false;       // !IndirectDraws::Failed
		std::uint32_t shadowRasterStates = 0;  // DrawPipelines::ShadowRasterStateCount
		// BeforeShadowMaps: the shadow views' candidates (ShadowViews::Candidates, the last Rebuild's). The occlusion maps' layout
		// as last captured (Impl::PredictedOcclusion: the enabled maps'), and the shadow views' placements (BuildShadowPlacements).
		std::uint32_t shadowCandidates = 0;
		std::vector<ShadowViewLayout> occlusionLayouts;
		std::vector<ShadowViewLayout> shadowPlacements;
		// The main epochs' captures (Impl::MainEpochInputs: per segment, whether one has run, its viewport and its frame blocks' sizes).
		std::array<bool, 2> mainKnown{};
		std::array<MainViewport, 2> viewport{};
		std::array<FrameBlockSizes, 2> blockSizes{};
		// The views' (ViewKind): their resources, the targets their pipelines are resolved for from the request's catalog, a face's
		// size (the reflection cube's import, the main colour viewport) and the depth range (first person's).
		std::array<ViewInputs, kViews> views;
		// What the scene tables' reserve sizes by besides the tables (ReserveSceneTables): the portal programs' words a frame's
		// encoding takes (PortalViews::Encoded), and tree LOD's mirror's shape and mesh slots. The shadow material rows the last
		// shadow build wanted (ReserveShadowRows).
		std::uint32_t portalWords = 0, treeLodShapeSlots = 0, treeLodMeshSlots = 0;
		std::uint32_t shadowRowsWanted = 0;
		// The shadow view slots' buffers (made by the frame: ServeRevisionRequests, SetupShadow).
		std::shared_ptr<const ShadowSlotBuffers> shadowSlots;
		// The host's owner-thread state the revision code needs: whether async (revision-driven) epochs run (AssembleRevision records
		// for them), and its uploader (a growth's fills: Growths::uploads; weak, so no post outlives a graph).
		bool asyncEpochs = false;
		std::weak_ptr<org::runtime::IUploadService> uploads;
	};

	/**
	 * @brief The revision producer's latches (T6b3b b2a: made by the revision code alone, Create'd off the host's world so its thread
	 * may make them; ReserveMainLatch, ReserveShadowLatch, ReserveReflection, ReserveLatchedBlock), held by the shapes it makes: a
	 * commit writes into the selected revision's shape's latch, and its parity names that shape's. A frame in flight keeps the block
	 * its shape holds; one replaced here drops with the last shape holding it.
	 */
	struct RevisionLatches
	{
		// The main epochs': the latch (a BuildDrawsLatch, the bucket map and tables, the cascades and shadow volumes), and per segment
		// (kDepthShape, kColourShape) the latched copies' block.
		std::shared_ptr<org::LatchBlock> main;
		MainLatchLayout mainLayout;
		std::array<std::shared_ptr<org::LatchBlock>, 2> mainLatched;
		// The shadow and occlusion epochs': the latch, its layout (made with the shadow resources' first slots: SetupShadow), the
		// counters' zeros and the shadow commit's latched values' block.
		std::shared_ptr<org::LatchBlock> shadow, shadowZeros, shadowLatched;
		ShadowLatchLayout shadowLayout;
		// The views' (ViewKind).
		std::array<ViewLatches, kViews> views;
	};

	/** @brief The shapes the revision producer made last (MakeRevision): what AssembleRevision assembles from. */
	struct MadeShapes
	{
		std::array<std::shared_ptr<const PassFrame>, 2> main;  // per segment
		std::array<std::vector<std::shared_ptr<const ShadowFrame>>, 2> shadow;  // per epoch (shadow, occlusion), a shape per layout
		std::array<std::shared_ptr<const ViewFrame>, kViews> views;
	};

	/**
	 * @brief Per view rasterizer state, its map row's buckets (BucketsOfRow), kept while what they are made from holds: the lookups
	 * (a map row changes only with their shadow generation) and the shadow pipeline set (its count and classes): the revision
	 * producer's.
	 */
	struct RowBucketsCache
	{
		std::uint64_t lookups = 0;
		std::uint32_t generation = 0;
		std::shared_ptr<const void> version;
		std::array<std::vector<std::optional<RowBuckets>>, kShadowModeCount> rows;  // by mode, then state
	};

	/**
	 * @brief The versions of every versioned buffer (VersionRegistry) as a scene revision names them (R3c): a preparation for the
	 * revision resolves each buffer to its version here (org::IResourceVersions), whatever is current when it runs.
	 */
	struct VersionSet final : org::IResourceVersions
	{
		std::vector<std::pair<const void*, std::shared_ptr<const org::BufferVersion>>> versions;  // by VersionedBuffer::Key
		std::vector<std::weak_ptr<org::VersionedBuffer>> buffers;  // by versions' order: what Current compares (the frame's)
		std::uint64_t changes = 0;  // VersionRegistry::changes when it was taken: the versions are current while that holds
		std::shared_ptr<const org::BufferVersion> Find(const org::VersionedBuffer& a_buffer) const noexcept override;
		/**
		 * @brief The revision code: the registry's buffers' current versions, but a ready growth's where it has one (Growths::Ready):
		 * such a set is not current until its selection adopts them, so it takes a value of its own (VersionRegistry::Next).
		 */
		static std::shared_ptr<const VersionSet> Snapshot();
		/** @brief The frame: whether these are the set's buffers' current versions, every one. */
		bool Current() const;
	};

	/** @brief A scene revision's recording of one epoch prepares for this (host data): the revision's versions and its epoch's shape. */
	struct EpochRevisionData final : org::IHostExecutionData
	{
		std::shared_ptr<const VersionSet> versions;
		RevisionShapes shapes;
		const void* TryGet(std::type_index a_type) const noexcept override
		{
			if (a_type == typeid(RevisionShapes))
				return &shapes;
			if (a_type == typeid(org::IResourceVersions))
				return static_cast<const org::IResourceVersions*>(versions.get());
			return nullptr;
		}
	};

	/** @brief A shadow or occlusion epoch's shapes in a revision: one per view layout it has seen (ShadowViewLayout). */
	struct ShadowVariants
	{
		std::vector<std::shared_ptr<const ShadowFrame>> shapes;
	};
	/** @brief An epoch's recordings for a revision: one per shape (a shadow epoch's variants), by the shapes' order. */
	struct RevisionRecordings
	{
		std::vector<std::shared_ptr<const org::PersistentGraphHost::EpochRecording>> recordings;
	};
	/**
	 * @brief An epoch's part of the selected revision (Impl::RevisionOf): its shape fragment and its recordings. A commit writes the
	 * frame's values into the shape and submits the recording, trusting both (the frame's coverage was decided before the engine
	 * drew: DecideCoverage, DecideShadowCoverage, OcclusionReady).
	 */
	struct EpochRevision
	{
		std::shared_ptr<const org::async::RevisionFragment> shape;
		std::shared_ptr<const RevisionRecordings> recordings;
		explicit operator bool() const { return shape && recordings && !recordings->recordings.empty(); }
	};
}
