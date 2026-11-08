// The game's separable compute blur, transcribed from the vanilla shader so it can build with DXC. Community Shaders replaces
// it only for the screen-space reflection blur (ShaderCache.cpp), whose defines are GRIDSIZE_X=960 (horizontal) or GRIDSIZE_Y=540
// (vertical), TEXTAP=9, BLUR_SCALE=2, SIGMA=2 and OUTPUT_CHANNEL_COUNT=4. The thread group sizes are vanilla's, because the game
// sizes the dispatch for them.

#if TEXTAP != 9 || SIGMA != 2 || OUTPUT_CHANNEL_COUNT != 4
#	error ISBlurCS.hlsl only has vanilla's reflection blur: 9 taps, sigma 2, four channels
#endif

Texture2D<float4> InputTexture : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer PerGeometry : register(b0)
{
	float4 InvSize;  // xy: one over the input size
	float4 MaxUV;    // xy: the largest UV to sample
}

#if defined(GRIDSIZE_X)
#	define SPAN GRIDSIZE_X
#else
#	define SPAN GRIDSIZE_Y
#endif
#define HALO TEXTAP
#define GROUP (SPAN + HALO * 2)

// Vanilla's weights for 9 taps at sigma 2.
static const float TapWeights[9] = { 0.028532, 0.067234, 0.124009, 0.179044, 0.202360, 0.179044, 0.124009, 0.067234, 0.028532 };

groupshared float4 Line[GROUP];

#if defined(GRIDSIZE_X)
[numthreads(GROUP, 1, 1)]
#else
[numthreads(1, GROUP, 1)]
#endif
void main(uint3 groupThreadId : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
#if defined(GRIDSIZE_X)
	const int lane = groupThreadId.x;
	const int along = lane - HALO;
	const int2 pixel = int2(groupId.x * SPAN + along, groupId.y);
#else
	const int lane = groupThreadId.y;
	const int along = lane - HALO;
	const int2 pixel = int2(groupId.x, groupId.y * SPAN + along);
#endif

	// Every lane loads one texel, the halo lanes included. The clamp keeps reads inside the rendered area.
	const float2 uv = min((float2(pixel) + 0.5) * InvSize.xy, MaxUV.xy);
	Line[lane] = InputTexture.SampleLevel(LinearSampler, uv, 0);

	GroupMemoryBarrierWithGroupSync();

	if (along < 0 || along >= SPAN)
		return;

	// D3D drops stores outside the texture. Vulkan does not promise to, so test here.
	uint2 size;
	OutputTexture.GetDimensions(size.x, size.y);
	if (any(uint2(pixel) >= size))
		return;

	float4 sum = 0;
	[unroll] for (int i = 0; i < TEXTAP; ++i)
		sum += Line[lane + (i - TEXTAP / 2) * BLUR_SCALE] * TapWeights[i];
	OutputTexture[pixel] = sum;
}
