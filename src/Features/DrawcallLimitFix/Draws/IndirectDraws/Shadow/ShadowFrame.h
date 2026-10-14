#pragma once

// The shadow and occlusion epochs' types: captured views (render thread hooks), shapes (revision producer), the index pool and
// the graph resources (render thread). Included by Internal.h.

namespace DCLF::Draws
{
	/** @brief A view the hook captured for the frame's epoch (CaptureShadowView, ExecuteShadowFrame). */
	struct PendingView
	{
		std::uint32_t viewId = 0, renderMode = 0, modeIndex = 0, targetIndex = 0, slice = 0;
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		std::uint32_t slot = 0;  // its placement's (Impl::shadowPlacements)
		float minDepth = 0.0f, maxDepth = 1.0f;
		RE::NiPoint3 eye;
		bool hasViewProj = false;
		std::array<float, 16> viewProj{};
		// The engine's caster volume (NiCullingProcess::customCullPlanes), when its culling process has one.
		float cullPlanes[6][4] = {};
		std::uint32_t cullPlaneMask = 0;
		// ShadowRasterStateId of the engine's caster state at cull mode 1, which the Utility shader sets for every pass but a
		// two-sided property's (engine notes); two-sided keys draw unculled (kRasterTwoSided).
		std::uint32_t rasterState = 0;
		// 0: ordinary casters (kObjectVolumetricOnly inputs skipped); 1: the volumetric lighting copy (volumetric-only casters).
		std::uint32_t casterClass = 0;
		bool sunView = false;  // the directional light's: its casters are its full-frustum entries' (kCullSunEntry)
		bool focus = false;    // a focus shadow's: its casters are focusTarget's members (BuildDrawsLatch::focusRoot)
		const void* focusTarget = nullptr;
		float viewBlock[12] = {};  // PerTechnique: HighDetailRange, ParabolaParam, EyeDelta
		std::array<std::byte, 1024> perFrame{};
		std::uint32_t perFrameBytes = 0;
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
	};

	/**
	 * @brief One plain indirect draw with one pipeline over a range of a view slot's sequences; one per pipeline in the view's
	 * map row, in row order.
	 */
	struct ShadowBucket
	{
		std::uint32_t pipeline = 0;  // shadow set index (ShadowIndirectState::pipelines)
		std::uint32_t first = 0;     // range start, in sequences
		std::uint32_t capacity = 0;  // max draw count: every draw its key slots can produce, grown by doubling

		bool operator==(const ShadowBucket&) const = default;
	};

	/** @brief One captured view of the frame's shadow epoch: where it draws its render mode's inputs, with which blocks. */
	struct ShadowFrameView
	{
		std::uint32_t slot = 0;       // the view's sequence and count buffers, record region and latch
		std::uint32_t modeIndex = 0;  // the inputs buffer (render mode 0xD, 0xE, 0xF)
		std::uint32_t capacity = 0;   // ExecuteIndirect max count (GrowCapacity, per slot)
		std::uint32_t x = 0, y = 0, width = 0, height = 0;  // viewport in its slice
		float minDepth = 0.0f, maxDepth = 1.0f;
		std::uint32_t target = 0;  // index into ShadowResources::depth
		std::uint32_t slice = 0;
		std::uint64_t materialRows = 0;  // the shadow material rows' address (shared by every view)
		std::uint32_t sequenceDraws = 0;  // its slot's sequence buffer, in draws
		std::array<std::uint32_t, kShadowPushWords> push{};
		// Depth-only class pipelines first (ShadowBuckets).
		std::vector<ShadowBucket> buckets;
		std::uint32_t rasterState = 0;  // whose pipeline map row the buckets follow (ShadowViewLayout)

		bool operator==(const ShadowFrameView&) const = default;
	};

	struct RowBuckets;
	/**
	 * @brief The frame's shadow epoch shape. All captured views draw in one graph execution at AfterShadowMaps (each execution has
	 * a fixed CPU cost). Per-view matrices live in the latch; the shape changes only with the view layout or the pipelines.
	 */
	struct ShadowFrame
	{
		std::uint64_t generation = 0;
		rhi::DescriptorHeapHandle resourceHeap{};
		rhi::DescriptorHeapHandle samplerHeap{};
		ShadowIndirectState indirect{};
		std::vector<ShadowFrameView> views;
		// The latch block (the one the revision's set names: a new one when the layout grows); pass preparation reads it from here.
		std::shared_ptr<const org::LatchBlock> latch;
		// Latched copies: view blocks, zeroed counts, and the commit's SharedData/FeatureData (kCommitList).
		std::array<LatchedList, 3> latched;
		static constexpr std::size_t kCommitList = 2;
		// Not compared (implied by the latch and the buckets): the latch layout and each view's map row a commit writes against.
		ShadowLatchLayout latchLayout;
		std::shared_ptr<const std::vector<RowBuckets>> rows;

		bool SameShape(const ShadowFrame& o) const
		{
			// The version covers the pipelines the buckets bind.
			return latch == o.latch && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
			       SameHandle(indirect.layout, o.indirect.layout) && indirect.version == o.indirect.version && views == o.views;
		}
	};

	/**
	 * @brief Every geometry slot's indices copied into one buffer, so plain indexed draws bind one index buffer and keep vertex
	 * reuse. Ranges are per slot, never shared: a buffer address is no identity (freed addresses are reused at once). The frame's
	 * first drawing commit reassigns logged slots' ranges (UpdateIndexPool), copied before its draws. Render thread. Its capacities
	 * (indices, geometry slots, copies) are the named versions' units (Impl::RequireIndexPool).
	 */
	struct IndexPool
	{
		static constexpr std::uint32_t kNoRange = ~0u;
		// A slot's range: first index and count (even: 4-byte aligned), and the source buffer.
		struct Range
		{
			std::uint32_t first = kNoRange, count = 0;
			std::uint64_t address = 0, bytes = 0;
		};
		Versioned indices;  // 16-bit
		Versioned firsts;   // per geometry slot: its range's first index, kNoRange without one
		Versioned copies;   // this commit's copies: source address (low, high), first index, words
		// The versions the last update filled: an indices version it did not lay out is laid out and every range copied again; a
		// firsts version it did not fill is sent every slot's first index.
		org::services::FilledVersion indicesFilled, firstsFilled;
		// Slots waiting for growth (no room, past the first-index table or this commit's copies); retried by every update.
		std::vector<std::uint32_t> waiting;
		// No room though the pool has twice what it holds: the serial of the indices version found fragmented, published by the
		// render thread; the producer requires a version newer than it (a relayout at the same size). A serial alone, so relaxed
		// order suffices.
		std::atomic<std::uint64_t> fragmentedAt{ 0 };
		// The revision producer's bound (RevisionProducer::poolBound): every slot's index count, kept from the log.
		struct Bound
		{
			LogCursor cursor;
			std::vector<std::uint32_t> indices;
			std::uint64_t total = 0;
		} bound;
		std::shared_ptr<const ComputeProgram> program;
		rhi::CommandSignaturePtr dispatchSignature;
		LogCursor cursor;
		std::vector<Range> slots;                                   // per geometry slot: its range
		std::vector<std::uint32_t> slotFirst;                       // per geometry slot: what firsts holds
		std::map<std::uint32_t, std::uint32_t> free;                // first -> count
		std::uint32_t end = 0;                                      // past the last range ever given out
		std::uint64_t indicesHeld = 0;                              // live indices, for the report
	};

	/** @brief What a shadow shape indexes by: the units the named versions of the shadow buffers hold (MakeRevision). */
	struct ShadowCapacity
	{
		// Every view slot's sequences in draws (the least slot's).
		std::uint32_t sequenceDraws = 0;
		// Bucket count words per view slot (the least slot's); a pipeline past them waits for the growth (TrimmedRow).
		std::uint32_t bucketCountWords = 0;
		// View slots the view-blocks buffer holds; a layout past them is not drawn.
		std::uint32_t viewSlots = 0;
	};

	/** @brief The shadow latch's first layout (CS_DCLF_TABLE_START=small: Skylighting's map, one view, a few key slots). */
	ShadowLatchLayout InitialShadowLatchLayout();

	/** @brief The shadow views' graph resources: the main path's set per view slot, without targets or an HZB. */
	struct ShadowResources
	{
		std::shared_ptr<org::Buffer> constants;
		Versioned visibility;
		// The material rows every view's draws name (ShadowMaterialRow).
		GrowableRows materialRows;
		std::shared_ptr<SceneBuffers> scene;
		// The views' own inputs, for dispatches that do not read the frame's ring entry.
		Versioned inputs;
		// Per view slot (occlusion views, then shadow views): sequence and count buffers, and the counters' readback view.
		std::vector<Versioned> sequences;
		std::vector<std::shared_ptr<org::Buffer>> count;
		// Per view slot, one count word per bucket.
		std::vector<Versioned> bucketCounts;
		std::shared_ptr<IndexPool> pool;
		std::vector<winrt::com_ptr<ID3D11Buffer>> countD3D11;
		// The view slots' blocks (kShadowViewSlotBytes a row: b0, b12), written by each epoch's commit.
		GrowableRows viewBlocks;
		std::uint64_t constantsAddress = 0;
		std::shared_ptr<const ComputeProgram> buildDraws;
		// The engine's shadow map arrays, imported once each (again when the engine recreates them), a DSV per slice.
		std::array<std::shared_ptr<org::ExternalTextureResource>, kShadowDepthTargets> depth;
		std::array<ID3D11Texture2D*, kShadowDepthTargets> depthTexture{};
		std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};
		// The latch blocks are the revision's set's (NamedVersions::Latch). Targets the latched-copies pass declared:
		std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;  // LatchedTarget::key
		rhi::CommandSignaturePtr dispatchSignature;
		// This frame's engine views, for the culling readback's report.
		struct ViewLabel
		{
			std::uint32_t viewId = 0, renderMode = 0, slot = 0;
		};
		std::vector<ViewLabel> labels;
	};
}
