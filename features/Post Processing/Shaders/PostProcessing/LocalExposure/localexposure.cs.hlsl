/// Local Exposure luminance analysis
/// Builds an edge-aware base in log-luminance space. The tonal remapping is
/// evaluated later by Composite so it uses the same global exposure as the
/// final scene color.

#include "Common/Color.hlsli"

#define GRID_DEPTH 32
#define GRID_TILE_SIZE 64
#define GRID_THREAD_SIZE 8
#define GRID_SAMPLE_STRIDE 8
#define GRID_QUANTIZATION 4096

cbuffer LocalExposureCB : register(b1)
{
	float ManualExposure;
	float Strength;
	float HighlightContrast;
	float ShadowContrast;

	float DetailStrength;
	float BaseBlend;
	float BaseMip;
	float MiddleGreyBias;

	float HighlightThreshold;
	float ShadowThreshold;
	float HighlightThresholdStrength;
	float ShadowThresholdStrength;

	uint InputWidth;
	uint InputHeight;
	uint ActiveMipCount;
	uint Padding0;

	float LogLuminanceMin;
	float LogLuminanceMax;
	float2 Padding1;
};

Texture2D<float4> TexColor : register(t0);
Texture2D<float> TexLogLuminance : register(t1);
Texture3D<float2> TexLuminanceGrid : register(t2);
SamplerState LinearSampler : register(s0);

RWTexture2D<float> RWTexOutput : register(u0);
RWTexture3D<float2> RWTexLuminanceGrid : register(u1);

float SceneLogLuminance(float3 color)
{
	float luminance = Color::RGBToLuminance(max(color, 0.0));
	return clamp(log2(max(luminance, exp2(LogLuminanceMin))), LogLuminanceMin, LogLuminanceMax);
}

[numthreads(8, 8, 1)] void CSSetupLogLuminance(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= InputWidth || tid.y >= InputHeight)
		return;

	RWTexOutput[tid] = SceneLogLuminance(TexColor[tid].rgb);
}

	[numthreads(8, 8, 1)] void CSDownsampleLogLuminance(uint2 tid : SV_DispatchThreadID)
{
	uint2 outputSize;
	RWTexOutput.GetDimensions(outputSize.x, outputSize.y);
	if (any(tid >= outputSize))
		return;

	uint2 inputSize;
	TexLogLuminance.GetDimensions(inputSize.x, inputSize.y);

	float2 uv = (float2(tid) + 0.5) / float2(outputSize);
	float2 radius = 0.5 / float2(inputSize);
	float result = 0.0;
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(-radius.x, -radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(radius.x, -radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(-radius.x, radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(radius.x, radius.y), 0);
	RWTexOutput[tid] = result * 0.25;
}

groupshared uint ThreadGridWeights[GRID_DEPTH][GRID_THREAD_SIZE * GRID_THREAD_SIZE];
groupshared uint ThreadGridLogSums[GRID_DEPTH][GRID_THREAD_SIZE * GRID_THREAD_SIZE];

[numthreads(GRID_THREAD_SIZE, GRID_THREAD_SIZE, 1)] void CSBuildLuminanceGrid(
	uint3 groupID : SV_GroupID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex) {
	[unroll] for (uint bin = 0; bin < GRID_DEPTH; bin++)
	{
		ThreadGridWeights[bin][groupIndex] = 0;
		ThreadGridLogSums[bin][groupIndex] = 0;
	}

	const float inverseLogRange = rcp(LogLuminanceMax - LogLuminanceMin);
	const uint2 tileOrigin = groupID.xy * GRID_TILE_SIZE;

	[unroll] for (uint y = 0; y < GRID_SAMPLE_STRIDE; y++)
	{
		[unroll] for (uint x = 0; x < GRID_SAMPLE_STRIDE; x++)
		{
			uint2 pixel = tileOrigin + groupThreadID.xy + uint2(x, y) * GRID_THREAD_SIZE;
			if (pixel.x < InputWidth && pixel.y < InputHeight) {
				float normalizedLog = saturate((TexLogLuminance[pixel] - LogLuminanceMin) * inverseLogRange);
				float binPosition = normalizedLog * (GRID_DEPTH - 1);
				uint lowerBin = min((uint)binPosition, GRID_DEPTH - 1);
				uint upperBin = min(lowerBin + 1, GRID_DEPTH - 1);
				uint upperWeight = (uint)(frac(binPosition) * GRID_QUANTIZATION + 0.5);
				uint lowerWeight = GRID_QUANTIZATION - upperWeight;

				ThreadGridWeights[lowerBin][groupIndex] += lowerWeight;
				ThreadGridLogSums[lowerBin][groupIndex] += (uint)(normalizedLog * lowerWeight + 0.5);
				ThreadGridWeights[upperBin][groupIndex] += upperWeight;
				ThreadGridLogSums[upperBin][groupIndex] += (uint)(normalizedLog * upperWeight + 0.5);
			}
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex < GRID_DEPTH) {
		uint gridWeight = 0;
		uint gridLogSum = 0;
		[unroll] for (uint threadIndex = 0; threadIndex < GRID_THREAD_SIZE * GRID_THREAD_SIZE; threadIndex++)
		{
			gridWeight += ThreadGridWeights[groupIndex][threadIndex];
			gridLogSum += ThreadGridLogSums[groupIndex][threadIndex];
		}

		const float normalization = rcp((float)(GRID_QUANTIZATION * GRID_TILE_SIZE * GRID_TILE_SIZE));
		RWTexLuminanceGrid[uint3(groupID.xy, groupIndex)] = float2(gridLogSum, gridWeight) * normalization;
	}
}

float SampleBroadBase(float2 uv)
{
	float mip = min(BaseMip, (float)(ActiveMipCount - 1));
	float2 stepUV = exp2(mip) / float2(InputWidth, InputHeight);
	float result = 0.0;

	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
		{
			float weight = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
			result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(x, y) * stepUV, mip) * weight;
		}
	}

	return result * (1.0 / 16.0);
}

[numthreads(8, 8, 1)] void CSResolveBaseLuminance(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= InputWidth || tid.y >= InputHeight)
		return;

	float logLuminance = TexLogLuminance.Load(int3(tid, 0));
	float normalizedLog = saturate((logLuminance - LogLuminanceMin) / (LogLuminanceMax - LogLuminanceMin));
	float2 uv = (float2(tid) + 0.5) / float2(InputWidth, InputHeight);

	uint gridWidth, gridHeight, gridDepth;
	TexLuminanceGrid.GetDimensions(gridWidth, gridHeight, gridDepth);
	float3 gridUV;
	gridUV.xy = (float2(tid) + 0.5) / (GRID_TILE_SIZE * float2(gridWidth, gridHeight));
	gridUV.z = (normalizedLog * (gridDepth - 1) + 0.5) / gridDepth;

	float2 gridMoments = TexLuminanceGrid.SampleLevel(LinearSampler, gridUV, 0);
	float bilateralBase = lerp(LogLuminanceMin, LogLuminanceMax, gridMoments.x / max(gridMoments.y, 1e-5));
	float broadBase = SampleBroadBase(uv);

	// Isolated range cells are unreliable at thin highlights and frame edges.
	// Fade them into the broad base before applying the user-controlled blend.
	float gridConfidence = smoothstep(0.00015, 0.0015, gridMoments.y);
	float edgeAwareBase = lerp(broadBase, bilateralBase, gridConfidence);
	RWTexOutput[tid] = lerp(edgeAwareBase, broadBase, BaseBlend);
}
