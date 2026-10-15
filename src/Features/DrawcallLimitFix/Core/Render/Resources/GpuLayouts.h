#pragma once

// IndirectDraws' capacities and the layouts the GPU reads, which must match the HLSL (BuildDrawsCS, HzbCS, Lighting.hlsl's DCLF
// paths). Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
	constexpr const char* kExtensionId = "cs.dclf.main-opaque";
	constexpr const char* kShadowExtensionId = "cs.dclf.shadow";
	constexpr const char* kReflectionExtensionId = "cs.dclf.reflection";
	constexpr const char* kFirstPersonExtensionId = "cs.dclf.first-person";
	// Render modes 0xD plain, 0xE clamped, 0xF paraboloid, then one per occlusion view (kOcclusionViews; render mode 0x1C both).
	constexpr std::uint32_t kFirstOcclusionMode = 3;
	constexpr std::uint32_t kShadowModeCount = kFirstOcclusionMode + kOcclusionViews;
	constexpr std::uint32_t kOcclusionRenderMode = 0x1C;
	inline constexpr bool IsOcclusionMode(std::uint32_t a_mode) { return a_mode >= kFirstOcclusionMode; }
	inline constexpr std::uint32_t OcclusionOfMode(std::uint32_t a_mode) { return a_mode - kFirstOcclusionMode; }
	inline constexpr std::uint32_t OcclusionModeOf(std::uint32_t a_view) { return kFirstOcclusionMode + a_view; }
	// The shadow epoch's view slots: occlusion views first, then the frame's shadow views (the exterior has four: two cascades,
	// twice; an interior two hemispheres per shadow-casting point light). These and the following capacities are grown before the
	// epoch (Impl::ReserveShadowLatch). Key slots: Lookups::shadowSlotKeys, one per caster key, one per occlusion key.
	constexpr std::uint32_t kInitialShadowViewSlots = 8;
	inline constexpr std::uint32_t OcclusionSlot(std::uint32_t a_view) { return a_view; }
	constexpr std::uint32_t kFirstShadowViewSlot = kOcclusionViews;
	constexpr std::uint32_t kInitialShadowKeySlots = 1024;
	// Pipeline map rows (one per view rasterizer state) and the sun's full-frustum processes (six and six at the exterior save).
	constexpr std::uint32_t kInitialShadowRasterStates = 8;
	constexpr std::uint32_t kInitialSunProcesses = 8;
	// The main latch block's cascades (two at the exterior save) and local shadow light volumes (at most three lights a frame).
	constexpr std::uint32_t kInitialSunCascades = 4;
	constexpr std::uint32_t kInitialShadowVolumes = 8;
	constexpr std::uint64_t kShadowConstantBytes = 1ull << 20;
	// A view slot's blocks (ShadowResources::viewBlocks): Utility PerTechnique (b0), then its VS_PerFrame copy (b12).
	constexpr std::uint64_t kShadowPerFrameOffset = 256;
	constexpr std::uint64_t kShadowViewSlotBytes = 256 + 1024;
	// The shadow constants: the frame record (DrawBindings) the views' push data names, then the arena's blocks.
	constexpr std::uint64_t kShadowFrameRecordOffset = 0;
	constexpr std::uint64_t kShadowArenaBlocksOffset = 1024;
	// The arena's blocks at fixed places (ShadowArenaBlocksOf: zero block, SharedData, FeatureData), so shapes name them without a build.
	constexpr std::uint64_t kShadowZeroBlockBytes = 1024;
	// Depth targets: [0] kSHADOWMAPS_ESRAM (cascades, spot lights), [1] kSHADOWMAPS (point and focus lights), [2]
	// kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM: the engine's second draw of each cascade's accumulator (flag 0x100) draws batch group 15
	// alone (FUN_1414b44f0), i.e. the accumulation-hint-8 volumetric-only casters (kCullVolumetricOnly). [3] depth target 10 as
	// Skylighting swaps it in; [4] depth target 10 itself, the precipitation mask.
	constexpr std::uint32_t kFirstOcclusionTarget = 3;
	constexpr std::uint32_t kShadowDepthTargets = kFirstOcclusionTarget + kOcclusionViews;
	inline constexpr std::uint32_t OcclusionDepthTarget(std::uint32_t a_view) { return kFirstOcclusionTarget + a_view; }
	inline constexpr bool IsOcclusionTarget(std::uint32_t a_target) { return a_target >= kFirstOcclusionTarget; }
	// The sequence buffers' start sizes; each grows before its epoch to every draw the tracked objects can produce
	// (SceneDrawBound). A bound over the device's maxSequenceCount is a hard failure.
	constexpr std::uint32_t kInitialSequenceDraws = 16384;
	constexpr std::uint32_t kInitialDecalDraws = 2048;
	constexpr std::uint64_t kConstantAlignment = 256;  // uniform buffer address alignment (conservative)
	constexpr std::uint32_t kColorTargets = 8;
	// The scene tables' first capacities, doubled by the scene's set to Impl::RequireSceneTables. The object capacity sizes every per-object
	// buffer.
	constexpr std::uint32_t kInitialObjects = 32768;
	constexpr std::uint32_t kInitialGeometries = 16384;
	constexpr std::uint32_t kInitialPoolIndices = 1u << 23;  // the shadow views' index pool (IndexPool): 16 MB
	constexpr std::uint32_t kInitialFaceVertices = 1u << 18;
	constexpr std::uint32_t kNoRecord = ~0u;
	constexpr std::uint32_t kNoSkip = ~0u;
	// BuildDrawsCS's counter words: [0] drawn, [1] culled, [2] tested; a shadow view's [3] outside the sun's entry, [4] small, [5]
	// under a fading root, [16] of its caster class.
	constexpr std::uint32_t kCountWords = 40;
	/** @brief Zeros an epoch uploads to reset the counters it appends through. */
	inline constexpr std::uint32_t kZeroCounts[kCountWords] = {};
	// The sun test (kObjectSunTest): the colour dispatch's tested inputs, and those that missed every cascade.
	constexpr std::uint32_t kCountSunTestedWord = 23;
	constexpr std::uint32_t kCountSunMissedWord = 24;
	// The fade test (kObjectFadeTest): the depth segment's first phase's flagged inputs in the frustum, and those it dropped.
	constexpr std::uint32_t kCountFadeTestedWord = 25;
	constexpr std::uint32_t kCountFadeHiddenWord = 26;
	// Decals: the groups' slot counts (written by the CPU, read by their draws), then the culling's tallies. Must match
	// BuildDrawsCS.hlsl.
	constexpr std::uint32_t kCountDecalGroupWord = 19;  // group 1 at word 19, group 2 at word 20
	constexpr std::uint32_t kCountDecalsCulledWord = 21;
	constexpr std::uint32_t kCountDecalsTestedWord = 22;
	constexpr std::uint32_t kCountDecalLayerWord = 27;  // group 3 (the multi-index layers)
	// The terrain groups' draws (ObjectTerrainGroup), then depth phase 1's group 1 inputs tested and near. Must match BuildDrawsCS.hlsl.
	constexpr std::uint32_t kCountTerrainGroupWord = 28;  // group 1 at word 28, group 2 at word 29
	constexpr std::uint32_t kCountTerrainNearTestedWord = 30;
	constexpr std::uint32_t kCountTerrainNearWord = 31;
	// The forward range's draw count, then forward inputs tested, in the frustum and HZB-rejected. Must match BuildDrawsCS.hlsl
	// and ForwardSortCS.hlsl.
	constexpr std::uint32_t kCountForwardWord = 32;
	constexpr std::uint32_t kCountForwardTestedWord = 33;
	constexpr std::uint32_t kCountForwardInViewWord = 34;
	constexpr std::uint32_t kCountForwardOccludedWord = 35;
	// The refraction range's draw count, then its inputs tested, in the frustum and rejected. Must match BuildDrawsCS.hlsl.
	constexpr std::uint32_t kCountRefractionWord = 36;
	constexpr std::uint32_t kCountRefractionTestedWord = 37;
	constexpr std::uint32_t kCountRefractionInViewWord = 38;
	constexpr std::uint32_t kCountRefractionOccludedWord = 39;
	// Byte offsets of the count words the indirect draws read; must match BuildDrawsCS.hlsl.
	constexpr std::uint64_t kCountDrawnPhaseTwoBytes = 68;
	constexpr const char* kBuildDrawsShader = "DrawcallLimitFix/BuildDrawsCS.hlsl";
	constexpr const char* kHzbShader = "DrawcallLimitFix/HzbCS.hlsl";
	constexpr const char* kSortSequencesShader = "DrawcallLimitFix/SortSequencesCS.hlsl";
	constexpr const char* kTreeWindShader = "DrawcallLimitFix/TreeWindCS.hlsl";
	// TreeWindCS.hlsl's constants: buffer indices only; per-frame values are the frame row every commit uploads.
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
		std::uint32_t treeCount = 0, objectCount = 0, frame = 0;
		std::uint32_t treeSeedsIndex = 0;  // FrameValues::TreeSeedsIndex (the frame's pooled backing)
		TreeWindFrame inputs{};
		std::uint32_t padding2[2]{};
	};
	static_assert(sizeof(TreeWindFrameRow) == 64);
	constexpr std::uint32_t kTreeWindConstantWords = sizeof(TreeWindConstants) / 4;
	constexpr std::uint32_t kTreeWindGroup = 64;
	// The foliage record (Policy::RecordFoliagePixels, CS_DCLF_FOLIAGE_PARITY; Diagnostics/Draws/Foliage.cpp): alpha-tested colour
	// draws write each shaded pixel's object (index + 1, low 24 bits; the frame's low 8 above) and albedo and diffuse. The compare
	// pass's results: kFoliageCounters counters, then the samples, kFoliageSampleWords words each.
	enum FoliageCounter : std::uint32_t
	{
		// Against the frame before (a still camera).
		kFoliageCompared,    // pixels the frame before had a foliage object at
		kFoliageVanished,    // ... and this frame has none of that object at or beside
		kFoliageAppeared,    // pixels this frame has an object at that the frame before had none of at or beside
		kFoliageRecoloured,  // the same object both frames, its albedo or diffuse moved by more than the pass's step
		kFoliageWhitened,    // ... towards white (every albedo channel at least 0.85, from under 0.6)
		kFoliageFrameSampleCount,
		// Within the frame: pixels an alpha-tested Z-prepass draw owns, and of them those shaded with no object or another one.
		kFoliageOwned,
		kFoliageUnshaded,
		kFoliageOtherObject,
		// This frame's foliage pixels whose albedo, or diffuse, is near white (every channel at least 0.85).
		kFoliageWhiteAlbedo,
		kFoliageWhiteDiffuse,
		kFoliageEpochTag,  // the frame's tag, written by the pass
		// Foliage pixels whose motion vector is over 1 (and over 8) pixels from a static object's (tree clocks are frozen).
		kFoliageMotion,
		kFoliageMotionFar,
		kFoliageInFrameSampleCount,
		kFoliageWhiteSampleCount,
		// Against the frame before, reprojected: the lit colour brighter by more than 0.3 in luminance.
		kFoliageBrightened,
		// Of the owned pixels, terrain LOD's, and of them those unshaded or shaded with another object.
		kFoliageLandOwned,
		kFoliageLandUnshaded,
		kFoliageLandOtherObject,
		kFoliageLandSampleCount,
		kFoliageCounters = 24
	};
	// A sample: x | y << 16, kind (its counter), then by kind - frame before: object, diffuse and previous, specular and previous,
	// motion (half2); in-frame: owner, colour object, owner depth, albedo, or object, albedo, motion error, motion; near white:
	// object, albedo, diffuse.
	constexpr std::uint32_t kFoliageSampleWords = 8, kFoliageFrameSamples = 24, kFoliageInFrameSamples = 32, kFoliageWhiteSamples = 16, kFoliageLandSamples = 16;
	constexpr std::uint32_t kFoliageFrameSampleBase = 0, kFoliageInFrameSampleBase = kFoliageFrameSampleBase + kFoliageFrameSamples,
							kFoliageWhiteSampleBase = kFoliageInFrameSampleBase + kFoliageInFrameSamples,
							kFoliageLandSampleBase = kFoliageWhiteSampleBase + kFoliageWhiteSamples;
	constexpr std::uint32_t kFoliageResultWords = kFoliageCounters + (kFoliageLandSampleBase + kFoliageLandSamples) * kFoliageSampleWords;
	constexpr std::uint32_t kFoliageColourWords = 4;  // a pixel's: albedo, diffuse, motion error, motion vector
	// The tree buffers' first capacity (doubled as needed).
	constexpr std::uint32_t kInitialTrees = 1024;
	constexpr const char* kFadeStateShader = "DrawcallLimitFix/FadeStateCS.hlsl";
	// Tree LOD's cull (TreeLodCullCS.hlsl) in the depth segment's two phases: a thread per instance (phase 1) or retest entry (2).
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
	// The tree LOD tables' first capacities (shape slots, mesh slots), doubled as the mirror needs.
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
	// FadeStateCS.hlsl's constants: the depth latch (view-projection), the parity log's first root (~0u: none). Must match the HLSL.
	struct FadeStateConstants
	{
		std::uint32_t rootsIndex = 0, statesIndex = 0, frameIndex = 0;
		std::uint32_t latchIndex = 0, latchOffset = 0, logIndex = 0;
		std::uint32_t outIndices[2]{};  // the published states, by the scene frame's parity
		std::uint32_t programsIndex = 0;      // ByteAddressBuffer: the main camera's portal programs (Records.h, kPortal*)
		std::uint32_t rootProgramsIndex = 0;  // StructuredBuffer<uint>: each root's program (PortalViews::ProgramOf)
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
	// VisibilityEventsCS.hlsl: per-root and per-object main-camera visibility as change events; threads: root slots, then objects.
	constexpr const char* kVisibilityEventsShader = "DrawcallLimitFix/VisibilityEventsCS.hlsl";
	struct VisibilityEventsConstants
	{
		std::uint32_t rootsIndex = 0, statesIndex = 0, frameIndex = 0, frustumIndex = 0;
		std::uint32_t rootReportedIndex = 0, objectReportedIndex = 0, eventsIndex = 0, eventCapacity = 0;
		std::uint32_t rootSlots = 0, objectSlots = 0;
		std::uint32_t padding[2]{};
	};
	static_assert(sizeof(VisibilityEventsConstants) == 48);
	constexpr std::uint32_t kVisibilityEventsConstantWords = sizeof(VisibilityEventsConstants) / 4;
	constexpr std::uint32_t kVisibilityEventsGroup = 64;
	/**
	 * @brief One event (VisibilityEventsCS.hlsl): a slot whose verdict changed since last reported. A root's generation is its
	 * listing's (FadeRootStatic::generation); an object's is 0 (the occupant is the frame's tables').
	 */
	struct VisibilityEvent
	{
		std::uint32_t slot;
		std::uint32_t generation;
		std::uint32_t kind;  // kVisibilityKind*
		std::uint32_t bits;  // kVisibilityBit*
	};
	static_assert(sizeof(VisibilityEvent) == 16);
	constexpr std::uint32_t kVisibilityKindRoot = 0, kVisibilityKindObject = 1;
	constexpr std::uint32_t kVisibilityBitVisible = 1u << 0;   // a root: InView (its kAccumulated); an object: stamped, not fade-hidden
	constexpr std::uint32_t kVisibilityBitInMargin = 1u << 1;  // reserved: InView against widened planes (unimplemented)
	constexpr std::uint32_t kVisibilityBitVerdict = 1u << 2;   // a root: FadeStateCS updated its listing this frame (else no DCLF verdict)
	constexpr std::uint32_t kVisibilityBitFadeHidden = 1u << 3;  // an object: in view, dropped by its root's fade
	// The list's header: the count appended (past the capacity: what the list grows to), the scene frame; two words spare.
	constexpr std::uint32_t kVisibilityEventHeaderWords = 4;
	constexpr std::uint32_t kInitialVisibilityEvents = 4096;
	inline constexpr std::uint64_t VisibilityEventBytes(std::uint32_t a_events)
	{
		return (kVisibilityEventHeaderWords + std::uint64_t(a_events) * 4) * sizeof(std::uint32_t);
	}
	// HzbCS.hlsl's constants (PassHelpers.h, HzbConstants): the source, its valid and padded extents, the levels and descriptors.
	constexpr std::uint32_t kHzbConstantWords = 24;
	// The levels one single-pass downsample (FidelityFX SPD) writes: a 4096-texel domain to one texel.
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
		/** @brief Its kView* bits (tested first against BuildDrawsLatch::viewBits) and the keys each view family draws it by. */
		struct ViewWords
		{
			std::uint32_t mask = 0;
			std::uint32_t shadowKey = ~0u;                      // a caster's key slot (Lookups::shadowSlots), every caster mode's
			std::uint32_t occlusionKey[2]{ ~0u, ~0u };        // each occlusion map's key slot
		} view;
		// Index into the frame's object table: lets the colour segment find the visibility the depth segment published.
		std::uint32_t objectIndex;
		std::uint32_t decalOrdinal;  // decals only: the slot in the group's range
		// Skins: bit i draws partition i; 0 draws the one geometry, kNoPartitions nothing. lodPartitions narrows a LOD skin's.
		std::uint32_t partitions;
		// A face shape's second vertex stream: the GeometryDraw holding its positions (FaceStreamGeometry), or ~0u.
		std::uint32_t streamIndex = ~0u;
		// The fade root slot (FadeStateCS's state row), ~0u when none: depth phase 1 drops it while its root's OnVisible stops, a
		// shadow view while its root fades.
		std::uint32_t fadeRoot = ~0u;
		std::uint32_t shadowRow = 0;      // a caster's ShadowMaterialRow
		// A LOD skin's partitions per level: byte L masks level L (0-3), picked by its fade root's level; 0: partitions as is.
		std::uint32_t lodPartitions = 0;
		// kObjectFadeTest: the fade-out distance (SceneTables::fadeDistance).
		float fadeDistance = 0.0f;
	};
	static_assert(sizeof(DrawInput) == 64);
	// An input with a bindings record: the depth segment culls every candidate but records only those it may draw.
	constexpr std::uint32_t kInputDrawable = 1u << 16;
	// DrawInput::ViewWords::mask and BuildDrawsLatch::viewBits: the view types an object takes part in. BuildDrawsCS.hlsl.
	constexpr std::uint32_t kViewMainCull = 1u << 0;       // a main-camera visibility candidate (the depth segment's inputs)
	constexpr std::uint32_t kViewMain = 1u << 1;           // drawn by the main camera (the set's main phase)
	constexpr std::uint32_t kViewSunCaster = 1u << 2;      // the sun's cascades
	constexpr std::uint32_t kViewSpotCaster = 1u << 3;     // the other clamped views (spot lights)
	constexpr std::uint32_t kViewPointCaster = 1u << 4;    // the point lights' paraboloids
	constexpr std::uint32_t kViewSkyOccluder = 1u << 5;    // Skylighting's occlusion map
	constexpr std::uint32_t kViewPrecipOccluder = 1u << 6; // the precipitation mask
	constexpr std::uint32_t kViewReflection = 1u << 7;     // the water reflection's faces
	constexpr std::uint32_t kViewDecal = 1u << 8;          // a decal: the colour segment's fixed ordinal slots only
	constexpr std::uint32_t kViewForward = 1u << 9;        // a forward object: the colour segment's forward range only
	constexpr std::uint32_t kViewRefraction = 1u << 10;    // a refraction object (kRasterRefraction): the refraction range only
	constexpr std::uint32_t kViewFirstPerson = 1u << 11;   // a first-person member (kSetFirstPerson): the first-person view only
	// The main segments' views of the one main list: depth culls every candidate; colour draws members and every decal's slot.
	constexpr std::uint32_t kDepthViewBits = kViewMainCull;
	constexpr std::uint32_t kColourViewBits = kViewMain | kViewDecal | kViewForward | kViewRefraction;
	static_assert(offsetof(DrawInput, view) == 16 && offsetof(DrawInput, shadowRow) == 52 && offsetof(DrawInput, fadeDistance) == 60);
	// The main sequence buffer's ranges: phase 1 and colour draws, phase 2's (fixed: the CPU records where its draw starts), then
	// one per decal group. A decal goes to its ordinal's slot in the engine's draw order (SceneTables::decalOrdinal), so decals
	// keep that order; a culled one has zero indices.
	constexpr std::uint32_t kDecalGroups = 3;
	/** @brief The decal ranges (ObjectDecalGroup - 1) in the order the second pass draws them: the engine's geometry groups 3, 2, 4. */
	constexpr std::array<std::uint32_t, kDecalGroups> kDecalDrawOrder{ 0, 2, 1 };
	/** @brief The count word a decal range's draw reads its slot count from. */
	constexpr std::uint32_t DecalCountWord(std::uint32_t a_range) { return a_range < 2 ? kCountDecalGroupWord + a_range : kCountDecalLayerWord; }
	/** @brief The main sequence buffer's slots for a_draws per draw range and a_decals per decal group. */
	constexpr std::uint64_t SequenceSlots(std::uint32_t a_draws, std::uint32_t a_decals) { return 2ull * a_draws + std::uint64_t(kDecalGroups) * a_decals; }
	// The terrain groups' colour draws reuse phase 2's range (the depth segment is done with it): group 1 from its start, group 2
	// after group 1's capacity (MainLayout::terrainCapacity).
	constexpr std::uint32_t kTerrainGroups = 2;
	// The forward range, after the terrain ranges: a staging half BuildDraws appends visible forward draws to (view depth in the
	// unexecuted firstInstance word), then a sorted half ForwardSortCS fills back to front. Each MainLayout::forwardCapacity.
	constexpr const char* kForwardSortShader = "DrawcallLimitFix/ForwardSortCS.hlsl";
	// The refraction range, after the forward halves: pooled sequences (StoreShadowSequence's) drawn into the refraction normals by
	// RefractionDrawPass with one plain indexed indirect draw.
	// BuildDrawsLatch::bucketMapOffset's entries: a terrain group 1 slot's bucket (PlanZBuckets), whose twin (the next bucket) takes
	// the slot's near blocks (BuildDrawsLatch::terrainNear).
	constexpr std::uint32_t kBucketTerrainPair = 1u << 31;

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
		std::uint32_t nextPartition;  // GeometryBuffers::nextPartition
	};
#pragma pack(pop)
	static_assert(sizeof(GeometryDraw) == 40);

	// BuildDrawsCS's push constants: only what is fixed for a pass across executions. Per-execution values are in its
	// BuildDrawsLatch, so the recorded dispatch never changes with the frame.
	struct BuildDrawsConstants
	{
		std::uint32_t latchIndex;        // the latch block's SRV
		std::uint32_t sequencesIndex;
		std::uint32_t countIndex;
		// The strides of the rows an input's y names (BuildDrawsCS.hlsl, RowsOf): the material rows (y's low 20 bits) and, below,
		// the pipeline rows (its high 12; stride 0 for the shadow views). Their addresses are the payload's, in the latch.
		std::uint32_t materialRowStride;
		std::uint32_t phaseBits;         // the culling phase in bits 4-7 (CullFlags' phase field)
		std::uint32_t visibilityIndex;   // RWByteAddressBuffer: one uint per object in the tables
		std::uint32_t latchOffset;       // set at record time: the slot's region plus the dispatch's latch
		std::uint32_t frustumIndex;      // RWByteAddressBuffer: per object, the stamp of its last in-frustum frame (depth phase 1 only)
		std::uint32_t hzbIndex;        // 0 when there is no HZB to test against
		std::uint32_t hzbSizePacked;   // mip 0: width in the low 16 bits, height in the high 16
		std::uint32_t hzbMips;
		// The sort by pipeline (DrawSort). 0 when appending into the sequences directly (phase 2, or the sort off).
		std::uint32_t sortCountsIndex;
		std::uint32_t sortStagingIndex;
		std::uint32_t sortRanksIndex;
		// Sequence buffer ranges, in sequences: phase 2's first slot, the first decal slot, each decal group's range.
		std::uint32_t phaseTwoBase;
		std::uint32_t decalBase;
		std::uint32_t decalStride;
		std::uint32_t pipelineRowStride;
		// Depth phase 1 and shadow views: the fade roots' static rows. 0 elsewhere.
		std::uint32_t fadeRootsIndex;
		// Depth phase 1: the portal programs (kPortal*), tested after the frustum. 0 elsewhere.
		std::uint32_t portalProgramsIndex;
		// A shadow view's bucket counts, or the depth phases' (a word per pipeline slot). 0 elsewhere.
		std::uint32_t bucketCountsIndex;
		// Shadow views and the depth segment: each geometry slot's first index in the index pool (IndexPool::firsts). 0 elsewhere.
		std::uint32_t poolFirstsIndex;
		std::uint32_t rootProgramsIndex;
		// Colour segment: the forward staging half. 0 capacity elsewhere.
		std::uint32_t forwardBase;
		std::uint32_t forwardCapacity;
		// Colour segment: the refraction range's capacity (first slot forwardBase + 2 * forwardCapacity). 0 elsewhere.
		std::uint32_t refractionCapacity;
	};
	static_assert(sizeof(BuildDrawsConstants) == 104);
	constexpr std::uint32_t kBuildDrawsConstantWords = sizeof(BuildDrawsConstants) / 4;

	/**
	 * @brief One BuildDrawsCS dispatch's values for one execution, in its slot of a latch block (org::LatchBlock): written by the
	 * render thread in the epoch; the dispatch takes its group count from the first three words (ExecuteIndirect with a dispatch
	 * signature).
	 */
	struct BuildDrawsLatch
	{
		std::uint32_t dispatch[3];      // groups x, y, z
		std::uint32_t drawCount;        // inputs to cull
		std::uint32_t cullFlags;        // mode in bits 0-3, NoNearPlane at 9 (the phase is pushed)
		std::uint32_t visibilityStamp;  // marks the verdicts as this frame's; see BuildDrawsCS.hlsl
		std::uint32_t hzbUvScalePacked; // rendered area over the area the HZB covers, 16-bit fixed point
		std::uint32_t cullPlaneMask;    // which of cullPlanes are tested; 0 for the main camera
		// Row-major, as the shader's float4x4 with mul(M, v), camera translation folded in so an absolute world position projects
		// directly.
		float viewProj[16];
		// A shadow view's caster volume as the engine culls it: its culling process's custom planes
		// (NiCullingProcess::customCullPlanes), absolute world space, (normal, constant), inside where dot(normal, p) - constant >= 0.
		float cullPlanes[6][4];
		// A shadow view's pipeline map row (bytes into the latch block): each key slot's pipeline under its state. 0: main camera.
		std::uint32_t pipelineMapOffset;
		// Colour pass: kSunTestOn to test kObjectSunTest inputs against the cascades (a miss marks kObjectSunMiss).
		std::uint32_t sunState;
		// Depth segment: BSTreeNode::OnVisible's height test (base, limit; +infinity when off).
		float treeHeight[2];
		// Depth segment: the camera's position and LOD factor (NiCamera +0x184), as BSFadeNode::OnVisible measures distance.
		float fadeEye[4];
		// Colour pass: the sun cascades (a SunRegion of SunAccumulation::GpuCascade), bytes into the latch block.
		std::uint32_t sunCascadeOffset;
		// A sun view (kCullSunEntry): the full-frustum processes (SunRegion of SunEntryProcess); an input whose entry sphere is
		// outside every one is no sun caster. 0: no entry test.
		std::uint32_t sunEntryOffset;
		// Colour pass: the local shadow lights (SunRegion of GpuShadowVolume); BuildDraws writes each input's Light Limit Fix shadow
		// mask into its object word (kObjectLocalShadowShift) for ShadowBitMask. 0 elsewhere.
		std::uint32_t localShadowOffset;
		// A shadow view: its bucket table (first, capacity per bucket), each bucket drawn by one plain indirect draw. 0 elsewhere.
		std::uint32_t bucketTableOffset;
		// Depth phase 1 and shadow views: FadeStateCS's states published the frame before (a per-frame descriptor). 0 elsewhere.
		std::uint32_t fadeStatesIndex;
		// Depth phase 2: its bucket table (MainLatchLayout::PhaseTwoBucketTableOffset). 0 elsewhere.
		std::uint32_t phaseTwoBucketTableOffset;
		// Depth phases: each pipeline slot's Z-prepass bucket (kNoBucket: none); a reflection face: each LOD slot's. 0 elsewhere.
		std::uint32_t bucketMapOffset;
		// The frame's placement rows (BindlessPlacement: bound, sun entry, fade node): FrameValues' pooled backing (PlacementsIndex).
		std::uint32_t placementsIndex;
		// The installed publication's payload (PayloadVersions, PayloadLatch): the scene list, the geometry table, and the rows an
		// input's y names (the shadow rows for a shadow or occlusion view, whose pipeline rows are 0).
		std::uint32_t inputsIndex;
		std::uint32_t geometriesIndex;
		// The view types this dispatch draws (kView*): an input whose mask has none of them is not this view's. 0: no test.
		std::uint32_t viewBits;
		// A focus view: its target's fade root slot + 1, which an input's root word (+48) must name. 0: no test.
		std::uint32_t focusRoot;
		std::uint32_t materialRowsLo, materialRowsHi, pipelineRowsLo, pipelineRowsHi;
		// The frame's fade seeds (FadeSeedRow): a root's node values and state until its first FadeStateCS update.
		std::uint32_t fadeSeedsIndex;
		// Colour segment: the terrain groups' capacities past phase 2's base. 0 elsewhere.
		std::uint32_t terrainCapacity[2];
		// The depth segment: the stage word of the feature stage before its build that asks for one (Api::GpuStage::latchWord, at byte 300;
		// e.g. the texture transforms' frame-value rows, which their stage stores into the material rows above). 0 elsewhere (and the word
		// keeps terrainNear's 16-byte alignment). One word: a second stage needing one grows this latch (the shaders read it by byte offset).
		std::uint32_t stageWord;
		// Depth segment under Terrain Blending: its eye and near distance in w. A terrain group 1 draw within it goes to its slot's
		// twin bucket (kBucketTerrainPair), drawn into the terrain depth and, offset, the main depth. w 0: no test.
		float terrainNear[4];
	};
	// BuildDrawsLatch::focusRoot for a target that is no listed root: no input's root word is its - 1.
	constexpr std::uint32_t kFocusRootNone = 0xFFFFFFFFu;
	static_assert(sizeof(BuildDrawsLatch) == 320 && offsetof(BuildDrawsLatch, terrainNear) == 304 && offsetof(BuildDrawsLatch, terrainCapacity) == 292 && offsetof(BuildDrawsLatch, focusRoot) == 268 && offsetof(BuildDrawsLatch, fadeSeedsIndex) == 288 && offsetof(BuildDrawsLatch, inputsIndex) == 256 &&
				  offsetof(BuildDrawsLatch, viewBits) == 264 && offsetof(BuildDrawsLatch, materialRowsLo) == 272 &&
				  offsetof(BuildDrawsLatch, viewProj) == 32 && offsetof(BuildDrawsLatch, cullPlanes) == 96 &&
				  offsetof(BuildDrawsLatch, pipelineMapOffset) == 192 && offsetof(BuildDrawsLatch, sunState) == 196 &&
				  offsetof(BuildDrawsLatch, treeHeight) == 200 && offsetof(BuildDrawsLatch, fadeEye) == 208 &&
				  offsetof(BuildDrawsLatch, sunCascadeOffset) == 224 && offsetof(BuildDrawsLatch, sunEntryOffset) == 228 &&
				  offsetof(BuildDrawsLatch, localShadowOffset) == 232 && offsetof(BuildDrawsLatch, bucketTableOffset) == 236 &&
				  offsetof(BuildDrawsLatch, fadeStatesIndex) == 240 && offsetof(BuildDrawsLatch, phaseTwoBucketTableOffset) == 244 &&
				  offsetof(BuildDrawsLatch, bucketMapOffset) == 248 && offsetof(BuildDrawsLatch, placementsIndex) == 252 &&
				  offsetof(BuildDrawsLatch, materialRowsHi) == 276 && offsetof(BuildDrawsLatch, stageWord) == 300);  // the stage's shader
	// BuildDrawsLatch::bucketMapOffset: a pipeline slot without a bucket (BuildDrawsCS's kNoPipeline).
	constexpr std::uint32_t kNoBucket = 0xFFFFFFFFu;
	// A first-person forward slot's map word, its range's table entry in the low bits (BuildDrawsCS's kBucketForward).
	constexpr std::uint32_t kFirstPersonForwardBucket = 0x40000000u;
	constexpr std::uint32_t kSunTestOn = 1u << 31;

	/**
	 * @brief A sun plane set in a latch block (a SunRegion: a 16-byte count header, then records): the full-frustum processes here,
	 * the cascades (SunAccumulation::GpuCascade) in the main block. The engine bounds neither, so they grow.
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

	/** @brief The sun's entry rule as BuildDrawsCS applies it: outside every process. */
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

	/** @brief SunAccumulation's sphere test against one cascade (as BuildDrawsCS does it). */
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
	 * @brief The shadow latch block's region per frame slot: the view slots' latches, a pipeline map row per render mode and view
	 * rasterizer state (DrawPipelines::ShadowRasterStateId, 1 to rasterStates), then the sun's full-frustum processes. Each
	 * dimension is grown before the epoch (Impl::ReserveShadowLatch).
	 */
	struct ShadowLatchLayout
	{
		std::uint32_t viewSlots = 0, keySlots = 0, rasterStates = 0, sunProcesses = 0;
		std::uint32_t MapOffset() const { return viewSlots * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t MapRowBytes() const { return keySlots * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t MapRows() const { return kShadowModeCount * rasterStates; }
		/** @brief A view's map row: its mode's rows, then its state's (1 to rasterStates). */
		std::uint32_t MapRowOf(std::uint32_t a_mode, std::uint32_t a_state) const { return a_mode * rasterStates + (a_state - 1); }
		std::uint32_t SunEntryOffset() const { return MapOffset() + MapRows() * MapRowBytes(); }
		// A view slot's bucket table (BuildDrawsLatch::bucketTableOffset): (first, capacity) per bucket, at most one per key slot.
		std::uint32_t BucketTableBytes() const { return keySlots * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t BucketOffset(std::uint32_t a_slot) const
		{
			return SunEntryOffset() + SunRegionBytes<SunEntryProcess>(sunProcesses) + a_slot * BucketTableBytes();
		}
		// The index pool's copies (IndexPool): their dispatch's groups, then their count.
		std::uint32_t PoolOffset() const { return BucketOffset(viewSlots); }
		// A view slot's blocks, copied to the view-blocks buffer by LatchedCopiesPass (the commit records no copy).
		std::uint32_t ViewBlockOffset(std::uint32_t a_slot) const
		{
			const std::uint32_t first = (PoolOffset() + 4 * static_cast<std::uint32_t>(sizeof(std::uint32_t)) + 255u) & ~255u;
			return first + a_slot * static_cast<std::uint32_t>(kShadowViewSlotBytes);
		}
		std::uint32_t Bytes() const { return ViewBlockOffset(viewSlots); }
		bool operator==(const ShadowLatchLayout&) const = default;
	};
	/**
	 * @brief One cull volume of a local shadow light (LocalShadowLights::Light::Volume) as BuildDraws tests it: the light's sphere,
	 * then a shadowmap descriptor's frustum and custom planes. A light of several descriptors has a record each, same mask bit.
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

	/**
	 * @brief The main latch block's region per frame slot: the passes' BuildDrawsLatch, then the colour pass's cascades and local
	 * shadow volumes, the Z-prepass's bucket tables and map, and the index pool's copies' dispatch.
	 */
	struct MainLatchLayout
	{
		std::uint32_t cascades = 0;
		std::uint32_t shadowVolumes = 0;
		// Z-prepass buckets: one per group of pipeline slots sharing a depth pipeline (IndirectState::zGroups), in each phase's
		// table (first, capacity), and the slots' map to them.
		std::uint32_t buckets = 0;
		static constexpr std::uint32_t CascadeOffset() { return static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t ShadowVolumeOffset() const { return CascadeOffset() + SunRegionBytes<SunAccumulation::GpuCascade>(cascades); }
		std::uint32_t BucketTableOffset() const { return ShadowVolumeOffset() + SunRegionBytes<GpuShadowVolume>(shadowVolumes); }
		std::uint32_t PhaseTwoBucketTableOffset() const { return BucketTableOffset() + buckets * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t BucketMapOffset() const { return PhaseTwoBucketTableOffset() + buckets * 2 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		// The index pool's copies' dispatch (IndexPool), as in the shadow latch, for the depth commit's pool update.
		std::uint32_t PoolOffset() const { return BucketMapOffset() + buckets * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
		std::uint32_t Bytes() const { return PoolOffset() + 4 * static_cast<std::uint32_t>(sizeof(std::uint32_t)); }
	};
	// cullFlags: a clamped shadow view (0xE) pancakes casters in front of its near plane onto it (Utility.hlsl:
	// RENDER_SHADOWMAP_CLAMPED), so the near plane rejects nothing.
	constexpr std::uint32_t kCullNoNearPlane = 0x200;
	// cullFlags: which caster class a shadow view draws: the volumetric copy's views only volumetric-only casters, other views of a
	// class-split mode the rest. Neither for the main camera, paraboloids or occlusion maps.
	constexpr std::uint32_t kCullCastersOnly = 0x400;
	constexpr std::uint32_t kCullVolumetricOnly = 0x800;
	// cullFlags: a sun view (cascades and their volumetric copies): applies the sun's entry rule (sunEntryOffset).
	constexpr std::uint32_t kCullSunEntry = 0x1000;
	// cullFlags: Skylighting's size test (Skylighting::OcclusionTechnique) for its occlusion map: bound radius <= 32 draws nothing.
	constexpr std::uint32_t kCullMinRadius = 0x2000;
	// cullFlags: occlusion map view: an occluder under a fade root BSFadeNode::OnVisible stops at without cameraRelatedUpdates
	// (Precipitation::SetupMask's cull; FadeStateCS's state) draws nothing, in place of the shadow views' fading test. Set while the
	// engine's fades are on (0x142032dfd).
	constexpr std::uint32_t kCullFadeOnVisible = 0x4000;
	// The key word a view reads (BuildDrawsCS: KeyWord), bits 16-17: 0 pipeline word (main, reflection); 1 shadowKey; 2 and 3
	// occlusionKey[0], [1].
	constexpr std::uint32_t kCullKeyShift = 16;
	constexpr std::uint32_t kCullKeyCaster = 1u << kCullKeyShift;
}
