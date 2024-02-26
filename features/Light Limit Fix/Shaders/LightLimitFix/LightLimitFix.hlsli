struct LightGrid
{
	uint offset;
	uint lightCount;
	float pad0[2];
};

struct StructuredLight
{
	float3 color;
	float radius;
	float4 positionWS[2];
	float4 positionVS[2];
	uint firstPersonShadow;
	float pad0[3];
};

struct PerPassLLF
{
	bool EnableGlobalLights;
	bool EnableContactShadows;
	bool EnableLightsVisualisation;
	uint LightsVisualisationMode;
	float LightsNear;
	float LightsFar;
	uint FrameCount;
};

StructuredBuffer<StructuredLight> lights : register(t17);
StructuredBuffer<uint> lightList : register(t18);       //MAX_CLUSTER_LIGHTS * 16^3
StructuredBuffer<LightGrid> lightGrid : register(t19);  //16^3

StructuredBuffer<PerPassLLF> perPassLLF : register(t32);

struct StrictLightData
{
	StructuredLight StrictLights[15];
	uint StrictLightCount;
};

StructuredBuffer<StrictLightData> strictLightData : register(t37);

bool GetClusterIndex(in float2 uv, in float z, out uint clusterIndex)
{
	if (z < perPassLLF[0].LightsNear || z > perPassLLF[0].LightsFar)
		return false;

	float clampedZ = clamp(z, perPassLLF[0].LightsNear, perPassLLF[0].LightsFar);
	uint clusterZ = uint(max((log2(z) - log2(perPassLLF[0].LightsNear)) * 16.0 / log2(perPassLLF[0].LightsFar / perPassLLF[0].LightsNear), 0.0));
	uint2 clusterDim = ceil(lightingData[0].BufferDim / float2(16, 16));
	uint3 cluster = uint3(uint2((uv * lightingData[0].BufferDim) / clusterDim), clusterZ);

	clusterIndex = cluster.x + (16 * cluster.y) + (16 * 16 * cluster.z);
	return true;
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
		loopMax = round(11.0 * shadowQualityScale);
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
	const float4 kRedVec4 = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
	const float4 kGreenVec4 = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
	const float4 kBlueVec4 = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
	const float2 kRedVec2 = float2(-152.94239396, 59.28637943);
	const float2 kGreenVec2 = float2(4.27729857, 2.82956604);
	const float2 kBlueVec2 = float2(-89.90310912, 27.34824973);

	x = saturate(x);
	float4 v4 = float4(1.0, x, x * x, x * x * x);
	float2 v2 = v4.zw * v4.z;
	return float3(
		dot(v4, kRedVec4) + dot(v2, kRedVec2),
		dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
		dot(v4, kBlueVec4) + dot(v2, kBlueVec2));
}