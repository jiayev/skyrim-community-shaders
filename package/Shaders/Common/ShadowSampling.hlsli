#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

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

Texture2DArray<float4> TexShadowMapSampler : register(t25);
StructuredBuffer<PerGeometry> perShadow : register(t26);

#if defined(WATER) || defined(EFFECT)

cbuffer PerWaterType : register(b7)
{
	float3 ShallowColorNoWeather;
	uint pad0water;
	float3 DeepColorNoWeather;
	uint pad1water;
};

float3 GetShadow(float3 positionWS, uint a_eyeIndex = 0)
{
	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

	float shadowMapDepth = length(positionWS.xyz);

	half cascadeIndex = 0;
	half4x3 lightProjectionMatrix = sD.ShadowMapProj[a_eyeIndex][0];
	half shadowMapThreshold = sD.AlphaTestRef.y;

	[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
	{
		lightProjectionMatrix = sD.ShadowMapProj[a_eyeIndex][2];
		shadowMapThreshold = sD.AlphaTestRef.z;
		cascadeIndex = 2;
	}
	else if (sD.EndSplitDistances.x < shadowMapDepth)
	{
		lightProjectionMatrix = sD.ShadowMapProj[a_eyeIndex][1];
		shadowMapThreshold = sD.AlphaTestRef.z;
		cascadeIndex = 1;
	}

	half3 positionLS = mul(transpose(lightProjectionMatrix), half4(positionWS.xyz, 1)).xyz;
	float deltaZ = positionLS.z - shadowMapThreshold;

	float4 depths = TexShadowMapSampler.GatherRed(LinearSampler, half3(positionLS.xy, cascadeIndex), 0);

	float shadow = dot(depths > deltaZ, 0.25);

#	if defined(CLOUD_SHADOWS)
	if (shadow > 0.0) {
		shadow *= GetCloudShadowMult(positionWS, LinearSampler);
	}
#	endif

	return shadow;
}

float GetVL(float3 startPosWS, float3 endPosWS, float2 screenPosition, uint a_eyeIndex = 0)
{
	const static uint nSteps = 16;
	const static float step = 1.0 / float(nSteps);

	float3 worldDir = endPosWS - startPosWS;

	float noise = InterleavedGradientNoise(screenPosition);

	startPosWS += worldDir * step * noise;

	noise = noise * 2.0 * M_PI;
	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

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

			float shadowMapDepth = length(samplePositionWS.xyz);

			half cascadeIndex = 0;
			half4x3 lightProjectionMatrix = sD.ShadowMapProj[a_eyeIndex][0];
			half shadowMapThreshold = sD.AlphaTestRef.y;

			half shadowRange = sD.EndSplitDistances.x;

			[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[a_eyeIndex][2];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 2;
				shadowRange = sD.EndSplitDistances.z - sD.EndSplitDistances.y;
			}
			else if (sD.EndSplitDistances.x < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[a_eyeIndex][1];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 1;
				shadowRange = sD.EndSplitDistances.y - sD.EndSplitDistances.x;
			}

			half3 samplePositionLS = mul(transpose(lightProjectionMatrix), half4(samplePositionWS.xyz, 1)).xyz;

			half2 offset = mul(PoissonDisk[(float(i) + noise) % 16].xy, rotationMatrix);
			samplePositionLS.xy += 8.0 * offset / shadowRange;

			float deltaZ = samplePositionLS.z - shadowMapThreshold;

			shadow = TexShadowMapSampler.SampleLevel(LinearSampler, half3(samplePositionLS.xy, cascadeIndex), 0) > deltaZ;

#	if defined(TERRA_OCC)
			if (shadow > 0.0) {
				float terrainShadow = 1;
				float terrainAo = 1;
				GetTerrainOcclusion(samplePositionWS + CameraPosAdjust[a_eyeIndex], length(samplePositionWS), LinearSampler, terrainShadow, terrainAo);
				shadow *= terrainShadow;
			}
#	endif

			shadow *= (dot(normalize(samplePositionWS.xyz), DirLightDirectionShared.xyz) * 0.5 + 0.5);
		}
		vl += shadow;
	}
	return vl * step;
}
#endif
