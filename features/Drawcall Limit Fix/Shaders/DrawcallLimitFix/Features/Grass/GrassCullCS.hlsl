// Drawcall Limit Fix: grass's instances culled for the main camera, in the depth segment's two phases like every other object
// (dclf-architecture.md, "Grass: the draws"; Features/Grass/Grass.h, GrassLayout.h).
//
// Phase 1 (before the Z-prepass's draw): one thread per instance slot. A slot no shape owns (owners ~0u), or past the draw row's
// instance slots (0: grass does not draw this frame), draws nothing. The rest take Grass Optimizations' tests
// (GrassOptimizations/GrassCullingCS.hlsl): the cost-biased distance, the frustum (on the instance's bound), the pixel size, the
// density thinning, the edge, distance, spawn and invisible fades; then the LOD tier, and the instance's frame values for the vertex
// stage (its wind scalars, fade and flags) before the occlusion verdict. Then the HZB the previous frame left (HzbTest.hlsli): one
// occluded goes on the retest list; one in view is appended to its bucket tier's visible range (draw slot bucket * 3 + tier).
//
// Phase 2 (after the HZB is rebuilt from this frame's depth): one thread per retest entry, tested again against the rebuilt HZB. One it
// brings back is appended to its draw slot's range after phase 1's (GrassArgsCS's phase 2 draws start at phase 1's count).
//
// The depth commit zeroes the header; GrassArgsCS's colour mode zeroes the counts after it has read them.
#include "DrawcallLimitFix/HzbTest.hlsli"

// Must match GrassCullConstants (Features/Grass/GrassTables.h).
cbuffer GrassCullConstants : register(b0)
{
	uint DrawIndex;       // ByteAddressBuffer: GrassInstances::DrawRow
	uint InstancesIndex;  // ByteAddressBuffer: GrassInstances::Instance per instance slot
	uint OwnersIndex;     // ByteAddressBuffer: the slot's shape slot (~0u: free)
	uint ShapesIndex;     // ByteAddressBuffer: GrassInstances::ShapeRow per shape slot
	uint BucketsIndex;    // ByteAddressBuffer: GrassInstances::BucketRow per bucket slot
	uint FrameIndex;      // RWByteAddressBuffer: GrassInstances::InstanceFrame per instance slot
	uint VisibleIndex;    // RWByteAddressBuffer: the visible entries, then the retest list
	uint DrawsIndex;      // RWByteAddressBuffer: the draws buffer (DrawsHeader, the counts, the arguments' arrays)
	uint LatchIndex;      // ByteAddressBuffer: the depth segment's BuildDrawsLatch (its view-projection, its HZB's UV scale)
	uint LatchOffset;
	uint Phase;           // 1 or 2
	uint RetestOffset;    // bytes: the retest list's first entry
	uint HzbIndex;        // Texture2D<float>: 0 when there is no HZB to test against
	uint HzbSizePacked;   // mip 0: width in the low 16 bits, height in the high 16
	uint HzbMips;
	uint Padding0;
}

// GrassLayout.h.
static const uint kInstanceBytes = 32;
static const uint kShapeRowBytes = 32;
static const uint kBucketRowBytes = 160;
static const uint kFrameBytes = 16;
static const uint kTiers = 3;
static const uint kBucketTierOne = 4;
static const uint kBucketTierTwo = 8;
static const uint kFrameCollision = 4;
static const uint kFrameSimpleShading = 8;
static const uint kCullMidLod = 1;
static const uint kCullFarLod = 2;
static const uint kCullOcclusion = 4;
// DrawsHeader.
static const uint kRetests = 16;
static const uint kTested = 20;
static const uint kPhaseOneInstances = 24;
static const uint kPhaseTwoInstances = 28;
static const uint kStatLive = 32;
static const uint kStatOutside = 36;
static const uint kStatThinned = 40;
static const uint kStatFaded = 44;
static const uint kCountsOffset = 64;
// DrawRow.
static const uint kRowInstanceSlots = 56;
static const uint kRowTimeBase = 80;
static const uint kRowEye = 224;
static const uint kRowCull = 256;

static const float kPi = 3.1415926535897932;
static const float kTau = 6.2831853071795864;

// The depth segment's view-projection (camera translation folded in), as BuildDrawsCS's.
float4x4 LatchViewProj()
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	return float4x4(asfloat(latch.Load4(LatchOffset + 32)), asfloat(latch.Load4(LatchOffset + 48)),
		asfloat(latch.Load4(LatchOffset + 64)), asfloat(latch.Load4(LatchOffset + 80)));
}

// The cull's sphere test against the main camera's frustum, as TreeLodCullCS's InView: its planes normalised; NoNearPlane (the
// latch's cull flags, bit 9) drops the near plane.
bool InView(float4x4 a_viewProj, float3 a_centre, float a_radius)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint cullFlags = latch.Load(LatchOffset + 16);
	const float4 r0 = a_viewProj[0], r1 = a_viewProj[1], r2 = a_viewProj[2], r3 = a_viewProj[3];
	float4 planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
	[unroll] for (uint p = 0; p < 6; ++p) {
		if (p == 4 && (cullFlags & 0x200u) != 0)
			continue;
		const float length = sqrt(dot(planes[p].xyz, planes[p].xyz));
		if (length <= 0.0f)
			continue;
		if ((dot(planes[p].xyz, a_centre) + planes[p].w) / length < -a_radius)
			return false;
	}
	return true;
}

// The HZB the phase tests against: the latch's cull mode 2 (frustum then the HZB), an HZB bound and Grass Optimizations' occlusion on.
bool Occluded(float4x4 a_viewProj, float3 a_centre, float a_radius, uint a_cullFlags)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	if ((latch.Load(LatchOffset + 16) & 0xFu) < 2 || (a_cullFlags & kCullOcclusion) == 0)
		return false;
	HzbView view;
	view.ViewProj = a_viewProj;
	view.Index = HzbIndex;
	view.BaseSize = HzbUnpackSize(HzbSizePacked);
	view.Mips = HzbMips;
	view.UvScale = HzbUnpackUvScale(latch.Load(LatchOffset + 24));
	return HzbTest(view, a_centre, a_radius).Occluded;
}

// Common/Random.hlsli's pcg2d: the density and LOD dither, by the global instance slot (stable while the shape lives).
uint2 Pcg2d(uint2 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v = v ^ (v >> 16u);
	v.x += v.y * 1664525u;
	v.y += v.x * 1664525u;
	v = v ^ (v >> 16u);
	return v;
}

// Hashed bits to a uniform value in [0, 1).
float RandFloat(uint a_bits)
{
	return asfloat((a_bits & 0x007FFFFFu) | 0x3F800000u) - 1.0;
}

// The per-instance wind term the vertex stage takes (Grass Optimizations' WindScalar).
float WindScalar(float a_basis, float a_timer)
{
	const float a = 0.4 * (a_basis + a_timer);
	float sa, ca;
	sincos(a, sa, ca);
	const float t3 = 0.2 * cos(kPi * ca);
	const float t1 = sin(kPi * sa);
	const float t2 = sin(kTau * sa);
	return (t1 + t2) * 0.3 + t3;
}

float2 Halfs(uint a_word)
{
	return float2(f16tof32(a_word & 0xFFFF), f16tof32(a_word >> 16));
}

// The cull's frame values (GrassInstances::CullRow).
struct Cull
{
	float MinPixelSize, FullDetailPx, MinKeep, FadeBand;
	float CostBias, CostBiasStart, ProjScale, MaxDistSq;
	float EdgeFadeStart, AlphaParam1, AlphaParam2, InvisibleFadeCull;
	float SimpleShadingPx, CollisionDistSq, MidLodPx, FarLodPx;
	float LodBandPx;
	uint Flags;
};

Cull LoadCull(ByteAddressBuffer a_draw)
{
	const float4 a = asfloat(a_draw.Load4(kRowCull));
	const float4 b = asfloat(a_draw.Load4(kRowCull + 16));
	const float4 c = asfloat(a_draw.Load4(kRowCull + 32));
	const float4 d = asfloat(a_draw.Load4(kRowCull + 48));
	const uint2 e = a_draw.Load2(kRowCull + 64);
	Cull cull;
	cull.MinPixelSize = a.x;
	cull.FullDetailPx = a.y;
	cull.MinKeep = a.z;
	cull.FadeBand = a.w;
	cull.CostBias = b.x;
	cull.CostBiasStart = b.y;
	cull.ProjScale = b.z;
	cull.MaxDistSq = b.w;
	cull.EdgeFadeStart = c.x;
	cull.AlphaParam1 = c.y;
	cull.AlphaParam2 = c.z;
	cull.InvisibleFadeCull = c.w;
	cull.SimpleShadingPx = d.x;
	cull.CollisionDistSq = d.y;
	cull.MidLodPx = d.z;
	cull.FarLodPx = d.w;
	cull.LodBandPx = asfloat(e.x);
	cull.Flags = e.y;
	return cull;
}

// An instance slot's owner: its shape's origin and spawn time, its bucket's row offset. False for a free slot.
bool Owner(uint a_index, out float3 a_origin, out float a_spawnStart, out uint a_bucket)
{
	a_origin = float3(0, 0, 0);
	a_spawnStart = 0;
	a_bucket = 0;
	ByteAddressBuffer owners = ResourceDescriptorHeap[OwnersIndex];
	const uint shape = owners.Load(a_index * 4);
	if (shape == 0xFFFFFFFFu)
		return false;
	ByteAddressBuffer shapes = ResourceDescriptorHeap[ShapesIndex];
	const uint base = shape * kShapeRowBytes;
	if (shapes.Load(base + 20) == 0)
		return false;
	a_origin = asfloat(shapes.Load3(base));
	a_bucket = shapes.Load(base + 12);
	a_spawnStart = asfloat(shapes.Load(base + 24));
	return true;
}

// The instance's bound (Grass Optimizations' occlusion sphere): the model bound's centre turned by the instance's rotation and grown
// by its scale variance, its radius the model's grown likewise.
void Bound(uint4 a_raw0, uint4 a_raw1, float3 a_world, float3 a_boundCenter, float a_modelRadius, out float3 a_centre, out float a_radius)
{
	const float sizeVariance = f16tof32(a_raw1.z >> 16);
	const float3 rot0 = float3(Halfs(a_raw0.z), f16tof32(a_raw0.w & 0xFFFF));
	const float3 rot1 = float3(Halfs(a_raw1.x), f16tof32(a_raw1.y & 0xFFFF));
	const float3 rot2 = float3(f16tof32(a_raw1.z & 0xFFFF), f16tof32(a_raw0.w >> 16), f16tof32(a_raw1.y >> 16));
	const float3 centre = a_boundCenter * (1.0 + max(sizeVariance, 0.0));
	a_centre = a_world + float3(dot(rot0, centre), dot(rot1, centre), dot(rot2, centre));
	a_radius = a_modelRadius * (1.0 + max(sizeVariance, 0.0)) + length(a_boundCenter) * abs(sizeVariance);
}

// The draw slot's count and the instance's entry in its bucket tier's visible range.
void Append(RWByteAddressBuffer a_draws, uint a_index, uint a_bucket, uint a_tier, uint a_counter)
{
	ByteAddressBuffer buckets = ResourceDescriptorHeap[BucketsIndex];
	RWByteAddressBuffer visible = ResourceDescriptorHeap[VisibleIndex];
	const uint slot = a_bucket * kTiers + a_tier;
	uint at, scratch;
	a_draws.InterlockedAdd(kCountsOffset + slot * 4, 1u, at);
	a_draws.InterlockedAdd(a_counter, 1u, scratch);
	visible.Store((buckets.Load(a_bucket * kBucketRowBytes + 16 + a_tier * 4) + at) * 4, a_index);
}

// Phase 2: a retest entry's instance against the rebuilt HZB, appended after phase 1's when it is in view now.
void Retest(uint a_id, RWByteAddressBuffer a_draws, Cull a_cull)
{
	RWByteAddressBuffer visible = ResourceDescriptorHeap[VisibleIndex];
	if (a_id >= a_draws.Load(kRetests))
		return;
	const uint index = visible.Load(RetestOffset + a_id * 4);
	float3 origin;
	float spawnStart;
	uint bucket;
	if (!Owner(index, origin, spawnStart, bucket))
		return;
	ByteAddressBuffer instances = ResourceDescriptorHeap[InstancesIndex];
	ByteAddressBuffer buckets = ResourceDescriptorHeap[BucketsIndex];
	RWByteAddressBuffer frames = ResourceDescriptorHeap[FrameIndex];
	const uint4 raw0 = instances.Load4(index * kInstanceBytes);
	const uint4 raw1 = instances.Load4(index * kInstanceBytes + 16);
	const uint bucketBase = bucket * kBucketRowBytes;
	const float3 world = origin + float3(Halfs(raw0.x), f16tof32(raw0.y & 0xFFFF));
	float3 centre;
	float radius;
	Bound(raw0, raw1, world, asfloat(buckets.Load3(bucketBase + 48)), asfloat(buckets.Load(bucketBase + 60)), centre, radius);
	if (Occluded(LatchViewProj(), centre, radius, a_cull.Flags))
		return;
	const uint tier = frames.Load(index * kFrameBytes + 12) & 3u;
	Append(a_draws, index, bucket, tier, kPhaseTwoInstances);
}

// The LOD tier (Grass Optimizations' dither of both transitions by one value), clamped to the tiers the bucket has a mesh for.
uint Tier(Cull a_cull, float a_projPx, float a_dither, uint a_bucketFlags)
{
	const float halfBand = a_cull.LodBandPx * 0.5;
	const float bandRcp = 1.0 / max(a_cull.LodBandPx, 1e-4);
	uint tier = 0;
	if ((a_cull.Flags & kCullMidLod) != 0 && a_dither < saturate((a_cull.MidLodPx + halfBand - a_projPx) * bandRcp))
		tier = 1;
	if ((a_cull.Flags & kCullFarLod) != 0 && a_dither < saturate((a_cull.FarLodPx + halfBand - a_projPx) * bandRcp))
		tier = 2;
	if (tier == 2 && (a_bucketFlags & kBucketTierTwo) == 0)
		tier = 1;
	if (tier == 1 && (a_bucketFlags & kBucketTierOne) == 0)
		tier = 0;
	return tier;
}

[numthreads(64, 1, 1)] void main(uint a_id : SV_DispatchThreadID)
{
	ByteAddressBuffer draw = ResourceDescriptorHeap[DrawIndex];
	RWByteAddressBuffer draws = ResourceDescriptorHeap[DrawsIndex];
	const Cull cull = LoadCull(draw);
	if (Phase == 2) {
		Retest(a_id, draws, cull);
		return;
	}
	const uint index = a_id;
	float3 origin;
	float spawnStart;
	uint bucket;
	if (index >= draw.Load(kRowInstanceSlots) || !Owner(index, origin, spawnStart, bucket))
		return;
	ByteAddressBuffer buckets = ResourceDescriptorHeap[BucketsIndex];
	const uint bucketBase = bucket * kBucketRowBytes;
	if (buckets.Load(bucketBase + 28) == 0)
		return;  // not drawable yet (no mesh or texture)
	uint scratch;
	draws.InterlockedAdd(kStatLive, 1u, scratch);

	ByteAddressBuffer instances = ResourceDescriptorHeap[InstancesIndex];
	const uint4 raw0 = instances.Load4(index * kInstanceBytes);
	const uint4 raw1 = instances.Load4(index * kInstanceBytes + 16);
	const float2 localXY = Halfs(raw0.x);
	const float3 world = origin + float3(localXY, f16tof32(raw0.y & 0xFFFF));
	const float3 eye = asfloat(draw.Load3(kRowEye));
	const float3 dv = world - eye;
	const float distSq = dot(dv, dv);
	const float dist = sqrt(distSq);

	// The mesh-cost bias, ramped in past its start distance: a costlier mesh is drawn less far and must be larger.
	const float costRamp = saturate((dist - cull.CostBiasStart) / max(cull.CostBiasStart, 1e-4));
	const float effCostBias = cull.CostBias * costRamp;
	const float dScale = lerp(1.0, asfloat(buckets.Load(bucketBase + 68)), effCostBias);
	const float effMaxDistSq = cull.MaxDistSq * dScale * dScale;
	float3 centre;
	float radius;
	Bound(raw0, raw1, world, asfloat(buckets.Load3(bucketBase + 48)), asfloat(buckets.Load(bucketBase + 60)), centre, radius);
	const float4x4 viewProj = LatchViewProj();
	if (distSq > effMaxDistSq || !InView(viewProj, centre, radius)) {
		draws.InterlockedAdd(kStatOutside, 1u, scratch);
		return;
	}

	// The pixel size and the density thinning (by the global slot: stable while the shape lives).
	const float sizeVariance = f16tof32(raw1.z >> 16);
	const float instanceRadius = asfloat(buckets.Load(bucketBase + 60)) * (1.0 + max(sizeVariance, 0.0));
	const float projPx = (instanceRadius / max(dist, 1e-4)) * cull.ProjScale;
	const float pxScale = lerp(1.0, asfloat(buckets.Load(bucketBase + 72)), effCostBias);
	const float effMinPx = cull.MinPixelSize * pxScale;
	const uint2 rand = Pcg2d(uint2(index, 0u));
	float lodFade = 1.0;
	const float effFullPx = cull.FullDetailPx * pxScale;
	bool thinned = projPx < effMinPx;
	if (!thinned && projPx < effFullPx) {
		const float t = saturate((effFullPx - projPx) / max(effFullPx - effMinPx, 1e-4));
		const float keep = lerp(1.0, cull.MinKeep, t);
		const float h = RandFloat(rand.x);
		thinned = h > keep + cull.FadeBand;
		lodFade = saturate((keep + cull.FadeBand - h) / max(cull.FadeBand, 1e-4));
	}
	if (thinned) {
		draws.InterlockedAdd(kStatThinned, 1u, scratch);
		return;
	}

	// The fades: the edge of the (cost-biased) distance, the engine's distance fade, the spawn fade-in.
	const float maxDist = sqrt(effMaxDistSq);
	const float edgeStart = maxDist * cull.EdgeFadeStart;
	const float edgeFade = saturate((maxDist - dist) / max(maxDist - edgeStart, 1e-4));
	const float4 clip = mul(viewProj, float4(world, 1.0));
	const float distFade = 1.0 - saturate((length(clip.xyz) - cull.AlphaParam1) / cull.AlphaParam2);
	const float4 times = asfloat(draw.Load4(kRowTimeBase));  // timeBase, prevTimeBase, fadeNow, fadeInTimeRcp
	const float spawnFade = saturate((times.z - spawnStart) * times.w);
	const float fade = distFade * spawnFade * lodFade * edgeFade;
	if (fade <= cull.InvisibleFadeCull) {
		draws.InterlockedAdd(kStatFaded, 1u, scratch);
		return;
	}

	// The instance's frame values, for the vertex stage, whatever the occlusion says (phase 2 reads its tier).
	const uint tier = Tier(cull, projPx, RandFloat(rand.y), buckets.Load(bucketBase + 12));
	const float wavePeriod = asfloat(buckets.Load(bucketBase + 64));
	const float basis = (localXY.x + localXY.y) * -0.0078125;
	uint flags = tier;
	flags |= distSq < cull.CollisionDistSq ? kFrameCollision : 0u;
	flags |= cull.SimpleShadingPx > 0.0 && projPx < cull.SimpleShadingPx ? kFrameSimpleShading : 0u;
	RWByteAddressBuffer frames = ResourceDescriptorHeap[FrameIndex];
	frames.Store4(index * kFrameBytes, uint4(asuint(WindScalar(basis, times.x * wavePeriod)), asuint(WindScalar(basis, times.y * wavePeriod)), asuint(fade), flags));

	draws.InterlockedAdd(kTested, 1u, scratch);
	if (Occluded(viewProj, centre, radius, cull.Flags)) {
		RWByteAddressBuffer visible = ResourceDescriptorHeap[VisibleIndex];
		uint at;
		draws.InterlockedAdd(kRetests, 1u, at);
		visible.Store(RetestOffset + at * 4, index);
		return;
	}
	Append(draws, index, bucket, tier, kPhaseOneInstances);
}
