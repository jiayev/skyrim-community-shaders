#ifndef ENVIRONMENT_PROBE_SHADOWS_HLSLI
#define ENVIRONMENT_PROBE_SHADOWS_HLSLI

namespace EnvironmentProbe
{
	Texture2DArray<float> SunShadowMap : register(t104);
	Texture2D<float> CoarseSunShadowMap : register(t105);

	// First four registers of Utility's native PerGeometry buffer, snapshotted
	// before the engine reuses it. No GPU readback or duplicate INI bias formula.
	cbuffer SunShadowParameters : register(b13)
	{
		float4 SunDebugColor;
		float4 SunPropertyColor;
		float4 SunAlphaTestRef;
		float4 SunShadowLightParam;
	};

	bool SunCoordinates(float3 positionWS, uint cascade, out float3 positionLS)
	{
		positionLS = mul(SunProjection[cascade], float4(positionWS, 1)).xyz;
		// Z is a comparison reference, not a coverage test. Receivers behind the
		// shadow volume still compare against its casters, as in Utility's shadow mask.
		return all(positionLS.xy >= 0) && all(positionLS.xy <= 1);
	}

	float SampleSunCascade(float3 positionLS, uint cascade, uint2 dimensions)
	{
		// Bilinear comparison filtering without taking a material's sampler slot.
		// All 16 sampler slots can be occupied by landscape/LOD permutations.
		float2 pixel = positionLS.xy * dimensions - 0.5;
		int2 base = int2(floor(pixel));
		float2 fraction = frac(pixel);
		float4 comparisons;
		float receiverDepth = positionLS.z - (cascade == 0 ? SunAlphaTestRef.y : SunAlphaTestRef.z);
		[unroll] for (uint tap = 0; tap < 4; ++tap)
		{
			int2 offset = int2(tap & 1u, tap >> 1);
			int2 samplePixel = clamp(base + offset, 0, int2(dimensions) - 1);
			comparisons[tap] = SunShadowMap.Load(int4(samplePixel, cascade, 0)) >= receiverDepth;
		}
		return lerp(lerp(comparisons.x, comparisons.y, fraction.x), lerp(comparisons.z, comparisons.w, fraction.x), fraction.y);
	}

	float GetCoarseSunShadow(float3 position)
	{
		if (CoarseSunEnabled == 0)
			return 1;
		float3 ls = mul(CoarseSunProjection, float4(position, 1)).xyz;
		if (any(ls < 0) || any(ls > 1))
			return 1;
		uint width, height;
		CoarseSunShadowMap.GetDimensions(width, height);
		float2 pixel = ls.xy * uint2(width, height) - 0.5;
		int2 base = int2(floor(pixel));
		float2 fraction = frac(pixel);
		float4 comparisons;
		[unroll] for (uint tap = 0; tap < 4; ++tap)
		{
			int2 samplePixel = clamp(base + int2(tap & 1u, tap >> 1), 0, int2(width, height) - 1);
			comparisons[tap] = CoarseSunShadowMap.Load(int3(samplePixel, 0)) >= ls.z - CoarseSunBias;
		}
		return lerp(lerp(comparisons.x, comparisons.y, fraction.x), lerp(comparisons.z, comparisons.w, fraction.x), fraction.y);
	}

	float GetSunShadow(float3 position)
	{
		if (ReceiveSunShadows == 0)
			return 1;
		if (SunCascadeCount == 0)
			return GetCoarseSunShadow(position);
		float3 positionWS = position + SunOrigin.xyz;
		float depth = max(0, dot(position, SunViewDepth.xyz) + SunViewDepth.w);
		uint width, height, layers;
		SunShadowMap.GetDimensions(width, height, layers);
		uint count = min(SunCascadeCount, layers);
		if (count == 0)
			return GetCoarseSunShadow(position);
		uint preferred = 0;
		[loop] while (preferred + 1 < count && depth > SunEndSplits[preferred])++ preferred;
		uint cascade = count;
		float3 positionLS = 0;
		// Inspect every footprint, including fine cascades outside the camera's
		// depth-selected split. Prefer the intended split or a covering coarser one;
		// keep a finer covering cascade if none of those can see this receiver.
		[loop] for (uint candidate = 0; candidate < count; ++candidate)
		{
			float3 candidateLS;
			if (SunCoordinates(positionWS, candidate, candidateLS)) {
				if (cascade == count || candidate >= preferred) {
					cascade = candidate;
					positionLS = candidateLS;
				}
				if (candidate >= preferred)
					break;
			}
		}
		if (cascade == count)
			return GetCoarseSunShadow(position);

		float shadow = SampleSunCascade(positionLS, cascade, uint2(width, height));
		if (cascade + 1 < min(SunCascadeCount, layers)) {
			float start = SunStartSplits[cascade + 1];
			float end = SunEndSplits[cascade];
			float3 nextLS;
			if (end > start && depth > start && SunCoordinates(positionWS, cascade + 1, nextLS))
				shadow = lerp(shadow, SampleSunCascade(nextLS, cascade + 1, uint2(width, height)), smoothstep(start, end, depth));
		}
		float3 mainRelative = position + WorldOriginDelta;
		float fade = 1.0 - pow(saturate(dot(mainRelative, mainRelative) / max(SunShadowLightParam.z, 1.0)), 8);
		return lerp(1.0, shadow, fade);
	}
}
#endif
