#ifndef DYNAMIC_CUBEMAPS_CAPTURE_COMMON_HLSLI
#define DYNAMIC_CUBEMAPS_CAPTURE_COMMON_HLSLI

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "DynamicCubemaps/CubemapCommon.hlsli"
#if defined(PROBE_CAPTURE)
#	include "Common/EnvironmentProbe.hlsli"
#	include "Common/EnvironmentProbeSampling.hlsli"
#endif

RWTexture2DArray<float4> DynamicCubemap : register(u0);
RWTexture2DArray<float4> DynamicCubemapRaw : register(u1);
RWTexture2DArray<float4> DynamicCubemapPosition : register(u2);

struct CaptureLightingState
{
	float ReferenceLuminance;
	uint PendingResetMask;
	uint Reset;
	uint Initialized;
};

RWStructuredBuffer<CaptureLightingState> LightingState : register(u3);

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> ColorTexture : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer UpdateData : register(b0)
{
	float3 CameraPreviousPosAdjust2;
	uint CaptureIndex;
	float CaptureDeltaTime;
	uint ResetCapture;
	uint2 UpdatePadding;
	float4 ProbeOriginOffsets[8];
	uint ProbeValidFaces;
	float ProbeFarDistance;
	uint2 ProbePadding;
}

static const float CaptureHistoryLifetime = 30.0;

float GetCaptureTime()
{
	return floor(fmod(SharedData::Timer, 64.0) * 16.0) / 16.0;
}

bool CaptureHistoryExpired(float lastSeen)
{
	return CaptureDeltaTime >= CaptureHistoryLifetime || fmod(GetCaptureTime() - lastSeen + 64.0, 64.0) >= CaptureHistoryLifetime;
}

float3 GetSamplingVector(uint3 texel)
{
	uint width, height, faces;
	DynamicCubemap.GetDimensions(width, height, faces);
	return GetCubemapSamplingVector(texel, float2(width, height));
}

#if defined(PROBE_CAPTURE)
bool ProbeTexelAvailable(float2 uv)
{
	uint face = EnvironmentProbe::GetFace(EnvironmentProbe::Decode(uv));
	return (ProbeValidFaces & (1u << face)) != 0;
}
#endif

bool SampleCapture(uint3 texel, out float3 position, out float3 color, out float2 uv)
{
	position = 0.0;
	color = 0.0;
#if defined(PROBE_CAPTURE)
	uint width, height;
	DepthTexture.GetDimensions(width, height);
	uv = EnvironmentProbe::Encode(GetSamplingVector(texel));
	float2 radianceUV = uv;
	uint2 pixel = min(uint2(saturate(uv) * float2(width, height)), uint2(width, height) - 1);
	uv = (pixel + 0.5) / float2(width, height);
	float3 ray = EnvironmentProbe::Decode(uv);
	uint face = EnvironmentProbe::GetFace(ray);
	if ((ProbeValidFaces & (1u << face)) == 0)
		return false;
	float distance = DepthTexture.Load(int3(pixel, 0));
	if (!isfinite(distance) || distance <= 0 || distance >= ProbeFarDistance)
		return false;
	// The probe already stores scene-linear radiance without pre-exposure. Each
	// rotating octant retains its own origin, independent of the current view.
	position = ray * (distance * 0.001) + ProbeOriginOffsets[face].xyz;
	color = EnvironmentProbe::SampleRadiance(ColorTexture, LinearSampler, radianceUV, uint2(width, height));
#else
	float3 viewDirection = FrameBuffer::WorldToView(-GetSamplingVector(texel), false);
	uv = FrameBuffer::ViewToUV(viewDirection, false);
	if (viewDirection.z >= 0.0 || !all(isfinite(uv)) || FrameBuffer::IsOutsideFrame(uv))
		return false;

	float2 sampleUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
#	if defined(REFLECTIONS)
	bool excludeSky = SharedData::PostWaterComposite != 0;
#	else
	bool excludeSky = true;
#	endif
	if (excludeSky) {
		float4 depths = DepthTexture.GatherRed(LinearSampler, sampleUV);
		if (any(depths >= 1.0))
			return false;
	}
	float depth = DepthTexture.SampleLevel(LinearSampler, sampleUV, 0);
	if (SharedData::GetScreenDepth(depth) <= 16.5)
		return false;

	float4 positionCS = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth, 1.0));
	position = positionCS.xyz / positionCS.w * 0.001;
	// The capture and its amortized filtering retain physical units, independent
	// of the exposure of the screen frame from which each sample was captured.
	color = ColorManagement::SceneToLinear(PreExposure::Decode(ColorTexture.SampleLevel(LinearSampler, sampleUV, 0).rgb));
#endif
	if (!all(isfinite(position)) || !all(isfinite(color)))
		return false;
	color = clamp(color, 0.0, 65504.0);
	return true;
}

float3 AdjustCapturePosition(float3 position)
{
	return position + (CameraPreviousPosAdjust2 - FrameBuffer::CameraPosAdjust.xyz) * 0.001;
}

bool CaptureGeometryMatches(float3 previousPosition, float3 position)
{
	return length(previousPosition - position) <= max(0.032, length(position) * 0.03);
}

#endif
