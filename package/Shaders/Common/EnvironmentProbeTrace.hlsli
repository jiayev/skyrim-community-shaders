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
		face = EnvironmentProbe::GetFace(position - ProbeOriginOffsets[face].xyz);
	float3 delta = position - ProbeOriginOffsets[face].xyz;
	float radius = length(delta);
	if ((ProbeValidFaces & (1u << face)) == 0 || radius <= 1e-4 || EnvironmentProbe::GetFace(delta) != face)
		return false;
	uv = EnvironmentProbe::Encode(delta);
	uint2 pixel = min(uint2(saturate(uv) * dimensions), dimensions - 1);
	uint pixelFace = EnvironmentProbe::GetFace(EnvironmentProbe::Decode((pixel + 0.5) / dimensions));
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
		float fraction = float(i) / steps;
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
		if (valid && previousValid && previousGap <= 0 && gap >= 0) {
			float lo = previousDistance;
			float hi = distance;
			bool bracketValid = true;
			[loop] for (uint j = 0; j < 6; ++j)
			{
				float mid = (lo + hi) * 0.5;
				float midGap, midFootprint;
				float2 midUV;
				if (!ProbeSampleDepth(origin + direction * mid, dimensions, midGap, midFootprint, midUV)) {
					bracketValid = false;
					break;
				}
				if (midGap < 0)
					lo = mid;
				else
					hi = mid;
			}
			float hitGap, hitFootprint;
			float2 hitUV;
			if (bracketValid && ProbeSampleDepth(origin + direction * hi, dimensions, hitGap, hitFootprint, hitUV) &&
				hitGap >= 0 && hitGap <= max(thickness, 0.5) + hitFootprint) {
				float3 color = EnvironmentProbe::SampleRadiance(ProbeRadianceTexture, PROBE_TRACE_SAMPLER, hitUV, dimensions);
				if (all(isfinite(color))) {
					radiance = max(color, 0);  // already scene-linear, without pre-exposure
					hitDistance = hi;
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
