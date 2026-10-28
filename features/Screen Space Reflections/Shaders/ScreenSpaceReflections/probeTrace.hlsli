#ifndef SSR_PROBE_TRACE
#define SSR_PROBE_TRACE

#include "Common/EnvironmentProbe.hlsli"
#include "Common/EnvironmentProbeSampling.hlsli"

Texture2D<float4> ProbeRadianceTexture : register(t7);
Texture2D<float> ProbeDepthTexture : register(t8);
TextureCube<float3> ReflectionCubemap : register(t9);

// Clip the physical ray against the six view-frustum planes. The Hi-Z cursor can
// overshoot a screen edge by a coarse tile; continuing there would skip geometry.
float ProbeScreenExitDistance(float4 originClip, float4 directionClip)
{
	float result = 1e30;
	float planes[6] = { originClip.w + originClip.x, originClip.w - originClip.x,
		originClip.w + originClip.y, originClip.w - originClip.y, originClip.z, originClip.w - originClip.z };
	float slopes[6] = { directionClip.w + directionClip.x, directionClip.w - directionClip.x,
		directionClip.w + directionClip.y, directionClip.w - directionClip.y, directionClip.z, directionClip.w - directionClip.z };
	[unroll] for (uint i = 0; i < 6; ++i) if (slopes[i] < -1e-8)
		result = min(result, max(0.0, -planes[i] / slopes[i]));
	return result;
}

#define PROBE_TRACE_SAMPLER LinearSampler
#define PROBE_TRACE_VISIBILITY
#if defined(PROBE_HIERARCHY)
#	include "Common/EnvironmentProbeHierarchy.hlsli"
#else
#	include "Common/EnvironmentProbeTrace.hlsli"
#endif
#undef PROBE_TRACE_SAMPLER
#include "Common/EnvironmentProbeVisibility.hlsli"

// Keep the public intersection-only entry point for callers that do not resolve
// lighting fallbacks (including raster/hierarchy regression fixtures).
bool TraceEnvironmentProbe(float3 origin, float3 direction, float startDistance, float thickness,
	out float3 radiance, out float hitDistance)
{
	bool unobstructed;
	return TraceEnvironmentProbe(origin, direction, startDistance, thickness, radiance, hitDistance, unobstructed);
}

#endif
