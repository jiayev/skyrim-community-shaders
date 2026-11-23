#ifndef PHYSICAL_SKY_CLOUD_AMBIENT_HLSLI
#define PHYSICAL_SKY_CLOUD_AMBIENT_HLSLI

cbuffer CloudAmbientVolumeConstants : register(b2)
{
	float4 CloudAmbientPhase;
};

float WeatherAmbientVisibility(float3 pos);

namespace CloudAmbientLight
{
	float3 MiddleSource(float2 opticalDepth)
	{
		const float3 sky = 0.5 * SampleCloudAmbientRadiance();
		const float3 lower = 0.5 * TexCloudAmbientSH[int2(4, 0)].rgb;
		const float2 response = MiddleAmbientResponse(opticalDepth);
		return sky * response.x + lower * response.y;
	}

	float3 SkySource(float3 pos)
	{
		// The shared-light volume stores the hemispherical occlusion.
		const float3 sky = SampleCloudAmbientRadiance();
		return lerp(length(sky).xxx, sky, 0.7) * WeatherAmbientVisibility(pos);
	}
}

#endif
