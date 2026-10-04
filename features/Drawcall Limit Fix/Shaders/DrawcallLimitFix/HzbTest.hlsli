// Drawcall Limit Fix: the occlusion test against the hierarchical depth buffer (HzbCS.hlsl), shared by everything the main
// camera's two-phase culling tests: BuildDrawsCS's objects (object and terrain LOD among them) and TreeLodCullCS's instances.
// Phase 1 tests against the HZB the previous frame left, phase 2 what phase 1 rejected against the one rebuilt from this
// frame's depth (Passes.cpp, "The two-phase tail").
#ifndef DCLF_HZB_TEST_HLSLI
#define DCLF_HZB_TEST_HLSLI

struct HzbView
{
	// The main pass's view-projection with the camera translation folded in, so an absolute world position projects directly.
	float4x4 ViewProj;
	uint Index;       // Texture2D<float>: the HZB; 0 when there is none to test against
	uint2 BaseSize;   // mip 0's
	uint Mips;
	// The HZB covers a power-of-two area larger than the rendered image, so a texture coordinate in the image is NOT one in
	// the HZB: the ratio between them, per axis (BuildDrawsLatch::hzbUvScalePacked, 16-bit fixed point).
	float2 UvScale;
};

uint2 HzbUnpackSize(uint a_packed) { return uint2(a_packed & 0xFFFF, a_packed >> 16); }
float2 HzbUnpackUvScale(uint a_packed) { return float2(a_packed & 0xFFFF, a_packed >> 16) / 65535.0; }

// The bounding sphere's screen-space extent and its nearest depth, in the same clip space the draws use.
// Returns false when the projection cannot be trusted - a corner behind the near plane - so that the
// object is kept.
bool HzbScreenExtent(float4x4 a_viewProj, float3 boundCentre, float boundRadius, out float2 uvMin, out float2 uvMax, out float nearestZ)
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
		const float4 clip = mul(a_viewProj, float4(centre + offset, 1.0));
		if (clip.w <= 1e-4)
			return false;
		const float3 ndc = clip.xyz / clip.w;
		ndcMin = min(ndcMin, ndc.xy);
		ndcMax = max(ndcMax, ndc.xy);
		minZ = min(minZ, ndc.z);
	}
	// Clip space to texture space. The y axis flips: the draws use a y-flipped viewport, which puts clip
	// y = +1 at the top of the image, where v = 0. Measured: the other orientation gives 571 false
	// negatives against 45, so this is not a guess.
	uvMin = saturate(float2(ndcMin.x, -ndcMax.y) * 0.5 + 0.5);
	uvMax = saturate(float2(ndcMax.x, -ndcMin.y) * 0.5 + 0.5);
	nearestZ = minZ;
	return true;
}

// One test: whether the HZB was sampled (a view with an HZB, the bound's projection trusted and past the near plane), and
// what it held under the bound against the bound's nearest depth.
struct HzbResult
{
	bool Sampled;
	bool Occluded;
	float Farthest;
	float NearestZ;
	float2 UvMin, UvMax;  // in HZB texture space
	int Mip;
};

// Occluded when every depth under the object's screen extent is NEARER than the object's nearest point, which
// means the object is entirely behind what has already been drawn.
//
// The HZB holds the FARTHEST depth under each texel, so one value per corner of the extent at a mip whose
// texels are large enough that four of them cover it. Taking the maximum of those four and requiring it to
// be nearer than the object is conservative twice over: the mip is a max reduction, and the object is
// represented by the nearest point of a box that already contains its sphere.
HzbResult HzbTest(HzbView a_view, float3 boundCentre, float boundRadius)
{
	HzbResult result = (HzbResult)0;
	if (a_view.Index == 0 || a_view.Mips == 0)
		return result;
	float2 uvMin, uvMax;
	float nearestZ;
	if (!HzbScreenExtent(a_view.ViewProj, boundCentre, boundRadius, uvMin, uvMax, nearestZ))
		return result;
	if (nearestZ <= 0.0)
		return result;  // in front of the near plane: nothing can occlude it

	// Image texture space to HZB texture space, before anything is measured in HZB texels.
	uvMin *= a_view.UvScale;
	uvMax *= a_view.UvScale;

	const uint2 baseSize = a_view.BaseSize;
	const float2 extent = (uvMax - uvMin) * float2(baseSize);
	// A mip whose texels are at least half the extent, so the four corner taps cover the whole rectangle.
	int mip = (int)ceil(log2(max(max(extent.x, extent.y), 1.0)));
	mip = clamp(mip, 0, (int)a_view.Mips - 1);
	const int2 mipSize = max(int2(baseSize) >> mip, int2(1, 1));
	const int2 texelMin = clamp(int2(uvMin * float2(mipSize)), int2(0, 0), mipSize - 1);
	const int2 texelMax = clamp(int2(uvMax * float2(mipSize)), int2(0, 0), mipSize - 1);

	Texture2D<float> hzb = ResourceDescriptorHeap[a_view.Index];
	const float farthest = max(
		max(hzb.Load(int3(texelMin.x, texelMin.y, mip)), hzb.Load(int3(texelMax.x, texelMin.y, mip))),
		max(hzb.Load(int3(texelMin.x, texelMax.y, mip)), hzb.Load(int3(texelMax.x, texelMax.y, mip))));

	// A margin, because the two sides of this comparison are not in the same space. nearestZ is raw clip
	// space, while the HZB holds what was actually stored, which the viewport depth range has scaled - and
	// the native depth pass and DCLF's own draws do not even use the same range ([0, 0.999968] against
	// [0, 0.999998]). The gap is small but it is systematically in the direction that culls, and it bites
	// hardest on flat objects lying against the surface behind them, whose nearest corner is barely in
	// front of their own stored depth. Erring towards drawing is free; erring the other way loses objects.
	const float kDepthMargin = 1e-4;
	result.Sampled = true;
	result.Occluded = farthest < nearestZ - kDepthMargin;
	result.Farthest = farthest;
	result.NearestZ = nearestZ;
	result.UvMin = uvMin;
	result.UvMax = uvMax;
	result.Mip = mip;
	return result;
}

#endif  // DCLF_HZB_TEST_HLSLI
