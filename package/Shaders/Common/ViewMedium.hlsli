#ifndef COMMON_VIEW_MEDIUM_HLSLI
#define COMMON_VIEW_MEDIUM_HLSLI

#ifdef PHYSICAL_SKY
#	include "PhysicalSky/Common.hlsli"
#endif

#ifdef MEDIUM_COMPOSITE
Texture2D<float4> TexMediumCloudDepth : register(t41);
#else
Texture2D<float4> TexMediumCloudDepth : register(t116);
#endif

namespace ViewMedium
{
	float4 SampleHeightMedium(float3 position)
	{
		float4 result = float4(0, 0, 0, 1);
#ifdef EXP_HEIGHT_FOG
		if (SharedData::exponentialHeightFogSettings.enabled && dot(position, position) > 1e-8) {
			const float4 fog = ExponentialHeightFog::GetExponentialHeightFog(position, FrameBuffer::CameraPosAdjust.xyz, 0.0.xxx);
			result = float4(ColorManagement::SceneToLinear(fog.rgb) * fog.a, 1.0 - fog.a);
		}
#endif
		return result;
	}

	float4 SampleViewMedium(float3 position, float2 screenUV, SamplerState samplerState, bool fullCloudRay)
	{
		float4 medium = SampleHeightMedium(position);

#ifdef PHYSICAL_SKY
		if (!SharedData::physSkyData.enabled || !SharedData::physSkyData.enableVolumetricClouds)
			return medium;
		const float2 uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(screenUV);
		uint2 dimensions;
		TexMediumCloudDepth.GetDimensions(dimensions.x, dimensions.y);
		const float2 maximum = max(floor(float2(dimensions) * FrameBuffer::DynamicResolutionParams1.xy) - 0.5, 0.5);
		const float2 sampleUv = clamp(uv * float2(dimensions), 0.5, maximum) / float2(dimensions);
		const float4 metadata = TexMediumCloudDepth.SampleLevel(samplerState, sampleUv, 0);
		const float cloudDistance = metadata.x * 1000.0 / GAME_UNIT_TO_M;
		const float distance = length(position);
		const float visibility = fullCloudRay ? 1.0 : saturate((distance - cloudDistance) * GAME_UNIT_TO_M * 0.025);
		const float4 cloud = PhysSky::TexVolLum.SampleLevel(samplerState, sampleUv, 0);
		precise float cloudT = (1.0 - visibility) + visibility * PhysSky::TexVolTr.SampleLevel(samplerState, sampleUv, 0);
		if (cloudT >= 1.0)
			return medium;
		const float3 cloudL = cloud.rgb * visibility;
		const float4 front = SampleHeightMedium(position * (min(cloudDistance, distance) / max(distance, 1e-6)));
		medium.rgb = lerp(front.rgb, medium.rgb, cloudT) + front.a * cloudL;
		medium.a *= cloudT;
#endif
		return medium;
	}

	float4 SampleViewMedium(float3 position, float2 screenUV, SamplerState samplerState)
	{
		return SampleViewMedium(position, screenUV, samplerState, false);
	}

	float3 CompositeViewMedium(float3 color, float3 position, float2 screenUV, SamplerState samplerState, bool fullCloudRay)
	{
		const float4 medium = SampleViewMedium(position, screenUV, samplerState, fullCloudRay);
		return ColorManagement::SceneColor::ScaleAndAddLinear(color, medium.a, medium.rgb);
	}
}

#endif
