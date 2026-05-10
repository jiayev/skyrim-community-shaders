#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

#define OMIT_PS_NAMESPACE
#define PS_PREPASS_SAMPLERS
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/VR.hlsli"
#include "PhysicalSky/Common.hlsli"

#if defined(HALF_RES)
#	define RES_MULT 2
#else
#	define RES_MULT 1
#endif

Texture2D<float> TexDepth : register(t0);
Texture2DArray<float4> TexDirectShadows : register(t1);

struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
};
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t98);
#define TERRAIN_SHADOW_REGISTER t3
#include "TerrainShadows/TerrainShadows.hlsli"
#define CLOUD_SHADOW_REGISTER t4
#include "CloudShadows/CloudShadows.hlsli"

RWTexture2D<unorm float> RWTexOutput : register(u0);

const static uint nStep = 30;
const static float rcpNStep = rcp(nStep);

float SampleShadow(float3 posWorldRel, uint eyeIndex)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

	float shadow = 1.0;

	// cloud shadows
	shadow *= Remap(CloudShadows::GetCloudShadowMult(posWorldRel, SampTr), data.cloudShadowRemapRange.x, data.cloudShadowRemapRange.y, 0, 1);
	[branch] if (all(shadow < 1e-8)) return 0;

	// dir shadow map
	{
		DirectionalShadowLightData directionalShadowLightData = DirectionalShadowLights[0];
		float shadowMapDepth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(posWorldRel, eyeIndex));

		[branch] if (directionalShadowLightData.EndSplitDistances.y > 0.0 &&
					 shadowMapDepth < directionalShadowLightData.EndSplitDistances.y)
		{
			float cascadeSelect = saturate(
				(shadowMapDepth - directionalShadowLightData.StartSplitDistances.y) /
				(directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));
			uint cascadeIndex = uint(cascadeSelect);

			float3 posWorldAbs = posWorldRel + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
			float3 positionLS = mul(directionalShadowLightData.ShadowProj[cascadeIndex], float4(posWorldAbs, 1)).xyz;
			float4 depths = TexDirectShadows.GatherRed(SampTr, float3(saturate(positionLS.xy), cascadeIndex), 0);
			shadow *= dot(float4(depths > positionLS.z), 0.25);
		}
	}
	[branch] if (all(shadow < 1e-8)) return 0;

	// terrain shadow
	float3 posWorldAbs = posWorldRel + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	shadow *= TerrainShadows::GetTerrainShadow(posWorldAbs, SampTr);
	[branch] if (all(shadow < 1e-8)) return 0;

	return shadow;
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const uint2 pxCoords = tid;

	const uint2 seed = Random::pcg2d(pxCoords.xy);
	const float2 rnd = Random::R2Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f);

	float2 stereoUv = (pxCoords + 0.5) * data.rcpFrameDim;
	if (stereoUv.x >= 1.f || stereoUv.y >= 1.f)
		return;
	stereoUv *= RES_MULT;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(stereoUv);
	const float2 uv = Stereo::ConvertFromStereoUV(stereoUv, eyeIndex);

	const float depth = TexDepth.SampleLevel(SampTr, stereoUv, 0);
	float4 posWorld = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	posWorld = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], posWorld);
	posWorld.xyz /= posWorld.w;

	float dist = length(posWorld.xyz);
	float distClamped = min(AP_MAX_DIST, dist);
	float3 dir = posWorld.xyz / dist;

	const float extGr = dot(data.rayleighScatter + data.aerosolAbsorption + data.aerosolScatter, 1 / 3.f);
	const float rcpExtGr = rcp(extGr);
	const float estContrib = 1 - exp(-extGr * distClamped);

	float shadow = 0;
	for (uint i = 1; i <= nStep; ++i) {
		float tSample = -rcpExtGr * log(1 - (i - 1 + rnd.x) * rcpNStep * estContrib);  // map to truncated exponential distribution

		float shadowSample = SampleShadow(dir * tSample, eyeIndex);
		shadow += (1 - shadowSample) * rcpNStep;
	}

	RWTexOutput[pxCoords] = shadow;
}
