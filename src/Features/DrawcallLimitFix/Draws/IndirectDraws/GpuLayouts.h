#pragma once

// IndirectDraws' capacities and the layouts the GPU reads, which must match the HLSL (BuildDrawsCS, HzbCS,
// Lighting.hlsl's DCLF paths). Included by Internal.h, after the headers these declarations use.

namespace DCLF::Draws
{
	constexpr const char* kExtensionId = "cs.dclf.main-opaque";
	constexpr const char* kShadowExtensionId = "cs.dclf.shadow";
	// The shadow views' material rows (ShadowMaterialRow): row 0 for every draw without alpha testing, and one per material
	// of the alpha-tested casters (their texture offset and diffuse). One table for every view, grown when the kept state
	// needs more rows (GrowableRows), from this many.
	constexpr std::uint32_t kShadowMaterialRowsInitial = 256;
	// The views one frame's shadow epoch can hold: the exterior has four (two cascades, twice); an
	// interior with several shadow-casting point lights has two hemispheres per light.
	constexpr std::uint32_t kMaxShadowViews = 16;
	// Render modes 0xD plain, 0xE clamped, 0xF paraboloid, and kSkyMode: Skylighting's occlusion map (render mode 0x1C),
	// whose occluders the frame's shadow build lists too and whose own epoch draws them (ExecuteSkyOcclusion).
	constexpr std::uint32_t kShadowModeCount = 4;
	constexpr std::uint32_t kSkyMode = 3;
	constexpr std::uint32_t kSkyRenderMode = 0x1C;
	// The view slot Skylighting's occlusion map draws through; the shadow views take the others.
	constexpr std::uint32_t kSkySlot = kMaxShadowViews - 1;
	// Key slots a shadow epoch can name (Lookups::shadowSlotKeys): one per caster key and render mode.
	constexpr std::uint32_t kMaxShadowSlots = 1024;
	constexpr std::uint64_t kShadowConstantBytes = 1ull << 20;
	// Fixed slots at the head of the shadow constants: the view's Utility PerTechnique block and its
	// VS_PerFrame block, rewritten per view so that the records built once a frame can point at them.
	// The arena's head holds one slot per view: its PerTechnique block (b0) then its VS_PerFrame copy (b12).
	constexpr std::uint64_t kShadowPerFrameOffset = 256;
	constexpr std::uint64_t kShadowViewSlotBytes = 256 + 1024;
	// After the view slots: the frame record (the shadow draws' textures and samplers, DrawBindings), which the views' push
	// data names, then the arena's blocks (the zero block, SharedData, FeatureData).
	constexpr std::uint64_t kShadowFrameRecordOffset = kShadowViewSlotBytes * kMaxShadowViews;
	constexpr std::uint64_t kShadowArenaBlocksOffset = kShadowFrameRecordOffset + 1024;
	// [0] kSHADOWMAPS_ESRAM (cascades, spot lights), [1] kSHADOWMAPS (point and focus lights), and
	// [2] kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM, the volumetric lighting copy: the engine's second draw of
	// each cascade's accumulator, with flag 0x100, draws batch group 15 alone (FUN_1414b44f0) - the passes
	// registered with accumulation hint 8, which are the volumetric-only casters
	// (ShadowReject::VolumetricOnly). DCLF's views of it draw those casters alone (kCullVolumetricOnly).
	// See CaptureShadowView.
	// [3] depth target 10 as Skylighting swaps it in: its own occlusion map (ExecuteSkyOcclusion).
	constexpr std::uint32_t kShadowDepthTargets = 4;
	constexpr std::uint32_t kSkyDepthTarget = 3;
	constexpr std::uint32_t kMaxDraws = 16384;
	constexpr std::uint64_t kConstantBytes = 48ull << 20;
	// The Z-prepass segment's own constants buffer: its blocks are the colour segment's pipelines' and pairs' (a few MB).
	constexpr std::uint64_t kDepthConstantBytes = 16ull << 20;
	constexpr std::uint64_t kConstantAlignment = 256;  // uniform buffer address alignment (conservative)
	constexpr std::uint32_t kColorTargets = 8;
	constexpr std::uint32_t kMaxGeometries = kMaxDraws;
	// The per-object record table (ObjectRecord) is indexed by a table index, not a draw index, so it
	// covers every candidate the frame tracks rather than only the ones drawn.
	constexpr std::uint32_t kMaxObjects = 32768;
	// Draw inputs and the visibility buffer are sized by the table, not by the draw count: the depth
	// segment submits a cull-only input for every candidate it may not draw, and BuildDraws indexes the
	// visibility word by the object's TABLE index. Sizing either by kMaxDraws is an overrun waiting for
	// a cell with more than kMaxDraws tracked objects.
	constexpr std::uint32_t kMaxInputs = kMaxObjects;
	// Distinct binding records an epoch may hold, and the size of the buffer behind them. Deduplicated
	// they are one per (material, pipeline) pair: 85 behind 727 candidates in the Bannered Mare, 105
	// behind 1616 in Dragonsreach. 2048 is about twenty times the worst seen, and 1.6 MB against the
	// 13.1 MB a record per draw would need: everything per object is in the object record (BindlessObject).
	constexpr std::uint32_t kMaxRecordsDeduplicated = 2048;
	// The 32-bit record offset BuildDraws computes (input.y * RecordStride) has to address all of it.
	static_assert(std::uint64_t(kMaxDraws) * sizeof(DrawBindings) < (std::uint64_t(1) << 32));
	constexpr std::uint32_t kNoRecord = ~0u;
	constexpr std::uint32_t kNoSkip = ~0u;
	// BuildDrawsCS's counter words: [0] drawn, [1] culled, [2] tested, [3] engine-culled and gated out,
	// [4] false negatives (the engine kept it, the culling rejected it), [5] rescued.
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
	// Byte offsets of the count words the indirect draws read; they must match BuildDrawsCS.hlsl.
	constexpr std::uint64_t kCountDrawnPhaseTwoBytes = 68;
	constexpr const char* kBuildDrawsShader = "DrawcallLimitFix/BuildDrawsCS.hlsl";
	constexpr const char* kHzbShader = "DrawcallLimitFix/HzbCS.hlsl";
	constexpr const char* kSortSequencesShader = "DrawcallLimitFix/SortSequencesCS.hlsl";
	// HzbCS.hlsl's constants: source, target, target size, source size, from-depth, padding.
	constexpr std::uint32_t kHzbConstantWords = 8;


	// BuildDrawsCS.hlsl's inputs (byte-address buffers).
	struct DrawInput
	{
		std::uint32_t pipelineIndex;  // in the pipeline sets; a shadow input's key slot (Lookups::shadowSlots)
		std::uint32_t recordIndex;    // DrawBindings record
		std::uint32_t geometryIndex;  // GeometryDraw
		std::uint32_t flags;          // object flags, plus kInputDrawable for this epoch
		float boundCentre[3];         // absolute world space; the camera is folded into the matrix
		float boundRadius;
		// Index into the frame's object table. The two segments emit different subsets in different
		// orders, so this is what lets the colour segment look up the visibility the depth segment
		// published for the same object.
		std::uint32_t objectIndex;
		std::uint32_t decalOrdinal;  // decals only: the slot in the group's range
		// Skins of several partitions: bit i draws partition i (Tables::skinPartitions); 0 draws the one
		// geometry. Left 0 by every input that is not such a skin.
		std::uint32_t partitions;
		// A shadow input's second vertex stream: the GeometryDraw holding a face shape's positions (the shadow
		// payload appends them after the geometry slots), or ~0u. The main pass never has one.
		std::uint32_t streamIndex = ~0u;
		// kObjectFadeTest (the depth segment's inputs): the entry root's centre and the fade-out distance
		// (SceneStore::Tables::fadeDistance). Zero on every other input.
		float fade[4] = {};
	};
	static_assert(sizeof(DrawInput) == 64);
	// BuildDrawsCS.hlsl: set on an input the epoch has built a bindings record for. The depth segment
	// submits an input for every candidate so the culling covers them all, but builds records only for
	// the ones it may draw.
	constexpr std::uint32_t kInputDrawable = 1u << 16;
	// A shadow input whose entry is outside the sun's full-frustum processes (OutsideSunEntry): the sun's views
	// skip it (kCullSunEntry); a spot light's views, which share the mode's inputs, do not.
	constexpr std::uint32_t kInputOutsideSunEntry = 1u << 25;
	// Where phase 2 appends its sequences; see BuildDrawsCS.hlsl.
	constexpr std::uint32_t kPhaseTwoSequenceBase = kMaxDraws;
	// The decal ranges: one of kMaxDecalDraws fixed slots per group after phase 2's range. A decal's
	// sequence goes to the slot of its ordinal in the engine's draw order (SceneStore::Tables::
	// decalOrdinal), so the second pass draws decals in that order every frame; a culled one is the
	// same sequence with an index count of zero.
	constexpr std::uint32_t kMaxDecalDraws = 2048;
	// The bones buffer (VS t126): every skinned object's palette rows, current then previous, per epoch.
	// 131,072 float4 rows is 2 MB: each skin keeps its block (SceneStore PlaceBones), so there are holes; the exterior needs
	// ~13,500-18,000 current rows.
	constexpr std::uint32_t kMaxBoneRows = 131072;
	constexpr std::uint32_t kDecalGroups = 2;
	constexpr std::uint32_t kDecalSequenceBase = 2 * kMaxDraws;
	constexpr std::uint32_t kSequenceSlots = kDecalSequenceBase + kDecalGroups * kMaxDecalDraws;

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
		std::uint32_t recordsAddressLo;
		std::uint32_t recordsAddressHi;
		std::uint32_t recordStride;
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
	};
	static_assert(sizeof(BuildDrawsConstants) == 72);
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
		std::uint32_t cullFlags;        // mode in bits 0-3, RequireNativeVisible at 8, NoNearPlane at 9 (the phase is pushed)
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
		// The main colour pass only: kSunTestOn and the number of cascades below, which BuildDraws tests every
		// kObjectSunTest input's bound against (a miss marks the draw kObjectSunMiss). 0 elsewhere.
		std::uint32_t sunState;
		// The depth segment: BSTreeNode::OnVisible's height test for the kObjectHeightTest inputs, the base and the
		// limit (PrimaryCull::TreeHeightTest; +infinity when the test is off). 0 elsewhere.
		float treeHeight[2];
		// Per cascade: the active masks of its planes and its custom planes (0: none), then 6 planes and 6 custom
		// planes as (normal, constant), outside when dot(normal, c) - constant < -r (SunAccumulation's test).
		std::uint32_t sunMasks[4][2];
		float sunPlanes[4][12][4];
		// The depth segment: the main camera's position and LOD factor (NiCamera +0x184), as BSFadeNode::OnVisible
		// measures a fade root's distance (PrimaryCull::FadeEye), for the kObjectFadeTest inputs. 0 elsewhere.
		float fadeEye[4];
		// A view of the sun (kCullSunEntry): the frame's full-frustum culling processes (ShadowInputs::sunEntryPlanes), each
		// 6 planes with its active mask. An input whose entry sphere (its fade row, SetSunEntryRow) is outside every one is
		// none of the sun's casters. A count of 0 leaves the verdict to the input's kInputOutsideSunEntry.
		std::uint32_t sunEntryCount;
		std::uint32_t sunEntryMasks[8];
		std::uint32_t sunEntryPad[3];
		float sunEntryPlanes[8][6][4];
		std::uint8_t reserved[208];
	};
	static_assert(sizeof(BuildDrawsLatch) == 2048 && offsetof(BuildDrawsLatch, viewProj) == 32 && offsetof(BuildDrawsLatch, cullPlanes) == 96 &&
				  offsetof(BuildDrawsLatch, pipelineMapOffset) == 192 && offsetof(BuildDrawsLatch, sunState) == 196 &&
				  offsetof(BuildDrawsLatch, sunMasks) == 208 && offsetof(BuildDrawsLatch, sunPlanes) == 240 && offsetof(BuildDrawsLatch, fadeEye) == 1008 &&
				  offsetof(BuildDrawsLatch, sunEntryCount) == 1024 && offsetof(BuildDrawsLatch, sunEntryMasks) == 1028 &&
				  offsetof(BuildDrawsLatch, sunEntryPlanes) == 1072);

	/** @brief The sun's entry rule on the latch, as BuildDrawsCS applies it: outside every full-frustum process. */
	inline bool OutsideSunEntryLatch(const BuildDrawsLatch& a_latch, const float a_entry[4])
	{
		if (!a_latch.sunEntryCount)
			return false;
		for (std::uint32_t process = 0; process < a_latch.sunEntryCount; ++process) {
			bool outside = false;
			for (std::uint32_t p = 0; p < 6 && !outside; ++p) {
				if (!(a_latch.sunEntryMasks[process] & (1u << p)))
					continue;
				const float* plane = a_latch.sunEntryPlanes[process][p];
				outside = plane[0] * a_entry[0] + plane[1] * a_entry[1] + plane[2] * a_entry[2] - plane[3] < -a_entry[3];
			}
			if (!outside)
				return false;
		}
		return true;
	}
	constexpr std::uint32_t kSunTestOn = 1u << 31;

	/** @brief SunAccumulation's sphere test against one cascade of the latch (what BuildDrawsCS does). */
	inline bool InSunCascade(const BuildDrawsLatch& a_latch, std::uint32_t a_cascade, const float a_centre[3], float a_radius)
	{
		for (std::uint32_t set = 0; set < 2; ++set) {
			const std::uint32_t mask = a_latch.sunMasks[a_cascade][set];
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(mask & (1u << p)))
					continue;
				const float* plane = a_latch.sunPlanes[a_cascade][set * 6 + p];
				if (plane[0] * a_centre[0] + plane[1] * a_centre[1] + plane[2] * a_centre[2] - plane[3] < -a_radius)
					return false;
			}
		}
		return true;
	}
	// The shadow latch block: the views' latches, then one pipeline map row per view rasterizer state.
	constexpr std::uint32_t kShadowPipelineMapOffset = kMaxShadowViews * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
	constexpr std::uint32_t kShadowPipelineMapRowBytes = kMaxShadowSlots * static_cast<std::uint32_t>(sizeof(std::uint32_t));
	constexpr std::uint32_t kShadowLatchBytes = kShadowPipelineMapOffset + DrawPipelines::kMaxShadowRasterStates * kShadowPipelineMapRowBytes;
	// cullFlags: a clamped shadow view (0xE) pancakes casters in front of its near plane onto it
	// (Utility.hlsl: RENDER_SHADOWMAP_CLAMPED), so the near plane rejects nothing there.
	constexpr std::uint32_t kCullNoNearPlane = 0x200;
	// cullFlags: which caster class a shadow view draws (kObjectVolumetricOnly). The mode's inputs hold both
	// classes; a view of the volumetric lighting copy draws the volumetric-only casters alone, every other
	// shadow view everything else. Neither is set for the main camera.
	constexpr std::uint32_t kCullCastersOnly = 0x400;
	constexpr std::uint32_t kCullVolumetricOnly = 0x800;
	// cullFlags: a view of the sun (its cascades and their volumetric copies), which skips kInputOutsideSunEntry.
	constexpr std::uint32_t kCullSunEntry = 0x1000;
}
