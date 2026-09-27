// Drawcall Limit Fix: writes the indirect draw sequences (DrawSequence, Records.h) of this frame's draws
// from the per-draw inputs and the geometry table, appending them with an atomic count. Compiled to SPIR-V at runtime
// (ComputeProgram) for the render graph, with BasicRHI's descriptor-heap ABI; buffers are fetched from the descriptor heap by index.

// Push constants: only what is fixed for a pass across executions (descriptor indices, addresses, the
// culling phase, the HZB's shape). Everything that changes from one execution to the next is read from
// the pass's region of the latch block (org::LatchBlock), which the host writes just before submission;
// the recorded commands then never change with the frame, which is what lets them be reused or recorded
// ahead of it. Must match BuildDrawsConstants and BuildDrawsLatch in IndirectDraws.cpp.
cbuffer BuildDrawsConstants : register(b0)
{
	uint LatchIndex;       // ByteAddressBuffer: the latch block
	uint InputsIndex;      // ByteAddressBuffer: DrawInput[DrawCount]
	uint GeometriesIndex;  // ByteAddressBuffer: GeometryDraw[]
	uint SequencesIndex;   // RWByteAddressBuffer: DrawSequence[]
	uint CountIndex;       // RWByteAddressBuffer: uint (zeroed before the dispatch)
	uint RecordsAddressLo;  // device address of DrawBindings[0]
	uint RecordsAddressHi;
	uint RecordStride;
	// Bits 4-7: the culling phase, which is the pass's; the mode and the native-visible gate are the
	// frame's and come from the latch.
	uint PhaseBits;
	// RWByteAddressBuffer: one uint per object in the frame's tables, written by the depth segment's two
	// culling phases and read by the colour segment. This is how the colour pass draws exactly what the
	// depth passes drew: the decision is made once, in the depth segment, and published here, instead of
	// each segment testing for itself and quietly disagreeing.
	uint VisibilityIndex;
	uint LatchOffset;  // this dispatch's BuildDrawsLatch, in bytes into the latch block
	// RWByteAddressBuffer: per object, the stamp of the last frame its bound was inside the main camera's frustum.
	// Written by the depth segment's first phase only (0 elsewhere): the visibility feedback (IndirectDraws).
	uint FrustumIndex;
	// Occlusion culling against the hierarchical depth buffer built at the end of the depth pass
	// (HzbCS.hlsl). HzbIndex is zero when there is no HZB, which includes the first frame.
	uint HzbIndex;
	uint HzbSizePacked;  // mip 0: width in the low 16 bits, height in the high 16
	uint HzbMips;
	// The sort by pipeline (IndirectDraws.cpp, SortDraws): with SortCountsIndex set, a sequence goes to its append slot of
	// the staging buffer, with its rank among its pipeline's sequences (the count before it, from the atomic on its pipeline's
	// word of SortCountsIndex). SortSequencesCS.hlsl then writes it to the sequences, grouped by pipeline, once the prefix
	// sum has turned the counts into each pipeline's first slot. 0: append into the sequences directly.
	uint SortCountsIndex;   // RWByteAddressBuffer: uint[kSortKeys], zero before the dispatch
	uint SortStagingIndex;  // RWByteAddressBuffer: DrawSequence[kPhaseTwoSequenceBase]
	uint SortRanksIndex;    // RWByteAddressBuffer: uint[kPhaseTwoSequenceBase]
}

// The execution's values, from the latch (BuildDrawsLatch): read once per thread at the top of main. The
// first three words of the latch are this dispatch's own indirect arguments.
static uint DrawCount;
// Bits 0-3 are the mode (0 off, 1 frustum, 2 frustum then the HZB), bits 4-7 the phase (from the push
// constants), bit 8 RequireNativeVisible, and bit 9 NoNearPlane.
static uint CullFlags;
// Stamps the verdicts this frame publishes. A word that does not carry the current stamp was not
// written this frame, and the colour segment then treats the object as visible rather than trusting a
// stale verdict. Visible is the safe default: drawing something the depth passes did not cover means
// the colour draw's EQUAL test fails and the native pass shades it, while believing a stale "hidden"
// would drop an object that has depth and no other draw left to shade it.
static uint VisibilityStamp;
// The HZB covers a power-of-two area that is larger than the rendered image, so a texture coordinate in
// the image is NOT one in the HZB. This is the ratio between them, per axis, as 16-bit fixed point.
// Leaving it out makes the culling sample the padding, which is all far plane, and the HZB then looks
// uniformly empty however correctly it was built.
static uint HzbUvScalePacked;
// The main pass's view-projection with the camera translation already folded into it, so a bound's
// absolute world position projects directly.
static float4x4 ViewProj;
// A shadow view's caster volume, as the engine culls its casters (NiCullingProcess::customCullPlanes):
// absolute world space, (normal, constant), inside where dot(normal, p) - constant >= 0. CullPlaneMask says
// which are tested; it is 0 for the main camera.
static uint CullPlaneMask;
static float4 CullPlanes[6];
// A shadow view's row of the pipeline map, in bytes into the latch block: its inputs name key slots, and
// the row holds each slot's pipeline under the view's rasterizer state (depth bias, culling). 0 when the
// inputs name pipelines themselves (the main camera).
static uint PipelineMapOffset;
// The main colour pass: kSunTestOn and the number of cascades (the latch's sunMasks and sunPlanes follow).
static uint SunState;
static const uint kSunTestOn = 0x80000000u;
// The depth segment: the main camera's position and its LOD factor, the fade test's (kObjectFadeTest).
static float4 FadeEye;
// The depth segment: the tree height test's base and limit (kObjectHeightTest); the limit is +infinity when it is off.
static float2 TreeHeight;

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
	FadeEye = asfloat(latch.Load4(LatchOffset + 1008));
	TreeHeight = asfloat(latch.Load2(LatchOffset + 200));
}

// A synthetic pass whose pipeline carries the sun's bits (kObjectSunTest): whether its bound meets any of this
// frame's cascades, with SunAccumulation's sphere test (outside a plane when dot(n, c) - d < -r) against each
// cascade's planes and custom planes, as the engine's cascade culls used them.
bool InSunCascades(float3 a_centre, float a_radius)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint cascades = min(SunState & 0xFu, 4u);
	[loop] for (uint c = 0; c < cascades; ++c) {
		const uint2 masks = latch.Load2(LatchOffset + 208 + c * 8);
		bool inside = true;
		[loop] for (uint p = 0; p < 12 && inside; ++p) {
			const uint mask = p < 6 ? masks.x : masks.y;
			if ((mask & (1u << (p % 6))) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(LatchOffset + 240 + (c * 12 + p) * 16));
			inside = dot(plane.xyz, a_centre) - plane.w >= -a_radius;
		}
		if (inside)
			return true;
	}
	return false;
}

// A view of the sun: whether an entry sphere is outside every one of the frame's full-frustum processes (the latch's sun entry
// block, IndirectDraws.cpp: BuildDrawsLatch::sunEntryPlanes). A count of 0: no verdict here.
bool OutsideSunEntry(float4 a_entry)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint processes = min(latch.Load(LatchOffset + 1024), 8u);
	if (processes == 0)
		return false;
	[loop] for (uint process = 0; process < processes; ++process) {
		const uint mask = latch.Load(LatchOffset + 1028 + process * 4);
		bool outside = false;
		[loop] for (uint p = 0; p < 6 && !outside; ++p) {
			if ((mask & (1u << p)) == 0)
				continue;
			const float4 plane = asfloat(latch.Load4(LatchOffset + 1072 + (process * 6 + p) * 16));
			outside = dot(plane.xyz, a_entry.xyz) - plane.w < -a_entry.w;
		}
		if (!outside)
			return false;
	}
	return true;
}

// The draw's pipeline: the input's own, or its key slot's through the view's row of the pipeline map.
// kNoPipeline when the row has none, which the CPU never lets an input reach; the draw is then dropped
// rather than executed with an index outside the set.
static const uint kNoPipeline = 0xFFFFFFFFu;
uint DrawPipeline(uint a_input)
{
	if (PipelineMapOffset == 0)
		return a_input;
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return latch.Load(PipelineMapOffset + a_input * 4);
}

uint CullMode() { return CullFlags & 0xF; }
// 1: the depth segment's first phase, against the HZB left by the previous frame. 2: its second phase,
// against the HZB just rebuilt from this frame's depth, over what phase 1 rejected. 3: the colour segment,
// which does not test anything and just reads what the two phases decided.
uint CullPhase() { return (CullFlags >> 4) & 0xF; }
static const uint kPhaseSingle = 0;
static const uint kPhaseOne = 1;
static const uint kPhaseTwo = 2;
static const uint kPhaseColour = 3;

// Visibility values. Frustum rejection is final - nothing about a later depth buffer can bring an object
// back inside the frustum - while an occlusion rejection in phase 1 is provisional, because phase 1 tested
// against a depth buffer from the previous frame.
static const uint kVisibilityOccludedRetest = 0;
static const uint kVisibilityVisible = 1;
static const uint kVisibilityRejectedFinal = 2;
// Written only by the colour segment, for an object it drew without a verdict published this frame.
static const uint kVisibilityUnpublished = 3;
// The word: the verdict in bits 0-1, whether the depth segment drew the object (bit 2) and whether the colour
// segment did (bit 3), and the stamp above them. The two drawn bits are what CS_DCLF_SET_PARITY reads back: an
// object with depth and no colour, or colour and no depth, is visible damage.
static const uint kVisibilityVerdictMask = 3;
static const uint kVisibilityDepthDrawn = 4;
static const uint kVisibilityColourDrawn = 8;
static const uint kVisibilityStampShift = 4;
// When set, only objects the engine's own culling kept (kObjectNativeVisible) may be drawn, which is what
// the native loop skips and therefore what the frame has to contain. When clear, the GPU culling alone
// decides and DCLF draws objects the engine culled. The counters are written either way, so the
// cross-tabulation below measures the culling against the engine even while the gate is on.
bool RequireNativeVisible() { return (CullFlags & 0x100) != 0; }
// A clamped shadow view (render mode 0xE) pancakes what lies in front of its near plane onto it
// (Utility.hlsl: RENDER_SHADOWMAP_CLAMPED), so a caster there still writes depth and must be kept.
bool NoNearPlane() { return (CullFlags & 0x200) != 0; }
// Which caster class a shadow view draws (IndirectDraws.cpp: kCullCastersOnly, kCullVolumetricOnly): a view of
// the volumetric lighting copy draws the volumetric-only casters alone, every other shadow view the rest.
bool CastersOnly() { return (CullFlags & 0x400) != 0; }
bool VolumetricOnly() { return (CullFlags & 0x800) != 0; }
// A view of the sun: an input whose entry is outside the sun's full-frustum processes is none of its casters
// (IndirectDraws.cpp: kCullSunEntry, kInputOutsideSunEntry).
bool SunEntry() { return (CullFlags & 0x1000) != 0; }
static const uint kInputOutsideSunEntry = 1u << 25;

uint2 HzbBaseSize() { return uint2(HzbSizePacked & 0xFFFF, HzbSizePacked >> 16); }
float2 HzbUvScale() { return float2(HzbUvScalePacked & 0xFFFF, HzbUvScalePacked >> 16) / 65535.0; }

// Object flags (Records.h), as the draw input carries them.
static const uint kObjectNativeVisible = 1u << 3;
// A synthetic main pass with the sun's bits (Records.h), and the object word's mark for a draw that misses every
// cascade (kObjectSunMiss), which the pixel stage reads (DCLFObjects.hlsli).
static const uint kObjectSunTest = 1u << 26;
static const uint kObjectSunMiss = 1u << 31;
// A resident object under a fade root (Records.h): the depth segment's first phase drops it past its fade-out distance
// unless it was in view last frame (FadeHidden).
static const uint kObjectFadeTest = 1u << 27;
// A resident tree (Records.h): phase 1 drops it where BSTreeNode::OnVisible's height test would.
static const uint kObjectHeightTest = 1u << 28;
// A volumetric-only caster (Records.h), drawn only by the views of the volumetric lighting copy.
static const uint kObjectVolumetricOnly = 1u << 23;
// A decal, with its group (1 = the engine's opaque decal group, 2 = the blended one) in bits 20-21.
// Decals are never occluders: they are not submitted to the depth segment at all, and the colour segment
// tests them ONCE, here, against the HZB rebuilt at the end of this frame's depth segment - which is final
// by then - and writes their sequences to fixed slots (kDecalSequenceBase) rather than appending them, so
// that overlapping decals are drawn in the engine's order every frame.
static const uint kObjectDecal = 1u << 7;
static const uint kObjectDecalGroupShift = 20;
// Set by the epoch rather than by the object: this input carries a usable bindings record, so a sequence
// may be written for it. The depth segment submits an input for EVERY candidate so that the culling covers
// them all and the published visibility is complete, but it only builds records for the ones it is allowed
// to draw - which is what the native loop is skipping. The rest are cull-only.
static const uint kInputDrawable = 1u << 16;

// Counter words in the count buffer (zeroed before the dispatch).
static const uint kCountDrawn = 0;          // sequences written
static const uint kCountCulled = 4;         // rejected by this frame's culling
static const uint kCountTested = 8;         // looked at by the culling at all
static const uint kCountEngineCulled = 12;  // candidates the engine culled and the gate dropped
static const uint kCountFalseNegative = 16; // the engine kept it and the culling rejected it: a defect
static const uint kCountRescued = 20;       // the engine culled it and the culling kept it
static const uint kCountOccluded = 24;      // rejected by the HZB rather than by the frustum
// What the HZB actually held under the objects that were tested, so that a suspicious rejection count can
// be told apart from an HZB that is uniformly empty or uniformly full without a texture readback.
static const uint kCountHzbNear = 28;       // the footprint's farthest depth was ~0: the build wrote nothing
static const uint kCountHzbFar = 32;        // it was ~1: all far plane, so nothing can ever be occluded
static const uint kCountHzbSampled = 36;    // footprints sampled, to give the two above a denominator
// One rejection, recorded in full (see Occluded).
static const uint kSampleTaken = 40;
static const uint kSampleFarthest = 44;
static const uint kSampleNearestZ = 48;
static const uint kSampleUvMin = 52;
static const uint kSampleUvMax = 56;
static const uint kSampleMip = 60;
static const uint kCountOccludedVisible = 64;  // the HZB rejected it and the engine had kept it: the win
static const uint kCountDrawnPhaseTwo = 68;    // sequences phase 2 appended, and the count its draw reads
static const uint kCountRescuedByPhaseTwo = 72;  // objects phase 1 rejected and the rebuilt HZB brought back
// Decals: the two groups' slot counts (uploaded by the CPU, read by their draws) and the culling's tallies.
static const uint kCountDecalGroup1 = 76;
static const uint kCountDecalGroup2 = 80;
static const uint kCountDecalsCulled = 84;
static const uint kCountDecalsTested = 88;
// The sun on the GPU: kObjectSunTest inputs the colour pass tested, and those that missed every cascade.
static const uint kCountSunTested = 92;
static const uint kCountSunMissed = 96;
// The fade test: flagged inputs phase 1 found in the frustum, and those it dropped.
static const uint kCountFadeTested = 100;
static const uint kCountFadeHidden = 104;

// The frustum stamp's word: the stamp in the low 28 bits (the latch's), and whether the fade test dropped the object in
// that frame. The visibility feedback compares the low bits (PrimaryCull::ConsumeFeedback).
static const uint kFrustumStampMask = 0x0FFFFFFFu;
static const uint kFrustumFadeHidden = 0x80000000u;

// Phase 2 writes into the far half of the sequence buffer. Its draw is recorded on the CPU, which cannot
// know how many sequences phase 1 wrote, so the two need ranges that are fixed in advance rather than one
// range shared by an atomic counter.
static const uint kPhaseTwoSequenceBase = 16384;  // kMaxDraws
// The decal ranges follow phase 2's: one range of kMaxDecalDraws slots per group, slot = the decal's
// ordinal in the engine's draw order (IndirectDraws.cpp keeps the CPU side of these in step).
static const uint kDecalSequenceBase = 32768;  // 2 * kMaxDraws
static const uint kMaxDecalDraws = 2048;
// The pipelines a sort distinguishes: the pipeline sets' capacity (DrawPipelines::kMaxPipelines).
static const uint kSortKeys = 4096;

// DrawInput: 64 bytes (pipeline/record/geometry/flags, the world-space bounding sphere, the object index, the
// decal ordinal, the skin partitions to draw, the GeometryDraw of a second vertex stream or ~0, and the fade row:
// the entry root's centre and the fade-out distance, for kObjectFadeTest).
static const uint kInputStride = 64;
// GeometryDraw: 40 bytes (vertex buffer view, index buffer view, index count, first index, next partition).
static const uint kGeometryStride = 40;
// A skin of several partitions is one object drawn once per partition the engine would draw: the input's
// mask names them (bit i = partition i), and each partition's GeometryDraw links to the next one's.
static const uint kMaxPartitions = 8;
static const uint kNoPartition = 0xFFFFFFFFu;
// DrawSequence: 84 bytes, 4-byte packed. Words 1-3 are the root constants (DrawBindings address, then
// the object index), which is why the object index sits between the record address and the vertex buffer.
// The second vertex buffer view (slot 1) is a face shape's positions (FaceSnapshots), else the first again.
static const uint kSequenceStride = 84;
static const uint kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT
static const uint kNoStream = 0xFFFFFFFFu;

// One sequence at a slot of the dispatch's sequence buffer.
void StoreSequence(RWByteAddressBuffer sequences, uint slot, uint pipeline, uint recordLo, uint recordHi, uint objectIndex, uint4 vertexBuffer,
	uint4 streamBuffer, uint4 indexBuffer, uint indexCount, uint firstIndex)
{
	const uint base = slot * kSequenceStride;
	sequences.Store(base + 0, pipeline);
	sequences.Store3(base + 4, uint3(recordLo, recordHi, objectIndex));
	sequences.Store4(base + 16, vertexBuffer);
	sequences.Store4(base + 32, streamBuffer);
	sequences.Store4(base + 48, uint4(indexBuffer.xyz, kIndexFormatR16));
	sequences.Store4(base + 64, uint4(indexCount, 1, firstIndex, 0));
	sequences.Store(base + 80, 0);
}

// The bounding sphere's world-space AABB, projected corner by corner. The box contains the sphere, so
// every test built on it errs towards keeping the object: an object is only rejected when all eight
// corners are outside the same clip plane, which no visible object can be.
//
// The tests are on the homogeneous coordinates, before any divide (-w <= x <= w, -w <= y <= w, 0 <= z <= w):
// each is linear in the point, so it holds for a corner behind the eye (w <= 0) as for one in front, and an
// object wholly behind the camera is outside the near plane. (It used to keep any object with a corner behind
// the eye, which kept everything behind the main camera, whose culling has no world-space planes: at Riverwood
// about 2,600 objects a frame, which the HZB could not test either.)
bool Culled(float3 boundCentre, float boundRadius)
{
	const float3 centre = boundCentre;
	[unroll] for (uint p = 0; p < 6; ++p) {
		if ((CullPlaneMask & (1u << p)) && dot(CullPlanes[p].xyz, centre) - CullPlanes[p].w < -boundRadius)
			return true;
	}
	float4 planes = float4(1, 1, 1, 1);  // all-corners-outside accumulators: -x, +x, -y, +y
	float2 depthPlanes = float2(1, 1);   // near, far
	[unroll] for (uint corner = 0; corner < 8; ++corner) {
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

// The bounding sphere's screen-space extent and its nearest depth, in the same clip space the draws use.
// Returns false when the projection cannot be trusted - a corner behind the near plane - so that the
// object is kept.
bool ScreenExtent(float3 boundCentre, float boundRadius, out float2 uvMin, out float2 uvMax, out float nearestZ)
{
	uvMin = float2(1, 1);
	uvMax = float2(0, 0);
	nearestZ = 1;
	const float3 centre = boundCentre;
	float2 ndcMin = float2(1e30, 1e30);
	float2 ndcMax = float2(-1e30, -1e30);
	float minZ = 1e30;
	[unroll] for (uint corner = 0; corner < 8; ++corner) {
		const float3 offset = float3((corner & 1) ? boundRadius : -boundRadius,
			(corner & 2) ? boundRadius : -boundRadius,
			(corner & 4) ? boundRadius : -boundRadius);
		const float4 clip = mul(ViewProj, float4(centre + offset, 1.0));
		if (clip.w <= 1e-4)
			return false;
		const float3 ndc = clip.xyz / clip.w;
		ndcMin = min(ndcMin, ndc.xy);
		ndcMax = max(ndcMax, ndc.xy);
		minZ = min(minZ, ndc.z);
	}
	// Clip space to texture space. The y axis flips: clip space is y-up, the depth buffer y-down.
	// Clip space to texture space. The y axis flips: the draws use a y-flipped viewport, which puts clip
	// y = +1 at the top of the image, where v = 0. Measured: the other orientation gives 571 false
	// negatives against 45, so this is not a guess.
	uvMin = saturate(float2(ndcMin.x, -ndcMax.y) * 0.5 + 0.5);
	uvMax = saturate(float2(ndcMax.x, -ndcMin.y) * 0.5 + 0.5);
	nearestZ = minZ;
	return true;
}

// True when every depth under the object's screen extent is NEARER than the object's nearest point, which
// means the object is entirely behind what has already been drawn.
//
// The HZB holds the FARTHEST depth under each texel, so one value per corner of the extent at a mip whose
// texels are large enough that four of them cover it. Taking the maximum of those four and requiring it to
// be nearer than the object is conservative twice over: the mip is a max reduction, and the object is
// represented by the nearest point of a box that already contains its sphere.
bool Occluded(float3 boundCentre, float boundRadius, bool nativeVisibleForSample)
{
	if (HzbIndex == 0 || HzbMips == 0)
		return false;
	float2 uvMin, uvMax;
	float nearestZ;
	if (!ScreenExtent(boundCentre, boundRadius, uvMin, uvMax, nearestZ))
		return false;
	if (nearestZ <= 0.0)
		return false;  // in front of the near plane: nothing can occlude it

	// Image texture space to HZB texture space, before anything is measured in HZB texels.
	const float2 scale = HzbUvScale();
	uvMin *= scale;
	uvMax *= scale;

	const uint2 baseSize = HzbBaseSize();
	const float2 extent = (uvMax - uvMin) * float2(baseSize);
	// A mip whose texels are at least half the extent, so the four corner taps cover the whole rectangle.
	int mip = (int)ceil(log2(max(max(extent.x, extent.y), 1.0)));
	mip = clamp(mip, 0, (int)HzbMips - 1);
	const int2 mipSize = max(int2(baseSize) >> mip, int2(1, 1));
	const int2 texelMin = clamp(int2(uvMin * float2(mipSize)), int2(0, 0), mipSize - 1);
	const int2 texelMax = clamp(int2(uvMax * float2(mipSize)), int2(0, 0), mipSize - 1);

	Texture2D<float> hzb = ResourceDescriptorHeap[HzbIndex];
	const float farthest = max(
		max(hzb.Load(int3(texelMin.x, texelMin.y, mip)), hzb.Load(int3(texelMax.x, texelMin.y, mip))),
		max(hzb.Load(int3(texelMin.x, texelMax.y, mip)), hzb.Load(int3(texelMax.x, texelMax.y, mip))));

	RWByteAddressBuffer counters = ResourceDescriptorHeap[CountIndex];
	uint scratch;
	counters.InterlockedAdd(kCountHzbSampled, 1, scratch);
	if (farthest <= 1e-6)
		counters.InterlockedAdd(kCountHzbNear, 1, scratch);
	else if (farthest >= 0.9999)
		counters.InterlockedAdd(kCountHzbFar, 1, scratch);

	// A margin, because the two sides of this comparison are not in the same space. nearestZ is raw clip
	// space, while the HZB holds what was actually stored, which the viewport depth range has scaled - and
	// the native depth pass and DCLF's own draws do not even use the same range ([0, 0.999968] against
	// [0, 0.999998]). The gap is small but it is systematically in the direction that culls, and it bites
	// hardest on flat objects lying against the surface behind them, whose nearest corner is barely in
	// front of their own stored depth. Erring towards drawing is free; erring the other way loses objects.
	const float kDepthMargin = 1e-4;
	const bool occluded = farthest < nearestZ - kDepthMargin;

	// One sample of a rejection, so that a suspicious count can be read rather than guessed at: what the
	// HZB held, what the object's nearest corner was, where it was sampled and at which level. Written by
	// whichever thread gets there first; only the first rejection of the dispatch lands.
	if (occluded) {
		uint previous;
		counters.InterlockedCompareExchange(kSampleTaken, 0, 1, previous);
		if (previous == 0) {
			counters.Store(kSampleFarthest, asuint(farthest));
			counters.Store(kSampleNearestZ, asuint(nearestZ));
			counters.Store(kSampleUvMin, (uint(saturate(uvMin.x) * 65535.0) & 0xFFFF) | (uint(saturate(uvMin.y) * 65535.0) << 16));
			counters.Store(kSampleUvMax, (uint(saturate(uvMax.x) * 65535.0) & 0xFFFF) | (uint(saturate(uvMax.y) * 65535.0) << 16));
			counters.Store(kSampleMip, uint(mip) | (uint(nativeVisibleForSample) << 8));
		}
	}
	return occluded;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	LoadLatch();
	const uint draw = dispatchID.x;
	if (draw >= DrawCount)
		return;

	ByteAddressBuffer inputs = ResourceDescriptorHeap[InputsIndex];
	ByteAddressBuffer geometries = ResourceDescriptorHeap[GeometriesIndex];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	RWByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	RWByteAddressBuffer visibility = ResourceDescriptorHeap[VisibilityIndex];

	const uint inputOffset = draw * kInputStride;
	const uint4 input = inputs.Load4(inputOffset);  // pipeline index, record index, geometry index, flags
	const uint objectIndex = inputs.Load(inputOffset + 32);
	const bool nativeVisible = (input.w & kObjectNativeVisible) != 0;
	// The word the draw's root constants carry: the object index, and kObjectSunMiss when a synthetic pass with the
	// sun's bits meets no cascade this frame. Tested and counted for every such input, drawn or not, so the count
	// compares with the CPU's over the same inputs.
	uint objectWord = objectIndex;
	if ((SunState & kSunTestOn) != 0 && (input.w & kObjectSunTest) != 0) {
		uint sunScratch;
		RWByteAddressBuffer sunCount = ResourceDescriptorHeap[CountIndex];
		sunCount.InterlockedAdd(kCountSunTested, 1, sunScratch);
		const float4 sunBound = asfloat(inputs.Load4(inputOffset + 16));
		if (!InSunCascades(sunBound.xyz, sunBound.w)) {
			objectWord |= kObjectSunMiss;
			sunCount.InterlockedAdd(kCountSunMissed, 1, sunScratch);
		}
	}
	const bool drawable = (input.w & kInputDrawable) != 0;
	const uint phase = CullPhase();
	uint scratch;

	// A shadow view draws one caster class: the mode's inputs hold both.
	const bool volumetricCaster = (input.w & kObjectVolumetricOnly) != 0;
	if ((CastersOnly() && volumetricCaster) || (VolumetricOnly() && !volumetricCaster))
		return;
	// The sun's entry rule: the input's entry sphere (its fade row) outside every full-frustum process of the frame, which the
	// view's latch holds as its cascade plane sets (IndirectDraws.cpp: SetSunEntryRow); or the CPU's verdict, where the frame
	// has more processes than the latch holds.
	if (SunEntry()) {
		if ((input.w & kInputOutsideSunEntry) != 0)
			return;
		if (OutsideSunEntry(asfloat(inputs.Load4(inputOffset + 48))))
			return;
	}

	// Decals: single-phase, fixed slot. Every decal input writes its slot, culled or not, so nothing a
	// previous frame left there can be executed: a culled or undrawable decal writes the same sequence
	// with an index count of zero, which the indirect draw fetches and skips.
	const uint decalGroup = (input.w & kObjectDecal) ? (input.w >> kObjectDecalGroupShift) & 3 : 0;
	if (decalGroup != 0) {
		if (phase != kPhaseColour && phase != kPhaseSingle)
			return;  // the depth segment never submits one; belt and braces
		const uint decalOrdinal = inputs.Load(inputOffset + 36);
		if (decalOrdinal >= kMaxDecalDraws)
			return;
		bool culled = !drawable;
		if (drawable && CullMode() != 0) {
			count.InterlockedAdd(kCountDecalsTested, 1, scratch);
			const float4 bound = asfloat(inputs.Load4(inputOffset + 16));
			culled = Culled(bound.xyz, bound.w) || (CullMode() >= 2 && Occluded(bound.xyz, bound.w, nativeVisible));
			if (culled)
				count.InterlockedAdd(kCountDecalsCulled, 1, scratch);
		}
		const uint geometryOffset = input.z * kGeometryStride;
		const uint4 vertexBuffer = geometries.Load4(geometryOffset);
		const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);
		const uint firstIndex = geometries.Load(geometryOffset + 32);
		const uint recordOffset = input.y * RecordStride;
		const uint recordLo = RecordsAddressLo + recordOffset;
		const uint recordHi = RecordsAddressHi + (recordLo < RecordsAddressLo ? 1 : 0);
		const uint decalStreamIndex = inputs.Load(inputOffset + 44);
		const uint4 decalStream = decalStreamIndex != kNoStream ? geometries.Load4(decalStreamIndex * kGeometryStride) : vertexBuffer;
		StoreSequence(sequences, kDecalSequenceBase + (decalGroup - 1) * kMaxDecalDraws + decalOrdinal, input.x, recordLo, recordHi, objectWord,
			vertexBuffer, decalStream, indexBuffer, culled ? 0 : indexBuffer.w, firstIndex);
		// A decal's word: never drawn in depth, drawn in colour unless culled.
		if (phase == kPhaseColour)
			visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | (culled ? kVisibilityRejectedFinal : (kVisibilityVisible | kVisibilityColourDrawn)));
		return;
	}

	// Phase 2 only revisits what phase 1 provisionally rejected, and the colour segment tests nothing at
	// all - it draws what the two phases decided. Reading the decision rather than repeating it is what
	// keeps the depth and colour passes drawing the same set; the colour pass tests depth EQUAL, so an
	// object it draws without matching depth is invisible, and one the depth pass writes without a colour
	// draw is a hole that the native pass can no longer fill.
	const uint published = visibility.Load(objectIndex * 4);
	const bool publishedThisFrame = (published >> kVisibilityStampShift) == VisibilityStamp;
	const uint verdict = publishedThisFrame ? (published & kVisibilityVerdictMask) : kVisibilityVisible;
	if (phase == kPhaseTwo && !(publishedThisFrame && verdict == kVisibilityOccludedRetest))
		return;
	if (phase == kPhaseColour && verdict != kVisibilityVisible)
		return;

	bool cullRejected = false, frustumRejected = false, occlusionRejected = false;
	if (phase != kPhaseColour && CullMode() != 0) {
		// Word 2 separates "the culling rejected nothing" from "the culling did not run".
		count.InterlockedAdd(kCountTested, 1, scratch);
		const float4 bound = asfloat(inputs.Load4(inputOffset + 16));  // centre (world), radius
		// Phase 2 has already had its frustum answer from phase 1 and only revisits occlusion.
		frustumRejected = phase != kPhaseTwo && Culled(bound.xyz, bound.w);
		if (frustumRejected) {
			count.InterlockedAdd(kCountCulled, 1, scratch);
		} else if (CullMode() >= 2 && Occluded(bound.xyz, bound.w, nativeVisible)) {
			occlusionRejected = true;
			count.InterlockedAdd(kCountOccluded, 1, scratch);
		}
		cullRejected = frustumRejected || occlusionRejected;
	}
	// The visibility feedback: the frustum alone (occlusion does not stop the engine's OnVisible), for every candidate
	// phase 1 tests, drawable or not.
	bool fadeHidden = false;
	if (FrustumIndex != 0 && phase == kPhaseOne && !frustumRejected) {
		RWByteAddressBuffer frustumStamps = ResourceDescriptorHeap[FrustumIndex];
		// A resident under a fade root (kObjectFadeTest), which nothing on the CPU services while it is out of view: past
		// its fade-out distance, BSFadeNode::OnVisible snaps the fade of a root that was not in view last frame to 0, and
		// keeps it at 0 once it has, so the draw is dropped. One that was in view and drawn fades out over frames in the
		// engine; it is drawn until the feedback has the root fading and the entry leaves residency.
		const float4 fade = asfloat(inputs.Load4(inputOffset + 48));
		if ((input.w & kObjectFadeTest) != 0 && fade.w != 0.0 && FadeEye.w != 0.0) {
			const uint previous = frustumStamps.Load(objectIndex * 4);
			const bool wasInView = (previous & kFrustumStampMask) == ((VisibilityStamp - 1) & kFrustumStampMask);
			const bool wasHidden = wasInView && (previous & kFrustumFadeHidden) != 0;
			// FUN_14147b110: the distance to the root's centre, scaled by the camera's LOD factor over the LOD type's
			// divisor (folded into a positive distance), or by a constant (a negative one).
			const float distance = length(fade.xyz - FadeEye.xyz) * (fade.w > 0.0 ? FadeEye.w : 1.0);
			fadeHidden = distance > abs(fade.w) && (!wasInView || wasHidden);
			count.InterlockedAdd(kCountFadeTested, 1, scratch);
			if (fadeHidden)
				count.InterlockedAdd(kCountFadeHidden, 1, scratch);
		}
		frustumStamps.Store(objectIndex * 4, VisibilityStamp | (fadeHidden ? kFrustumFadeHidden : 0));
	}
	// BSTreeNode::OnVisible draws nothing of a tree whose root is above the frame's height limit.
	if (phase == kPhaseOne && !frustumRejected && (input.w & kObjectHeightTest) != 0) {
		const float3 root = asfloat(inputs.Load3(inputOffset + 48));
		fadeHidden = fadeHidden || root.z - TreeHeight.x > TreeHeight.y;
	}
	// A final verdict, like the frustum's: the colour segment reads it, and phase 2 never revisits it.
	cullRejected = cullRejected || fadeHidden;

	// The engine's own decision is a useful reference, but only for the frustum test, where the engine is
	// exact and the two should agree: rejecting something it kept is then a defect, and is counted as one.
	//
	// It is NOT a reference for occlusion. The engine's occlusion is planes, boxes and room/portal
	// visibility, all of which keep plenty of geometry that is in fact hidden - that is the whole reason
	// for testing against a depth pyramid. An object the engine kept and the HZB rejected is therefore the
	// expected win, not a defect, and is counted separately so the two can never be confused. Whether such
	// a rejection was correct is a question about visibility, which only a depth test can answer
	// (CS_DCLF_CULL_VALIDATE), not one the engine's opinion can settle.
	if (frustumRejected && nativeVisible)
		count.InterlockedAdd(kCountFalseNegative, 1, scratch);
	if (occlusionRejected && nativeVisible)
		count.InterlockedAdd(kCountOccludedVisible, 1, scratch);
	if (!cullRejected && !nativeVisible && phase != kPhaseColour)
		count.InterlockedAdd(kCountRescued, 1, scratch);

	// Whether this dispatch appends a draw for the object: past the culling, the engine-visibility gate and
	// the bindings.
	const bool gated = RequireNativeVisible() && !nativeVisible;
	const bool draws = !cullRejected && !gated && drawable;

	// Publish the decision, and whether the depth segment drew the object. Phase 1 writes one for every
	// candidate, so the buffer is completely rewritten each frame and nothing stale survives into the colour
	// segment.
	if (phase == kPhaseOne || phase == kPhaseTwo) {
		const uint decided = (frustumRejected || fadeHidden) ? kVisibilityRejectedFinal :
			(occlusionRejected ? (phase == kPhaseOne ? kVisibilityOccludedRetest : kVisibilityRejectedFinal) : kVisibilityVisible);
		visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | decided | (draws ? kVisibilityDepthDrawn : 0));
		if (phase == kPhaseTwo && !occlusionRejected)
			count.InterlockedAdd(kCountRescuedByPhaseTwo, 1, scratch);
	}
	// The colour segment marks what it draws, in the same word.
	if (phase == kPhaseColour && draws) {
		if (publishedThisFrame)
			visibility.InterlockedOr(objectIndex * 4, kVisibilityColourDrawn, scratch);
		else
			visibility.Store(objectIndex * 4, (VisibilityStamp << kVisibilityStampShift) | kVisibilityColourDrawn | kVisibilityUnpublished);
	}

	if (cullRejected)
		return;
	if (gated) {
		if (phase != kPhaseColour)
			count.InterlockedAdd(kCountEngineCulled, 1, scratch);
		// The gate is about what may be DRAWN, so it must not change the published visibility: the colour
		// segment applies the same gate to the same objects and would otherwise disagree with itself.
		return;
	}
	// Cull-only inputs carry no bindings record, so there is nothing to draw even though the object is
	// visible and its visibility has been published.
	if (!drawable)
		return;
	const uint pipeline = DrawPipeline(input.x);
	if (pipeline == kNoPipeline)
		return;
	// Phase 2's sequences are few and draw from their own range; they are not sorted.
	const bool sorted = SortCountsIndex != 0 && phase != kPhaseTwo;
	if (sorted && pipeline >= kSortKeys)
		return;  // outside the sets, like kNoPipeline; before any slot is taken, so the sorted range has no hole

	// 64-bit record address = RecordsAddress + record index * RecordStride.
	const uint recordOffset = input.y * RecordStride;
	const uint recordLo = RecordsAddressLo + recordOffset;
	const uint recordHi = RecordsAddressHi + (recordLo < RecordsAddressLo ? 1 : 0);

	// One draw of the input's geometry, or - for a skin of several partitions - one per partition its mask
	// names, walking the partitions' GeometryDraw links. Every draw is the same object: one record, one
	// visibility word, one verdict.
	const uint partitions = inputs.Load(inputOffset + 40);
	// The second stream, the same for every partition: a face shape's positions are the whole shape's.
	const uint streamIndex = inputs.Load(inputOffset + 44);
	const uint4 stream = streamIndex != kNoStream ? geometries.Load4(streamIndex * kGeometryStride) : uint4(0, 0, 0, 0);
	uint geometryIndex = input.z;
	[loop] for (uint partition = 0; partition < kMaxPartitions && geometryIndex != kNoPartition; ++partition) {
		const uint geometryOffset = geometryIndex * kGeometryStride;
		if (partitions == 0 || ((partitions >> partition) & 1) != 0) {
			const uint4 vertexBuffer = geometries.Load4(geometryOffset);      // address lo, hi, size, stride
			const uint4 indexBuffer = geometries.Load4(geometryOffset + 16);  // address lo, hi, size, index count
			const uint firstIndex = geometries.Load(geometryOffset + 32);

			// Phase 2 appends into a reserved part of the same buffer, with a counter of its own, because its
			// draw is recorded separately and the offset a recorded draw starts at has to be known on the CPU.
			// The CPU caps the templates at kMaxDraws; the guard keeps a phase inside its own range regardless.
			uint slot;
			count.InterlockedAdd(phase == kPhaseTwo ? kCountDrawnPhaseTwo : kCountDrawn, 1, slot);
			if (slot >= kPhaseTwoSequenceBase)
				return;
			const uint4 secondStream = streamIndex != kNoStream ? stream : vertexBuffer;
			if (sorted) {
				RWByteAddressBuffer sortCounts = ResourceDescriptorHeap[SortCountsIndex];
				RWByteAddressBuffer staging = ResourceDescriptorHeap[SortStagingIndex];
				RWByteAddressBuffer ranks = ResourceDescriptorHeap[SortRanksIndex];
				// The scatter reads the key back from the rank word (the key in the high 16 bits, the rank below: a rank is
				// under kPhaseTwoSequenceBase), so that the key need not be a field of the sequence.
				const uint key = pipeline;
				uint rank;
				sortCounts.InterlockedAdd(key * 4, 1, rank);
				ranks.Store(slot * 4, (key << 16) | rank);
				StoreSequence(staging, slot, pipeline, recordLo, recordHi, objectWord, vertexBuffer, secondStream, indexBuffer, indexBuffer.w, firstIndex);
			} else {
				StoreSequence(sequences, slot + (phase == kPhaseTwo ? kPhaseTwoSequenceBase : 0), pipeline, recordLo, recordHi, objectWord, vertexBuffer,
					secondStream, indexBuffer, indexBuffer.w, firstIndex);
			}
		}
		if ((partitions >> (partition + 1)) == 0)
			break;
		geometryIndex = geometries.Load(geometryOffset + 36);  // GeometryDraw::nextPartition
	}
}
