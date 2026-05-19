/// Local Exposure Compute Shader
/// Exposure-fusion local adaptation adapted to output a raw-HDR multiplier
/// consumed later by Composite.
///
/// Raw scene color is normalized with global exposure when available so the
/// exposure-fusion weights operate in a stable perceptual luminance range.
///
/// Reference:
///   https://bartwronski.com/2022/02/28/exposure-fusion-local-tonemapping-for-real-time-rendering/

#include "Common/Color.hlsli"

cbuffer LocalExposureCB : register(b1)
{
	float ManualExposure;
	float HighlightExposure;
	float ShadowExposure;
	float ExposurePreferenceSigmaSq;

	uint InputWidth;
	uint InputHeight;
	uint MipLevel;
	uint DisplayMip;

	uint CurrentMip;
	uint HasCoarserMip;
	uint BoostLocalContrast;
	uint UseGlobalExposure;

	float ExposureCompensation;
	float AdaptationMin;
	float AdaptationMax;
	float DarkThreshold;
};

Texture2D<float4> TexInput0 : register(t0);
Texture2D<float4> TexInput1 : register(t1);
Texture2D<float4> TexInput2 : register(t2);
Texture2D<float> TexInput3 : register(t3);
StructuredBuffer<float> TexAdaptation : register(t4);
SamplerState LinearSampler : register(s0);

RWTexture2D<float4> RWTexOutput0 : register(u0);
RWTexture2D<float4> RWTexOutput1 : register(u1);
RWTexture2D<float> RWTexOutputFloat : register(u2);

float GetPreExposure()
{
	if (UseGlobalExposure != 0) {
		float adaptedLum = clamp(max(TexAdaptation[0], 1e-5), AdaptationMin, AdaptationMax);
		return 0.18 * ExposureCompensation / adaptedLum;
	}

	return ManualExposure;
}

float LinearLuminance(float3 preExposedColor)
{
	return max(Color::RGBToLuminance(max(preExposedColor, 0.0)), 1e-5);
}

float ExposureFusionTonemap(float linearLum)
{
	linearLum = max(linearLum, 0.0);
	return sqrt(linearLum / (1.0 + linearLum));
}

float ExposureFusionInverseTonemap(float tonemappedLum)
{
	float value = saturate(tonemappedLum);
	value *= value;
	return value / max(1.0 - value, 1e-4);
}

float ExposureFusionLuminance(float3 preExposedColor, float exposureScale)
{
	return ExposureFusionTonemap(LinearLuminance(preExposedColor) * exposureScale);
}

float3 NormalizeWeights(float3 weights)
{
	return weights / (weights.x + weights.y + weights.z + 0.00001);
}

[numthreads(8, 8, 1)] void CSSetup(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= InputWidth || tid.y >= InputHeight)
		return;

	float3 preExposedColor = TexInput0[tid].rgb * GetPreExposure();

	float highlightLum = ExposureFusionLuminance(preExposedColor, HighlightExposure);
	float midLum = ExposureFusionLuminance(preExposedColor, 1.0);
	float shadowLum = ExposureFusionLuminance(preExposedColor, ShadowExposure);
	float3 lums = float3(highlightLum, midLum, shadowLum);

	float3 diff = lums - 0.5;
	float3 weights = exp(-0.5 * diff * diff * ExposurePreferenceSigmaSq);

	RWTexOutput0[tid] = float4(lums, 1.0);
	RWTexOutput1[tid] = float4(NormalizeWeights(weights), 1.0);
}

	[numthreads(8, 8, 1)] void CSDownsample(uint2 tid : SV_DispatchThreadID)
{
	uint2 outDims;
	RWTexOutput0.GetDimensions(outDims.x, outDims.y);

	if (any(tid >= outDims))
		return;

	float2 uv = (float2(tid) + 0.5) / float2(outDims);
	RWTexOutput0[tid] = TexInput0.SampleLevel(LinearSampler, uv, 0);
	RWTexOutput1[tid] = TexInput1.SampleLevel(LinearSampler, uv, 0);
}

[numthreads(8, 8, 1)] void CSBlend(uint2 tid : SV_DispatchThreadID) {
	uint2 outDims;
	RWTexOutputFloat.GetDimensions(outDims.x, outDims.y);

	if (any(tid >= outDims))
		return;

	float3 exposures = TexInput0[tid].rgb;
	float3 weights = TexInput1[tid].rgb;
	float prevResult = 0.0;

	if (HasCoarserMip != 0) {
		float2 uv = (float2(tid) + 0.5) / float2(outDims);
		float3 coarserExposures = TexInput2.SampleLevel(LinearSampler, uv, 0).rgb;
		exposures -= coarserExposures;
		prevResult = TexInput3.SampleLevel(LinearSampler, uv, 0).r;

		if (BoostLocalContrast != 0)
			weights *= abs(exposures) + 0.00001;
	}

	weights = NormalizeWeights(weights);
	RWTexOutputFloat[tid] = prevResult + dot(exposures, weights);
}

	[numthreads(8, 8, 1)] void CSComputeExposure(uint2 tid : SV_DispatchThreadID)
{
	if (tid.x >= InputWidth || tid.y >= InputHeight)
		return;

	float2 uv = (float2(tid) + 0.5) / float2(InputWidth, InputHeight);

	uint2 displayDims;
	TexInput2.GetDimensions(displayDims.x, displayDims.y);
	float2 displayPixelSize = 1.0 / float2(displayDims);

	float momentX = 0.0;
	float momentY = 0.0;
	float momentX2 = 0.0;
	float momentXY = 0.0;
	float ws = 0.0;

	[unroll] for (int dy = -1; dy <= 1; dy++)
	{
		[unroll] for (int dx = -1; dx <= 1; dx++)
		{
			float2 sampleUV = uv + float2(dx, dy) * displayPixelSize;
			sampleUV = clamp(sampleUV, displayPixelSize * 0.5, 1.0 - displayPixelSize * 0.5);

			float x = TexInput1.SampleLevel(LinearSampler, sampleUV, 0).g;
			float y = TexInput2.SampleLevel(LinearSampler, sampleUV, 0).r;
			float w = exp(-0.5 * float(dx * dx + dy * dy) / (0.7 * 0.7));

			momentX += x * w;
			momentY += y * w;
			momentX2 += x * x * w;
			momentXY += x * y * w;
			ws += w;
		}
	}

	momentX /= ws;
	momentY /= ws;
	momentX2 /= ws;
	momentXY /= ws;

	float A = (momentXY - momentX * momentY) / (max(momentX2 - momentX * momentX, 0.0) + 0.00001);
	float B = momentY - A * momentX;

	float3 preExposedColor = TexInput0[tid].rgb * GetPreExposure();
	float linearLuminance = LinearLuminance(preExposedColor);
	float guideLuminance = ExposureFusionTonemap(linearLuminance);
	float fusedLuminance = max(A * guideLuminance + B, 0.0);
	float localExposure = ExposureFusionInverseTonemap(fusedLuminance) / linearLuminance;

	float shadowProtection = 1.0 - smoothstep(0.045, 0.18, linearLuminance);
	localExposure = lerp(localExposure, max(localExposure, 1.0), shadowProtection);

	localExposure = guideLuminance > DarkThreshold ? localExposure :
	                                                 lerp(1.0, localExposure, (guideLuminance / DarkThreshold) * (guideLuminance / DarkThreshold));

	if (UseGlobalExposure == 0)
		localExposure *= ManualExposure;

	RWTexOutputFloat[tid] = localExposure;
}
