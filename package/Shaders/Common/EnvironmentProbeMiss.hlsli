#ifndef ENVIRONMENT_PROBE_MISS_HLSLI
#define ENVIRONMENT_PROBE_MISS_HLSLI

// An empty texel defines a convex angular cone of captured free space. A
// segment whose endpoints stay in that cone needs no additional marching.
// Other misses remain unknown; this certificate never consumes trace budget.
bool ProbeKnownEmptySegment(float3 origin, float3 direction, float begin, float end, uint2 dimensions)
{
	uint face = EnvironmentProbe::GetFace(direction);
	if ((ProbeValidFaces & (1u << face)) == 0)
		return false;
	float3 localOrigin = origin - ProbeOriginOffsets[face].xyz;
	float3 a = localOrigin + direction * begin;
	float3 b = localOrigin + direction * end;
	if (EnvironmentProbe::GetFace(a) != face || EnvironmentProbe::GetFace(b) != face)
		return false;
	uint2 first = min(uint2(saturate(EnvironmentProbe::Encode(a)) * dimensions), dimensions - 1);
	uint2 last = min(uint2(saturate(EnvironmentProbe::Encode(b)) * dimensions), dimensions - 1);
	if (any(first != last) || EnvironmentProbe::GetFace(EnvironmentProbe::Decode((first + 0.5) / dimensions)) != face)
		return false;
	float depth = ProbeDepthTexture.Load(int3(first, 0));
	return isfinite(depth) && depth >= ProbeFarDistance;
}
#endif
