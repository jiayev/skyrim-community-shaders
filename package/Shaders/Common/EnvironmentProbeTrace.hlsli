#ifndef ENVIRONMENT_PROBE_TRACE_HLSLI
#define ENVIRONMENT_PROBE_TRACE_HLSLI

#include "Common/EnvironmentProbe.hlsli"
#include "Common/EnvironmentProbeSampling.hlsli"
#ifdef PROBE_TRACE_VISIBILITY
#	include "Common/EnvironmentProbeMiss.hlsli"
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
	if (!isfinite(depth) || depth <= 0)
		return false;
	footprint = radius * 2.0 / min(dimensions.x, dimensions.y);
	// An empty captured ray is known free space, unlike an uncaptured octant.
	gap = depth >= ProbeFarDistance ? -ProbeFarDistance : radius - depth;
	return true;
}

bool TraceEnvironmentProbe(float3 origin, float3 direction, float startDistance, float thickness,
	out float3 radiance, out float hitDistance
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
	float stepScale = pow(end / begin, rcp(float(steps)));
	float distance = begin;
	float previousDistance = begin;
	float previousGap, footprint;
	float2 uv;
	bool previousValid = ProbeSampleDepth(origin + direction * begin, dimensions, previousGap, footprint, uv);
#ifdef PROBE_TRACE_VISIBILITY
	bool clear = previousValid && previousGap < 0;
	uint2 previousPixel = min(uint2(saturate(uv) * dimensions), dimensions - 1);
#endif
	// A fixed angular resolution needs a world step that grows with radial
	// distance. Geometric spacing covers the whole interval at every range.
	[loop] for (uint i = 1; i <= steps; ++i)
	{
		distance = i == steps ? end : min(end, distance * stepScale);
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
		if (valid && previousValid && previousGap <= 0 && gap >= 0) {
			float hitT = distance;
			float hitGap = gap;
			float hitFootprint = footprint;
			float2 hitUV = uv;
			if (hitGap > max(thickness, 0.5) + hitFootprint) {
				float fraction = saturate(previousGap / min(previousGap - gap, -1e-6));
				float refinedDistance = lerp(previousDistance, distance, fraction);
				float refinedGap, refinedFootprint;
				float2 refinedUV;
				if (ProbeSampleDepth(origin + direction * refinedDistance, dimensions, refinedGap, refinedFootprint, refinedUV)) {
					hitT = refinedDistance;
					hitGap = refinedGap;
					hitFootprint = refinedFootprint;
					hitUV = refinedUV;
				}
			}
			if (hitGap >= 0 && hitGap <= max(thickness, 0.5) + hitFootprint) {
				float3 color = EnvironmentProbe::SampleRadiance(ProbeRadianceTexture, PROBE_TRACE_SAMPLER, hitUV, dimensions);
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

#endif
