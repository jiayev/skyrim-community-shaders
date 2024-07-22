#include "../Common/FrameBuffer.hlsl"
#include "../Common/GBuffer.hlsli"
#include "../Common/VR.hlsli"

Texture2D<half> DepthTexture : register(t0);

Texture2D<half4> SkylightingTexture : register(t1);
RWTexture2D<float4> SkylightingTextureRW : register(u0);

cbuffer PerFrame : register(b0)
{
	row_major float4x4 OcclusionViewProj;
	float4 EyePosition;
	float4 ShadowDirection;
	float4 BufferDim;
	float4 CameraData;
	uint FrameCount;
};

SamplerState LinearSampler : register(s0);

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

// samples = 8, min distance = 0.5, average samples on radius = 2
static const float3 g_Poisson8[8] = {
	float3(-0.4706069, -0.4427112, +0.6461146),
	float3(-0.9057375, +0.3003471, +0.9542373),
	float3(-0.3487388, +0.4037880, +0.5335386),
	float3(+0.1023042, +0.6439373, +0.6520134),
	float3(+0.5699277, +0.3513750, +0.6695386),
	float3(+0.2939128, -0.1131226, +0.3149309),
	float3(+0.7836658, -0.4208784, +0.8895339),
	float3(+0.1564120, -0.8198990, +0.8346850)
};

[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	half2 uv = half2(globalId.xy + 0.5) * (BufferDim.zw) * DynamicResolutionParams2.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half4 skylightingSH = 0;
	half weight = 0.0;

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	[unroll] for (uint i = 0; i < 8; i++)
	{

#if defined(HORIZONTAL)
		half2 testUV = uv + g_Poisson8[i].xy * BufferDim.zw * 8.0;
#else
		half2 testUV = uv + g_Poisson8[i].yx * BufferDim.zw * 16.0;
#endif

		if (any(testUV < 0) || any(testUV > 1))
			continue;

		half sampleDepth = GetScreenDepth(DepthTexture.SampleLevel(LinearSampler, testUV, 0));
		half attenuation = g_Poisson8[i].z * lerp(g_Poisson8[i].z * 0.01, 1.0, 1.0 - saturate(0.01 * abs(sampleDepth - depth)));

		[branch] if (attenuation > 0.0)
		{
			skylightingSH += SkylightingTexture.SampleLevel(LinearSampler, testUV, 0) * attenuation;
			weight += attenuation;
		}
	}

	SkylightingTextureRW[globalId.xy] = skylightingSH / weight;
}