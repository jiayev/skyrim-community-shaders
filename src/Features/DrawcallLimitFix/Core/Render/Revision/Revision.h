#pragma once

// What a scene revision is made from (RevisionInputs, RevisionLatchLayouts) and what it holds (versions, shapes, recordings).
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
	 * @brief What a scene revision reads of the frame, as the render thread last posted it (Impl::PostRevisionInputs, at each place a
	 * value is captured: BuildPoint, Setup, BeforeShadowMaps, the occlusion views, the shadow placements, the main epochs' captures,
	 * the views' work (IndirectDraws::PostViewInputs); and, while the revision is made on the render thread, before each make). Immutable once posted, latest wins
	 * (Impl::revisionInputsSlot); the revision code reads its copy and its request alone, never the frame's state. generation moves with
	 * every post; the generations it carries (the graph's build, the targets, the shadow format, the main resources) are what a
	 * snapshot's adoption compares. The toggles are none of it: a revision follows its publication's (RevisionProducer::toggles), which
	 * are the frame's once it is installed.
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
		// The pipeline generations a catalog's sets must be built for (GetIndirectState, GetShadowIndirectState, CatalogFamilyPipelines).
		std::uint32_t targetsGeneration = 0;
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		bool claims = false;       // !IndirectDraws::Failed
		std::uint32_t shadowRasterStates = 0;  // DrawPipelines::ShadowRasterStateCount
		// BeforeShadowMaps: the shadow views' candidates (ShadowViews::Candidates, the last Rebuild's). The occlusion maps' layout
		// as last captured (ShadowEngine::PredictedOcclusion: every captured map's, each revision taking its publication's maps), and
		// the shadow views' placements (BuildShadowPlacements).
		std::uint32_t shadowCandidates = 0;
		std::vector<ShadowViewLayout> occlusionLayouts;
		std::vector<ShadowViewLayout> shadowPlacements;
		// The placements the last shadow epoch drew (ShadowEngine::lastActiveViews).
		std::uint64_t shadowActiveViews = 0;
		// The main epochs' captures (Impl::MainEpochInputs: per segment, whether one has run, its viewport and its frame blocks' sizes).
		std::array<bool, 2> mainKnown{};
		std::array<MainViewport, 2> viewport{};
		std::array<FrameBlockSizes, 2> blockSizes{};
		// The views' (ViewKind): their resources, the targets their pipelines are resolved for from the request's catalog, a face's
		// size (the reflection cube's import, the main colour viewport) and the depth range (first person's).
		std::array<ViewInputs, kViews> views;
		// What the scene tables' requirements size by besides the tables (RequireSceneTables): the features' scene resources' words
		// (Api::SceneResources::postInputs, each at its registration's words: Features/TreeLod's mirror's shape and mesh slots, Features/
		// Portals' programs' words a frame's encoding takes).
		Extensions::SceneResourceInputs resourceInputs{};
		// The shadow view slots' buffers (made by the frame: ServeRevisionRequests, SetupShadow).
		std::shared_ptr<const ShadowSlotBuffers> shadowSlots;
		// The host's owner-thread state the revision code needs: whether async (revision-driven) epochs run (AssembleRevision records
		// for them), and its uploader (a growth's fills: SceneBufferSet::SetUploads; weak, so no post outlives a graph).
		bool asyncEpochs = false;
		std::weak_ptr<org::runtime::IUploadService> uploads;
	};

	/** @brief A latch layout's key for the scene's set (GrowthBuild::Require of a latch): another layout, another block. */
	template <class Layout>
	std::uint64_t LatchLayoutKey(const Layout& a_layout)
	{
		static_assert(std::is_trivially_copyable_v<Layout> && sizeof(Layout) % sizeof(std::uint32_t) == 0);
		std::array<std::uint32_t, sizeof(Layout) / sizeof(std::uint32_t)> words;
		std::memcpy(words.data(), &a_layout, sizeof(Layout));
		std::uint64_t key = 0xcbf29ce484222325ull;
		for (const std::uint32_t word : words)
			key = (key ^ word) * 0x100000001b3ull;
		return key;
	}

	/**
	 * @brief The revision producer's latch blocks, as the scene's set has them (SceneBufferSet::Register of a latch, once, at the
	 * first make): the blocks are the sealed set's (NamedVersions::Latch), required at every make (RequireLatches) and replaced by
	 * the seal when a layout changes, so a frame in flight keeps the block the set it adopted names.
	 */
	struct RevisionLatchHandles
	{
		// The main epochs': the latch (a BuildDrawsLatch, the bucket map and tables, the cascades and shadow volumes), and per segment
		// (kDepthShape, kColourShape) the latched copies' block.
		LatchHandle main;
		std::array<LatchHandle, 2> mainLatched;
		// The shadow and occlusion epochs': the latch, the counters' zeros and the shadow commit's latched values' block.
		LatchHandle shadow, shadowZeros, shadowLatched;
		// The views' (ViewKind): the latch and the zeros its latched copies take.
		std::array<LatchHandle, kViews> views, viewZeros;
		[[nodiscard]] explicit operator bool() const { return static_cast<bool>(main); }
	};

	/**
	 * @brief The revision producer's latch layouts: laid out at the first make that has their resources (EnsureRevisionLatches), grown
	 * by the reserves (ReserveMainLatch, ReserveShadowLatch, RequireView), and required of the scene's set as they stand
	 * (RequireLatches). The blocks themselves are the sealed set's alone.
	 */
	struct RevisionLatchLayouts
	{
		MainLatchLayout main;
		bool mainLaidOut = false;
		// Made with the shadow resources' first slots (SetupShadow).
		ShadowLatchLayout shadow;
		bool shadowLaidOut = false;
		std::array<ViewLatchLayout, kViews> views;
		std::array<bool, kViews> viewsLaidOut{};
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

	/** @brief A scene revision's recording of one epoch prepares for this (host data): the revision's versions and its epoch's shape. */
	struct EpochRevisionData final : org::IHostExecutionData
	{
		std::shared_ptr<const NamedVersions> versions;
		RevisionShapes shapes;
		const void* TryGet(std::type_index a_type) const noexcept override
		{
			if (a_type == typeid(RevisionShapes))
				return &shapes;
			if (a_type == typeid(NamedVersions))
				return versions.get();
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
