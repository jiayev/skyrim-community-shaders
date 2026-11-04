#include "ScreenSpaceReflections/common.hlsli"
SamplerState LinearSampler : register(s1);
#define PROBE_TRACE_VISIBILITY
#define PROBE_TRACE_OCCLUSION
#include "ScreenSpaceReflections/probeTrace.hlsli"
struct Ray
{
	float4 origin;
	float4 direction;
};
StructuredBuffer<Ray> Rays : register(t0);
RWTexture2D<float4> Output : register(u0);
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	Ray ray = Rays[id.x & 1023u];
	float3 radiance;
	float distance;
	bool unobstructed;
	bool occluded;
	bool hit = TraceEnvironmentProbe(ray.origin.xyz, ray.direction.xyz, ray.origin.w, ray.direction.w, radiance, distance, unobstructed, occluded);
	Output[uint2(id.x & 1023u, id.x >> 10)] = hit ? float4(radiance, distance) : float4(occluded, radiance.y, unobstructed, 0);
}
