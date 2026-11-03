#ifndef ENVIRONMENT_PROBE_TRACE_HLSLI
#define ENVIRONMENT_PROBE_TRACE_HLSLI

#include "Common/EnvironmentProbe.hlsli"
#include "Common/EnvironmentProbeSampling.hlsli"
#ifdef PROBE_TRACE_VISIBILITY
#	include "Common/EnvironmentProbeMiss.hlsli"
#endif

#ifdef PROBE_TRACE_WATER
// The radiance map is filtered against black at the sky silhouette. Use the
// matching four depths to recover the uncovered part of that filter footprint.
bool WaterProbeCanFilter(uint2 pixel, uint2 dimensions)
{
	// Across an octant boundary a neighbouring depth may have a different capture origin.
	int2 axis = int2(pixel) * 2 + 1 - int(dimensions.x);
	return all(pixel > 0) && all(pixel + 1 < dimensions) && all(abs(axis) > 3) &&
	       abs(abs(axis.x) + abs(axis.y) - int(dimensions.x)) > 4;
}

float WaterProbeFootprint(float2 uv, uint2 dimensions, out float sampleDepth)
{
	float2 f = frac(uv * dimensions - 0.5);
	float4 weights = float4((1 - f.x) * f.y, f.x * f.y, f.x * (1 - f.y), (1 - f.x) * (1 - f.y));
	float4 depths = ProbeDepthTexture.GatherRed(PROBE_TRACE_SAMPLER, uv);
	float4 support = weights * (depths > 0) * (depths < ProbeFarDistance);
	float strongest = max(max(support.x, support.y), max(support.z, support.w));
	sampleDepth = ProbeFarDistance;
	[unroll] for (uint i = 0; i < 4; ++i) if (support[i] > 0 && support[i] == strongest)
		sampleDepth = depths[i];
	return saturate(dot(support, 1.0.xxxx));
}
#endif

bool ProbeSampleDepth(float3 position, uint2 dimensions, out float gap, out float footprint, out float2 uv)
{
	gap = 0;
	footprint = 0;
	uv = 0;
	uint face = EnvironmentProbe::GetFace(position);
	// Rotating captures have independent origins. Resolve the octant in that
	// capture's frame, and reject ambiguous seams instead of mixing depth frames.
	[loop] for (uint i = 0; i < 3; ++i)
	{
		uint nextFace = EnvironmentProbe::GetFace(position - ProbeOriginOffsets[face].xyz);
		if (nextFace == face)
			break;
		face = nextFace;
	}
	float3 delta = position - ProbeOriginOffsets[face].xyz;
	float radius = length(delta);
	if ((ProbeValidFaces & (1u << face)) == 0 || radius <= 1e-4 || EnvironmentProbe::GetFace(delta) != face)
		return false;
	uv = EnvironmentProbe::Encode(delta);
	uint2 pixel = min(uint2(saturate(uv) * dimensions), dimensions - 1);
	int2 centered = int2(pixel) * 2 + 1 - int(dimensions.x);
	uint pixelFace = (centered.x < 0 ? 1u : 0u) | (centered.y < 0 ? 2u : 0u) |
	                 (abs(centered.x) + abs(centered.y) > int(dimensions.x) ? 4u : 0u);
	if ((ProbeValidFaces & (1u << pixelFace)) == 0 ||
		(pixelFace != face && any(abs(ProbeOriginOffsets[pixelFace].xyz - ProbeOriginOffsets[face].xyz) > 1e-3)))
		return false;
	float depth = ProbeDepthTexture.Load(int3(pixel, 0));
#ifdef PROBE_TRACE_WATER
	if (depth >= ProbeFarDistance && WaterProbeCanFilter(pixel, dimensions))
		WaterProbeFootprint(uv, dimensions, depth);
#endif
	if (!isfinite(depth) || depth <= 0)
		return false;
	footprint = radius * 2.0 / min(dimensions.x, dimensions.y);
	// An empty captured ray is known free space, unlike an uncaptured octant.
	gap = depth >= ProbeFarDistance ? -ProbeFarDistance : radius - depth;
	return true;
}

bool TraceEnvironmentProbeWithOffset(float3 origin, float3 direction, float startDistance, float thickness, float stepOffset,
	out float3 radiance, out float hitDistance
#ifdef PROBE_TRACE_WATER
	,
	out float hitCoverage
#endif
#ifdef PROBE_TRACE_VISIBILITY
	,
	out bool unobstructed
#endif
#ifdef PROBE_TRACE_NORMALS
	,
	out float3 sourceNormal
#endif
)
{
	radiance = 0;
	hitDistance = 0;
#ifdef PROBE_TRACE_WATER
	hitCoverage = 0;
#endif
#ifdef PROBE_TRACE_VISIBILITY
	unobstructed = false;
#endif
#ifdef PROBE_TRACE_NORMALS
	sourceNormal = 0;
#endif
	if (ProbeValidFaces == 0 || !all(isfinite(origin)) || !all(isfinite(direction)) || !isfinite(startDistance))
		return false;
	// Every octant's radial capture is inside this sphere, including old origins.
	float b = dot(origin, direction);
	float discriminant = b * b - dot(origin, origin) + ProbeBoundsRadius * ProbeBoundsRadius;
	if (discriminant <= 0)
		return false;
	float root = sqrt(discriminant);
	float begin = max(max(startDistance, -b - root), 0.5);
	float end = -b + root;
	if (begin >= end)
		return false;
	uint2 dimensions;
	ProbeDepthTexture.GetDimensions(dimensions.x, dimensions.y);
#ifdef PROBE_TRACE_VISIBILITY
	if (ProbeKnownEmptySegment(origin, direction, begin, end, dimensions)) {
		unobstructed = true;
		return false;
	}
#endif
	uint steps = clamp(ProbeMaxSteps, 8u, 128u);
	float rcpSteps = rcp(float(steps));
	stepOffset = saturate(stepOffset);
	float previousDistance = begin;
	float previousGap, footprint;
	float2 uv;
	bool previousValid = ProbeSampleDepth(origin + direction * begin, dimensions, previousGap, footprint, uv);
#ifdef PROBE_TRACE_VISIBILITY
	bool clear = previousValid && previousGap < 0;
	uint2 previousPixel = min(uint2(saturate(uv) * dimensions), dimensions - 1);
#endif
	// Quadratic spacing spends the finite budget close to the screen exit. A
	// front-surface crossing is refined in world ray distance, never in octahedral UV.
	[loop] for (uint i = 1; i <= steps; ++i)
	{
		float fraction = i == steps ? 1.0 : min((float(i) + stepOffset - 0.5) * rcpSteps, 1.0);
		float distance = lerp(begin, end, fraction * fraction);
		float gap;
		bool valid = ProbeSampleDepth(origin + direction * distance, dimensions, gap, footprint, uv);
#ifdef PROBE_TRACE_VISIBILITY
		uint2 pixel = min(uint2(saturate(uv) * dimensions), dimensions - 1);
		// Only certify intervals contained in one captured texel cone. Their
		// endpoint radii bound the whole segment by convexity. Skipped texels,
		// depth discontinuities and different octant origins remain unknown.
		clear = clear && valid && previousValid && previousGap < 0 && gap < 0 && all(pixel == previousPixel);
		previousPixel = pixel;
#endif
		float hitTolerance = max(thickness, 0.5) + footprint;
#ifdef PROBE_TRACE_COARSE_HITS
		hitTolerance += distance - previousDistance;
#endif
		bool hitCandidate = previousValid && previousGap <= 0;
#ifdef PROBE_TRACE_COARSE_HITS
		hitCandidate = hitCandidate || gap <= hitTolerance;
#endif
		if (valid && gap >= 0 && hitCandidate) {
			float hitT = distance;
			float hitGap = gap;
			float hitFootprint = footprint;
			float2 hitUV = uv;
#ifdef PROBE_TRACE_WATER
			// A coarse depth crossing can span both sky and foliage. Narrow the
			// first bracket before sampling its radiance, even for coarse hits.
			if (previousValid && previousGap <= 0 && hitGap > max(thickness, footprint)) {
				float frontT = previousDistance;
				float frontGap = previousGap;
				[loop] for (uint refine = 0; refine < 4; ++refine)
				{
					float fraction = clamp(-frontGap / max(hitGap - frontGap, 1e-5), 0.125, 0.875);
					float sampleT = lerp(frontT, hitT, fraction);
					float sampleGap, sampleFootprint;
					float2 sampleUV;
					if (!ProbeSampleDepth(origin + direction * sampleT, dimensions, sampleGap, sampleFootprint, sampleUV))
						break;
					if (abs(sampleGap) <= max(thickness, sampleFootprint)) {
						hitT = sampleT;
						hitGap = abs(sampleGap);
						hitUV = sampleUV;
						break;
					}
					if (sampleGap > 0) {
						hitT = sampleT;
						hitGap = sampleGap;
						hitUV = sampleUV;
					} else {
						frontT = sampleT;
						frontGap = sampleGap;
					}
				}
				hitTolerance = max(thickness, 0.5) + footprint + hitT - frontT;
			}
#else
			if (hitGap > hitTolerance) {
				float fraction = saturate(previousGap / min(previousGap - gap, -1e-6));
				float refinedDistance = lerp(previousDistance, distance, fraction);
				float refinedGap, refinedFootprint;
				float2 refinedUV;
				bool refinedValid = ProbeSampleDepth(origin + direction * refinedDistance, dimensions, refinedGap, refinedFootprint, refinedUV);
				if (refinedValid && refinedGap >= 0) {
					hitT = refinedDistance;
					hitGap = refinedGap;
					hitFootprint = refinedFootprint;
					hitUV = refinedUV;
				}
			}
#endif
			if (hitGap >= 0 && hitGap <= hitTolerance) {
				float3 color;
#ifdef PROBE_TRACE_WATER
				uint2 texel = min(uint2(saturate(hitUV) * dimensions), dimensions - 1);
				if (WaterProbeCanFilter(texel, dimensions)) {
					float unusedDepth;
					hitCoverage = WaterProbeFootprint(hitUV, dimensions, unusedDepth);
					color = ProbeRadianceTexture.SampleLevel(PROBE_TRACE_SAMPLER, hitUV, 0).rgb;
				} else {
					hitCoverage = 1;
					color = ProbeRadianceTexture.Load(int3(texel, 0)).rgb;
				}
#else
				color = EnvironmentProbe::SampleRadiance(ProbeRadianceTexture, PROBE_TRACE_SAMPLER, hitUV, dimensions);
#endif
				if (all(isfinite(color))) {
					radiance = max(color, 0);  // already scene-linear, without pre-exposure
					hitDistance = hitT;
#ifdef PROBE_TRACE_NORMALS
					uint2 hitPixel = min(uint2(saturate(hitUV) * dimensions), dimensions - 1);
					sourceNormal = GBuffer::DecodeNormal(ProbeNormalTexture.Load(int3(hitPixel, 0)));
#endif
					return true;
				}
			}
		}
		previousValid = valid;
		previousGap = gap;
		previousDistance = distance;
	}
#ifdef PROBE_TRACE_VISIBILITY
	unobstructed = clear;
#endif
	return false;
}

bool TraceEnvironmentProbe(float3 origin, float3 direction, float startDistance, float thickness,
	out float3 radiance, out float hitDistance
#ifdef PROBE_TRACE_WATER
	,
	out float hitCoverage
#endif
#ifdef PROBE_TRACE_VISIBILITY
	,
	out bool unobstructed
#endif
#ifdef PROBE_TRACE_NORMALS
	,
	out float3 sourceNormal
#endif
)
{
	return TraceEnvironmentProbeWithOffset(origin, direction, startDistance, thickness, 0.5, radiance, hitDistance
#ifdef PROBE_TRACE_WATER
		,
		hitCoverage
#endif
#ifdef PROBE_TRACE_VISIBILITY
		,
		unobstructed
#endif
#ifdef PROBE_TRACE_NORMALS
		,
		sourceNormal
#endif
	);
}

#endif
