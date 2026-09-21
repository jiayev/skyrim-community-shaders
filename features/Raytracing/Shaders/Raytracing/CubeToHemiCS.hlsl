#include "Common/SharedData.hlsli"

TextureCube<float4> CubeMap : register(t0);
TextureCube<float> OcclusionMap : register(t1);

SamplerState Sampler : register(s0);

RWTexture2D<float4> HemisphereOut : register(u0);

[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (id.x >= RESOLUTION || id.y >= RESOLUTION)
		return;

	const float2 uv = float2(id.xy + 0.5f) / float2(RESOLUTION, RESOLUTION);
	const float2 xy = uv * 2.0f - 1.0f;

	const float r = min(length(xy), 1.0);

	const float phi = atan2(xy.y, xy.x);

	const float z = 1.0f - r * r;
	const float k = sqrt(1.0f - z * z);

	float3 dir = float3(k * cos(phi), k * sin(phi), z);
#ifdef PHYSICAL_SKY
	if (SharedData::physSkyData.enabled) {
		const float azimuth = (uv.x - 0.5) * 2.0 * 3.14159265359;
		const float zenith = uv.y * 3.14159265359;
		dir = float3(sin(zenith) * cos(azimuth), sin(zenith) * sin(azimuth), cos(zenith));
	}
#endif

	const float3 color = CubeMap.SampleLevel(Sampler, dir, 0.0f).rgb;
	const float occlusion = OcclusionMap.SampleLevel(Sampler, dir, 0.0f).r;

	HemisphereOut[id.xy] = float4(color, occlusion);
}