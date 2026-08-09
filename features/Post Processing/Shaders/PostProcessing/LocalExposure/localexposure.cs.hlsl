/// Local Exposure Compute Shader
/// Exposure-fusion local adaptation adapted to output a raw-HDR multiplier
/// consumed later by Composite. The final reconstruction uses the scene's log
/// luminance as a range guide without requiring a separate 3D bilateral grid.
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

// Four-tap cardinal cubic B-spline weights. Unlike a box-weighted window,
// samples enter and leave the footprint with zero weight, keeping the
// reconstructed exposure continuous as scene features move across mip texels.
float4 CubicBSplineWeights(float t)
{
	float t2 = t * t;
	float t3 = t2 * t;
	float omt = 1.0 - t;

	return float4(
			   omt * omt * omt,
			   3.0 * t3 - 6.0 * t2 + 4.0,
			   -3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0,
			   t3) /
	       6.0;
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

	uint2 inDims;
	TexInput0.GetDimensions(inDims.x, inDims.y);

	float2 uv = (float2(tid) + 0.5) / float2(outDims);
	float2 inputPixelSize = 1.0 / float2(inDims);
	float2 minUV = inputPixelSize * 0.5;
	float2 maxUV = 1.0 - minUV;
	float4 exposureSum = 0.0;
	float4 weightSum = 0.0;

	// Four bilinear samples form a wider 4x4 low-pass footprint. The additional
	// prefiltering makes the pyramid substantially less phase-sensitive during
	// camera motion than a single 2x2 bilinear sample.
	[unroll] for (int y = -1; y <= 1; y += 2)
	{
		[unroll] for (int x = -1; x <= 1; x += 2)
		{
			float2 sampleUV = clamp(uv + float2(x, y) * inputPixelSize, minUV, maxUV);
			exposureSum += TexInput0.SampleLevel(LinearSampler, sampleUV, 0);
			weightSum += TexInput1.SampleLevel(LinearSampler, sampleUV, 0);
		}
	}

	RWTexOutput0[tid] = exposureSum * 0.25;
	RWTexOutput1[tid] = weightSum * 0.25;
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
	float3 preExposedColor = TexInput0[tid].rgb * GetPreExposure();
	float linearLuminance = LinearLuminance(preExposedColor);
	float guideLuminance = ExposureFusionTonemap(linearLuminance);
	float guideLogLuminance = log2(linearLuminance);

	float2 displayPosition = uv * float2(displayDims) - 0.5;
	int2 basePosition = int2(floor(displayPosition));
	float4 weightX = CubicBSplineWeights(frac(displayPosition.x));
	float4 weightY = CubicBSplineWeights(frac(displayPosition.y));
	float momentGuide = 0.0, momentFusion = 0.0, momentGuideSq = 0.0, momentGuideFusion = 0.0;
	float totalWeight = 0.0;

	// Smooth spatially in the display mip while rejecting samples from a
	// different log-luminance range. The moments feed a local linear fit against
	// the guide, so the reconstructed fusion value keeps its dependence on scene
	// luminance instead of collapsing to a neighbourhood average.
	[unroll] for (int y = 0; y < 4; y++)
	{
		[unroll] for (int x = 0; x < 4; x++)
		{
			int2 samplePosition = clamp(basePosition + int2(x - 1, y - 1), int2(0, 0), int2(displayDims) - 1);
			float sampleGuide = TexInput1.Load(int3(samplePosition, 0)).g;
			float sampleFusion = TexInput2.Load(int3(samplePosition, 0)).r;
			float sampleLogLuminance = log2(max(ExposureFusionInverseTonemap(sampleGuide), 1e-5));
			float logLuminanceDelta = sampleLogLuminance - guideLogLuminance;
			float spatialWeight = weightX[x] * weightY[y];
			// Roughly one EV of range tolerance. exp2 is intentional: the guide is
			// already expressed in stops and this gives a soft, stable rejection.
			float rangeWeight = exp2(-0.5 * logLuminanceDelta * logLuminanceDelta);
			float sampleWeight = spatialWeight * rangeWeight;

			momentGuide += sampleGuide * sampleWeight;
			momentFusion += sampleFusion * sampleWeight;
			momentGuideSq += sampleGuide * sampleGuide * sampleWeight;
			momentGuideFusion += sampleGuide * sampleFusion * sampleWeight;
			totalWeight += sampleWeight;
		}
	}

	float invWeight = 1.0 / max(totalWeight, 1e-5);
	float meanGuide = momentGuide * invWeight;
	float meanFusion = momentFusion * invWeight;
	float fitSlope = (momentGuideFusion * invWeight - meanGuide * meanFusion) /
	                 (max(momentGuideSq * invWeight - meanGuide * meanGuide, 0.0) + 0.00001);
	float fitOffset = meanFusion - fitSlope * meanGuide;

	float fallbackFusion = TexInput2.SampleLevel(LinearSampler, uv, 0).r;
	// Evaluated at the full-resolution guide: this is what transports scene detail
	// into the ratio below, which a plain weighted mean would divide back out.
	float bilateralFusion = fitSlope * guideLuminance + fitOffset;
	// Sparse range support occurs on sub-pixel highlights. Falling back toward
	// the broad spatial reconstruction avoids unstable amplification there.
	float bilateralConfidence = smoothstep(0.02, 0.20, totalWeight);
	float fusedLuminance = max(lerp(fallbackFusion, bilateralFusion, bilateralConfidence), 0.0);

	float localExposure = ExposureFusionInverseTonemap(fusedLuminance) / linearLuminance;

	float shadowProtection = 1.0 - smoothstep(0.045, 0.18, linearLuminance);
	localExposure = lerp(localExposure, max(localExposure, 1.0), shadowProtection);

	localExposure = guideLuminance > DarkThreshold ? localExposure :
	                                                 lerp(1.0, localExposure, (guideLuminance / DarkThreshold) * (guideLuminance / DarkThreshold));

	// Pyramid reconstruction can overshoot near high-contrast edges. Exposure
	// fusion is a blend of these three candidates, so values outside their range
	// are reconstruction artifacts and are a major source of motion pumping.
	localExposure = clamp(localExposure, min(HighlightExposure, 1.0), max(ShadowExposure, 1.0));

	if (UseGlobalExposure == 0)
		localExposure *= ManualExposure;

	RWTexOutputFloat[tid] = localExposure;
}
