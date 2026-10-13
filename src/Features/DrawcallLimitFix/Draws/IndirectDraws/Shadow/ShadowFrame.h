#pragma once

// The shadow and occlusion epochs: their captured views, shapes, index pool and graph resources.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

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
		// The engine's caster volume for the view (NiCullingProcess::customCullPlanes), when its culling
		// process has one; see BuildDrawsLatch::cullPlanes.
		float cullPlanes[6][4] = {};
		std::uint32_t cullPlaneMask = 0;
		// DrawPipelines::ShadowRasterStateId of the state the engine draws the view's casters with: its table entry at cull mode 1,
		// the one the Utility shader sets for every pass but a two-sided property's (engine notes, shadow maps), whose key draws
		// without culling (kRasterTwoSided) whatever the view's state.
		std::uint32_t rasterState = 0;
		// 0: ordinary casters (kObjectVolumetricOnly inputs skipped); 1: the volumetric lighting copy, which
		// draws the volumetric-only casters alone.
		std::uint32_t casterClass = 0;
		bool sunView = false;  // the directional light's: its casters are its full-frustum entries' (kCullSunEntry)
		bool focus = false;    // a focus shadow's (T3c): its casters are focusTarget's members (BuildDrawsLatch::focusRoot)
		const void* focusTarget = nullptr;
		float viewBlock[12] = {};  // PerTechnique: HighDetailRange, ParabolaParam, EyeDelta
		std::array<std::byte, 1024> perFrame{};
		std::uint32_t perFrameBytes = 0;
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
	};

	/**
	 * @brief One captured view of the frame's shadow epoch: where to draw its render mode's inputs and
	 * with which per-view blocks (its record region names them).
	 */
	/**
	 * @brief A shadow view's bucket: one plain indirect draw with one pipeline over a range of the view slot's sequences
	 * (ShadowViewPass). A view has a bucket per pipeline its rasterizer state's map row names, in the row's order, and the
	 * bucket's index is what the row holds for its key slots (BuildDrawsCS).
	 */
	struct ShadowBucket
	{
		std::uint32_t pipeline = 0;  // its shadow set index (ShadowIndirectState::pipelines)
		std::uint32_t first = 0;     // its range, in sequences
		std::uint32_t capacity = 0;  // its draw's max count: every draw its key slots' inputs can produce, grown by doubling

		bool operator==(const ShadowBucket&) const = default;
	};

	struct ShadowFrameView
	{
		std::uint32_t slot = 0;       // the view's sequence and count buffers, record region and latch
		std::uint32_t modeIndex = 0;  // the inputs buffer (render mode 0xD, 0xE, 0xF)
		std::uint32_t capacity = 0;   // its draw's ExecuteIndirect max count (GrowCapacity, per slot)
		std::uint32_t x = 0, y = 0, width = 0, height = 0;  // the view's viewport, in its slice
		float minDepth = 0.0f, maxDepth = 1.0f;
		std::uint32_t target = 0;  // index into ShadowResources::depth
		std::uint32_t slice = 0;
		std::uint64_t materialRows = 0;  // the shadow material rows' address (one table for every view)
		std::uint32_t sequenceDraws = 0;  // its slot's sequence buffer, in draws (ShadowResources::sequenceDraws)
		// The view's push data (DrawPipelines.h, kShadowPushWords): its blocks' addresses, pushed once for its draws.
		std::array<std::uint32_t, kShadowPushWords> push{};
		// Its draws, one per bucket with a capacity, the depth-only class's pipelines first (ShadowEpochs.cpp, ShadowBuckets).
		std::vector<ShadowBucket> buckets;
		std::uint32_t rasterState = 0;  // the state whose pipeline map row the buckets are (ShadowViewLayout)

		bool operator==(const ShadowFrameView&) const = default;
	};

	/**
	 * @brief The shape of the frame's shadow epoch: which views, where they draw and with what capacity. Every view
	 * the hooks captured is drawn by one graph execution at AfterShadowMaps, rather than one per view, because the
	 * graph's own execution costs about 0.9 ms of CPU per epoch whatever it draws, and the exterior has four views a
	 * frame. Each view's matrices and input count are in its BuildDrawsLatch; the shape changes only when the engine's
	 * view layout or the pipelines do, and its identity is the shadow passes' revision.
	 */
	struct RowBuckets;
	struct ShadowFrame
	{
		std::uint64_t generation = 0;
		rhi::DescriptorHeapHandle resourceHeap{};
		rhi::DescriptorHeapHandle samplerHeap{};
		ShadowIndirectState indirect{};
		std::vector<ShadowFrameView> views;
		// The latch block the views' values were written to (RevisionLatches::shadow, replaced when the slots grow): read by
		// the passes' preparation from here, on whichever thread prepares them.
		std::shared_ptr<const org::LatchBlock> latch;
		// The latched copies (LatchedCopiesPass): each view's blocks from the latch (ShadowLatchLayout::ViewBlockOffset) to its slot of
		// the view-blocks buffer, its draw count and bucket counts zeroed, and the commit's other per-frame values (CS's SharedData and
		// FeatureData blocks in the constants, LatchedUploads: kCommitList).
		std::array<LatchedList, 3> latched;
		static constexpr std::size_t kCommitList = 2;
		// Not compared (implied by the latch block and the views' buckets): what a commit writing its values into this shape writes
		// them against (R3c) - the latch's layout, and each view's map row (its key slots' buckets).
		ShadowLatchLayout latchLayout;
		std::shared_ptr<const std::vector<RowBuckets>> rows;

		bool SameShape(const ShadowFrame& o) const
		{
			// The version holds the pipelines the views' buckets bind (ShadowIndirectState::pipelines).
			return latch == o.latch && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
			       SameHandle(indirect.layout, o.indirect.layout) && indirect.version == o.indirect.version && views == o.views;
		}
	};

	/** @brief The shadow views' graph resources: the main path's set, without targets or an HZB, per view slot. */
	/**
	 * @brief The shadow views' index pool: the index buffers of the geometry slots, copied into one DCLF buffer, so that the
	 * views' and the Z-prepass's plain indexed draws bind one index buffer (ShadowViewPass, MainOpaquePass) and keep the
	 * hardware's vertex reuse. A range is a slot's own, never shared: a buffer's address is no identity (the device reuses a
	 * freed buffer's address at once). The first commit of a frame to draw from the pool gives a range out, and copies into
	 * it, for every slot the geometry log names, after taking back every named slot's old range (UpdateIndexPool); the
	 * copies run in that commit's epoch before its draws (IndexPoolCS). A pool that cannot fit a range doubles, and its
	 * ranges are laid out and copied again. Render thread.
	 */
	/**
	 * @brief What the index pool's buffers hold (Growths::Change at the join, Impl::ReserveIndexPool; adopted with the revision
	 * that names it): its indices, the geometry slots its first-index table holds, and the copies a commit may make. relayouts
	 * moves for a new indices version at the same size (a fragmented pool laid out again).
	 */
	struct PoolSizing
	{
		std::uint32_t capacity = 0;        // in indices
		std::uint32_t firstsCapacity = 0;  // geometry slots
		std::uint32_t copiesCapacity = 0;
		std::uint32_t relayouts = 0;
		bool operator==(const PoolSizing&) const = default;
	};

	struct IndexPool : PoolSizing
	{
		static constexpr std::uint32_t kNoRange = ~0u;
		// A geometry slot's range: its first index and count (even: 4-byte aligned), and the buffer it was copied from.
		struct Range
		{
			std::uint32_t first = kNoRange, count = 0;
			std::uint64_t address = 0, bytes = 0;
		};
		Versioned indices;  // 16-bit
		Versioned firsts;   // per geometry slot: its range's first index, kNoRange without one
		Versioned copies;   // this commit's copies: source address (low, high), first index, words
		// A new indices version adopted: every range laid out again from its start and copied (UpdateIndexPool, next).
		bool relayout = false;
		// Revision mode: the slots whose range waits for the pool's growth (no room, past the first-index table, or past this
		// commit's copies) - the unclaimed geometry the join asked room for, tried again by every update.
		std::vector<std::uint32_t> waiting;
		bool fragmented = false;  // a range found no room though the pool has twice what it holds: a relayout is asked for
		// The join's bound (ReserveIndexPool): every geometry slot's indices as the tables have them, kept from the log.
		struct Bound
		{
			LogCursor cursor;
			std::vector<std::uint32_t> indices;
			std::uint64_t total = 0;
		} bound;
		std::shared_ptr<const ComputeProgram> program;
		rhi::CommandSignaturePtr dispatchSignature;
		std::uint64_t layout = 0;  // bumped whenever a buffer gets a new backing
		LogCursor cursor;
		std::vector<Range> slots;                                   // per geometry slot: its range
		std::vector<std::uint32_t> slotFirst;                       // per geometry slot: its range's first index (what firsts holds)
		std::map<std::uint32_t, std::uint32_t> free;                // first -> count
		std::uint32_t end = 0;                                      // past the last range ever given out
		std::uint64_t indicesHeld = 0;                              // live indices, for the report
	};

	/** @brief What the shadow epochs' writers index by, changed with those buffers' growth (Growths::Change). */
	struct ShadowSizing
	{
		std::uint32_t objectCapacity = 0;  // what visibility and the inputs hold, as Resources::objectCapacity
		// Every view slot's sequences, in draws (of each caster class): every draw the scene can produce, grown with its bound
		// (Impl::ReserveShadowSequences). One size for all slots: a slot added while a growth is outstanding is made at the size
		// asked for (LatestSizing), so it holds what the sizing says once that is adopted, and more before.
		std::uint32_t sequenceDraws = 0;
		// Every view slot's bucket counts, a word a bucket (ShadowBucket): the latch's key slots, which bound a view's buckets,
		// grown with them. A shape's buckets stay within them (TrimmedRow): a pipeline past them waits for the growth.
		std::uint32_t bucketCountWords = 0;
		// The view slots whose view blocks the view-blocks buffer holds: what the epochs draw into (a frame's views past them stay
		// the engine's: IndirectDraws::ShadowViewCapacity). A slot's own buffers are made with the latch's slots, ahead of it.
		std::uint32_t viewSlots = 0;
		bool operator==(const ShadowSizing&) const = default;
	};

	/** @brief The shadow latch's first layout (CS_DCLF_TABLE_START=small: room for Skylighting's map and one view, and a few key slots). */
	ShadowLatchLayout InitialShadowLatchLayout();

	struct ShadowResources : ShadowSizing
	{
		std::shared_ptr<org::Buffer> constants;
		Versioned visibility;
		// The material rows every view's draws name (ShadowMaterialRow), grown with the kept state.
		GrowableRows materialRows;
		// The scene's tables, every epoch's (SceneBuffers).
		std::shared_ptr<SceneBuffers> scene;
		// The views' own inputs, which their dispatches bind: every commit's latch names the frame's ring entry (the scene list) instead.
		Versioned inputs;
		// Per view slot (the occlusion views', then the shadow views'), as many as the latch layout's viewSlots (Impl::ReserveShadowLatch):
		// its sequence and count buffers, its sequence buffer's draws - grown before the epoch that uses the slot to hold
		// every draw the scene can produce (Impl::ReserveShadowSequences) - and its counters' readback view. Render thread.
		std::vector<Versioned> sequences;
		std::vector<std::shared_ptr<org::Buffer>> count;
		// Per view slot, its buckets' counts (ShadowBucket), a word a bucket (ShadowSizing::bucketCountWords).
		std::vector<Versioned> bucketCounts;
		std::shared_ptr<IndexPool> pool;
		std::vector<winrt::com_ptr<ID3D11Buffer>> countD3D11;
		// The view slots' blocks (kShadowViewSlotBytes a row: b0, b12), written by each epoch's commit.
		GrowableRows viewBlocks;
		// The kept shadow state's versions (ShadowKept) each mode's input buffer and the material rows hold: 0 when a build
		// without the kept state wrote the buffer, or when the rows' backing is new.
		std::uint64_t inputsUploaded = 0;
		std::uint64_t materialRowsHeld = 0;
		std::uint64_t constantsAddress = 0;
		std::shared_ptr<const ComputeProgram> buildDraws;
		// The engine's shadow map arrays, imported once each (re-imported when the engine recreates
		// them), with a depth-stencil view per slice.
		std::array<std::shared_ptr<org::ExternalTextureResource>, kShadowDepthTargets> depth;
		std::array<ID3D11Texture2D*, kShadowDepthTargets> depthTexture{};
		std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};
		// The latch block (per frame slot, one BuildDrawsLatch per view slot and the pipeline map rows), the counters' zeros and the
		// commit's latched values' block are the revision producer's (RevisionLatches: shadow, shadowZeros, shadowLatched), held by
		// the shapes (ShadowFrame::latch, zeros, latched). The targets the latched-copies pass declared (the constants):
		std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;  // LatchedTarget::key
		rhi::CommandSignaturePtr dispatchSignature;
		// This frame's views as the engine named them (render thread), for the culling readback's report.
		struct ViewLabel
		{
			std::uint32_t viewId = 0, renderMode = 0, slot = 0;
		};
		std::vector<ViewLabel> labels;
	};
}
