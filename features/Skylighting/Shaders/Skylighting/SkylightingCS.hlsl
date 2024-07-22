#include "../Common/FrameBuffer.hlsl"
#include "../Common/GBuffer.hlsli"
#include "../Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "../Common/VR.hlsli"

Texture2D<float> DepthTexture : register(t0);

struct PerGeometry
{
	float4 VPOSOffset;
	float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam;
	float4 DebugColor;
	float4 PropertyColor;
	float4 AlphaTestRef;
	float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
	// Since PerGeometry is passed between c++ and hlsl, can't have different defines due to strong typing
	float4x3 ShadowMapProj[2][3];
	float4x4 CameraViewProjInverse[2];
};

Texture2DArray<unorm float> TexShadowMapSampler : register(t1);
StructuredBuffer<PerGeometry> perShadow : register(t2);
Texture2DArray<float4> BlueNoise : register(t3);
Texture2D<unorm float> OcclusionMapSampler : register(t4);

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
SamplerComparisonState ShadowSamplerPCF : register(s1);

half GetBlueNoise(half2 uv)
{
	return BlueNoise[uint3(uv % 128, FrameCount % 64)];
}

#if !defined(SHADOWMAP)
[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	float2 uv = float2(globalId.xy + 0.5) * (BufferDim.zw) * DynamicResolutionParams2.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);
	uv = ConvertFromStereoUV(uv, eyeIndex);

	float rawDepth = DepthTexture[globalId.xy];

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	float4 positionMS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	positionMS.xyz = positionMS.xyz + CameraPosAdjust[eyeIndex] - EyePosition;

	float3 startPositionMS = positionMS;

	half noise = GetBlueNoise(globalId.xy) * 2.0 * shPI;

	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	float2 PoissonDisc[] = {
		float2(0.881375f, 0.216315f),
		float2(0.0872829f, 0.987854f),
		float2(0.0710166f, 0.132633f),
		float2(0.517563f, 0.643117f)
	};

	uint sampleCount = 4;

	float occlusionThreshold = mul(OcclusionViewProj, float4(positionMS.xyz, 1)).z;

	bool fadeOut = length(startPositionMS) > 1024;

	half weight = 0;
	half wetnessWeight = 0;

	sh2 shSkylighting = shZero();
	float wetnessOcclusion = 0;

	half fadeFactor = pow(saturate(length(positionMS.xyz) / 10000.0), 8);

	[unroll] for (uint i = 0; i < sampleCount; i++)
	{
		float3 rayDir = float3(PoissonDisc[i].xy * 2.0 - 1.0, 0);
		rayDir.xy = mul(rayDir.xy, rotationMatrix);

		positionMS.xy = startPositionMS + rayDir.xy * 128;

		half3 occlusionPosition = mul(OcclusionViewProj, float4(positionMS.xyz, 1));
		occlusionPosition.y = -occlusionPosition.y;
		half2 occlusionUV = occlusionPosition.xy * 0.5 + 0.5;

		if ((occlusionUV.x == saturate(occlusionUV.x) && occlusionUV.y == saturate(occlusionUV.y)) || !fadeOut) {
			float shadowMapValues = OcclusionMapSampler.SampleLevel(LinearSampler, occlusionUV, 0);

			shadowMapValues = 1.0 - shadowMapValues;
			occlusionPosition.z = 1.0 - occlusionPosition.z;

			half skylightingContribution = 1.0 - saturate((shadowMapValues - occlusionPosition.z) * 4096);

			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, lerp(skylightingContribution, 1.0, fadeFactor)));

			weight++;
		} else {
			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, 1));
			weight++;
		}
	}

	if (weight > 0.0) {
		float shFactor = 4.0 * shPI * 1.0 / weight;
		shSkylighting = shScale(shSkylighting, shFactor);
	}

	SkylightingTextureRW[globalId.xy] = shSkylighting;
}
#else

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	float2 uv = float2(globalId.xy + 0.5) * BufferDim.zw * DynamicResolutionParams2.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);
	uv = ConvertFromStereoUV(uv, eyeIndex);

	float rawDepth = DepthTexture[globalId.xy];

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];

	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

	float4 positionMS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	float3 startPositionMS = positionMS;

	half fadeFactor = pow(saturate(dot(positionMS.xyz, positionMS.xyz) / sD.ShadowLightParam.z), 8);

	fadeFactor = lerp(1.0, fadeFactor, pow(saturate(dot(float3(0, 0, -1), ShadowDirection.xyz)), 0.5));

	half noise = GetBlueNoise(globalId.xy) * 2.0 * shPI;

	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	float2 PoissonDisc[] = {
		float2(0.881375f, 0.216315f),
		float2(0.0872829f, 0.987854f),
		float2(0.0710166f, 0.132633f),
		float2(0.517563f, 0.643117f)
	};

	uint sampleCount = 4;

	sh2 shSkylighting = shZero();

	half weight = 0.0;

	[unroll] for (uint i = 0; i < sampleCount; i++)
	{
		float3 rayDir = float3(PoissonDisc[i].xy * 2.0 - 1.0, 0);
		rayDir.xy = mul(rayDir.xy, rotationMatrix);

		positionMS.xy = startPositionMS + rayDir.xy * 128 + length(rayDir.xy) * ShadowDirection.xy * 128;

		rayDir = normalize(rayDir);

		float shadowMapDepth = length(positionMS.xyz);

		[flatten] if (sD.EndSplitDistances.z > shadowMapDepth)
		{
			half cascadeIndex = 0;
			float4x3 lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][0];

			[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][2];
				cascadeIndex = 2;
			}
			else if (sD.EndSplitDistances.x < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][1];
				cascadeIndex = 1;
			}

			float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionMS.xyz, 1)).xyz;

			half shadowMapValues = TexShadowMapSampler.SampleLevel(LinearSampler, float3(positionLS.xy, cascadeIndex), 0);

			shadowMapValues = 1.0 - shadowMapValues;
			positionLS.z = 1.0 - positionLS.z;

			half skylightingContribution = 1.0 - saturate((shadowMapValues - positionLS.z) * 4096);

			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, lerp(skylightingContribution, 1.0, fadeFactor)));

			weight++;
		}
		else
		{
			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, 1.0));

			weight++;
		}
	}

	if (weight > 0.0) {
		float shFactor = 4.0 * shPI * 1.0 / weight;
		shSkylighting = shScale(shSkylighting, shFactor);
	}

	SkylightingTextureRW[globalId.xy] = shSkylighting;
}
#endif