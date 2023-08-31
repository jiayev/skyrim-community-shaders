#include "Common/VR.hlsl"

struct LightGrid
{
	uint offset;
	uint lightCount;
};

struct StructuredLight
{
	float3 color;
	float radius;
	float3 positionWS[2];
	float3 positionVS[2];
	uint firstPersonShadow;
};

struct PerPassLLF
{
	bool EnableGlobalLights;
	bool EnableContactShadows;
	bool EnableLightsVisualisation;
	uint LightsVisualisationMode;
	uint StrictLightsCount;
	float LightsNear;
	float LightsFar;
	float4 CameraData;
	float2 BufferDim;
	uint FrameCount;
};

StructuredBuffer<StructuredLight> lights : register(t17);
StructuredBuffer<uint> lightList : register(t18);       //MAX_CLUSTER_LIGHTS * 16^3
StructuredBuffer<LightGrid> lightGrid : register(t19);  //16^3

#if !defined(SCREEN_SPACE_SHADOWS)
Texture2D<float4> TexDepthSampler : register(t20);
#endif  // SCREEN_SPACE_SHADOWS

StructuredBuffer<PerPassLLF> perPassLLF : register(t32);

uint GetClusterIndex(float2 uv, float z)
{
	float clampedZ = clamp(z, perPassLLF[0].LightsNear, perPassLLF[0].LightsFar);
	uint clusterZ = uint(max((log2(clampedZ) - log2(perPassLLF[0].LightsNear)) * 16.0 / log2(perPassLLF[0].LightsFar / perPassLLF[0].LightsNear), 0.0));
	uint2 clusterDim = ceil(perPassLLF[0].BufferDim / float2(32, 16));
	uint3 cluster = uint3(uint2((uv * perPassLLF[0].BufferDim) / clusterDim), clusterZ);
	return cluster.x + (32 * cluster.y) + (32 * 16 * cluster.z);;
}

// Get a raw depth from the depth buffer. [0,1] in uv space
float GetDepth(float2 uv, uint a_eyeIndex = 0)
{
	uv = ConvertToStereoUV(uv, a_eyeIndex);
	return TexDepthSampler.Load(int3(uv * perPassLLF[0].BufferDim, 0));
}

bool IsSaturated(float value) { return value == saturate(value); }
bool IsSaturated(float2 value) { return IsSaturated(value.x) && IsSaturated(value.y); }

// Derived from the interleaved gradient function from Jimenez 2014 http://goo.gl/eomGso
float InterleavedGradientNoise(float2 uv)
{
	// Temporal factor
	float frameStep = float(perPassLLF[0].FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

float GetScreenDepth(float depth)
{
	return (perPassLLF[0].CameraData.w / (-depth * perPassLLF[0].CameraData.z + perPassLLF[0].CameraData.x));
}

float GetScreenDepth(float2 uv, uint a_eyeIndex = 0)
{
	float depth = GetDepth(uv, a_eyeIndex);
	return GetScreenDepth(depth);
}

float ContactShadows(float3 rayPos, float2 texcoord, float offset, float3 lightDirectionVS, float shadowQualityScale, float radius = 0.0, uint a_eyeIndex = 0)
{
	float2 depthDeltaMult;
	uint loopMax;
	if (radius > 0) {  // long
		lightDirectionVS *= radius / 16;
		depthDeltaMult = float2(1.00, 0.01);
		loopMax = round(16 * shadowQualityScale);
	} else {
		depthDeltaMult = float2(0.20, 0.10);
		loopMax = round(6.0 * shadowQualityScale);
	}

	// Offset starting position with interleaved gradient noise
	rayPos += lightDirectionVS * offset;

	// Accumulate samples
	float shadow = 0.0;
	[loop] for (uint i = 0; i < loopMax; i++)
	{
		// Step the ray
		rayPos += lightDirectionVS;
		float2 rayUV = ViewToUV(rayPos, true, a_eyeIndex);

		// Ensure the UV coordinates are inside the screen
		if (!IsSaturated(rayUV))
			break;

		// Compute the difference between the ray's and the camera's depth
		float rayDepth = GetScreenDepth(rayUV, a_eyeIndex);

		// Difference between the current ray distance and the marched light
		float depthDelta = rayPos.z - rayDepth;
		if (rayDepth > 16.5)  // First person
			shadow += saturate(depthDelta * depthDeltaMult.x) - saturate(depthDelta * depthDeltaMult.y);
	}

	return 1.0 - saturate(shadow);
}

#if defined(SKINNED) || defined(ENVMAP) || defined(EYE) || defined(MULTI_LAYER_PARALLAX)
#	define DRAW_IN_WORLDSPACE
#endif

// Copyright 2019 Google LLC.
// SPDX-License-Identifier: Apache-2.0

// Polynomial approximation in GLSL for the Turbo colormap
// Original LUT: https://gist.github.com/mikhailov-work/ee72ba4191942acecc03fe6da94fc73f

// Authors:
//   Colormap Design: Anton Mikhailov (mikhailov@google.com)
//   GLSL Approximation: Ruofei Du (ruofei@google.com)

// See also: https://ai.googleblog.com/2019/08/turbo-improved-rainbow-colormap-for.html

float3 TurboColormap(float x)
{
    const float4 kRedVec4   = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    const float4 kGreenVec4 = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    const float4 kBlueVec4  = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    const float2 kRedVec2   = float2(-152.94239396, 59.28637943);
    const float2 kGreenVec2 = float2(4.27729857, 2.82956604);
    const float2 kBlueVec2  = float2(-89.90310912, 27.34824973);

    x = saturate(x);
    float4 v4 = float4(1.0, x, x * x, x * x * x);
    float2 v2 = v4.zw * v4.z;
    return float3(
        dot(v4, kRedVec4)   + dot(v2, kRedVec2),
        dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
        dot(v4, kBlueVec4)  + dot(v2, kBlueVec2)
    );
}