#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

Texture2D<unorm float> OcclusionMapSampler : register(t29);
Texture2D<float4> SkylightingTexture : register(t30);

cbuffer SkylightingData : register(b8)
{
	row_major float4x4 OcclusionViewProj;
	float4 EyePosition;
	float4 ShadowDirection;
};

float GetVLSkylighting(float3 startPosWS, float3 endPosWS, float2 screenPosition)
{
	const static uint nSteps = 16;
	const static float step = 1.0 / float(nSteps);

	float3 worldDir = endPosWS - startPosWS;

	float noise = InterleavedGradientNoise(screenPosition);

	startPosWS += worldDir * step * noise;

	float noise2 = noise * 2.0 * M_PI;

	half2x2 rotationMatrix = half2x2(cos(noise2), sin(noise2), -sin(noise2), cos(noise2));

	float vl = 0;

	half2 PoissonDisk[16] = {
		half2(-0.94201624, -0.39906216),
		half2(0.94558609, -0.76890725),
		half2(-0.094184101, -0.92938870),
		half2(0.34495938, 0.29387760),
		half2(-0.91588581, 0.45771432),
		half2(-0.81544232, -0.87912464),
		half2(-0.38277543, 0.27676845),
		half2(0.97484398, 0.75648379),
		half2(0.44323325, -0.97511554),
		half2(0.53742981, -0.47373420),
		half2(-0.26496911, -0.41893023),
		half2(0.79197514, 0.19090188),
		half2(-0.24188840, 0.99706507),
		half2(-0.81409955, 0.91437590),
		half2(0.19984126, 0.78641367),
		half2(0.14383161, -0.14100790)
	};

	for (uint i = 0; i < nSteps; ++i) {
		float t = saturate(i * step);

		float shadow = 0;
		{
			float3 samplePositionWS = startPosWS + worldDir * t;

			half2 offset = mul(PoissonDisk[(float(i) + noise) % 16].xy, rotationMatrix);
			samplePositionWS.xy += offset * 128;

			half3 samplePositionLS = mul(OcclusionViewProj, float4(samplePositionWS.xyz, 1));
			samplePositionLS.y = -samplePositionLS.y;

			float deltaZ = samplePositionLS.z - (1e-2 * 0.05);

			half2 occlusionUV = samplePositionLS.xy * 0.5 + 0.5;

			float4 depths = OcclusionMapSampler.GatherRed(LinearSampler, occlusionUV.xy, 0);

			shadow = dot(depths > deltaZ, 0.25);
		}
		vl += shadow;
	}
	return vl * step;
}

float GetSkylightOcclusion(float3 worldPosition, float noise)
{
	const static uint nSteps = 16;
	const static float step = 1.0 / float(nSteps);

	float noise2 = noise * 2.0 * M_PI;

	half2x2 rotationMatrix = half2x2(cos(noise2), sin(noise2), -sin(noise2), cos(noise2));

	float vl = 0;

	half2 PoissonDisk[16] = {
		half2(-0.94201624, -0.39906216),
		half2(0.94558609, -0.76890725),
		half2(-0.094184101, -0.92938870),
		half2(0.34495938, 0.29387760),
		half2(-0.91588581, 0.45771432),
		half2(-0.81544232, -0.87912464),
		half2(-0.38277543, 0.27676845),
		half2(0.97484398, 0.75648379),
		half2(0.44323325, -0.97511554),
		half2(0.53742981, -0.47373420),
		half2(-0.26496911, -0.41893023),
		half2(0.79197514, 0.19090188),
		half2(-0.24188840, 0.99706507),
		half2(-0.81409955, 0.91437590),
		half2(0.19984126, 0.78641367),
		half2(0.14383161, -0.14100790)
	};

	for (uint i = 0; i < nSteps; ++i) {
		float t = saturate(i * step);

		float shadow = 0;
		{
			float3 samplePositionWS = worldPosition;

			half2 offset = mul(PoissonDisk[i].xy, rotationMatrix);
			samplePositionWS.xy += offset * 16;

			half3 samplePositionLS = mul(OcclusionViewProj, float4(samplePositionWS.xyz, 1));
			samplePositionLS.y = -samplePositionLS.y;

			float deltaZ = samplePositionLS.z - (1e-2 * 0.2);

			half2 occlusionUV = samplePositionLS.xy * 0.5 + 0.5;

			float4 depths = OcclusionMapSampler.GatherRed(LinearSampler, occlusionUV.xy, 0);

			shadow = dot(depths > deltaZ, 0.25);
		}
		vl += shadow;
	}
	return sqrt(vl * step);
}
