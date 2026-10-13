#pragma once

// The main epochs' graph resources, their bindings and bucket plans, their shapes' inputs, the pass statistics.
// Part of IndirectDraws' implementation: included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief What the main epochs' writers and shapes index the sequences and the Z-prepass's bucket counts by (Impl::
	 * ReserveMainSequences): changed with those buffers' growth, as one (Growths::Change; a revision's shapes take the change it
	 * names, RevisionSizing).
	 */
	struct MainSizing
	{
		// The Z-prepass's plain draws (MainOpaquePass, DCLF_PULLED): per pipeline slot, its bucket's range of each phase's sequences
		// - every draw the slot's objects can produce, held while they fit and grown to a power of two past it, so the recorded
		// calls change only then (zBucketsLayout) - and each phase's count words, a word a slot, zeroed by the depth commit.
		std::vector<std::uint32_t> zBucketCapacity, zBucketFirst;
		std::uint64_t zBucketsLayout = 0;
		std::uint32_t zBucketCountWords = 0;
		// The sequence buffer's ranges (GpuLayouts.h, SequenceSlots): draws per draw range (phase 1 and colour, phase 2) and per
		// decal group, holding every draw the scene can produce.
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
		// T7c1: the colour segment's terrain groups' ranges, in phase 2's (GpuLayouts.h, kTerrainGroups): every draw each group's
		// pipeline slots can produce, grown to a power of two past it; the two together within sequenceDraws.
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
		// T7d: each half of the colour segment's forward range (GpuLayouts.h, kForwardSortShader), after the terrain ranges: every draw
		// the forward pipeline slots can produce, grown to a power of two past it; the terrain ranges and both halves within sequenceDraws.
		std::uint32_t forwardCapacity = 0;
		// T7d's refraction: the refraction range (GpuLayouts.h), after the forward halves: every draw the refraction pipeline slots can
		// produce, grown to a power of two past it; within sequenceDraws with the ranges before it.
		std::uint32_t refractionCapacity = 0;
		// The objects the per-object buffers hold (inputs, inputsDepth, visibility, frustum): the scene's object capacity, grown
		// with it (ReserveObjectBuffers).
		std::uint32_t objectCapacity = 0;
		bool operator==(const MainSizing&) const = default;
	};

	struct Resources : MainSizing
	{
		// The draws the newest change of the sequences asks for (ReserveMainSequences, the revision code's; this sizing's at first):
		// what a commit past the adopted ranges is checked against - draws waiting for that change, or a defect of the bound.
		std::atomic<std::uint32_t> askedSequenceDraws{ 0 };
		std::vector<FrameBuffer> frameBuffers;
		// The main pass's rows (DrawPipelines.h, kMaterialRowBytes / kPipelineRowBytes), one table each for both segments,
		// indexed by the scene's material and pipeline slots; grown before an epoch to the tables' slot counts
		// (Impl::ReserveMainSequences). The versions of the kept rows (MainRows) they hold: 0 in a new backing.
		GrowableRows materialRows, pipelineRows;
		std::uint64_t materialRowsHeld = 0, pipelineRowsHeld = 0;
		// The scene's tables, every epoch's (SceneBuffers): the object records, bone rows, geometry table and face positions.
		std::shared_ptr<SceneBuffers> scene;
		Versioned inputs, sequences;  // BuildDraws: in, out (and the scene's geometries)
		std::shared_ptr<org::Buffer> count;
		// The Z-prepass's bucket counts (MainSizing) and the index pool (the scene's).
		std::array<Versioned, 2> zBucketCounts;
		std::shared_ptr<IndexPool> pool;
		// The Z-prepass segment's draw inputs, for the commits that do not read the frame's ring entry (the scene list): a payload
		// built at the epoch (CS_DCLF_BINDLESS_PARITY), one per segment and each with its own drawable bits, so each segment keeps
		// its own buffer (the colour segment's is `inputs`), with the version of the region the buffer holds, per segment
		// (0 Z-prepass, 1 colour), written by the commit that uploads it.
		Versioned inputsDepth;
		std::array<std::uint64_t, 2> residentUploaded{};
		std::shared_ptr<const ComputeProgram> buildDraws;
		winrt::com_ptr<ID3D11Buffer> sequencesD3D11, countD3D11;  // CS_DCLF_BUILD_PARITY readback
		// The foliage record (GpuLayouts.h; Policy::RecordFoliagePixels): by frame parity, each pixel's object and colours, and the
		// observers' compare pass's results (Diagnostics/Draws/Foliage.cpp: its program and a host copy of the results per frame slot,
		// read when the slot comes round again).
		struct FoliageRecord
		{
			std::array<std::shared_ptr<org::Buffer>, 2> ids, colours;
			std::array<std::uint64_t, 2> idsAddress{}, coloursAddress{};
			std::shared_ptr<org::Buffer> results;
			std::uint64_t resultsAddress = 0;
			// The Z-prepass's alpha-tested fragments' owner per pixel (GpuLayouts.h, kFoliageOwned), its UAV's index.
			std::shared_ptr<org::Buffer> owners;
			std::uint32_t ownersIndex = 0;
			std::uint32_t epoch = 0;  // colour commits so far: the buffers' parity and tag (the store's frame can repeat or skip)
			std::shared_ptr<const ComputeProgram> program;
			std::vector<std::shared_ptr<org::Buffer>> readback;
			std::vector<std::uint32_t> readbackFrame;  // 1 where the slot's copy holds a frame's results
			std::uint32_t width = 0, height = 0;
		};
		std::shared_ptr<FoliageRecord> foliage;
		winrt::com_ptr<ID3D11Buffer> visibilityD3D11;              // CS_DCLF_SET_PARITY readback
		winrt::com_ptr<ID3D11Buffer> frustumD3D11;                 // CS_DCLF_SET_PARITY readback (the fade test's drops)
		// The per-frame constant blocks at fixed slots (FrameSlotOffset), so a build can name them before
		// their contents exist.
		std::shared_ptr<org::Buffer> frameConstants;
		// The targets the passes declared for the commits' latched copies (LatchedCopiesPass), which a commit's LatchedUploads
		// take; anything else is staged. The copies' blocks, per segment (kDepthShape, kColourShape), are the revision producer's
		// (RevisionLatches::mainLatched), held by the shapes (PassFrame::latched).
		std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;  // LatchedTarget::key
		std::uint64_t frameConstantsAddress = 0;
		// The zeroed StrictLightData block every bindless draw's b3 reads (kFrameSlotSharedLight): constant, so sent once, by the
		// first commit, as a staged upload outside the latched layout.
		bool sharedLightZeroed = false;
		// The main pass's own targets and depth, imported: DCLF draws into them, and the native loop skips the
		// objects it drew (DrawcallLimitFix's RenderPassImmediately hooks).
		std::uint32_t targetCount = 0;
		std::array<std::shared_ptr<org::ExternalTextureResource>, kColorTargets> native;
		std::shared_ptr<org::ExternalTextureResource> nativeDepth;
		// T7c1: Terrain Blending's terrain depth (TerrainBlending::terrainDepth), imported once with the main depth's description,
		// which the Z-prepass draws its near terrain blocks into (MainOpaquePass). Null without that feature.
		std::shared_ptr<org::ExternalTextureResource> terrainDepth;
		// T7d's refraction: the engine's refraction normals (kREFRACTION_NORMALS), imported once with their format, which the forward
		// epoch's RefractionDrawPass draws into. Null without the target.
		std::shared_ptr<org::ExternalTextureResource> refractionNormals;
		DXGI_FORMAT refractionFormat = DXGI_FORMAT_UNKNOWN;
		// CS_DCLF_GBUFFER_PROBE: one texel of every target, copied inside the epoch both before and
		// after the colour draws. The copies are graph passes so the graph orders them against the
		// draws; a D3D11 readback issued around the epoch is not ordered against ORG's submissions at
		// all, and silently reports that nothing changed.
		std::shared_ptr<org::Buffer> probe;
		winrt::com_ptr<ID3D11Buffer> probeD3D11;
		// Phase 4's hierarchical depth buffer. Its source is nativeDepth, which is imported with a
		// shader-resource view as well as a depth-stencil one so that the graph sees the depth the draws
		// write and the depth the build reads as one resource and orders them.
		Versioned visibility;  // per-object verdict, published by the depth segment
		// Per object, the stamp of the last frame the main camera's depth phase 1 found its bound inside the frustum
		// (occlusion aside: the engine's OnVisible semantics). Never cleared: a stale stamp is simply not this frame's.
		Versioned frustum;
		std::shared_ptr<PassStats> passStats;  // CS_DCLF_PASS_STATS
		// The explicit DGC preprocesses' state list of the colour segment's passes. The depth segment's draws are plain
		// (PassFrame::zCalls).
		std::shared_ptr<PreprocessStates> preprocessMain;
		// T7d: the forward epoch's (ForwardDrawPass), which records in an epoch of its own; and the forward range's sort.
		std::shared_ptr<PreprocessStates> preprocessForward;
		std::shared_ptr<const ComputeProgram> forwardSort;
		std::shared_ptr<org::PixelBuffer> hzb;
		std::shared_ptr<const ComputeProgram> hzbProgram;
		// The single-pass downsample's count of finished groups: zeroed by the first depth commit (hzbCounterZeroed,
		// render thread), and after that by the last group of each dispatch.
		std::shared_ptr<org::Buffer> hzbCounter;
		bool hzbCounterZeroed = false;
		std::uint32_t hzbWidth = 0, hzbHeight = 0, hzbMips = 0;
		std::uint32_t width = 0, height = 0;
		bool lightLimitFix = false;  // LLF's graph buffers are registered (they are read at t35-t37)
		// The latch block (per frame slot, one BuildDrawsLatch, then the colour pass's cascades) is the revision producer's
		// (RevisionLatches::main, grown by ReserveMainLatch), held by the shapes (PassFrame::latch, latchLayout).
		rhi::CommandSignaturePtr dispatchSignature;
		// The sort by pipeline of phase 1's and the colour segment's sequences (one view); null when it is off.
		std::shared_ptr<DrawSort> sort;
		// Per drawing segment, its last commit (render thread): the scene frame, its inputs, and the backings it wrote into (the
		// scene tables' growths, the per-object buffers' capacity). The reflection's faces draw from the depth segment's inputs
		// and the colour segment's frame record as the frame before left them, and only while nothing has grown since.
		struct Committed
		{
			std::uint32_t frame = ~0u, inputs = 0;
			std::uint64_t sceneGeneration = 0;
			std::uint32_t objectCapacity = 0;
			// The main rows' backings its inputs name by address (MainRowsGeneration): a growth adopted since leaves them on a
			// version nothing holds.
			std::uint64_t rowsGeneration = 0;
		};
		/** @brief The main rows' backings (material and pipeline rows' generations): what draw inputs embed the addresses of. */
		std::uint64_t MainRowsGeneration() const { return (materialRows.generation << 32) ^ pipelineRows.generation; }
		std::array<Committed, 2> committed;
	};

	struct PassBindings
	{
		std::array<org::DeclaredViewToken, kColorTargets> targets{};
		org::DeclaredViewToken depth;
		org::DeclaredViewToken terrainDepth;  // the Z-prepass's, with Terrain Blending (Resources::terrainDepth)
		org::ResourceBindingToken sequences, count;
		org::ResourceBindingToken pool, bucketCounts;  // the depth segment's plain draws
		org::ResourceBindingToken treeLodVisible;      // tree LOD's draw's arguments (UploadTreeLod, TreeLodCullPass)
	};

	/*
	 * The main epochs' shape producers (Revision/Producer.cpp; dclf-async-publication.md, R3c). A segment's shape (PassFrame) is a function
	 * of what a scene revision names - its resource versions, the scene's draw bound, the pipeline set and the lookups - and of
	 * what changes only with the engine's setup (the main pass's viewport, the frame blocks' sizes); never of a frame's values,
	 * which the commit writes into the latch and the latched copies' layout the shape names. A revision's shape is made from
	 * these at the scene work's join (MakeRevision).
	 */

	/**
	 * @brief The Z-prepass's plain draws (MainOpaquePass): a bucket per group of pipeline slots that share a depth pipeline
	 * (IndirectState::zGroups), in the order the slots first name them, its range the slots' ranges together
	 * (Resources::zBucketCapacity, which the sequences' phase ranges hold); each slot's bucket (kNoBucket: a slot without a
	 * published pipeline, whose draws BuildDraws drops); each phase's table of (first, capacity) per bucket; a draw call per
	 * bucket. The calls are the shape's; the map and the tables the commit's latch.
	 */
	struct ZBucketPlan
	{
		std::vector<std::uint32_t> map;    // per pipeline slot
		std::vector<std::uint32_t> table;  // phase 1's (first, capacity) per bucket, then phase 2's
		std::vector<PassFrame::ZCall> calls;
		std::uint32_t Buckets() const { return static_cast<std::uint32_t>(calls.size()); }
	};
	void PlanZBuckets(const MainSizing& a_resources, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out);

	/** @brief The sizes of a main epoch's frame blocks (FrameBlocks), per stage and register: what its latched copies of them take. */
	struct FrameBlockSizes
	{
		std::array<std::uint32_t, kConstantBufferRegisters> vs{}, ps{};
		bool operator==(const FrameBlockSizes&) const = default;
	};

	/**
	 * @brief A main commit's latched copies (LatchedUploads), in the order it writes them and packed as LatchedUploads packs
	 * them: a function of its resources, its segment, its frame blocks' sizes and the Z-prepass's bucket count.
	 */
	std::vector<LatchedCopy> MainLatchedLayout(const Resources& a_resources, bool a_depthOnly, const FrameBlockSizes& a_blocks, std::uint32_t a_buckets);
	/** @brief The bytes a layout takes of its latch block's slot. */
	std::size_t LatchedBytes(const std::vector<LatchedCopy>& a_copies);
	/** @brief A latched copies' block holding at least a_bytes a slot: a new, larger one when it does not (LatchedUploads' rule). */
	void ReserveLatchedBlock(std::shared_ptr<org::LatchBlock>& a_block, std::size_t a_bytes, std::uint32_t a_slots);

	/** @brief The main pass's render area and depth range, as a segment's last capture had them. */
	struct MainViewport
	{
		std::uint32_t width = 0, height = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
		bool operator==(const MainViewport&) const = default;
	};

	/** @brief What a main segment's shape is made from (MakeMainShape). */
	struct MainShapeInputs
	{
		bool depthOnly = false;
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // the sequence buffer's ranges (ReserveMainSequences)
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};  // T7c1: the terrain groups' (MainSizing::terrainCapacity)
		std::uint32_t forwardCapacity = 0;                    // T7d: each forward half's (MainSizing::forwardCapacity)
		std::uint32_t refractionCapacity = 0;                 // T7d's refraction range's (MainSizing::refractionCapacity)
		std::uint64_t materialRows = 0, pipelineRows = 0;     // the rows' tables' addresses
		MainViewport viewport;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};
		std::uint32_t cullMode = 0;
		std::string_view probe;                        // CS_DCLF_GBUFFER_PROBE's value (a revision's: its inputs')
		std::shared_ptr<const org::LatchBlock> latch;  // the main latch (the revision producer's: ReserveMainLatch)
		MainLatchLayout latchLayout;                   // its layout
		std::vector<PassFrame::ZCall> zCalls;          // the depth segment's (PlanZBuckets)
		std::shared_ptr<const ZBucketPlan> zPlan;      // the plan they are the calls of
		LatchedList latched;                           // MainLatchedLayout, in its reserved block
	};
	/**
	 * @brief The segment's resources' part of its shape's inputs: the ranges, the rows, the cull mode (a_cullMode: the toggles', as
	 * the caller has them). Not the latch (the revision producer's, which the caller names) nor the descriptor heaps (the graph's).
	 */
	MainShapeInputs MainShapeInputsOf(const Resources& a_resources, bool a_depthOnly, std::uint32_t a_cullMode, bool a_revision = false);
	std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in);


	/**
	 * @brief CS_DCLF_PASS_STATS=1: pipeline statistics (input vertices and primitives, vertex and pixel shader
	 * invocations) of the main epochs' draw passes, per frame slot, read back when the slot comes round again: the counter
	 * check that DCLF's passes draw and depth-test what they should (the colour pass's pixel shader invocations, above all).
	 */
	struct PassStats
	{
		enum Kind : std::uint32_t
		{
			kDepth,
			kDepthPhaseTwo,
			kColour,
			kKinds
		};
		static constexpr std::uint32_t kFields = 4;  // IA vertices, IA primitives, VS invocations, PS invocations
		rhi::QueryPoolPtr pool;
		std::vector<std::shared_ptr<org::Buffer>> readback;  // per frame slot, kKinds results
		std::vector<std::array<bool, kKinds>> written;
		std::mutex mutex;
		std::array<std::array<std::uint64_t, kFields>, kKinds> totals{};
		std::array<std::uint32_t, kKinds> samples{};

		static bool Enabled()
		{
			return Diagnostics::Observe().passStats;
		}

		/** @brief The slot's results from its last use, then its query reset; before the pass begins. */
		void Start(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
		{
			if (a_slot >= readback.size())
				return;
			const std::lock_guard lock(mutex);
			if (written[a_slot][a_kind]) {
				auto resource = readback[a_slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					const auto* values = static_cast<const std::uint64_t*>(mapped) + std::size_t(a_kind) * kFields;
					for (std::uint32_t f = 0; f < kFields; ++f)
						totals[a_kind][f] += values[f];
					++samples[a_kind];
					resource.Unmap(0, 0);
				}
			}
			if (a_kind == kColour && samples[kColour] >= Reports::kInterval) {
				Reports::PassStatsLine(totals, samples);
				totals = {};
				samples = {};
			}
			a_commands.ResetQueries(pool->GetHandle(), a_slot * kKinds + a_kind, 1);
		}
		void Begin(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
		{
			if (a_slot < readback.size())
				a_commands.BeginQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
		}
		void End(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
		{
			if (a_slot < readback.size())
				a_commands.EndQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
		}
		/** @brief After the pass ends. */
		void Resolve(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
		{
			if (a_slot >= readback.size())
				return;
			a_commands.ResolveQueryData(pool->GetHandle(), a_slot * kKinds + a_kind, 1, readback[a_slot]->GetAPIResource().GetHandle(),
				std::uint64_t(a_kind) * kFields * sizeof(std::uint64_t));
			const std::lock_guard lock(mutex);
			written[a_slot][a_kind] = true;
		}
	};

	// Frame push: the pass-wide constant buffers' addresses, as the layout's push address ranges read them.
	inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants);

	struct PreparedDraws
	{
		PassStats* stats = nullptr;  // CS_DCLF_PASS_STATS
		PreprocessStates* preprocess = nullptr;  // the frame slot's explicit DGC preprocess state list
		std::array<std::uint32_t, kFramePushWords> framePushWords{};
		std::shared_ptr<const PassFrame> frame;
		std::array<org::PreparedDescriptorReference, kColorTargets> targetViews{};
		org::PreparedDescriptorReference depthView{};
		org::PreparedDescriptorReference terrainDepthView{};  // T7c1: Terrain Blending's terrain depth (the Z-prepass's)
		bool terrainDepth = false;
		std::uint32_t targetCount = 0;
		bool phaseTwo = false;
		bool zPrepass = false;
		// Tree LOD's draw (UploadTreeLod): its pipelines and the draw row's address, which its push data names.
		std::shared_ptr<const TreeLodPipelines> treeLod;
		std::uint64_t treeLodDraw = 0;
	};
}
