#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

#define OMIT_PS_NAMESPACE
#define PS_PREPASS_SAMPLERS
#define PS_PREPASS_RSRCS
#define PS_NO_RSRCS
#define PS_ENABLE_DIRLIGHT_TRANSMITTANCE
#define PS_LINEAR_SHADOW_SAMPLER
#define CLOUD_SHADOW_REGISTER t4
#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
Texture3D<float> TexShadowVolume : register(t5);
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
#include "CloudShadows/CloudShadows.hlsli"
#include "TerrainShadows/TerrainShadows.hlsli"

RWTexture2D<unorm float> RWTexOutput : register(u0);

const static uint nStep = 30;
const static float rcpNStep = rcp(nStep);

float3 GetVolumetricCloudTransmittance(float3 posWorldRel)
{
	return GetDirlightTransmittance(posWorldRel + FrameBuffer::CameraPosAdjust.xyz, SampTr);
}

struct ShadowRayData
{
	float2 endSplitDistances;
	float2 startSplitDistances;
	float2 viewOriginZW;
	float2 viewDirectionZW;
	float3 lightOrigin[2];
	float3 lightDirection[2];
};

ShadowRayData BuildShadowRay(float3 dir)
{
	const DirectionalShadowLightData light = DirectionalShadowLights[0];
	ShadowRayData ray;
	ray.endSplitDistances = light.EndSplitDistances;
	ray.startSplitDistances = light.StartSplitDistances;
	ray.viewOriginZW = mul(FrameBuffer::CameraViewProj, float4(0, 0, 0, 1)).zw;
	ray.viewDirectionZW = mul(FrameBuffer::CameraViewProj, float4(dir, 0)).zw;
	[unroll] for (uint cascade = 0; cascade < 2; ++cascade)
	{
		ray.lightOrigin[cascade] = mul(light.ShadowProj[cascade], float4(FrameBuffer::CameraPosAdjust.xyz, 1)).xyz;
		ray.lightDirection[cascade] = mul(light.ShadowProj[cascade], float4(dir, 0)).xyz;
	}
	return ray;
}

float SampleShadow(float3 posWorldRel, float distance, ShadowRayData ray)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

	float shadow = 1.0;

	// Resolve scene and terrain occlusion first: a blocked sample needs neither
	// spherical cloud projection nor a 3D cloud-shadow lookup.
	{
		const float2 viewZW = ray.viewOriginZW + distance * ray.viewDirectionZW;
		float shadowMapDepth = SharedData::GetScreenDepth(viewZW.x / viewZW.y);

		[branch] if (ray.endSplitDistances.y > 0.0 &&
					 shadowMapDepth < ray.endSplitDistances.y)
		{
			float cascadeSelect = saturate(
				(shadowMapDepth - ray.startSplitDistances.y) /
				(ray.endSplitDistances.x - ray.startSplitDistances.y));
			uint cascadeIndex = uint(cascadeSelect);

			float3 positionLS = ray.lightOrigin[cascadeIndex] + distance * ray.lightDirection[cascadeIndex];
			float4 depths = TexDirectShadows.GatherRed(SampTr, float3(saturate(positionLS.xy), cascadeIndex), 0);
			shadow *= dot(float4(depths > positionLS.z), 0.25);
		}
	}
	[branch] if (all(shadow < 1e-8)) return 0;

	// terrain shadow
	float3 posWorldAbs = posWorldRel + FrameBuffer::CameraPosAdjust.xyz;
	shadow *= TerrainShadows::GetTerrainShadow(posWorldAbs, SampTr);
	[branch] if (all(shadow < 1e-8)) return 0;

	float cloudShadow = 1.0;
	[branch] if (SharedData::cloudShadowsSettings.Opacity != 0.0)
		cloudShadow = CloudShadows::GetCloudShadowMult(posWorldRel, SampTr);
	shadow *= Remap(cloudShadow, data.cloudShadowRemapRange.x, data.cloudShadowRemapRange.y, 0, 1);
	[branch] if (all(shadow < 1e-8)) return 0;

	shadow *= Remap(dot(GetVolumetricCloudTransmittance(posWorldRel), float3(0.2126, 0.7152, 0.0722)), data.cloudShadowRemapRange.x, data.cloudShadowRemapRange.y, 0, 1);
	[branch] if (all(shadow < 1e-8)) return 0;

	return shadow;
}

float TraceShadow(float2 pixelCenter, float depth, float jitter)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	const float2 uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(pixelCenter * data.rcpTexDim);
	float4 posWorld = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	posWorld = mul(FrameBuffer::CameraViewProjInverse, posWorld);
	posWorld.xyz /= posWorld.w;

	float dist = length(posWorld.xyz);
	if (dist <= 0.0)
		return 0.0;
	float distClamped = min(AP_MAX_DIST, dist);
	float3 dir = posWorld.xyz / dist;
	const ShadowRayData shadowRay = BuildShadowRay(dir);

	const float extGr = dot(data.rayleighScatter + data.aerosolAbsorption + data.aerosolScatter, 1 / 3.f);
	const float opticalDepth = extGr * distClamped;
	const float estContrib = 1 - exp(-opticalDepth);
	const float rcpExtGr = rcp(max(extGr, 1e-20));

	float shadow = 0;
	for (uint i = 1; i <= nStep; ++i) {
		const float sampleFraction = (i - 1 + jitter) * rcpNStep;
		float tSample;
		[branch] if (opticalDepth < 1e-3)
			tSample = sampleFraction * distClamped * (1.0 - 0.5 * (1.0 - sampleFraction) * opticalDepth);
		else tSample = -rcpExtGr * log(1 - sampleFraction * estContrib);

		float shadowSample = SampleShadow(dir * tSample, tSample, shadowRay);
		shadow += 1 - shadowSample;
	}

	return shadow * rcpNStep;
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const uint2 frameDim = uint2(SharedData::physSkyData.frameDim);
	const uint2 pxCoords = tid * RES_MULT;
	if (any(pxCoords >= frameDim))
		return;

	const uint2 seed = Random::pcg2d(tid);
	const float jitter = Random::R2Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f).x;

#if defined(HALF_RES)
	const uint2 lastCoords = min(pxCoords + 1u, frameDim - 1u);
	const float4 depths = float4(
		TexDepth[pxCoords],
		TexDepth[uint2(lastCoords.x, pxCoords.y)],
		TexDepth[uint2(pxCoords.x, lastCoords.y)],
		TexDepth[lastCoords]);
	const float4 linearDepths = SharedData::GetScreenDepths(depths);
	const float minDepth = min(min(linearDepths.x, linearDepths.y), min(linearDepths.z, linearDepths.w));
	const float maxDepth = max(max(linearDepths.x, linearDepths.y), max(linearDepths.z, linearDepths.w));
	const bool sharedRay = maxDepth - minDepth <= minDepth * 0.01;
	const uint rayCount = sharedRay ? 1u : 4u;
	[loop] for (uint i = 0; i < rayCount; ++i)
	{
		const uint2 pixel = pxCoords + uint2(i & 1u, i >> 1u);
		if (any(pixel >= frameDim))
			continue;
		const float2 pixelCenter = sharedRay ? (float2(pxCoords) + float2(lastCoords) + 1.0) * 0.5 : pixel + 0.5;
		const float depth = sharedRay ? dot(depths, 0.25) : depths[i];
		const float shadow = TraceShadow(pixelCenter, depth, jitter);
		[branch] if (sharedRay)
		{
			[unroll] for (uint j = 0; j < 4; ++j)
			{
				const uint2 outputPixel = pxCoords + uint2(j & 1u, j >> 1u);
				if (all(outputPixel < frameDim))
					RWTexOutput[outputPixel] = shadow;
			}
		}
		else
		{
			RWTexOutput[pixel] = shadow;
		}
	}
#else
	RWTexOutput[pxCoords] = TraceShadow(pxCoords + 0.5, TexDepth[pxCoords], jitter);
#endif
}
