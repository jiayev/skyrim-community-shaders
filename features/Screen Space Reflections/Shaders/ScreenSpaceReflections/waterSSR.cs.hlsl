// Independent post-water Hi-Z SSR. Water.hlsl writes its final perturbed
// world-space normal to a dedicated MRT; this pass traces against the opaque
// Hi-Z pyramid and composites into kMAIN only after water refraction is done.
//
// Hierarchical traversal derived from AMD FidelityFX SSSR.
// Copyright (C) 2024 Advanced Micro Devices, Inc. — SPDX-License-Identifier: MIT

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/SharedData.hlsli"
#include "ScreenSpaceReflections/common.hlsli"

Texture2D<float> WaterDepthTexture : register(t0);
Texture2D<float4> WaterNormalTexture : register(t1);
Texture2D<float4> ScreenColorTexture : register(t2);
Texture2D<float> HiZDepthTexture : register(t3);

RWTexture2D<float4> OutScreenColor : register(u0);

static const float kFloatMax = 3.402823466e+38;

float3 ProjectPosition(float3 position)
{
	float4 projected = mul(FrameBuffer::CameraProj, float4(position, 1.0));
	projected.xyz /= projected.w;
	projected.xy = projected.xy * float2(0.5, -0.5) + 0.5;
	return projected.xyz;
}

float3 ProjectDirection(float3 origin, float3 direction, float3 projectedOrigin)
{
	return ProjectPosition(origin + direction) - projectedOrigin;
}

float3 InvProjectPosition(float3 coord)
{
	coord.y = 1.0 - coord.y;
	coord.xy = coord.xy * 2.0 - 1.0;
	float4 projected = mul(FrameBuffer::CameraProjInverse, float4(coord, 1.0));
	return projected.xyz / projected.w;
}

float2 GetMipResolution(float2 screenDimensions, int mip)
{
	return screenDimensions * exp2(-float(mip));
}

float LoadHiZ(int2 pixel, int mip)
{
	return HiZDepthTexture.Load(int3(pixel, mip));
}

void InitialAdvanceRay(
	float3 origin,
	float3 direction,
	float3 invDirection,
	float2 mipResolution,
	float2 rcpMipResolution,
	float2 floorOffset,
	float2 uvOffset,
	out float3 position,
	out float currentT)
{
	float2 mipPosition = mipResolution * origin.xy;
	float2 boundary = (floor(mipPosition) + floorOffset) * rcpMipResolution + uvOffset;
	float2 t = (boundary - origin.xy) * invDirection.xy;
	currentT = min(t.x, t.y);
	position = origin + currentT * direction;
}

bool AdvanceRay(
	float3 origin,
	float3 direction,
	float3 invDirection,
	float2 mipPosition,
	float2 rcpMipResolution,
	float2 floorOffset,
	float2 uvOffset,
	float surfaceZ,
	inout float3 position,
	inout float currentT)
{
	float2 xyBoundary = (floor(mipPosition) + floorOffset) * rcpMipResolution + uvOffset;
	float3 boundaries = float3(xyBoundary, surfaceZ);
	float3 t = (boundaries - origin) * invDirection;
	t.z = direction.z > 0.0 ? t.z : kFloatMax;

	float tMin = min(min(t.x, t.y), t.z);
	bool aboveSurface = surfaceZ > position.z;
	bool skippedTile = asuint(tMin) != asuint(t.z) && aboveSurface;
	currentT = aboveSurface ? tMin : currentT;
	position = origin + currentT * direction;
	return skippedTile;
}

float3 HierarchicalRaymarch(
	float3 origin,
	float3 direction,
	float2 screenSize,
	uint maxMip,
	uint maxIterations,
	out bool validHit)
{
	float3 invDirection = abs(direction) > 1.0e-12 ? rcp(direction) : kFloatMax;
	int currentMip = 0;
	float2 mipResolution = GetMipResolution(screenSize, currentMip) * FrameBuffer::DynamicResolutionParams1.xy;
	float2 rcpMipResolution = rcp(mipResolution);
	float2 uvOffset = 0.005 / (screenSize * FrameBuffer::DynamicResolutionParams1.xy);
	uvOffset = direction.xy < 0.0 ? -uvOffset : uvOffset;
	float2 floorOffset = direction.xy < 0.0 ? 0.0 : 1.0;

	float currentT;
	float3 position;
	InitialAdvanceRay(origin, direction, invDirection, mipResolution, rcpMipResolution, floorOffset, uvOffset, position, currentT);

	uint iteration = 0;
	bool inBounds = true;
	while (iteration < maxIterations && currentMip >= 0) {
		inBounds = all(position.xy >= 0.0) && all(position.xy <= 1.0) && position.z <= 1.0 - 1.0e-6;
		if (!inBounds)
			break;

		float2 mipPosition = mipResolution * position.xy;
		float surfaceZ = LoadHiZ(mipPosition, currentMip);
		bool skippedTile = AdvanceRay(origin, direction, invDirection, mipPosition, rcpMipResolution, floorOffset, uvOffset, surfaceZ, position, currentT);
		bool nextMipOutOfRange = skippedTile && currentMip >= int(maxMip);
		if (!nextMipOutOfRange) {
			currentMip += skippedTile ? 1 : -1;
			mipResolution *= skippedTile ? 0.5 : 2.0;
			rcpMipResolution *= skippedTile ? 2.0 : 0.5;
		}
		++iteration;
	}

	validHit = inBounds && currentMip < 0 && iteration < maxIterations;
	return position;
}

float ValidateHit(float3 hit, float2 originUV, float2 screenSize, float thickness)
{
	if (any(hit.xy < 0.0) || any(hit.xy > 1.0))
		return 0.0;

	int2 texel = int2(screenSize * hit.xy * FrameBuffer::DynamicResolutionParams1.xy);
	float surfaceZ = LoadHiZ(texel / 2, 1);
	if (surfaceZ >= 1.0 - 1.0e-6)
		return 0.0;

	float3 viewSurface = InvProjectPosition(float3(hit.xy, surfaceZ));
	float3 viewHit = InvProjectPosition(hit);
	float distanceToSurface = length(viewSurface - viewHit);
	float confidence = 1.0 - smoothstep(0.0, thickness, distanceToSurface);
	confidence *= confidence;

	float2 renderSize = screenSize * FrameBuffer::DynamicResolutionParams1.xy;
	float2 originDistance = abs(hit.xy - originUV);
	if (originDistance.x < 2.0 / renderSize.x && originDistance.y < 2.0 / renderSize.y)
		return 0.0;

	return confidence;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint2 pixel = dispatchThreadID.xy;
	uint2 screenSize = SharedData::BufferDim.xy;
	uint2 activeSize = uint2(screenSize * FrameBuffer::DynamicResolutionParams1.xy);
	if (any(pixel >= activeSize))
		return;

	float4 baseColor = ScreenColorTexture.Load(int3(pixel, 0));
	float4 waterNormal = WaterNormalTexture.Load(int3(pixel, 0));
	if (waterNormal.w <= 0.0) {
		OutScreenColor[pixel] = baseColor;
		return;
	}

	float depth = WaterDepthTexture.Load(int3(pixel, 0));
	if (depth >= 1.0 - 1.0e-6) {
		OutScreenColor[pixel] = baseColor;
		return;
	}

	float2 uv = float2(pixel + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
	float3 viewPosition = InvProjectPosition(float3(uv, depth));
	float3 viewDirection = normalize(viewPosition);
	float3 viewNormal = normalize(mul(FrameBuffer::CameraView, float4(normalize(waterNormal.xyz), 0.0)).xyz);

	viewPosition += viewNormal * NormalBias * viewPosition.z * GAME_UNIT_TO_M;
	float3 reflectedDirection = reflect(viewDirection, viewNormal);
	float3 projectedOrigin = ProjectPosition(viewPosition);
	float3 projectedDirection = ProjectDirection(viewPosition, reflectedDirection, projectedOrigin);

	bool validHit;
	float3 hit = HierarchicalRaymarch(
		projectedOrigin,
		projectedDirection,
		float2(screenSize),
		max(SpecMaxMips, 1u) - 1u,
		max(SpecMaxSteps, 1u),
		validHit);

	float confidence = validHit ? ValidateHit(hit, uv, float2(screenSize), SpecThickness) : 0.0;
	if (confidence <= 0.0) {
		OutScreenColor[pixel] = baseColor;
		return;
	}

	float3 reflection = ScreenColorTexture.SampleLevel(samplerLinearClamp, hit.xy * FrameBuffer::DynamicResolutionParams1.xy, 0).xyz;
	const float waterF0 = 0.02;
	float NdotV = saturate(dot(-viewDirection, viewNormal));
	float fresnel = waterF0 + (1.0 - waterF0) * pow(1.0 - NdotV, 5.0);
	OutScreenColor[pixel] = float4(lerp(baseColor.xyz, reflection, confidence * fresnel), baseColor.w);
}
