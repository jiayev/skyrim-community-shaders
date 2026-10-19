// Drawcall Limit Fix: culls this frame's draw inputs and writes their indirect draw sequences (DrawSequence, Records.h) from the
// inputs and the geometry table. Compiled to SPIR-V at runtime (ComputeProgram) with BasicRHI's descriptor-heap ABI.

#include "DrawcallLimitFix/HzbTest.hlsli"
#include "DrawcallLimitFix/PortalPrograms.hlsli"
#include "DrawcallLimitFix/PortalQueries.hlsli"

// Push constants: only what is fixed for a pass across executions. Everything per-execution is read from the pass's region of
// the latch block (org::LatchBlock), written just before submission, so recorded commands can be reused or recorded ahead.
// Must match BuildDrawsConstants and BuildDrawsLatch in Core/GpuLayouts.h.
cbuffer BuildDrawsConstants : register(b0)
{
	uint LatchIndex;      // ByteAddressBuffer: the latch block
	uint SequencesIndex;  // RWByteAddressBuffer: DrawSequence[]
	uint CountIndex;      // RWByteAddressBuffer: uint (zeroed before the dispatch)
	// The strides of the rows an input's y names (RowsOf): material row (low 20 bits), pipeline row (high 12; stride 0 for the
	// shadow views). Their addresses are the payload's, in the latch.
	uint MaterialRowStride;
	// Bits 4-7: the pass's culling phase; the mode and other bits come from the latch.
	uint PhaseBits;
	// RWByteAddressBuffer: one uint per object, written by the depth segment's two phases and read by the colour segment, so the
	// colour pass draws exactly what the depth passes drew (one decision, not two that can disagree).
	uint VisibilityIndex;
	uint LatchOffset;  // this dispatch's BuildDrawsLatch, in bytes into the latch block
	// RWByteAddressBuffer: per object, the stamp of the last frame its bound was in the main frustum. Depth segment phase 1 only
	// (0 elsewhere), including the decal, forward and refraction inputs it otherwise skips: the visibility feedback
	// (VisibilityEventsCS).
	uint FrustumIndex;
	// Occlusion culling against the HZB built after the depth pass (HzbCS.hlsl); 0 when there is none (including the first frame).
	uint HzbIndex;
	uint HzbSizePacked;  // mip 0: width in the low 16 bits, height in the high 16
	uint HzbMips;
	// The sort by pipeline (DrawSort): with SortCountsIndex set, a sequence goes to the staging buffer with its rank among its
	// pipeline's sequences (an atomic per sort key: its state class's first key plus its index in the class); SortSequencesCS.hlsl
	// scatters it into its class's sub-range after the prefix sum. 0: append directly.
	uint SortCountsIndex;   // RWByteAddressBuffer: uint[kSortKeys], zero before the dispatch
	uint SortStagingIndex;  // RWByteAddressBuffer: DrawSequence[PhaseTwoBase]
	uint SortRanksIndex;    // RWByteAddressBuffer: uint[PhaseTwoBase]
	// Sequence buffer ranges, in sequences (sized before each epoch for every draw it can produce). Phase 2 has its own fixed range:
	// its draw is recorded on the CPU, which cannot know phase 1's count. The decal sub-ranges follow (one per decal group and state
	// class, where each starts the latch's class table says): DecalStride slots each, slot = the decal's ordinal in the engine's draw
	// order.
	uint PhaseTwoBase;
	uint DecalBase;
	uint DecalStride;
	uint PipelineRowStride;
	// StructuredBuffer<FadeRootStatic> (FadeStateCS.hlsl; Records.h). Phase 1 drops an input while its root's OnVisible stops; a
	// shadow view drops a caster while its root fades; every dispatch picks LOD partitions by the root's level (LodPartitions). Root
	// state is FadeStateCS's model, not the node's. 0 where unbound.
	uint FadeRootsIndex;
	// Shadow views and the colour segment: StructuredBuffer<uint> of each object's strict-light gate (the cull binding ObjectGates), which the
	// latch's StrictGatesIndex verdicts are of. 0 elsewhere.
	uint ObjectGatesIndex;
	// Shadow views and depth phases: RWByteAddressBuffer of bucket counts, a word per bucket (BucketTableOffset). 0 elsewhere.
	uint BucketCountsIndex;
	// Shadow views and depth segment: ByteAddressBuffer of each geometry slot's first index in the index pool, ~0 without one.
	uint PoolFirstsIndex;
	// Main query preparation: StructuredBuffer<uint> of each root's program. Draw construction reads MemberResultsIndex instead.
	uint RootProgramsIndex;
	// Colour segment: the forward range (its classes' staging and sorted halves: the class table's), and its slots in all; visible
	// forward draws append to their class's staging half with their view depth in firstInstance (ForwardSortCS.hlsl sorts them).
	uint ForwardBase;
	uint ForwardCapacity;
	// Colour segment: the refraction range (after the forward range's two halves); visible refraction draws append here, pooled
	// as a shadow view's, for the forward epoch's plain draw into the refraction normals.
	uint RefractionCapacity;
	// Colour segment: RWByteAddressBuffer of the state class sub-ranges' counts, a word per (class, range) (ClassCountWord). 0 elsewhere.
	uint ClassCountsIndex;
	uint MemberQueriesIndex;
	uint MemberResultsIndex;
	uint MemberClaimsIndex;
}

// StructuredBuffer<FadeNodeState>: FadeStateCS's states as the previous frame published them (latch fadeStatesIndex).
static uint FadeStatesIndex;
// StructuredBuffer<FadeRootStatic>: the frame's fade seeds (latch fadeSeedsIndex; FrameValues, two rows per slot).
static uint FadeSeedsIndex;

// FadeStateCS.hlsl's rows: must match Records.h's FadeNodeState and FadeRootStatic.
struct FadeNodeState
{
	uint Flags;
	float CurrentFade;
	float SnapRadius;
	int LastVisible;
	float AmountFade;
	float Metric;
	float PreviousMetric;
	float Blend;
	uint Levels;
	uint Generation;
	uint Verdict;
	uint Frame;
};
struct FadeRootStatic
{
	FadeNodeState Initial;
	float Radius;
	float FadeAmount;
	float NearDistance;
	float FarDistance;
	uint Object;
	uint Bits;
	float LodScale;
	uint Generation;
};
static const uint kFadeVerdictServiced = 1u << 2;
static const uint kFadeVerdictDrawn = 1u << 3;
static const uint kFadeFlagSettled = 1u << 15;

// A root's state until FadeStateCS's first update of its generation: its seed (FrameValues, FadeSeedRow), else the row's.
FadeNodeState SeedState(uint a_root, FadeRootStatic a_row)
{
	if (FadeSeedsIndex != 0) {
		StructuredBuffer<FadeRootStatic> seeds = ResourceDescriptorHeap[FadeSeedsIndex];
		uint count, stride;
		seeds.GetDimensions(count, stride);
		const uint at = 2u * a_root + ((a_row.Bits & (1u << 23)) != 0u ? 1u : 0u);
		if (at < count && seeds[at].Generation == a_row.Generation)
			return seeds[at].Initial;
	}
	return a_row.Initial;
}

// Whether a fade root slot (~0u: none) is not fully faded in.
bool RootFading(uint a_root)
{
	if (a_root == 0xFFFFFFFFu)
		return false;
	StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
	const FadeRootStatic row = fadeRoots[a_root];
	StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
	const FadeNodeState state = fadeStates[a_root];
	return (state.Generation == row.Generation ? state.CurrentFade : SeedState(a_root, row).CurrentFade) < 1.0;
}

// A LOD skin's partitions (DrawInput::lodPartitions, byte L = level L's mask) at its root's level (FUN_14147a430's level,
// +0x152 & 0xF), as GetRenderPasses gives every view's pass; a_partitions when there is no state or LOD.
//
// a_copy: while the root crosses levels (+0x153 & 0x70 not 0x20), GetRenderPasses' hint-10 copy: row L+1 less row L, drawn with
// the cross-fade factor (+0x14C) on its alpha. Its stencil test (mode 0xB: ref int(blend * 31) GREATER than the stencil, 0
// outside, room index + 1 inside) is taken as outside's: drawn while int(blend * 31) > 0. Main views only.
uint LodPartitions(uint a_partitions, uint a_lod, uint a_root, bool a_main, out uint a_copy)
{
	a_copy = 0;
	if (a_lod == 0 || a_root == 0xFFFFFFFFu || FadeRootsIndex == 0 || FadeStatesIndex == 0)
		return a_partitions;
	StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
	const FadeRootStatic row = fadeRoots[a_root];
	StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
	FadeNodeState state = fadeStates[a_root];
	if (state.Generation != row.Generation)
		state = SeedState(a_root, row);
	const uint level = min(state.Levels & 0xFu, 3u);
	const uint mask = (a_lod >> (8u * level)) & 0xFFu;
	if (a_main && ((state.Levels >> 8) & 0x70u) != 0x20u && level < 3 && int(state.Blend * 31.0) > 0)
		a_copy = ((a_lod >> (8u * (level + 1))) & 0xFFu) & ~mask;
	return mask == 0 && a_copy == 0 ? (1u << 8) : mask;  // kNoPartitions (declared below)
}

// Whether BSFadeNode::OnVisible stops at a fade root (~0u: none) for a culling process without cameraRelatedUpdates
// (Precipitation::SetupMask, Skylighting's occlusion map): an unsettled root recurses only while its fade (as the main camera's
// cull leaves it) is above 0 and its fadeAmount is not 0.
bool FadedOutOfOcclusion(uint a_root)
{
	if (a_root == 0xFFFFFFFFu)
		return false;
	StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
	const FadeRootStatic row = fadeRoots[a_root];
	StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
	FadeNodeState state = fadeStates[a_root];
	if (state.Generation != row.Generation)
		state = SeedState(a_root, row);
	if ((state.Flags & kFadeFlagSettled) != 0 && row.FadeAmount == 1.0 && state.CurrentFade == 1.0)
		return false;
	return !(state.CurrentFade > 0.0) || row.FadeAmount == 0.0;
}

// The execution's values, from the latch (BuildDrawsLatch), read at the top of main. Its first three words are this dispatch's
// indirect arguments.
static uint DrawCount;
// Bits 0-3 mode (0 off, 1 frustum, 2 frustum then HZB), 4-7 phase (push constants), bit 9 NoNearPlane.
static uint CullFlags;
// Stamps this frame's verdicts. A word without the current stamp is treated as visible by the colour segment: a needless draw
// fails its EQUAL test, while a stale "hidden" would drop an object that has depth and nothing else to shade it.
static uint VisibilityStamp;
// The HZB covers a power-of-two area larger than the image: this is the image-to-HZB UV ratio per axis, 16-bit fixed point.
// Without it the test samples the far-plane padding and the HZB looks empty.
static uint HzbUvScalePacked;
// The main pass's view-projection with the camera translation folded in: absolute world positions project directly.
static float4x4 ViewProj;
// A shadow view's caster volume (NiCullingProcess::customCullPlanes): absolute world (normal, constant), inside where
// dot(normal, p) - constant >= 0. CullPlaneMask selects the tested planes; 0 for the main camera.
static uint CullPlaneMask;
static float4 CullPlanes[6];
// A shadow view's pipeline map row (latch bytes): its inputs name key slots, the row gives each slot's pipeline under the view's
// rasterizer state. 0 when inputs name pipelines themselves (main camera).
static uint PipelineMapOffset;
// Main colour pass: kSunTestOn when the sun test applies, and the cascades' latch offset (a count, then per cascade two plane
// masks and 12 planes: GpuLayouts.h, GpuCascade).
static uint SunState;
static const uint kSunTestOn = 0x80000000u;
static uint SunCascadeOffset;
// A sun view: the full-frustum processes' latch offset (a count, then per process a plane mask and 6 planes: SunEntryProcess),
// shared by the slot's sun views. 0: no entry test.
static uint SunEntryOffset;
static const uint kSunRegionHeader = 16;
static const uint kSunCascadeBytes = 208;
static const uint kSunEntryProcessBytes = 112;
// Colour pass: local shadow lights' latch offset (a count, then per volume two plane masks, mask bit, landscape flag, sphere and
// 12 planes: GpuShadowVolume). 0: none tested.
static uint LocalShadowOffset;
static const uint kShadowVolumeBytes = 240;
// Depth segment: the main camera's position and LOD factor (kObjectFadeTest).
static float4 FadeEye;
// Depth segment: the tree height test's base and limit (kObjectHeightTest); limit +infinity when off.
static float2 TreeHeight;
// A shadow view's bucket table (latch bytes): its map row names a bucket per key slot, the table gives each bucket its sequence
// range (first, capacity). 0 elsewhere.
static uint BucketTableOffset;
// Depth phases: each pipeline slot's bucket (latch bytes).
static uint BucketMapOffset;
// The view types this dispatch draws (kView*); 0: every input.
static uint ViewBits;
// A focus view: its target's fade root slot + 1, which the input's root word must name. 0: no test.
static uint FocusRoot;
// Depth segment while Terrain Blending holds the terrain: its eye, and in w its near distance (0: no terrain test).
static float4 TerrainNear;
// The state classes (GpuLayouts.h, kClassRangeMain): the colour segment's class table, and the class map (colour segment, a view's
// forward part), latch bytes. 0: none.
static uint ClassTableOffset;
static uint ClassMapOffset;
// The frame's light context atlas and strict gates (0: none), and a portal-strict point light's view's atlas entry + 1 (0: not gated).
static uint LightContextsIndex;
static uint StrictGatesIndex;
static uint StrictContext;

// The placement rows the culling reads (BindlessPlacement): world bound, sun entry sphere, fade node centre, and the forward
// sort's (nearest BSOrderedNode's centre, ordinal).
struct PlacementRows
{
	float4 rows[10];
};
static const uint kObjectBoundRow = 6;
static const uint kObjectSunEntryRow = 7;
static const uint kObjectFadeNodeRow = 8;
static const uint kObjectForwardSortRow = 9;
// StructuredBuffer of the frame's 160-byte placement rows (BindlessPlacement): an input's bound, sun entry and fade node are its
// object's row (a move rewrites the row, never the inputs).
static uint PlacementsIndex;
// The payload's buffers: the installed publication's PayloadVersions (the latch names them).
static uint InputsBuffer;
static uint GeometriesBuffer;
static uint MaterialRowsLo;
static uint MaterialRowsHi;
static uint PipelineRowsLo;
static uint PipelineRowsHi;
float4 ObjectRow(uint a_object, uint a_row)
{
	StructuredBuffer<PlacementRows> placements = ResourceDescriptorHeap[PlacementsIndex];
	return placements[a_object].rows[a_row];
}

void LoadLatch()
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint4 head = latch.Load4(LatchOffset + 12);  // after the dispatch arguments
	DrawCount = head.x;
	CullFlags = (head.y & ~0xF0u) | (PhaseBits & 0xF0u);
	VisibilityStamp = head.z;
	HzbUvScalePacked = head.w;
	CullPlaneMask = latch.Load(LatchOffset + 28);
	[unroll] for (uint p = 0; p < 6; ++p)
		CullPlanes[p] = asfloat(latch.Load4(LatchOffset + 96 + p * 16));
	ViewProj = float4x4(asfloat(latch.Load4(LatchOffset + 32)), asfloat(latch.Load4(LatchOffset + 48)),
		asfloat(latch.Load4(LatchOffset + 64)), asfloat(latch.Load4(LatchOffset + 80)));
	PipelineMapOffset = latch.Load(LatchOffset + 192);
	SunState = latch.Load(LatchOffset + 196);
	TreeHeight = asfloat(latch.Load2(LatchOffset + 200));
	FadeEye = asfloat(latch.Load4(LatchOffset + 208));
	const uint3 sunRegions = latch.Load3(LatchOffset + 224);
	SunCascadeOffset = sunRegions.x;
	SunEntryOffset = sunRegions.y;
	LocalShadowOffset = sunRegions.z;
	// Each depth phase has its own bucket table; they share the slot-to-bucket map.
	BucketTableOffset = latch.Load(LatchOffset + (((PhaseBits >> 4) & 0xFu) == 2 ? 244 : 236));
	BucketMapOffset = latch.Load(LatchOffset + 248);
	FadeStatesIndex = latch.Load(LatchOffset + 240);
	PlacementsIndex = latch.Load(LatchOffset + 252);
	FadeSeedsIndex = latch.Load(LatchOffset + 288);
	// The installed publication's payload: the scene list, the geometry table and the rows.
	const uint4 payload = latch.Load4(LatchOffset + 256);
	const uint4 rows = latch.Load4(LatchOffset + 272);
	InputsBuffer = payload.x;
	GeometriesBuffer = payload.y;
	ViewBits = payload.z;
	FocusRoot = payload.w;
	TerrainNear = asfloat(latch.Load4(LatchOffset + 304));
	const uint3 strict = latch.Load3(LatchOffset + 324);
	LightContextsIndex = strict.x;
	StrictGatesIndex = strict.y;
	StrictContext = strict.z;
	const uint2 classRegions = latch.Load2(LatchOffset + 292);
	ClassTableOffset = classRegions.x;
	ClassMapOffset = classRegions.y;
	MaterialRowsLo = rows.x;
	MaterialRowsHi = rows.y;
	PipelineRowsLo = rows.z;
	PipelineRowsHi = rows.w;
}

// Light Limit Fix's shadow mask of an input, as the main pass's selection (LocalShadowLights): local shadow lights whose sphere
// its bound meets and whose planes it is inside, less those a landscape property does not take, and a portal-strict light's whose
// gates the object (a_object) does not pass (GpuShadowVolume::portalContext).
uint LocalShadowMask(float3 a_centre, float a_radius, bool a_landscape, uint a_object)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint volumes = latch.Load(LocalShadowOffset);
	uint mask = 0;
	[loop] for (uint v = 0; v < volumes; ++v)
	{
		const uint record = LocalShadowOffset + kSunRegionHeader + v * kShadowVolumeBytes;
		const uint4 head = latch.Load4(record);  // plane masks, mask bit, landscape
		if ((mask & head.z) != 0 || (a_landscape && head.w == 0))
			continue;
		const float4 sphere = asfloat(latch.Load4(record + 16));
		const float3 offset = a_centre - sphere.xyz;
		const float reach = sphere.w + a_radius;
		if (dot(offset, offset) > reach * reach)
			continue;
		bool inside = true;
		[loop] for (uint p = 0; p < 12 && inside; ++p)
		{
			const uint planeMask = p < 6 ? head.x : head.y;
			if ((planeMask & (1u << (p % 6))) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(record + 32 + p * 16));
			inside = dot(plane.xyz, a_centre) - plane.w >= -a_radius;
		}
		const uint portalContext = latch.Load(record + 224);
		if (inside && portalContext != 0 && StrictGateRejects(StrictGatesIndex, LightContextsIndex, ObjectGatesIndex, portalContext - 1, a_object, a_centre, a_radius))
			inside = false;
		if (inside)
			mask |= head.z;
	}
	return mask;
}

// kObjectSunTest: whether the bound meets any cascade, with SunAccumulation's sphere test (outside when dot(n, c) - d < -r)
// against each cascade's planes and custom planes, as the engine's cascade culls.
bool InSunCascades(float3 a_centre, float a_radius)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint cascades = latch.Load(SunCascadeOffset);
	[loop] for (uint c = 0; c < cascades; ++c)
	{
		const uint record = SunCascadeOffset + kSunRegionHeader + c * kSunCascadeBytes;
		const uint2 masks = latch.Load2(record);
		bool inside = true;
		[loop] for (uint p = 0; p < 12 && inside; ++p)
		{
			const uint mask = p < 6 ? masks.x : masks.y;
			if ((mask & (1u << (p % 6))) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(record + 16 + p * 16));
			inside = dot(plane.xyz, a_centre) - plane.w >= -a_radius;
		}
		if (inside)
			return true;
	}
	return false;
}

// A sun view: whether an entry sphere is outside every full-frustum process (OutsideSunEntryProcesses). None: no verdict.
bool OutsideSunEntry(float4 a_entry)
{
	if (SunEntryOffset == 0)
		return false;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint processes = latch.Load(SunEntryOffset);
	if (processes == 0)
		return false;
	[loop] for (uint process = 0; process < processes; ++process)
	{
		const uint record = SunEntryOffset + kSunRegionHeader + process * kSunEntryProcessBytes;
		const uint mask = latch.Load(record);
		bool outside = false;
		[loop] for (uint p = 0; p < 6 && !outside; ++p)
		{
			if ((mask & (1u << p)) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(record + 16 + p * 16));
			outside = dot(plane.xyz, a_entry.xyz) - plane.w < -a_entry.w;
		}
		if (!outside)
			return false;
	}
	return true;
}

// The draw's pipeline: the input's own, or (shadow view) its key slot's through the view's map row. kNoPipeline (which the CPU
// never lets an input reach) drops the draw rather than execute an index outside the set.
static const uint kNoPipeline = 0xFFFFFFFFu;
uint DrawPipeline(uint a_input)
{
	if (PipelineMapOffset == 0)
		return a_input;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(PipelineMapOffset + a_input * 4);
}

uint CullMode() { return CullFlags & 0xF; }
// 1: depth phase 1, against the previous frame's HZB. 2: depth phase 2, against the HZB rebuilt from this frame's depth, over
// what phase 1 rejected. 3: the colour segment, which only reads what the two phases decided.
uint CullPhase() { return (CullFlags >> 4) & 0xF; }
static const uint kPhaseSingle = 0;
static const uint kPhaseOne = 1;
static const uint kPhaseTwo = 2;
static const uint kPhaseColour = 3;

// Visibility values. Frustum rejection is final; a phase 1 occlusion rejection is provisional (previous frame's depth).
static const uint kVisibilityOccludedRetest = 0;
static const uint kVisibilityVisible = 1;
static const uint kVisibilityRejectedFinal = 2;
// Written only by the colour segment, for an object it drew without a verdict published this frame.
static const uint kVisibilityUnpublished = 3;
// The word: verdict in bits 0-1, depth-drawn (bit 2), colour-drawn (bit 3), the stamp above. CS_DCLF_SET_PARITY reads the drawn
// bits back: depth without colour, or colour without depth, is visible damage.
static const uint kVisibilityVerdictMask = 3;
static const uint kVisibilityDepthDrawn = 4;
static const uint kVisibilityColourDrawn = 8;
static const uint kVisibilityStampShift = 4;
// A clamped shadow view (render mode 0xE) pancakes casters in front of its near plane onto it (RENDER_SHADOWMAP_CLAMPED): keep them.
bool NoNearPlane() { return (CullFlags & 0x200) != 0; }
// Caster class (kCullCastersOnly, kCullVolumetricOnly): the volumetric lighting copy's views draw only the volumetric-only casters,
// every other shadow view the rest.
bool CastersOnly() { return (CullFlags & 0x400) != 0; }
bool VolumetricOnly() { return (CullFlags & 0x800) != 0; }
// A sun view: an input whose entry is outside the full-frustum processes is not a caster (kCullSunEntry).
bool SunEntry() { return (CullFlags & 0x1000) != 0; }
// Skylighting's occlusion map view (kCullMinRadius): a bound radius of 32 or less draws nothing (OcclusionTechnique).
bool MinRadius() { return (CullFlags & 0x2000) != 0; }
// The occlusion map view (kCullFadeOnVisible): fade roots tested as OnVisible without cameraRelatedUpdates (FadedOutOfOcclusion),
// not RootFading.
bool FadeOnVisible() { return (CullFlags & 0x4000) != 0; }
// Object word bit 30: a LOD cross-fade copy draw (LodPartitions); the pixel stage uses the cross-fade factor as alpha (DCLFAlphaFade).
static const uint kObjectCrossfadeCopy = 1u << 30;
// The key word a view reads (kCullKeyShift): 0 the pipeline and rows words (main segments, reflection); otherwise a shadow view's
// (material row = shadowRow): 1 a caster's key (ViewWords::shadowKey), 2 and 3 an occlusion map's (occlusionKey[0], [1]).
uint KeyWord() { return (CullFlags >> 16) & 3; }

// Object flags (Records.h), as the draw input carries them.
// A synthetic main pass with the sun's bits, and the object word's mark for a draw that misses every cascade (read by the pixel stage).
static const uint kObjectSunTest = 1u << 26;
static const uint kObjectSunMiss = 1u << 31;
// The object word's local shadow lights, and a landscape property's flag (only lights that take landscape).
static const uint kObjectLocalShadowShift = 26;
static const uint kObjectLandscapeLights = 1u << 29;
static const uint kObjectMember = 1u << 30;
// Under a fade root with a fade-out distance: phase 1 drops it past that distance unless it was in view last frame (used when
// its root's state has no previous OnVisible).
static const uint kObjectFadeTest = 1u << 27;
// A resident tree: phase 1 drops it where BSTreeNode::OnVisible's height test would.
static const uint kObjectHeightTest = 1u << 28;
// A volumetric-only caster, drawn only by the volumetric lighting copy's views.
static const uint kObjectVolumetricOnly = 1u << 23;
// A decal, group in bits 20-21 (1 = the engine's opaque decal group, 2 = blended). Never occluders or depth inputs: the colour
// segment tests them once against this frame's final HZB and writes fixed slots (the decal sub-ranges past DecalBase), so overlapping
// decals draw in the engine's order.
static const uint kObjectDecal = 1u << 7;
static const uint kObjectDecalGroupShift = 20;
// The same bits without kObjectDecal are a terrain group (ObjectTerrainGroup): 1 Terrain Blending's terrain, 2 the meshes it
// redraws after. The colour segment appends each group to its own range (in phase 2's range, unused there: kTerrainGroups),
// drawn after opaque and decals; the depth segment sends a group 1 draw within the near distance to its bucket's twin.
uint TerrainGroup(uint a_flags) { return (a_flags & kObjectDecal) != 0 ? 0 : (a_flags >> kObjectDecalGroupShift) & 3; }
static const uint kBucketTerrainPair = 0x80000000u;
// A blended Lighting object drawn by the forward epoch. Like a decal, no occluder or depth input: the colour segment culls it once
// (frustum, then this frame's HZB) and stages each visible draw with its view depth; ForwardSortCS.hlsl sorts back to front.
static const uint kObjectForward = 0x80000000u;
// A refraction object (kRasterRefraction): culled as a forward input plus its root's fade verdict (the engine registers nothing
// under a root its OnVisible stops at); draws go pooled to the refraction range (RefractionDrawPass). No sort: the engine draws
// group 5 by batches, and the draw neither blends nor writes depth.
static const uint kViewRefraction = 1u << 10;
// First-person forward: the first-person view (single phase) frustum-culls a forward input, and its slot map sends a forward slot
// to the forward range with kBucketForward: a main-format sequence staged with its sort key for ForwardSortCS (FirstPerson.cpp).
static const uint kViewFirstPerson = 1u << 11;
static const uint kBucketForward = 0x40000000u;
// TerrainBlending.cpp's test exactly: a block is in its terrain depth unless its bound's nearest point is past the distance.
bool TerrainNearOf(float4 a_bound) { return distance(a_bound.xyz, TerrainNear.xyz) - a_bound.w <= TerrainNear.w; }
// Set by the epoch: this input may get a sequence. The depth segment submits every candidate so the published visibility is
// complete; the rest are cull-only.
static const uint kInputDrawable = 1u << 16;

// Counter words in the count buffer (zeroed before the dispatch).
static const uint kCountDrawn = 0;   // sequences written
static const uint kCountCulled = 4;  // rejected by this frame's culling
static const uint kCountTested = 8;  // looked at by the culling at all
// What a shadow view drops before culling: wrong caster class, outside the sun entry, at most the minimum radius, under a fading
// root (occlusion map: a root its OnVisible stops at).
static const uint kCountSunEntryOut = 12;
static const uint kCountMinRadius = 16;
static const uint kCountRootFading = 20;
static const uint kCountCasterClass = 64;
static const uint kCountOccluded = 24;  // rejected by the HZB rather than by the frustum
// What the HZB held under tested objects, to tell a suspicious rejection count from an empty or full HZB without a readback.
static const uint kCountHzbNear = 28;     // the footprint's farthest depth was ~0: the build wrote nothing
static const uint kCountHzbFar = 32;      // it was ~1: all far plane, so nothing can ever be occluded
static const uint kCountHzbSampled = 36;  // footprints sampled, to give the two above a denominator
// One rejection, recorded in full (see Occluded).
static const uint kSampleTaken = 40;
static const uint kSampleFarthest = 44;
static const uint kSampleNearestZ = 48;
static const uint kSampleUvMin = 52;
static const uint kSampleUvMax = 56;
static const uint kSampleMip = 60;
static const uint kCountDrawnPhaseTwo = 68;      // sequences phase 2 appended, and the count its draw reads
static const uint kCountRescuedByPhaseTwo = 72;  // objects phase 1 rejected and the rebuilt HZB brought back
// Decals: the two groups' slot counts (uploaded by the CPU, read by their draws) and the culling's tallies.
static const uint kCountDecalGroup1 = 76;
static const uint kCountDecalGroup2 = 80;
static const uint kCountDecalsCulled = 84;
static const uint kCountDecalsTested = 88;
// The sun on the GPU: kObjectSunTest inputs the colour pass tested, and those that missed every cascade.
static const uint kCountSunTested = 92;
static const uint kCountSunMissed = 96;
// The fade test: inputs under a fade root phase 1 found in the frustum, and those it dropped.
static const uint kCountFadeTested = 100;
static const uint kCountFadeHidden = 104;
// The terrain groups' draws appended, a tally (zeroed by the colour commit; each class sub-range's count is its classCounts word), then
// phase 1's terrain group 1 inputs and those found near
// (kCountTerrainGroupWord).
static const uint kCountTerrainGroup1 = 112;
static const uint kCountTerrainNearTested = 120;
static const uint kCountTerrainNear = 124;
// The forward draws appended, a tally (zeroed by the colour commit; each class's staging half is counted in classCounts), then forward
// inputs tested, in the frustum, HZB-rejected (kCountForwardWord).
static const uint kCountForward = 128;
static const uint kCountForwardTested = 132;
static const uint kCountForwardInView = 136;
static const uint kCountForwardOccluded = 140;
// The refraction draw count (zeroed by the colour commit), then refraction inputs tested, in the frustum, rejected by HZB or fade
// (kCountRefractionWord).
static const uint kCountRefraction = 144;
static const uint kCountRefractionTested = 148;
static const uint kCountRefractionInView = 152;
static const uint kCountRefractionOccluded = 156;
// Colour draws dropped for want of a state class sub-range (kCountClassUnplannedWord): no class map word for the draw's set index, or
// past its class sub-range's capacity. Never drawn with another class's state.
static const uint kCountClassUnplanned = 160;
static const uint kCountClassOverflow = 164;

// The frustum stamp word: the stamp in the low 28 bits, and whether the fade test dropped the object that frame
// (VisibilityEventsCS.hlsl compares the low bits).
static const uint kFrustumStampMask = 0x0FFFFFFFu;
static const uint kFrustumFadeHidden = 0x80000000u;

// Sort keys: the main set's capacity (DrawPipelines::kMaxPipelines), which the state classes' keys (each class's first key and size,
// the class table's) partition. A rank word holds the key in its top 12 bits and the rank below (the draws' range is capped at
// kSortRankLimit).
static const uint kSortKeys = 4096;
static const uint kSortRankBits = 20;

// The state classes (GpuLayouts.h; DrawPipelines.h, kSetClassShift). A class map word: class << kSetClassShift | the pipeline's index in
// its class's execution sets (what a colour sequence's pipeline index is). A class table entry: the class's first sort key and its
// size, then (first, capacity) per range: kClassRangeMain, ClassRangeOfSlot(slot) = 1 + slot (decal groups, then terrain groups),
// kClassRangeForward.
static const uint kSetClassShift = 16;
static const uint kSetClassIndexMask = 0xFFFFu;
static const uint kNoSetClass = 0xFFFFFFFFu;
static const uint kClassRangeMain = 0;
static const uint kClassRangeForward = 6;
static const uint kClassRanges = 7;
static const uint kClassHeaderBytes = 16;
static const uint kClassBytes = 64;  // kClassWords words

// A set index's state class and index in it, from the latch's class map; false without a map or a word for it (a pipeline the
// revision's set version did not hold), whose draw is dropped (counted), never drawn with another class's state.
bool SetClassOf(uint a_set, out uint a_class, out uint a_index)
{
	a_class = 0;
	a_index = 0;
	if (ClassMapOffset == 0)
		return false;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	if (a_set >= latch.Load(ClassMapOffset))
		return false;
	const uint word = latch.Load(ClassMapOffset + kClassHeaderBytes + a_set * 4);
	if (word == kNoSetClass)
		return false;
	a_class = word >> kSetClassShift;
	a_index = word & kSetClassIndexMask;
	return true;
}

// The class table's classes (0 without one: no sub-range at all).
uint ClassCount()
{
	if (ClassTableOffset == 0)
		return 0;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(ClassTableOffset);
}

// A class's sub-range of a range: its first sequence slot and capacity (0: the class has none there).
uint2 ClassRange(uint a_class, uint a_range)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load2(ClassTableOffset + kClassHeaderBytes + a_class * kClassBytes + 8 + a_range * 8);
}

// A class's first sort key: the classes' sizes before it, so a class's keys are contiguous and its draws sort within it.
uint ClassKeyBase(uint a_class)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(ClassTableOffset + kClassHeaderBytes + a_class * kClassBytes);
}

// The next slot of a class's sub-range (its classCounts word): its sequence slot, or false past the capacity (counted, dropped; the
// count word then exceeds the capacity, which the call's max count clamps, so every executed slot is written).
bool TakeClassSlot(uint a_class, uint a_range, out uint a_slot)
{
	const uint2 range = ClassRange(a_class, a_range);
	RWByteAddressBuffer classCounts = ResourceDescriptorHeap[ClassCountsIndex];
	uint taken;
	classCounts.InterlockedAdd((a_class * kClassRanges + a_range) * 4, 1, taken);
	a_slot = range.x + taken;
	if (taken < range.y)
		return true;
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	count.InterlockedAdd(kCountClassOverflow, 1, scratch);
	return false;
}

// DrawInput (Core/GpuLayouts.h): 64 bytes (pipeline/record/geometry/flags, the world-space bounding sphere, the object index, the
// decal ordinal, the skin partitions to draw, the GeometryDraw of a second vertex stream or ~0, and the fade row:
// the entry root's centre and the fade-out distance, for kObjectFadeTest).
static const uint kInputStride = 64;
// GeometryDraw (Core/GpuLayouts.h): 40 bytes (vertex buffer view, index buffer view, index count, first index, next partition).
static const uint kGeometryStride = 40;
// A multi-partition skin draws once per partition the engine would: the input's mask names them (bit i = partition i), and
// each partition's GeometryDraw links to the next.
static const uint kMaxPartitions = 8;
static const uint kNoPartition = 0xFFFFFFFFu;
// The mask of a skin whose LOD level draws none of its partitions (Records.h): no draw at all.
static const uint kNoPartitions = 1u << 8;
// A chain (Records.h, kPartitionChain): every slot linked from the first, the count in the low bits (object LOD's visible ranges).
static const uint kPartitionChain = 1u << 15;
static const uint kPartitionChainCount = kPartitionChain - 1;
// DrawSequence (Records.h): 92 bytes, 4-byte packed. Words 1-5 are the root constants (the pipeline row's address, the material row's,
// then the object index), which is why the object index sits between the rows' addresses and the vertex buffer.
// The second vertex buffer view (slot 1) is a face shape's positions (FaceSnapshots), else the first again.
static const uint kSequenceStride = 92;
static const uint kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT
static const uint kNoStream = 0xFFFFFFFFu;

void StoreSequence(RWByteAddressBuffer sequences, uint slot, uint pipeline, uint4 rows, uint objectIndex, uint4 vertexBuffer,
	uint4 streamBuffer, uint4 indexBuffer, uint indexCount, uint firstIndex)
{
	const uint base = slot * kSequenceStride;
	sequences.Store(base + 0, pipeline);
	sequences.Store4(base + 4, rows);
	sequences.Store(base + 20, objectIndex);
	sequences.Store4(base + 24, vertexBuffer);
	sequences.Store4(base + 40, streamBuffer);
	sequences.Store4(base + 56, uint4(indexBuffer.xyz, kIndexFormatR16));
	sequences.Store4(base + 72, uint4(indexCount, 1, firstIndex, 0));
	sequences.Store(base + 88, 0);
}

// A shadow view's sequence: a plain indexed draw (ShadowViewPass) from the index pool (a_poolFirst, ~0: no indices), no vertex
// offset so the vertex stage fetches by the geometry's own index, and the bucket slot as first instance.
void StoreShadowSequence(RWByteAddressBuffer sequences, uint slot, uint instance, uint4 rows, uint objectIndex, uint4 vertexBuffer,
	uint4 streamBuffer, uint4 indexBuffer, uint indexCount, uint firstIndex, uint a_poolFirst)
{
	StoreSequence(sequences, slot, 0, rows, objectIndex, vertexBuffer, streamBuffer, indexBuffer, indexCount, firstIndex);
	const bool pooled = a_poolFirst != 0xFFFFFFFFu;
	sequences.Store4(slot * kSequenceStride + 72, uint4(pooled ? indexCount : 0, 1, pooled ? a_poolFirst + firstIndex : 0, 0));
	sequences.Store(slot * kSequenceStride + 88, instance);
}

uint2 AddressOf(uint a_lo, uint a_hi, uint a_offset)
{
	const uint lo = a_lo + a_offset;
	return uint2(lo, a_hi + (lo < a_lo ? 1 : 0));
}

// An input's rows (y: pipeline row high 12 bits, material row low 20) as addresses: the draw's first four push words.
uint4 RowsOf(uint a_rows)
{
	const uint2 pipelineRow = PipelineRowStride ? AddressOf(PipelineRowsLo, PipelineRowsHi, (a_rows >> 20) * PipelineRowStride) : uint2(0, 0);
	const uint2 materialRow = AddressOf(MaterialRowsLo, MaterialRowsHi, (a_rows & 0xFFFFFu) * MaterialRowStride);
	return uint4(pipelineRow, materialRow);
}

// A staged forward draw's sort key, in three words ForwardSortCS.hlsl reads and restores: view depth (clip w, the engine's blended
// sort key, FUN_1414e8fc0) in firstInstance. A BSOrderedNode group's leaf sorts at its node's centre (forwardSort, ordinal in w),
// where the engine places the whole group; the node (a hash of that centre) and ordinal go in instanceCount and vertexOffset.
void StageForwardKey(RWByteAddressBuffer sequences, uint a_slot, uint a_object)
{
	const float4 forwardSort = ObjectRow(a_object, kObjectForwardSortRow);
	const uint ordinal = asuint(forwardSort.w);
	const float3 sortCentre = ordinal != 0 ? forwardSort.xyz : ObjectRow(a_object, kObjectBoundRow).xyz;
	const float clipW = mul(ViewProj, float4(sortCentre, 1.0)).w;
	const uint group = ordinal != 0 ? (asuint(forwardSort.x) * 0x9E3779B1u) ^ (asuint(forwardSort.y) * 0x85EBCA77u) ^ (asuint(forwardSort.z) * 0xC2B2AE3Du) : 0u;
	const uint stagedAt = a_slot * kSequenceStride;
	sequences.Store(stagedAt + 76, group);
	sequences.Store(stagedAt + 84, ordinal);
	sequences.Store(stagedAt + 88, asuint(isnan(clipW) ? 0.0 : clipW));  // a NaN would break the rank
}

// The bounding sphere's world AABB, projected corner by corner: rejected only when all eight corners are outside one clip plane.
// Tests are on homogeneous coordinates before any divide, linear in the point, so they hold for corners behind the eye (w <= 0),
// and an object wholly behind the camera fails the near plane (nothing else would reject it).
bool Culled(float3 boundCentre, float boundRadius)
{
	const float3 centre = boundCentre;
	[unroll] for (uint p = 0; p < 6; ++p)
	{
		if ((CullPlaneMask & (1u << p)) && dot(CullPlanes[p].xyz, centre) - CullPlanes[p].w < -boundRadius)
			return true;
	}
	float4 planes = float4(1, 1, 1, 1);  // all-corners-outside accumulators: -x, +x, -y, +y
	float2 depthPlanes = float2(1, 1);   // near, far
	[unroll] for (uint corner = 0; corner < 8; ++corner)
	{
		const float3 offset = float3((corner & 1) ? boundRadius : -boundRadius,
			(corner & 2) ? boundRadius : -boundRadius,
			(corner & 4) ? boundRadius : -boundRadius);
		const float4 clip = mul(ViewProj, float4(centre + offset, 1.0));
		planes.x *= (clip.x < -clip.w) ? 1 : 0;
		planes.y *= (clip.x > clip.w) ? 1 : 0;
		planes.z *= (clip.y < -clip.w) ? 1 : 0;
		planes.w *= (clip.y > clip.w) ? 1 : 0;
		depthPlanes.x *= (clip.z < 0 && !NoNearPlane()) ? 1 : 0;
		depthPlanes.y *= (clip.z > clip.w) ? 1 : 0;
	}
	return any(planes != 0) || any(depthPlanes != 0);
}

// Occlusion against the HZB (HzbTest.hlsli), with diagnostic counters.
bool Occluded(float3 boundCentre, float boundRadius)
{
	HzbView view;
	view.ViewProj = ViewProj;
	view.Index = HzbIndex;
	view.BaseSize = HzbUnpackSize(HzbSizePacked);
	view.Mips = HzbMips;
	view.UvScale = HzbUnpackUvScale(HzbUvScalePacked);
	const HzbResult result = HzbTest(view, boundCentre, boundRadius);
	if (!result.Sampled)
		return false;

	RWByteAddressBuffer counters = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	counters.InterlockedAdd(kCountHzbSampled, 1, scratch);
	if (result.Farthest <= 1e-6)
		counters.InterlockedAdd(kCountHzbNear, 1, scratch);
	else if (result.Farthest >= 0.9999)
		counters.InterlockedAdd(kCountHzbFar, 1, scratch);

	// The dispatch's first rejection in full (HZB value, nearest corner, UV range, mip), so a suspicious count can be read.
	if (result.Occluded) {
		uint previous;
		counters.InterlockedCompareExchange(kSampleTaken, 0, 1, previous);
		if (previous == 0) {
			counters.Store(kSampleFarthest, asuint(result.Farthest));
			counters.Store(kSampleNearestZ, asuint(result.NearestZ));
			counters.Store(kSampleUvMin, (uint(saturate(result.UvMin.x) * 65535.0) & 0xFFFF) | (uint(saturate(result.UvMin.y) * 65535.0) << 16));
			counters.Store(kSampleUvMax, (uint(saturate(result.UvMax.x) * 65535.0) & 0xFFFF) | (uint(saturate(result.UvMax.y) * 65535.0) << 16));
			counters.Store(kSampleMip, uint(result.Mip));
		}
	}
	return result.Occluded;
}

// Stamps the visibility feedback word of a phase 1 input that passed the frustum (and portal) test, with whether its root's fade
// dropped it (the engine's cull does not recurse past a root whose OnVisible stops); returns that drop. a_count: own inputs only.
bool StampFrustum(ByteAddressBuffer a_inputs, uint a_inputOffset, uint a_flags, uint a_objectIndex, bool a_count)
{
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	bool fadeHidden = false;
	RWByteAddressBuffer frustumStamps = ResourceDescriptorHeap[FrustumIndex];
	// Under a fade root: the root's previous OnVisible verdict (FadeStateCS), as the engine's cull decides whether to recurse.
	// DCLF's state is authoritative.
	const uint fadeRoot = a_inputs.Load(a_inputOffset + 48);
	bool serviced = false;
	if (fadeRoot != 0xFFFFFFFFu && FadeRootsIndex != 0) {
		StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
		const FadeRootStatic rootRow = fadeRoots[fadeRoot];
		StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
		const FadeNodeState fadeState = fadeStates[fadeRoot];
		serviced = fadeState.Generation == rootRow.Generation && (fadeState.Verdict & kFadeVerdictServiced) != 0;
		if (serviced) {
			fadeHidden = (fadeState.Verdict & kFadeVerdictDrawn) == 0;
			if (a_count) {
				count.InterlockedAdd(kCountFadeTested, 1, scratch);
				if (fadeHidden)
					count.InterlockedAdd(kCountFadeHidden, 1, scratch);
			}
		}
	}
	// An unserviced root (out of view, or newly listed): BSFadeNode::OnVisible snaps the fade of a root not in view last frame to 0
	// past its fade-out distance and keeps it there, so the draw is dropped. Centre from the record, distance from the input.
	const float4 fade = float4(ObjectRow(a_objectIndex, kObjectFadeNodeRow).xyz, asfloat(a_inputs.Load(a_inputOffset + 60)));
	if (!serviced && (a_flags & kObjectFadeTest) != 0 && fade.w != 0.0 && FadeEye.w != 0.0) {
		const uint previous = frustumStamps.Load(a_objectIndex * 4);
		const bool wasInView = (previous & kFrustumStampMask) == ((VisibilityStamp - 1) & kFrustumStampMask);
		const bool wasHidden = wasInView && (previous & kFrustumFadeHidden) != 0;
		// FUN_14147b110: distance to the root's centre, scaled by the camera's LOD factor over the LOD type's divisor (folded into a
		// positive distance), or by a constant (a negative one).
		const float distance = length(fade.xyz - FadeEye.xyz) * (fade.w > 0.0 ? FadeEye.w : 1.0);
		fadeHidden = distance > abs(fade.w) && (!wasInView || wasHidden);
		if (a_count) {
			count.InterlockedAdd(kCountFadeTested, 1, scratch);
			if (fadeHidden)
				count.InterlockedAdd(kCountFadeHidden, 1, scratch);
		}
	}
	frustumStamps.Store(a_objectIndex * 4, VisibilityStamp | (fadeHidden ? kFrustumFadeHidden : 0));
	return fadeHidden;
}

// The main camera's cull before the HZB: the frustum, then the portals and occluders its root is culled through (the engine's
// Process1 on the member after its root's; PortalPrograms.hlsli). Also the feedback verdict for kStampOnlyViews inputs.
bool MainFrustumRejected(uint objectIndex)
{
	if (MemberResultsIndex == 0)
		return true;
	StructuredBuffer<uint2> results = ResourceDescriptorHeap[MemberResultsIndex];
	const uint2 result = results[objectIndex];
	return result.x != VisibilityStamp || asint(result.y) != 1;
}

// Decal, forward and refraction inputs (culled once by the colour segment) still need the main camera's cull for kAccumulated
// (dclf-architecture.md, "BuildDraws"): phase 1 stamps their frustum verdict and nothing else. GpuLayouts.h's bits.
static const uint kStampOnlyViews = (1u << 8) | (1u << 9) | (1u << 10);

// Every slot is initialized before draw inputs claim their object. Shared skin-partition inputs perform just one bound test.
[numthreads(64, 1, 1)] void ClearPortalQueries(uint3 id : SV_DispatchThreadID) {
	LoadLatch();
	RWStructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[MemberQueriesIndex];
	RWStructuredBuffer<uint> claims = ResourceDescriptorHeap[MemberClaimsIndex];
	uint capacity, stride;
	queries.GetDimensions(capacity, stride);
	const uint objectIndex = id.x + id.y * (64u * 65535u);
	if (objectIndex >= capacity)
		return;
	PortalQuery query = (PortalQuery)0;
	query.Flags = 0x80000000u;
	query.Destination = objectIndex;
	query.Generation = VisibilityStamp;
	queries[objectIndex] = query;
	claims[objectIndex] = 0;
}

	[numthreads(64, 1, 1)] void PreparePortalQueries(uint3 id : SV_DispatchThreadID)
{
	LoadLatch();
	const uint draw = id.x;
	if (draw >= DrawCount)
		return;
	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsBuffer];
	const uint inputOffset = draw * kInputStride;
	if (ViewBits != 0 && (inputs.Load(inputOffset + 16) & (ViewBits | kStampOnlyViews)) == 0)
		return;
	const uint objectIndex = inputs.Load(inputOffset + 32);
	RWStructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[MemberQueriesIndex];
	RWStructuredBuffer<uint> claims = ResourceDescriptorHeap[MemberClaimsIndex];
	uint capacity, stride;
	queries.GetDimensions(capacity, stride);
	if (objectIndex >= capacity)
		return;  // ObjectFits prevents admission beyond the published batch.
	uint previous;
	InterlockedCompareExchange(claims[objectIndex], 0u, 1u, previous);
	if (previous != 0)
		return;
	PortalQuery query = (PortalQuery)0;
	query.Bound = ObjectRow(objectIndex, kObjectBoundRow);
	query.Flags = 0x80000000u;
	query.Destination = objectIndex;
	query.Generation = VisibilityStamp;
	query.Program = Culled(query.Bound.xyz, query.Bound.w) ? 0u : 1u;
	// The frame's published programs (0: none this frame, the frustum alone).
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint programsIndex = latch.Load(LatchOffset + kLatchPortalPrograms);
	if (query.Program != 0 && programsIndex != 0 && RootProgramsIndex != 0) {
		const uint root = inputs.Load(inputOffset + 48);
		if (root != 0xFFFFFFFFu) {
			StructuredBuffer<uint> rootPrograms = ResourceDescriptorHeap[RootProgramsIndex];
			ByteAddressBuffer programs = ResourceDescriptorHeap[programsIndex];
			const uint program = rootPrograms[root];
			if (program < programs.Load(kPortalSetBytes)) {
				query.Program = program;
				query.Flags = 0;
			}
		}
	}
	queries[objectIndex] = query;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	LoadLatch();
	const uint draw = dispatchID.x;
	if (draw >= DrawCount)
		return;

	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsBuffer];
	ByteAddressBuffer geometries = ResourceDescriptorHeap[GeometriesBuffer];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	RWByteAddressBuffer visibility = ResourceDescriptorHeap[VisibilityIndex];

	const uint inputOffset = draw * kInputStride;
	const uint4 input = inputs.Load4(inputOffset);  // pipeline index, record index, geometry index, flags
	// This dispatch's view types against the input's view mask (one scene, a view is a mask), before reading anything else.
	if (ViewBits != 0 && (inputs.Load(inputOffset + 16) & ViewBits) == 0) {
		// kStampOnlyViews: phase 1 stamps the frustum verdict.
		if (FrustumIndex != 0 && CullPhase() == kPhaseOne && (inputs.Load(inputOffset + 16) & kStampOnlyViews) != 0) {
			const uint stampObject = inputs.Load(inputOffset + 32);
			if (CullMode() == 0 || !MainFrustumRejected(stampObject))
				StampFrustum(inputs, inputOffset, input.w, stampObject, false);
		}
		return;
	}
	// A focus view's casters: its target's members (the engine registers the target's subtree whole).
	if (FocusRoot != 0 && inputs.Load(inputOffset + 48) != FocusRoot - 1)
		return;
	const uint objectIndex = inputs.Load(inputOffset + 32);
	// The view's key and rows words (KeyWord): a shadow view's are its family's key and the shadow row.
	const uint keyWord = KeyWord();
	const uint drawKey = keyWord != 0 ? inputs.Load(inputOffset + 16 + keyWord * 4) : input.x;
	const uint rowsWord = keyWord != 0 ? inputs.Load(inputOffset + 52) : input.y;
	if (drawKey == 0xFFFFFFFFu)
		return;  // no key in the view's family: never an input its mask lets through
	// The draw's object word: the object index, plus kObjectSunMiss when a sun-test pass meets no cascade. Counted for every such
	// input, drawn or not, to compare with the CPU's count.
	uint objectWord = objectIndex;
	if ((SunState & kSunTestOn) != 0 && (input.w & kObjectSunTest) != 0) {
		uint sunScratch;
		RWByteAddressBuffer sunCount = ResourceDescriptorHeap[CountIndex];
		sunCount.InterlockedAdd(kCountSunTested, 1, sunScratch);
		const float4 sunBound = ObjectRow(objectIndex, kObjectBoundRow);
		if (!InSunCascades(sunBound.xyz, sunBound.w)) {
			objectWord |= kObjectSunMiss;
			sunCount.InterlockedAdd(kCountSunMissed, 1, sunScratch);
		}
	}
	if (LocalShadowOffset != 0) {
		const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);
		objectWord |= (LocalShadowMask(bound.xyz, bound.w, (input.w & kObjectLandscapeLights) != 0, objectIndex) & 0xFu) << kObjectLocalShadowShift;
	}
	// The drawable bit is the main family's; a shadow view draws every input its bits select.
	const bool drawable = keyWord != 0 || (input.w & kInputDrawable) != 0;
	uint scratch;
	const uint phase = CullPhase();

	// A shadow view draws one caster class: the mode's inputs hold both.
	const bool volumetricCaster = (input.w & kObjectVolumetricOnly) != 0;
	if ((CastersOnly() && volumetricCaster) || (VolumetricOnly() && !volumetricCaster))
		return;
	if (phase == kPhaseSingle)
		count.InterlockedAdd(kCountCasterClass, 1, scratch);
	// The sun entry rule: the entry sphere (SetSunEntryRow) outside every full-frustum process.
	if (SunEntry() && OutsideSunEntry(ObjectRow(objectIndex, kObjectSunEntryRow))) {
		count.InterlockedAdd(kCountSunEntryOut, 1, scratch);
		return;
	}
	if (MinRadius() && ObjectRow(objectIndex, kObjectBoundRow).w <= 32.0) {
		count.InterlockedAdd(kCountMinRadius, 1, scratch);
		return;
	}
	// A caster under a fading or faded-out root (FadeStateCS's fade): the engine casts no fading caster (ShadowReject::Faded,
	// fade * materialAlpha < 1; the CPU's verdict takes the material's alpha alone).
	if (phase == kPhaseSingle && FadeRootsIndex != 0 &&
		(FadeOnVisible() ? FadedOutOfOcclusion(inputs.Load(inputOffset + 48)) : RootFading(inputs.Load(inputOffset + 48)))) {
		count.InterlockedAdd(kCountRootFading, 1, scratch);
		return;
	}

	// The <- TB NEAR observer's tally of phase 1's terrain group 1 inputs, repeated by the CPU (ReadCullCounters).
	if (phase == kPhaseOne && TerrainNear.w > 0.0 && TerrainGroup(input.w) == 1) {
		count.InterlockedAdd(kCountTerrainNearTested, 1, scratch);
		if (TerrainNearOf(ObjectRow(objectIndex, kObjectBoundRow)))
			count.InterlockedAdd(kCountTerrainNear, 1, scratch);
	}

	// Decals: fixed slots, in a sub-range per state class present in the group (each its own call, with its class's state). Every decal
	// writes its slots in every sub-range of its group, culled or not: its own class's the real sequence (index count 0 when culled),
	// every other class's a zero-count one naming that class's pipeline 0. So every slot below the group's count of every sub-range is
	// written each frame, nothing stale is ever executed, and the decals keep the engine's order within each class.
	const uint decalGroup = keyWord == 0 && (input.w & kObjectDecal) ? (input.w >> kObjectDecalGroupShift) & 3 : 0;
	if (decalGroup != 0) {
		if (phase != kPhaseColour && phase != kPhaseSingle)
			return;  // the depth segment never submits one; belt and braces
		const uint decalOrdinal = inputs.Load(inputOffset + 36);
		if (decalOrdinal >= DecalStride)
			return;  // never: the range holds every ordinal of the group
		bool culled = !drawable;
		if (drawable && CullMode() != 0) {
			count.InterlockedAdd(kCountDecalsTested, 1, scratch);
			const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);
			culled = Culled(bound.xyz, bound.w) || (CullMode() >= 2 && Occluded(bound.xyz, bound.w));
			if (culled)
				count.InterlockedAdd(kCountDecalsCulled, 1, scratch);
		}
		// The decal's own class and its pipeline's index there; a drawable decal whose class has no sub-range in its group (or no class map
		// word) is dropped, counted: it writes fillers alone. The group's range slot is ClassRangeOfSlot(group - 1) = group.
		const uint classes = ClassCount();
		uint decalClass = 0, decalIndex = 0;
		bool decalPlanned = drawable && SetClassOf(input.x, decalClass, decalIndex);
		if (decalPlanned)
			decalPlanned = decalClass < classes && ClassRange(min(decalClass, classes - 1), decalGroup).y != 0;
		if (drawable && !decalPlanned)
			count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
		const uint4 rows = RowsOf(input.y);
		const uint decalStreamIndex = inputs.Load(inputOffset + 44);
		// Blood decals: a multi-partition skinned decal (BSTempEffectGeometryDecal copies its actor's dismember skin) takes a slot per
		// partition from its ordinal (OrderDecals, DecalSpanOf; encoding in word +40); a partition its mask leaves out writes a zero-count draw.
		const uint decalPartitions = inputs.Load(inputOffset + 40);
		const bool decalChain = (decalPartitions & kPartitionChain) != 0;
		const uint decalSpan = decalChain                                                 ? (decalPartitions & kPartitionChainCount) :
		                       (decalPartitions == 0 || decalPartitions == kNoPartitions) ? 1u :
		                                                                                    firstbithigh(decalPartitions & (kNoPartitions - 1)) + 1;
		uint decalGeometry = input.z;
		[loop] for (uint k = 0; k < decalSpan; ++k)
		{
			if (decalOrdinal + k >= DecalStride)
				break;  // never: the range holds every slot of the group
			const bool partitionDrawn = decalChain || decalPartitions == 0 || ((decalPartitions >> k) & 1) != 0;
			const uint geometryOffset = (decalGeometry != kNoPartition ? decalGeometry : input.z) * kGeometryStride;
			const uint4 vertexBuffer = geometries.Load4(geometryOffset);
			const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);
			const uint firstIndex = geometries.Load(geometryOffset + 32);
			const uint4 decalStream = decalStreamIndex != kNoStream ? geometries.Load4(decalStreamIndex * kGeometryStride) : vertexBuffer;
			const uint indexCount = (culled || !partitionDrawn || decalGeometry == kNoPartition) ? 0 : indexBuffer.w;
			[loop] for (uint c = 0; c < classes; ++c)
			{
				const uint2 range = ClassRange(c, decalGroup);
				if (range.y == 0 || decalOrdinal + k >= range.y)
					continue;  // no sub-range of the group (or, never, one short of the slot)
				const bool own = decalPlanned && c == decalClass;
				StoreSequence(sequences, range.x + decalOrdinal + k, own ? decalIndex : 0u, rows, objectWord, vertexBuffer, decalStream, indexBuffer,
					own ? indexCount : 0u, firstIndex);
			}
			if (decalGeometry != kNoPartition)
				decalGeometry = geometries.Load(geometryOffset + 36);  // GeometryDraw::nextPartition
		}
		// A decal's word: never drawn in depth, drawn in colour unless culled.
		if (phase == kPhaseColour)
			visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | (culled ? kVisibilityRejectedFinal : (kVisibilityVisible | kVisibilityColourDrawn)));
		return;
	}

	// A forward input is culled once, in the colour segment; its visibility word as a decal's.
	const bool forwardInput = keyWord == 0 && (input.w & kObjectForward) != 0;
	const bool refractionInput = forwardInput && (inputs.Load(inputOffset + 16) & kViewRefraction) != 0;
	// The first-person view's forward input is culled below with its other inputs.
	const bool firstPersonForward = forwardInput && !refractionInput && phase == kPhaseSingle && (ViewBits & kViewFirstPerson) != 0;
	if (forwardInput && !firstPersonForward) {
		if (phase != kPhaseColour || (refractionInput ? RefractionCapacity == 0 || PoolFirstsIndex == 0 : ForwardCapacity == 0))
			return;  // the depth segment never submits one; a shape without the range has no forward (or refraction) draws
		bool forwardCulled = !drawable;
		if (drawable && CullMode() != 0) {
			count.InterlockedAdd(refractionInput ? kCountRefractionTested : kCountForwardTested, 1, scratch);
			const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);
			forwardCulled = Culled(bound.xyz, bound.w);
			if (!forwardCulled) {
				count.InterlockedAdd(refractionInput ? kCountRefractionInView : kCountForwardInView, 1, scratch);
				if (CullMode() >= 2 && Occluded(bound.xyz, bound.w)) {
					forwardCulled = true;
					count.InterlockedAdd(refractionInput ? kCountRefractionOccluded : kCountForwardOccluded, 1, scratch);
				}
			}
		}
		// Refraction normals take no root fade (GetRenderPasses' refraction pass has no alpha): under a root whose OnVisible stopped
		// (serviced, not drawn) nothing is drawn, as the engine registers nothing there.
		if (refractionInput && !forwardCulled && FadeRootsIndex != 0 && FadeStatesIndex != 0) {
			const uint refractionRoot = inputs.Load(inputOffset + 48);
			if (refractionRoot != 0xFFFFFFFFu) {
				StructuredBuffer<FadeRootStatic> fadeRoots = ResourceDescriptorHeap[FadeRootsIndex];
				StructuredBuffer<FadeNodeState> fadeStates = ResourceDescriptorHeap[FadeStatesIndex];
				const FadeRootStatic rootRow = fadeRoots[refractionRoot];
				const FadeNodeState rootState = fadeStates[refractionRoot];
				if (rootState.Generation == rootRow.Generation && (rootState.Verdict & kFadeVerdictServiced) != 0 && (rootState.Verdict & kFadeVerdictDrawn) == 0) {
					forwardCulled = true;
					count.InterlockedAdd(kCountRefractionOccluded, 1, scratch);
				}
			}
		}
		visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | (forwardCulled ? kVisibilityRejectedFinal : (kVisibilityVisible | kVisibilityColourDrawn)));
		if (forwardCulled)
			return;
	}

	// Phase 2 revisits only phase 1's provisional rejections; the colour segment draws what the phases decided. Reading the decision
	// keeps depth and colour drawing the same set: under EQUAL, colour without depth is invisible, depth without colour is a hole.
	const uint published = visibility.Load(objectIndex * 4);
	const bool publishedThisFrame = (published >> kVisibilityStampShift) == VisibilityStamp;
	const uint verdict = publishedThisFrame ? (published & kVisibilityVerdictMask) : kVisibilityVisible;
	if (phase == kPhaseTwo && !(publishedThisFrame && verdict == kVisibilityOccludedRetest))
		return;
	if (phase == kPhaseColour && !forwardInput && verdict != kVisibilityVisible)
		return;

	bool cullRejected = false, frustumRejected = false, occlusionRejected = false;
	if (phase != kPhaseColour && CullMode() != 0) {
		// Word 2 separates "the culling rejected nothing" from "the culling did not run".
		count.InterlockedAdd(kCountTested, 1, scratch);
		const float4 bound = ObjectRow(objectIndex, kObjectBoundRow);  // centre (world), radius
		// Phase 2 only revisits occlusion.
		frustumRejected = phase == kPhaseOne ? MainFrustumRejected(objectIndex) : (phase != kPhaseTwo && Culled(bound.xyz, bound.w));
		// A portal-strict point light's view: its cull's rooms, shared nodes and room frusta (StrictGateRejects), by the caster's gate.
		if (!frustumRejected && phase == kPhaseSingle && StrictContext != 0)
			frustumRejected = StrictGateRejects(StrictGatesIndex, LightContextsIndex, ObjectGatesIndex, StrictContext - 1, objectIndex, bound.xyz, bound.w);
		if (frustumRejected) {
			count.InterlockedAdd(kCountCulled, 1, scratch);
		} else if (CullMode() >= 2 && Occluded(bound.xyz, bound.w)) {
			occlusionRejected = true;
			count.InterlockedAdd(kCountOccluded, 1, scratch);
		}
		cullRejected = frustumRejected || occlusionRejected;
	}
	// The visibility feedback: frustum only (occlusion does not stop the engine's OnVisible), every phase 1 candidate.
	bool fadeHidden = false;
	if (FrustumIndex != 0 && phase == kPhaseOne && !frustumRejected)
		fadeHidden = StampFrustum(inputs, inputOffset, input.w, objectIndex, true);
	// BSTreeNode::OnVisible draws nothing of a tree whose root is above the frame's height limit.
	if (phase == kPhaseOne && !frustumRejected && (input.w & kObjectHeightTest) != 0) {
		const float3 root = ObjectRow(objectIndex, kObjectFadeNodeRow).xyz;
		fadeHidden = fadeHidden || root.z - TreeHeight.x > TreeHeight.y;
	}
	// Final, like the frustum's: phase 2 never revisits it.
	cullRejected = cullRejected || fadeHidden;

	const bool draws = !cullRejected && drawable;

	// Publish the decision and the depth-drawn bit. Phase 1 writes every candidate, so nothing stale survives into the colour segment.
	if (phase == kPhaseOne || phase == kPhaseTwo) {
		const uint decided = (frustumRejected || fadeHidden) ? kVisibilityRejectedFinal :
			(occlusionRejected ? (phase == kPhaseOne ? kVisibilityOccludedRetest : kVisibilityRejectedFinal) : kVisibilityVisible);
		visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | decided | (draws ? kVisibilityDepthDrawn : 0));
		if (phase == kPhaseTwo && !occlusionRejected)
			count.InterlockedAdd(kCountRescuedByPhaseTwo, 1, scratch);
	}
	if (phase == kPhaseColour && draws && !forwardInput) {
		if (publishedThisFrame)
			visibility.InterlockedOr(objectIndex * 4, kVisibilityColourDrawn, scratch);
		else
			visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | kVisibilityColourDrawn | kVisibilityUnpublished);
	}

	if (cullRejected)
		return;
	// Cull-only inputs have nothing to draw.
	if (!drawable)
		return;
	const uint pipeline = DrawPipeline(drawKey);  // a shadow view's: the draw's bucket
	// A draw's bucket: a shadow view's is its pipeline map entry; with a slot map (depth segment, reflection faces), the pipeline slot's
	// (rows' top 12 bits) entry: the group sharing its depth pipeline, or a reflection's forward pipeline for LOD slots; kNoPipeline: none.
	uint bucketKey = pipeline;
	if (BucketMapOffset != 0) {
		ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
		bucketKey = latch.Load(BucketMapOffset + (rowsWord >> 20) * 4);
	}
	if (pipeline == kNoPipeline || bucketKey == kNoPipeline)
		return;
	// kBucketForward entries take forward inputs only, and forward inputs only those.
	const bool forwardBucket = BucketMapOffset != 0 && (bucketKey & kBucketForward) != 0;
	bucketKey &= ~kBucketForward;
	if (forwardBucket != firstPersonForward)
		return;
	// The draw's state class: the colour segment's ranges and a view's forward range are split by class (a device-generated call per
	// class sub-range, with that class's execution set), and the sequence names its pipeline by its index in the class. Both come from
	// the input's own set index through the latch's class map, so the stored index and the sub-range cannot disagree. Refraction draws
	// are plain draws (no set). A draw without a class sub-range is dropped and counted, never drawn with another class's state.
	const bool classed = (phase == kPhaseColour && keyWord == 0 && !refractionInput) || forwardBucket;
	uint setClass = 0, classIndex = 0;
	if (classed) {
		bool planned = SetClassOf(drawKey, setClass, classIndex);
		if (planned && !forwardBucket)
			planned = setClass < ClassCount();
		if (!planned) {
			count.InterlockedAdd(kCountClassUnplanned, 1, scratch);
			return;
		}
	}
	// A view's forward range has an entry per class, from the one its map word names (PlanViewBuckets).
	if (forwardBucket)
		bucketKey += setClass;
	// Terrain group 1's bucket and twin (PlanZBuckets): near blocks go to the twin (drawn into Terrain Blending's depth and, offset,
	// the main depth), far blocks to the bucket (main depth alone), as Terrain Blending's depth hook does.
	if ((bucketKey & kBucketTerrainPair) != 0) {
		bucketKey &= ~kBucketTerrainPair;
		if (TerrainNear.w > 0.0 && TerrainNearOf(ObjectRow(objectIndex, kObjectBoundRow)))
			bucketKey += 1;
	}
	// The colour segment's terrain groups append to their own ranges, unsorted.
	const uint terrainGroup = phase == kPhaseColour && keyWord == 0 ? TerrainGroup(input.w) : 0;
	// Phase 2's sequences are few and draw from their own range, unsorted.
	const bool secondRange = phase == kPhaseTwo;
	const bool sorted = SortCountsIndex != 0 && !secondRange;
	uint2 bucket = uint2(0, 0);
	if (BucketTableOffset != 0) {
		ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
		bucket = latch.Load2(BucketTableOffset + bucketKey * 8);
	}
	// The sort key: the class's first key plus the pipeline's index in it, so a class's draws sort within its keys (SortSequencesCS).
	const uint sortKey = classed ? ClassKeyBase(setClass) + classIndex : pipeline;
	if (sorted && sortKey >= kSortKeys)
		return;  // outside the sets, like kNoPipeline; before any slot is taken, so the sorted range has no hole

	const uint4 rows = RowsOf(rowsWord);

	// One draw per partition the mask names (walking the GeometryDraw links); all are one object with one visibility word.
	uint copyPartitions;
	const uint basePartitions = LodPartitions(inputs.Load(inputOffset + 40), inputs.Load(inputOffset + 56), inputs.Load(inputOffset + 48), keyWord == 0, copyPartitions);
	if (basePartitions == kNoPartitions)
		return;
	// Cross-fade copy partitions draw with the rest, marked kObjectCrossfadeCopy.
	const uint partitions = basePartitions | copyPartitions;
	// The second stream is the same for every partition (a face shape's positions are the whole shape's).
	const uint streamIndex = inputs.Load(inputOffset + 44);
	const uint4 stream = streamIndex != kNoStream ? geometries.Load4(streamIndex * kGeometryStride) : uint4(0, 0, 0, 0);
	uint geometryIndex = input.z;
	const bool chain = (partitions & kPartitionChain) != 0;
	const uint links = chain ? (partitions & kPartitionChainCount) : kMaxPartitions;
	[loop] for (uint partition = 0; partition < links && geometryIndex != kNoPartition; ++partition)
	{
		const uint geometryOffset = geometryIndex * kGeometryStride;
		if (chain || partitions == 0 || ((partitions >> partition) & 1) != 0) {
			const uint drawWord = ((copyPartitions >> partition) & 1) != 0 ? (objectWord | kObjectCrossfadeCopy) : objectWord;
			const uint4 vertexBuffer = geometries.Load4(geometryOffset);      // address lo, hi, size, stride
			const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);  // address lo, hi, size, index count
			const uint firstIndex = geometries.Load(geometryOffset + 32);

			// Each range holds every draw it can receive; the guards keep each inside its own range regardless.
			const uint4 secondStream = streamIndex != kNoStream ? stream : vertexBuffer;
			uint slot;
			if (BucketTableOffset != 0) {
				// Bucketed (shadow view, depth segment): the bucket's next slot. One plain indexed indirect draw per bucket executes the range
				// (ShadowViewPass, MainOpaquePass); the slot is the instance by which the pulled vertex stage finds the sequence (DCLF_PULLED).
				RWByteAddressBuffer bucketCounts = ResourceDescriptorHeap[BucketCountsIndex];
				bucketCounts.InterlockedAdd(bucketKey * 4, 1, slot);
				if (slot < bucket.y) {
					uint drawn;
					count.InterlockedAdd(phase == kPhaseTwo ? kCountDrawnPhaseTwo : kCountDrawn, 1, drawn);
					ByteAddressBuffer poolFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
					if (forwardBucket) {
						// First-person forward staging: a main-format sequence with its sort key, as the colour segment's forward draws below,
						// in its class's entry (its pipeline its index there).
						StoreSequence(sequences, bucket.x + slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
						StageForwardKey(sequences, bucket.x + slot, objectIndex);
					} else {
						StoreShadowSequence(sequences, bucket.x + slot, slot, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w,
							firstIndex, poolFirsts.Load(geometryIndex * 4));
					}
				}
			} else if (refractionInput) {
				// The refraction range: a pooled sequence, its slot the instance the pulled vertex stage finds it by (DCLF_REFRACTION).
				count.InterlockedAdd(kCountRefraction, 1, slot);
				if (slot >= RefractionCapacity)
					return;
				ByteAddressBuffer refractionFirsts = ResourceDescriptorHeap[PoolFirstsIndex];
				StoreShadowSequence(sequences, ForwardBase + 2 * ForwardCapacity + slot, slot, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w,
					firstIndex, refractionFirsts.Load(geometryIndex * 4));
			} else if (forwardInput) {
				// Its class's forward staging half, with the sort key (StageForwardKey); the forward inputs' draws tallied in kCountForward.
				count.InterlockedAdd(kCountForward, 1, scratch);
				if (!TakeClassSlot(setClass, kClassRangeForward, slot))
					return;
				StoreSequence(sequences, slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
				StageForwardKey(sequences, slot, objectIndex);
			} else if (terrainGroup != 0) {
				// Its class's sub-range of its terrain group's range (kTerrainGroups: range slot kDecalGroups + group - 1), unsorted; the
				// group's draws tallied in its old count word.
				count.InterlockedAdd(kCountTerrainGroup1 + (terrainGroup - 1) * 4, 1, scratch);
				if (!TakeClassSlot(setClass, 3 + terrainGroup, slot))
					return;
				StoreSequence(sequences, slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
			} else if (classed) {
				// The colour draws: their class's main sub-range (its capacity first, so the staging below takes only what the scatter
				// places), sorted within the class by pipeline; the draws tallied in kCountDrawn, which also counts the staged ones.
				uint classSlot;
				if (!TakeClassSlot(setClass, kClassRangeMain, classSlot))
					return;
				if (sorted) {
					RWByteAddressBuffer sortCounts = ResourceDescriptorHeap[SortCountsIndex];
					RWByteAddressBuffer staging = ResourceDescriptorHeap[SortStagingIndex];
					RWByteAddressBuffer ranks = ResourceDescriptorHeap[SortRanksIndex];
					count.InterlockedAdd(kCountDrawn, 1, slot);
					if (slot >= PhaseTwoBase)
						return;  // never: the classes' main sub-ranges hold at most PhaseTwoBase draws (RequireMain)
					// The scatter reads the key back from the rank word (key top 12 bits, rank low 20), and the class's first slot and first
					// key from the staged instanceCount and firstInstance words, which it restores (1, 0).
					uint rank;
					sortCounts.InterlockedAdd(sortKey * 4, 1, rank);
					ranks.Store(slot * 4, (sortKey << kSortRankBits) | rank);
					StoreSequence(staging, slot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
					staging.Store(slot * kSequenceStride + 76, ClassRange(setClass, kClassRangeMain).x);
					staging.Store(slot * kSequenceStride + 88, ClassKeyBase(setClass));
				} else {
					count.InterlockedAdd(kCountDrawn, 1, scratch);
					StoreSequence(sequences, classSlot, classIndex, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
				}
			} else {
				// Any other unbucketed draw (none: every colour draw is classed, the depth phases and views are bucketed), appended unsorted.
				count.InterlockedAdd(secondRange ? kCountDrawnPhaseTwo : kCountDrawn, 1, slot);
				if (slot >= PhaseTwoBase)
					return;
				StoreSequence(sequences, slot + (secondRange ? PhaseTwoBase : 0), pipeline, rows, drawWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w,
					firstIndex);
			}
		}
		if (!chain && (partitions >> (partition + 1)) == 0)
			break;
		geometryIndex = geometries.Load(geometryOffset + 36);  // GeometryDraw::nextPartition
	}
}
