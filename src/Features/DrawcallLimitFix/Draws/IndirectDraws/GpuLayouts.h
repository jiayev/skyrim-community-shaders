#pragma once

// IndirectDraws' capacities and the layouts the GPU reads, which must match the HLSL (BuildDrawsCS, HzbCS,
// Lighting.hlsl's DCLF paths). Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
	constexpr const char* kExtensionId = "cs.dclf.main-opaque";
	constexpr const char* kShadowExtensionId = "cs.dclf.shadow";
	constexpr const char* kReflectionExtensionId = "cs.dclf.reflection";
	// The shadow views' material rows (ShadowMaterialRow): row 0 for every draw without alpha testing, and one per material
	// of the alpha-tested casters (their texture offset and diffuse). One table for every view, grown when the kept state
	// needs more rows (GrowableRows), from this many.
	constexpr std::uint32_t kShadowMaterialRowsInitial = 256;
	// Render modes 0xD plain, 0xE clamped, 0xF paraboloid, then one per occlusion view (Records.h, kOcclusionViews:
	// Skylighting's occlusion map, the precipitation mask; render mode 0x1C both), whose occluders the frame's shadow build
	// lists too and whose own epoch draws them (ExecuteOcclusion).
	constexpr std::uint32_t kFirstOcclusionMode = 3;
	constexpr std::uint32_t kShadowModeCount = kFirstOcclusionMode + kOcclusionViews;
	constexpr std::uint32_t kOcclusionRenderMode = 0x1C;
	inline constexpr bool IsOcclusionMode(std::uint32_t a_mode) { return a_mode >= kFirstOcclusionMode; }
	inline constexpr std::uint32_t OcclusionOfMode(std::uint32_t a_mode) { return a_mode - kFirstOcclusionMode; }
	inline constexpr std::uint32_t OcclusionModeOf(std::uint32_t a_view) { return kFirstOcclusionMode + a_view; }
	// The shadow epoch's view slots (ShadowResources): the occlusion views draw through the first slots (OcclusionSlot), the
	// frame's shadow views through the slots after them (kFirstShadowViewSlot plus the view's index). There are as many as the frame has views,
	// grown before its epoch (Impl::ReserveShadowLatch) - the exterior has four (two cascades, twice), an interior two
	// hemispheres per shadow-casting point light - from this many. Likewise the key slots the pipeline map rows hold
	// (Lookups::shadowSlotKeys: one per caster key and render mode).
	constexpr std::uint32_t kInitialShadowViewSlots = 8;
	inline constexpr std::uint32_t OcclusionSlot(std::uint32_t a_view) { return a_view; }
	constexpr std::uint32_t kFirstShadowViewSlot = kOcclusionViews;
	constexpr std::uint32_t kInitialShadowKeySlots = 1024;
	// And the latch's pipeline map rows (one per view rasterizer state, DrawPipelines::ShadowRasterStateId) and the sun's
	// full-frustum processes it holds (six and six at the exterior save), grown likewise.
	constexpr std::uint32_t kInitialShadowRasterStates = 8;
	constexpr std::uint32_t kInitialSunProcesses = 8;
	// The main latch block's cascades (MainLatchLayout), grown by the colour epoch that needs more (two at the exterior save).
	constexpr std::uint32_t kInitialSunCascades = 4;
	// And its local shadow light volumes (GpuShadowVolume): three lights at most accumulate a frame, a point light's descriptor each.
	constexpr std::uint32_t kInitialShadowVolumes = 8;
	constexpr std::uint64_t kShadowConstantBytes = 1ull << 20;
	// A view slot's blocks, a row of their own table (ShadowResources::viewBlocks): the view's Utility PerTechnique block (b0),
	// then its VS_PerFrame copy (b12), written by its epoch's commit.
	constexpr std::uint64_t kShadowPerFrameOffset = 256;
	constexpr std::uint64_t kShadowViewSlotBytes = 256 + 1024;
	// The shadow constants: the frame record (the shadow draws' textures and samplers, DrawBindings), which the views' push
	// data names, then the arena's blocks (the zero block, SharedData, FeatureData).
	constexpr std::uint64_t kShadowFrameRecordOffset = 0;
	constexpr std::uint64_t kShadowArenaBlocksOffset = 1024;
	// The arena's blocks at fixed places (ShadowArenaBlocksOf): the zero block first, then SharedData and FeatureData at offsets
	// CS's block sizes fix, so a revision's shapes name them without a build.
	constexpr std::uint64_t kShadowZeroBlockBytes = 1024;
	// [0] kSHADOWMAPS_ESRAM (cascades, spot lights), [1] kSHADOWMAPS (point and focus lights), and
	// [2] kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM, the volumetric lighting copy: the engine's second draw of
	// each cascade's accumulator, with flag 0x100, draws batch group 15 alone (FUN_1414b44f0) - the passes
	// registered with accumulation hint 8, which are the volumetric-only casters
	// (ShadowReject::VolumetricOnly). DCLF's views of it draw those casters alone (kCullVolumetricOnly).
	// See CaptureShadowView.
	// [3] depth target 10 as Skylighting swaps it in: its own occlusion map; [4] depth target 10 itself, the precipitation
	// mask (ExecuteOcclusion, OcclusionDepthTarget).
	constexpr std::uint32_t kFirstOcclusionTarget = 3;
	constexpr std::uint32_t kShadowDepthTargets = kFirstOcclusionTarget + kOcclusionViews;
	inline constexpr std::uint32_t OcclusionDepthTarget(std::uint32_t a_view) { return kFirstOcclusionTarget + a_view; }
	inline constexpr bool IsOcclusionTarget(std::uint32_t a_target) { return a_target >= kFirstOcclusionTarget; }
	// The sequence buffers (BuildDraws' output) have no fixed draw capacity: each is sized, before its epoch, to hold every draw
	// the scene's tracked objects can produce (SceneDrawBound), growing when that does, and each indirect draw's max count
	// is its epoch's own bound. They start at these (CS_DCLF_TABLE_START=small: a few). A bound over the device's
	// maxSequenceCount is a hard failure.
	constexpr std::uint32_t kInitialSequenceDraws = 16384;
	constexpr std::uint32_t kInitialDecalDraws = 2048;
	constexpr std::uint64_t kConstantAlignment = 256;  // uniform buffer address alignment (conservative)
	constexpr std::uint32_t kColorTargets = 8;
	// The scene tables' first capacities (SceneBuffers), which Impl::ReserveSceneTables grows to what the tables hold, doubling
	// (CS_DCLF_TABLE_START=small: a few rows each). The object capacity sizes every per-object buffer too: the object records,
	// the visibility and frustum words (indexed by the object's table index), and the draw inputs - a segment or a shadow mode
	// has at most one input per object (a cull-only one for a candidate it may not draw), so an input buffer the size of the
	// object table holds any build's. The geometry rows are the slots' then one per face stream; the extras rows every object's
	// extras block (the palettes are FrameValues'); the face vertices every face shape's region.
	constexpr std::uint32_t kInitialObjects = 32768;
	constexpr std::uint32_t kInitialGeometries = 16384;
	constexpr std::uint32_t kInitialPoolIndices = 1u << 23;  // the shadow views' index pool (IndexPool): 16 MB
	constexpr std::uint32_t kInitialExtraRows = 16384;
	constexpr std::uint32_t kInitialFaceVertices = 1u << 18;
	constexpr std::uint32_t kNoRecord = ~0u;
	constexpr std::uint32_t kNoSkip = ~0u;
	// BuildDrawsCS's counter words: [0] drawn, [1] culled, [2] tested; a shadow view's [3] outside the sun's entry, [4] small, [5]
	// stood-in fading, [16] of its caster class.
	constexpr std::uint32_t kCountWords = 28;
	/** @brief A count buffer's worth of zeros: what an epoch uploads to reset the counters it appends through. */
	inline constexpr std::uint32_t kZeroCounts[kCountWords] = {};
	// The sun's bits on the GPU: the colour dispatch's inputs with kObjectSunTest, and those that missed every cascade.
	constexpr std::uint32_t kCountSunTestedWord = 23;
	constexpr std::uint32_t kCountSunMissedWord = 24;
	// The fade test (kObjectFadeTest): the depth segment's first phase's flagged inputs in the frustum, and those it dropped.
	constexpr std::uint32_t kCountFadeTestedWord = 25;
	constexpr std::uint32_t kCountFadeHiddenWord = 26;
	// Decals: the two groups' slot counts, written by the CPU and read by their draws, then the culling's
	// own tallies. Byte offsets must match BuildDrawsCS.hlsl.
	constexpr std::uint32_t kCountDecalGroupWord = 19;  // group 1 at word 19, group 2 at word 20
	constexpr std::uint32_t kCountDecalsCulledWord = 21;
	constexpr std::uint32_t kCountDecalsTestedWord = 22;
	constexpr std::uint32_t kCountDecalLayerWord = 27;  // group 3 (the multi-index layers)
	// Byte offsets of the count words the indirect draws read; they must match BuildDrawsCS.hlsl.
	constexpr std::uint64_t kCountDrawnPhaseTwoBytes = 68;
	constexpr const char* kBuildDrawsShader = "DrawcallLimitFix/BuildDrawsCS.hlsl";
	constexpr const char* kHzbShader = "DrawcallLimitFix/HzbCS.hlsl";
	constexpr const char* kSortSequencesShader = "DrawcallLimitFix/SortSequencesCS.hlsl";
	constexpr const char* kTreeWindShader = "DrawcallLimitFix/TreeWindCS.hlsl";
	// TreeWindCS.hlsl's constants: the buffers' indices, the two wind buffers' last. The count, the frame and its inputs are
	// the frame row (TreeWindFrameRow), which every commit uploads: the invocation is prepared ahead of the commit.
	struct TreeWindConstants
	{
		std::uint32_t treesIndex = 0, clocksIndex = 0, frameIndex = 0, padding0 = 0;
		std::uint32_t windIndices[2]{};
		std::uint32_t padding1[2]{};
	};
	// The wind buffers' rows per entry (TreeWindCS.hlsl): TreeParams, WindTimers, the listing's generation.
	constexpr std::uint32_t kTreeWindEntryRows = 3;
	static_assert(sizeof(TreeWindConstants) == 32);
	// TreeWindCS.hlsl's frame row (StructuredBuffer, one row).
	struct TreeWindFrameRow
	{
		std::uint32_t treeCount = 0, objectCount = 0, frame = 0, padding = 0;
		TreeWindFrame inputs{};
		std::uint32_t padding2[2]{};
	};
	static_assert(sizeof(TreeWindFrameRow) == 64);
	constexpr std::uint32_t kTreeWindConstantWords = sizeof(TreeWindConstants) / 4;
	constexpr std::uint32_t kTreeWindGroup = 64;
	/*
	 * CS_DCLF_FOLIAGE_PARITY (FoliageParityCS.hlsl): the colour pass's alpha-tested draws write each pixel they shade - its object
	 * (index + 1 in the low 24 bits, the frame's low 8 bits above them, so a pixel no draw wrote this frame reads as none) and
	 * its albedo and diffuse (RGBA8 each) - into the frame's pair of buffers, by the frame's parity; the compare pass reads
	 * them against the frame before's.
	 */
	constexpr const char* kFoliageParityShader = "DrawcallLimitFix/FoliageParityCS.hlsl";
	struct FoliageParityConstants
	{
		// Both pairs (by the epoch's parity): which is this frame's is the frame block's, as the colour pass read it.
		std::uint64_t ids[2]{}, colours[2]{};
		std::uint64_t frameBlock = 0;    // the colour epoch's words in the frame lighting block (PS b13, c14: DCLFFoliageParity)
		std::uint32_t resultsIndex = 0;  // the results' UAV (RWByteAddressBuffer)
		std::uint32_t ownersIndex = 0;   // the Z-prepass's owners (RWStructuredBuffer<uint64_t>), cleared as read
		std::uint32_t depthIndex = 0;    // the main depth (SRV)
		std::uint32_t padding0 = 0;
		std::uint32_t width = 0, height = 0, padding1 = 0, padding2 = 0;
	};
	constexpr std::uint32_t kFoliageParityConstantWords = sizeof(FoliageParityConstants) / 4;
	// The results (FoliageParityCS.hlsl): kFoliageCounters counters, then the samples, kFoliageSampleWords words each.
	enum FoliageCounter : std::uint32_t
	{
		// Against the frame before (a still camera).
		kFoliageCompared,    // pixels the frame before had a foliage object at
		kFoliageVanished,    // ... and this frame has none of that object at or beside
		kFoliageAppeared,    // pixels this frame has an object at that the frame before had none of at or beside
		kFoliageRecoloured,  // the same object both frames, its albedo or diffuse moved by more than the pass's step
		kFoliageWhitened,    // ... towards white (every albedo channel at least 0.85, from under 0.6)
		kFoliageFrameSampleCount,
		// Within the frame, whatever the camera does: the pixels whose depth an alpha-tested Z-prepass draw wrote (the closest
		// fragment its alpha test kept: the owner), and of them those the colour pass shaded with no object or another one.
		kFoliageOwned,
		kFoliageUnshaded,
		kFoliageOtherObject,
		// This frame's foliage pixels whose albedo, or diffuse, is near white (every channel at least 0.85).
		kFoliageWhiteAlbedo,
		kFoliageWhiteDiffuse,
		kFoliageEpochTag,  // the frame's tag, written by the pass
		// Foliage pixels whose motion vector is more than a pixel (and more than 8) from a static object's: the trees' clocks are
		// frozen, so the foliage is static.
		kFoliageMotion,
		kFoliageMotionFar,
		kFoliageInFrameSampleCount,
		kFoliageWhiteSampleCount,
		// Against the frame before, reprojected: the lit colour (diffuse and specular) brighter by more than 0.3 in luminance.
		kFoliageBrightened,
		// Of the owned pixels, terrain LOD's (whose depth the Z-prepass and the colour pass each compute from HighDetailRange),
		// and of them those left unshaded or shaded with another object.
		kFoliageLandOwned,
		kFoliageLandUnshaded,
		kFoliageLandOtherObject,
		kFoliageLandSampleCount,
		kFoliageCounters = 24
	};
	// A sample: x | y << 16, kind (its counter), then by kind - against the frame before: the object, its diffuse, the frame before's
	// diffuse, its specular, the frame before's specular, its motion vector (half2 pixels); in-frame: the owner, the colour pass's
	// object, the owner's depth, the albedo (unshaded, another object) or the object, its albedo, the motion vector's error
	// (float pixels) and the motion vector (motion); near white: the object, its albedo and diffuse.
	constexpr std::uint32_t kFoliageSampleWords = 8, kFoliageFrameSamples = 24, kFoliageInFrameSamples = 32, kFoliageWhiteSamples = 16, kFoliageLandSamples = 16;
	constexpr std::uint32_t kFoliageFrameSampleBase = 0, kFoliageInFrameSampleBase = kFoliageFrameSampleBase + kFoliageFrameSamples,
							kFoliageWhiteSampleBase = kFoliageInFrameSampleBase + kFoliageInFrameSamples,
							kFoliageLandSampleBase = kFoliageWhiteSampleBase + kFoliageWhiteSamples;
	constexpr std::uint32_t kFoliageResultWords = kFoliageCounters + (kFoliageLandSampleBase + kFoliageLandSamples) * kFoliageSampleWords;
	constexpr std::uint32_t kFoliageColourWords = 4;  // a pixel's: albedo, diffuse, motion error, motion vector
	// What the tree buffers hold at first (they double as the scene needs).
	constexpr std::uint32_t kInitialTrees = 1024;
	constexpr const char* kFadeStateShader = "DrawcallLimitFix/FadeStateCS.hlsl";
	// Tree LOD's cull (TreeLodCullCS.hlsl; Scene/TreeLod.h), in the depth segment's two phases: the tables' and the latch's
	// indices, the phase, the retest list's place, and the HZB as BuildDrawsConstants has it; a thread per instance record (phase
	// 1) or per retest entry (phase 2).
	constexpr const char* kTreeLodCullShader = "DrawcallLimitFix/TreeLodCullCS.hlsl";
	struct TreeLodCullConstants
	{
		std::uint32_t shapesIndex = 0, instancesIndex = 0, visibleIndex = 0, latchIndex = 0, latchOffset = 0, drawIndex = 0;
		std::uint32_t phase = 0;
		std::uint32_t retestOffset = 0;  // bytes into the visible list
		std::uint32_t hzbIndex = 0, hzbSizePacked = 0, hzbMips = 0, padding = 0;
	};
	constexpr std::uint32_t kTreeLodCullConstantWords = sizeof(TreeLodCullConstants) / 4;
	constexpr std::uint32_t kTreeLodCullGroup = 64;
	// What the tree LOD tables hold at first (shape slots, mesh slots); they double as the mirror needs.
	constexpr std::uint32_t kInitialTreeLodShapes = 1024, kInitialTreeLodMeshes = 256;
	constexpr const char* kIndexPoolShader = "DrawcallLimitFix/IndexPoolCS.hlsl";
	// IndexPoolCS.hlsl's constants: the latch block and its copies' dispatch (ShadowLatchLayout::PoolOffset), the copies, the pool.
	struct IndexPoolConstants
	{
		std::uint32_t latchIndex = 0, latchOffset = 0, copiesIndex = 0, poolIndex = 0;
	};
	constexpr std::uint32_t kIndexPoolConstantWords = sizeof(IndexPoolConstants) / 4;
	// The copies' dispatch: a group per copy, rows of IndexPoolCS's kGroupsX.
	constexpr std::uint32_t kIndexPoolGroupsX = 65535;
	// FadeStateCS.hlsl's constants: the buffers' indices, the depth segment's latch (its view-projection), the root count, the
	// scene frame, and the parity log's first root (~0u: no log). The frame's inputs are a buffer (Records.h, FadeFrame).
	struct FadeStateConstants
	{
		std::uint32_t rootsIndex = 0, statesIndex = 0, frameIndex = 0;
		std::uint32_t latchIndex = 0, latchOffset = 0, logIndex = 0;
		std::uint32_t outIndices[2]{};  // the published states, by the scene frame's parity
		std::uint32_t visibilityIndex = 0;  // ByteAddressBuffer: the list processes' cull tests (Records.h, kFadeVisibilityLists blocks)
		std::uint32_t rootListsIndex = 0;   // StructuredBuffer<uint>: each root's block (PrimaryCull::FadeRootLists)
		std::uint32_t animatedIndex = 0;    // StructuredBuffer<uint>: per root, the scene frame whose animation batch updated it
		std::uint32_t eventsIndex = 0;      // RWStructuredBuffer<uint>: the write-back's events (Records.h, FadeEvent)
		std::uint32_t reportedIndex = 0;    // RWStructuredBuffer<uint2>: per root, the generation and milestone last reported
		std::uint32_t eventCapacity = 0;    // the events the list and every frame slot's readback hold
		std::uint32_t padding[2]{};
	};
	static_assert(sizeof(FadeStateConstants) == 64);
	constexpr std::uint32_t kFadeStateConstantWords = sizeof(FadeStateConstants) / 4;
	constexpr std::uint32_t kFadeStateGroup = 64;
	constexpr std::uint32_t kInitialFadeRoots = 4096;
	// HzbCS.hlsl's constants (Internal.h, HzbConstants): the source, its valid and padded extents, the levels written and
	// their descriptors.
	constexpr std::uint32_t kHzbConstantWords = 24;
	// The levels one single-pass downsample writes (FidelityFX SPD): a 4096-texel domain to one texel.
	constexpr std::uint32_t kHzbDispatchMips = 12;
	// Its group's tile of the source.
	constexpr std::uint32_t kHzbTile = 64;


	// BuildDrawsCS.hlsl's inputs (byte-address buffers).
	struct DrawInput
	{
		std::uint32_t pipelineIndex;  // in the pipeline sets; a shadow input's key slot (Lookups::shadowSlots)
		std::uint32_t recordIndex;    // DrawBindings record
		std::uint32_t geometryIndex;  // GeometryDraw
		std::uint32_t flags;          // object flags, plus kInputDrawable for this epoch
		// Unused (zero): the bound is the placement row's (BindlessPlacement::bound), so a move rewrites no input.
		float boundCentre[3];
		float boundRadius;
		// Index into the frame's object table. The two segments emit different subsets in different
		// orders, so this is what lets the colour segment look up the visibility the depth segment
		// published for the same object.
		std::uint32_t objectIndex;
		std::uint32_t decalOrdinal;  // decals only: the slot in the group's range
		// Skins of several partitions: bit i draws partition i (Tables::skinPartitions); 0 draws the one
		// geometry, kNoPartitions nothing. Left 0 by every input that is not a skin.
		std::uint32_t partitions;
		// The second vertex stream of a face shape (or of a pipeline reading its position from it): the GeometryDraw holding
		// its positions, which both payloads append after the geometry slots (FaceStreamGeometry), or ~0u.
		std::uint32_t streamIndex = ~0u;
		// The object's fade root slot (SceneStore::Tables::objectFadeRoot; FadeStateCS's state row), ~0u when it has none: the
		// depth segment's first phase drops it while an owned root's OnVisible stops (kFadeRootOwned).
		std::uint32_t fadeRoot = ~0u;
		std::uint32_t fadeReserved[2]{};
		// kObjectFadeTest (the depth segment's inputs, a root DCLF does not own): the fade-out distance
		// (SceneStore::Tables::fadeDistance); the fade node's centre is the object record's.
		float fadeDistance = 0.0f;
	};
	static_assert(sizeof(DrawInput) == 64);
	// BuildDrawsCS.hlsl: set on an input the epoch has built a bindings record for. The depth segment
	// submits an input for every candidate so the culling covers them all, but builds records only for
	// the ones it may draw.
	constexpr std::uint32_t kInputDrawable = 1u << 16;
	// The main sequence buffer's ranges (Resources::sequenceDraws, sequenceDecals): phase 1 and the colour segment's draws,
	// then phase 2's (the CPU records where its draw starts, so the two need ranges fixed in advance rather than one shared
	// through an atomic counter), then one range per decal group. A decal's sequence goes to the slot of its ordinal in the
	// engine's draw order (SceneStore::Tables::decalOrdinal), so the second pass draws decals in that order every frame; a
	// culled one is the same sequence with an index count of zero. BuildDrawsConstants carries the bases.
	constexpr std::uint32_t kDecalGroups = 3;
	/** @brief The decal ranges (ObjectDecalGroup - 1) in the order the second pass draws them: the engine's geometry groups 3, 2, 4. */
	constexpr std::array<std::uint32_t, kDecalGroups> kDecalDrawOrder{ 0, 2, 1 };
	/** @brief The count word a decal range's draw reads its slot count from. */
	constexpr std::uint32_t DecalCountWord(std::uint32_t a_range) { return a_range < 2 ? kCountDecalGroupWord + a_range : kCountDecalLayerWord; }
	/** @brief The main sequence buffer's slots for a_draws per draw range and a_decals per decal group. */
	constexpr std::uint64_t SequenceSlots(std::uint32_t a_draws, std::uint32_t a_decals) { return 2ull * a_draws + std::uint64_t(kDecalGroups) * a_decals; }

#pragma pack(push, 4)
	struct GeometryDraw
	{
		std::uint64_t vertexBufferAddress;
		std::uint32_t vertexBufferSize;
		std::uint32_t vertexStride;
		std::uint64_t indexBufferAddress;
		std::uint32_t indexBufferSize;
		std::uint32_t indexCount;
		std::uint32_t firstIndex;
		std::uint32_t nextPartition;  // GeometryRecord::nextPartition
	};
#pragma pack(pop)
	static_assert(sizeof(GeometryDraw) == 40);

	// BuildDrawsCS's push constants: only what is fixed for a pass across executions. The execution's own
	// values are in its BuildDrawsLatch (below), so the recorded dispatch never changes with the frame.
	struct BuildDrawsConstants
	{
		std::uint32_t latchIndex;        // the latch block's SRV
		std::uint32_t inputsIndex;
		std::uint32_t geometriesIndex;
		std::uint32_t sequencesIndex;
		std::uint32_t countIndex;
		// The rows an input's y names (BuildDrawsCS.hlsl, RowsOf): the material rows' table (y's low 20 bits) and, below, the
		// pipeline rows' (its high 12; stride 0 for the shadow views).
		std::uint32_t materialRowsAddressLo;
		std::uint32_t materialRowsAddressHi;
		std::uint32_t materialRowStride;
		std::uint32_t phaseBits;         // the culling phase in bits 4-7 (CullFlags' phase field)
		std::uint32_t visibilityIndex;   // RWByteAddressBuffer: one uint per object in the tables
		std::uint32_t latchOffset;       // set at record time: the slot's region plus the dispatch's latch
		std::uint32_t frustumIndex;      // RWByteAddressBuffer: per object, the stamp of its last in-frustum frame (depth phase 1 only)
		std::uint32_t hzbIndex;        // 0 when there is no HZB to test against
		std::uint32_t hzbSizePacked;   // mip 0: width in the low 16 bits, height in the high 16
		std::uint32_t hzbMips;
		// The sort by pipeline (SortDraws): the per-pipeline counts, the unsorted sequences and each one's rank among its
		// pipeline's. 0 when the dispatch appends into the sequences themselves (phase 2, or the sort off).
		std::uint32_t sortCountsIndex;
		std::uint32_t sortStagingIndex;
		std::uint32_t sortRanksIndex;
		// The sequence buffer's ranges, in sequences: phase 2's first slot (the end of the draws' range, which a draw never
		// reaches: its epoch's bound fits), the first decal slot, and each decal group's range.
		std::uint32_t phaseTwoBase;
		std::uint32_t decalBase;
		std::uint32_t decalStride;
		std::uint32_t pipelineRowStride;
		std::uint32_t pipelineRowsAddressLo;
		std::uint32_t pipelineRowsAddressHi;
		// The depth segment's first phase and the shadow views: the fade roots' static rows (an owned root's members follow its
		// OnVisible verdict; the states are the latch's, BuildDrawsLatch::fadeStatesIndex). 0 elsewhere.
		std::uint32_t fadeRootsIndex;
		std::uint32_t fadeStatesUnused;
		// A shadow view: its slot's bucket counts (ShadowResources::bucketCounts), a word per bucket; the depth segment's phases: theirs
		// (Resources::zBucketCounts), a word per pipeline slot. 0 elsewhere.
		std::uint32_t bucketCountsIndex;
		// A shadow view and the depth segment: each geometry slot's first index in the index pool (IndexPool::firsts). 0 elsewhere.
		std::uint32_t poolFirstsIndex;
	};
	static_assert(sizeof(BuildDrawsConstants) == 112);
	constexpr std::uint32_t kBuildDrawsConstantWords = sizeof(BuildDrawsConstants) / 4;

	/**
	 * @brief One BuildDrawsCS dispatch's values for one execution, in its slot of a latch block
	 * (org::LatchBlock): written by the render thread in the epoch, read by the dispatch - which takes its
	 * own group count from the first three words (ExecuteIndirect with a dispatch signature).
	 */
	struct BuildDrawsLatch
	{
		std::uint32_t dispatch[3];      // groups x, y, z
		std::uint32_t drawCount;        // inputs to cull
		std::uint32_t cullFlags;        // mode in bits 0-3, NoNearPlane at 9 (the phase is pushed)
		std::uint32_t visibilityStamp;  // marks the verdicts as this frame's; see BuildDrawsCS.hlsl
		std::uint32_t hzbUvScalePacked; // rendered area over the area the HZB covers, 16-bit fixed point
		std::uint32_t cullPlaneMask;    // which of cullPlanes are tested; 0 for the main camera
		// Row-major, as the shader's float4x4 with mul(M, v), with the camera translation folded in so
		// that an absolute world position projects directly.
		float viewProj[16];
		// A shadow view's caster volume as the engine culls it: the view's culling process's custom planes
		// (NiCullingProcess::customCullPlanes), absolute world space, (normal, constant) with the inside
		// where dot(normal, p) - constant >= 0.
		float cullPlanes[6][4];
		// A shadow view's row of the pipeline map, in bytes into the latch block: the view's inputs name key
		// slots, and the row holds each slot's pipeline under the view's rasterizer state. 0 when the inputs
		// name pipelines themselves (the main camera).
		std::uint32_t pipelineMapOffset;
		// The main colour pass only: kSunTestOn when BuildDraws tests every kObjectSunTest input's bound against the frame's
		// cascades (sunCascadeOffset; a miss marks the draw kObjectSunMiss). 0 elsewhere.
		std::uint32_t sunState;
		// The depth segment: BSTreeNode::OnVisible's height test for the kObjectHeightTest inputs, the base and the
		// limit (PrimaryCull::TreeHeightTest; +infinity when the test is off). 0 elsewhere.
		float treeHeight[2];
		// The depth segment: the main camera's position and LOD factor (NiCamera +0x184), as BSFadeNode::OnVisible
		// measures a fade root's distance (PrimaryCull::FadeEye), for the kObjectFadeTest inputs. 0 elsewhere.
		float fadeEye[4];
		// The colour pass: the frame's sun cascades (a SunRegion of SunAccumulation::GpuCascade), in bytes into the latch block.
		std::uint32_t sunCascadeOffset;
		// A view of the sun (kCullSunEntry): the frame's full-frustum culling processes (a SunRegion of SunEntryProcess), in bytes
		// into the latch block, shared by every sun view of the slot. An input whose entry sphere (its fade row, SetSunEntryRow)
		// is outside every process is none of the sun's casters. 0: no entry test.
		std::uint32_t sunEntryOffset;
		// The colour pass: the frame's local shadow lights (a SunRegion of GpuShadowVolume, LocalShadowLights), in bytes into the
		// latch block. BuildDraws puts each input's Light Limit Fix shadow mask into its object word (kObjectLocalShadowShift),
		// which the pixel stage takes for ShadowBitMask and its DefShadow. 0 elsewhere.
		std::uint32_t localShadowOffset;
		// A shadow view: its bucket table (ShadowLatchLayout::BucketOffset), in bytes into the latch block. The view's map row
		// names a bucket per key slot, and the table gives each bucket its range of the slot's sequences (first, capacity),
		// which one plain indirect draw with the bucket's pipeline executes (ShadowViewPass). 0 elsewhere.
		std::uint32_t bucketTableOffset;
		// The depth segment's first phase and the shadow views: FadeStateCS's states as the frame before published them
		// (SceneBuffers::FadeStatesReadIndex), whose descriptor depends on the frame and so is the latch's. 0 elsewhere.
		std::uint32_t fadeStatesIndex;
		// The depth segment's second phase: its bucket table (MainLatchLayout::PhaseTwoBucketTableOffset), as bucketTableOffset is
		// the first phase's. 0 elsewhere.
		std::uint32_t phaseTwoBucketTableOffset;
		// The depth segment's phases: each pipeline slot's bucket (MainLatchLayout::BucketMapOffset), its Z-prepass group's in this
		// frame's tables, kNoBucket for a slot with none (no published pipeline); a reflection face's, each LOD slot's forward pipeline's bucket
		// (IndirectDraws::ExecuteReflection). 0 elsewhere.
		std::uint32_t bucketMapOffset;
		// The frame's placement rows (FrameValues::PlacementsIndex: StructuredBuffer<BindlessPlacement>): an input's bound, sun entry
		// and fade node are its object's row, which the frame's values wrote; the descriptor is the frame's (a ring buffer), so the
		// latch's.
		std::uint32_t placementsIndex;
		// Step 6e E4: the payload's buffers this execution reads when they are a payload ring entry (payloadValid; the upload queue
		// filled it from the frame's publication): its inputs' and geometry table's SRVs and its rows' tables' addresses. 0: the
		// pass's own (its push constants).
		std::uint32_t payloadValid;
		std::uint32_t inputsIndex;
		std::uint32_t geometriesIndex;
		std::uint32_t payloadPadding;
		std::uint32_t materialRowsLo, materialRowsHi, pipelineRowsLo, pipelineRowsHi;
	};
	static_assert(sizeof(BuildDrawsLatch) == 288 && offsetof(BuildDrawsLatch, payloadValid) == 256 && offsetof(BuildDrawsLatch, materialRowsLo) == 272 &&
				  offsetof(BuildDrawsLatch, viewProj) == 32 && offsetof(BuildDrawsLatch, cullPlanes) == 96 &&
				  offsetof(BuildDrawsLatch, pipelineMapOffset) == 192 && offsetof(BuildDrawsLatch, sunState) == 196 &&
				  offsetof(BuildDrawsLatch, treeHeight) == 200 && offsetof(BuildDrawsLatch, fadeEye) == 208 &&
				  offsetof(BuildDrawsLatch, sunCascadeOffset) == 224 && offsetof(BuildDrawsLatch, sunEntryOffset) == 228 &&
				  offsetof(BuildDrawsLatch, localShadowOffset) == 232 && offsetof(BuildDrawsLatch, bucketTableOffset) == 236 &&
				  offsetof(BuildDrawsLatch, fadeStatesIndex) == 240 && offsetof(BuildDrawsLatch, phaseTwoBucketTableOffset) == 244 &&
				  offsetof(BuildDrawsLatch, bucketMapOffset) == 248 && offsetof(BuildDrawsLatch, placementsIndex) == 252);
	// BuildDrawsLatch::bucketMapOffset: a pipeline slot without a bucket (BuildDrawsCS's kNoPipeline).
	constexpr std::uint32_t kNoBucket = 0xFFFFFFFFu;
	constexpr std::uint32_t kSunTestOn = 1u << 31;

	/**
	 * @brief The sun's per-frame plane sets in a latch block, grown with the frame: a 16-byte header (the record count), then the
	 * records. The sun's full-frustum processes (SunEntryProcess, in the shadow latch block) and its cascades
	 * (SunAccumulation::GpuCascade, in the main one); neither has a bound in the engine (the processes are the full-frustum
	 * cull's jobs, the cascades iNumSplits).
	 */
	struct SunEntryProcess
	{
		std::uint32_t mask = 0;  // the active planes
		std::uint32_t pad[3]{};
		float planes[6][4]{};    // (normal, constant), inside where dot(normal, p) - constant >= 0

		bool operator==(const SunEntryProcess&) const = default;
	};
	static_assert(sizeof(SunEntryProcess) == 112 && sizeof(SunAccumulation::GpuCascade) == 208);
	constexpr std::uint32_t kSunRegionHeader = 16;
	template <class Record>
	constexpr std::uint32_t SunRegionBytes(std::uint32_t a_records)
	{
		return kSunRegionHeader + a_records * static_cast<std::uint32_t>(sizeof(Record));
	}

	/** @brief The sun's entry rule, as BuildDrawsCS applies it to the latch's processes: outside every one. */
	inline bool OutsideSunEntryProcesses(std::span<const SunEntryProcess> a_processes, const float a_entry[4])
	{
		if (a_processes.empty())
			return false;
		for (const auto& process : a_processes) {
			bool outside = false;
			for (std::uint32_t p = 0; p < 6 && !outside; ++p) {
				if (!(process.mask & (1u << p)))
					continue;
				const float* plane = process.planes[p];
				outside = plane[0] * a_entry[0] + plane[1] * a_entry[1] + plane[2] * a_entry[2] - plane[3] < -a_entry[3];
			}
			if (!outside)
				return false;
		}
		return true;
	}

	/** @brief SunAccumulation's sphere test against one cascade (what BuildDrawsCS does). */
	inline bool InSunCascade(const SunAccumulation::GpuCascade& a_cascade, const float a_centre[3], float a_radius)
	{
		for (std::uint32_t set = 0; set < 2; ++set) {
			const std::uint32_t mask = a_cascade.masks[set];
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(mask & (1u << p)))
					continue;
				const float* plane = a_cascade.planes[set * 6 + p];
				if (plane[0] * a_centre[0] + plane[1] * a_centre[1] + plane[2] * a_centre[2] - plane[3] < -a_radius)
					return false;
			}
		}
		return true;
	}
	/**
	 * @brief The shadow latch block's region per frame slot: the view slots' latches, a pipeline map row per view rasterizer state
	 * (DrawPipelines::ShadowRasterStateId, 1 to rasterStates), then the sun's full-frustum processes. Each dimension is grown
	 * before the epoch (Impl::ReserveShadowLatch).
	 */
	struct ShadowLatchLayout
	{
		std::uint32_t viewSlots = 0, keySlots = 0, rasterStates = 0, sunProcesses = 0;
		std::uint32_t MapOffset() const { return viewSlots * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t MapRowBytes() const { return keySlots * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t SunEntryOffset() const { return MapOffset() + rasterStates * MapRowBytes(); }
		// A view slot's bucket table (BuildDrawsLatch::bucketTableOffset): (first, capacity) per bucket, at most one a key slot.
		std::uint32_t BucketTableBytes() const { return keySlots * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t BucketOffset(std::uint32_t a_slot) const
		{
			return SunEntryOffset() + SunRegionBytes<SunEntryProcess>(sunProcesses) + a_slot * BucketTableBytes();
		}
		// The index pool's copies (IndexPool): their dispatch's groups, then their count.
		std::uint32_t PoolOffset() const { return BucketOffset(viewSlots); }
		// A view slot's blocks (its view block, then its per-frame block at kShadowPerFrameOffset), as the view-blocks buffer holds
		// them: the epoch's latched copies (ShadowLatchedCopiesPass) copy them there, so the commit records no copy.
		std::uint32_t ViewBlockOffset(std::uint32_t a_slot) const
		{
			const std::uint32_t first = (PoolOffset() + 4 * static_cast<std::uint32_t>(sizeof(std::uint32_t)) + 255u) & ~255u;
			return first + a_slot * static_cast<std::uint32_t>(kShadowViewSlotBytes);
		}
		std::uint32_t Bytes() const { return ViewBlockOffset(viewSlots); }
		bool operator==(const ShadowLatchLayout&) const = default;
	};
	/** @brief The main latch block's region per frame slot: the passes' BuildDrawsLatch, then the colour pass's cascades. */
	/**
	 * @brief One cull volume of a local shadow light (LocalShadowLights::Light::Volume), as BuildDraws tests an input against it:
	 * the light's sphere, then a shadowmap descriptor's frustum and custom planes. A light of several descriptors has a record for
	 * each, with the same mask bit.
	 */
	struct GpuShadowVolume
	{
		std::uint32_t masks[2];     // the active planes: frustum (planes 0-5), custom (6-11)
		std::uint32_t maskBit;      // Light Limit Fix's ShadowBitMask bit (1 << maskIndex)
		std::uint32_t affectsLand;  // 1: taken by landscape properties too (kObjectLandscapeLights)
		float sphere[4];            // the light's centre and radius
		float planes[12][4];        // (normal, constant), inside where dot(normal, p) - constant >= 0
	};
	static_assert(sizeof(GpuShadowVolume) == 224);

	struct MainLatchLayout
	{
		std::uint32_t cascades = 0;
		std::uint32_t shadowVolumes = 0;
		// The Z-prepass's buckets: one per group of pipeline slots sharing a depth pipeline (IndirectState::zGroups), at most a
		// slot each, in each phase's table (first, capacity), and the slots' map to them.
		std::uint32_t buckets = 0;
		static constexpr std::uint32_t CascadeOffset() { return static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t ShadowVolumeOffset() const { return CascadeOffset() + SunRegionBytes<SunAccumulation::GpuCascade>(cascades); }
		std::uint32_t BucketTableOffset() const { return ShadowVolumeOffset() + SunRegionBytes<GpuShadowVolume>(shadowVolumes); }
		std::uint32_t PhaseTwoBucketTableOffset() const { return BucketTableOffset() + buckets * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t BucketMapOffset() const { return PhaseTwoBucketTableOffset() + buckets * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		// The index pool's copies' dispatch (IndexPool), as the shadow latch has it, for the depth commit's pool update.
		std::uint32_t PoolOffset() const { return BucketMapOffset() + buckets * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t Bytes() const { return PoolOffset() + 4 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
	};
	// cullFlags: a clamped shadow view (0xE) pancakes casters in front of its near plane onto it
	// (Utility.hlsl: RENDER_SHADOWMAP_CLAMPED), so the near plane rejects nothing there.
	constexpr std::uint32_t kCullNoNearPlane = 0x200;
	// cullFlags: which caster class a shadow view draws (kObjectVolumetricOnly). The mode's inputs hold both
	// classes; a view of the volumetric lighting copy draws the volumetric-only casters alone, every other
	// shadow view everything else. Neither is set for the main camera.
	constexpr std::uint32_t kCullCastersOnly = 0x400;
	constexpr std::uint32_t kCullVolumetricOnly = 0x800;
	// cullFlags: a view of the sun (its cascades and their volumetric copies), which applies the sun's entry rule (sunEntryOffset).
	constexpr std::uint32_t kCullSunEntry = 0x1000;
	// cullFlags: Skylighting's size test (Skylighting::OcclusionTechnique), for its occlusion map's view: an input whose bound
	// radius is 32 or less draws nothing.
	constexpr std::uint32_t kCullMinRadius = 0x2000;
	// cullFlags: the occlusion map's view: an occluder under a fade root BSFadeNode::OnVisible stops at without
	// cameraRelatedUpdates (Precipitation::SetupMask's cull; FadeStateCS's state) draws nothing, in place of the shadow views'
	// stood-in fading test. Set while the engine's fades are on (0x142032dfd).
	constexpr std::uint32_t kCullFadeOnVisible = 0x4000;
}
