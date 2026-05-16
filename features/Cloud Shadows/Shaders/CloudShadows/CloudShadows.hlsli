#ifndef CLOUD_SHADOWS_HLSLI
#define CLOUD_SHADOWS_HLSLI

#ifndef CLOUD_SHADOW_REGISTER
#	define CLOUD_SHADOW_REGISTER t25
#endif

// TODO move to PSky
#include "Common/Game.hlsli"

namespace CloudShadows
{
	TextureCube<float> CloudShadowsTexture : register(CLOUD_SHADOW_REGISTER);

	// 2 km cloud altitude / 6371 km planet radius converted from metres
	// to game units. The original 1.428e-2 is GAME_UNIT_TO_M (1.428f / 100),
	// so dividing by GAME_UNIT_TO_M is the exact same value. Game.hlsli
	// parenthesizes the macro, so the operator precedence is preserved.
	const static float CloudHeight = (2e3f / GAME_UNIT_TO_M) * 0.25;
	const static float PlanetRadius = (6371e3f / GAME_UNIT_TO_M);
	const static float RcpHPlusR = (1.0 / (CloudHeight + PlanetRadius));

	float IntersectCloudDist(float3 rel_pos, float3 dir)
	{
		float r = PlanetRadius;
		float3 p = (rel_pos + float3(0, 0, r)) * RcpHPlusR;
		float dotprod = dot(p, dir);
		float lengthsqr = dot(p, p);
		if (lengthsqr > 1)
			return -1;
		float t = -dotprod + sqrt(dotprod * dotprod - lengthsqr + 1);
		return t * (r + CloudHeight);
	}

	float GetCloudShadowMult(float3 worldPosition, SamplerState textureSampler)
	{
		float cloudDist = IntersectCloudDist(worldPosition, SharedData::DirLightDirection.xyz);
		if (cloudDist < 0)
			return 1.0;

		float3 cloudSampleDir = worldPosition + cloudDist * SharedData::DirLightDirection.xyz;
		float cloudCubeSample = CloudShadowsTexture.SampleLevel(textureSampler, cloudSampleDir, 0).x;
		return lerp(1.0, 1.0 - cloudCubeSample, SharedData::cloudShadowsSettings.Opacity);
	}
}
#endif  // CLOUD_SHADOWS_HLSLI
