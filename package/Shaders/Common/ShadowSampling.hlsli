Texture2DArray<float4> SharedTexShadowMapSampler : register(t25);

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

StructuredBuffer<PerGeometry> SharedPerShadow : register(t26);

float GetShadowDepth(float3 positionWS, uint eyeIndex)
{
	float4 positionCSShifted = mul(CameraViewProj[eyeIndex], float4(positionWS, 1));
	return positionCSShifted.z / positionCSShifted.w;
}

float Get3DFilteredShadow(float3 positionWS, float3 viewDirection, float2 screenPosition, uint eyeIndex)
{
	PerGeometry sD = SharedPerShadow[0];

	float fadeFactor = 1.0 - pow(saturate(dot(positionWS, positionWS) / sD.ShadowLightParam.z), 8);
	uint sampleCount = ceil(16.0 * (1.0 - saturate(length(positionWS) / sqrt(sD.ShadowLightParam.z))));

	if (sampleCount == 0)
		return 1.0;

	float rcpSampleCount = rcp(sampleCount);

	uint3 seed = Random::pcg3d(uint3(screenPosition.xy, screenPosition.x * M_PI));

	float2 compareValue;
	compareValue.x = mul(transpose(sD.ShadowMapProj[eyeIndex][0]), float4(positionWS, 1)).z;
	compareValue.y = mul(transpose(sD.ShadowMapProj[eyeIndex][1]), float4(positionWS, 1)).z;

	float shadow = 0.0;
	if (sD.EndSplitDistances.z >= GetShadowDepth(positionWS, eyeIndex)) {
		for (uint i = 0; i < sampleCount; i++) {
			float3 rnd = Random::R3Modified(i + FrameCount * sampleCount, seed / 4294967295.f);

			// https://stats.stackexchange.com/questions/8021/how-to-generate-uniformly-distributed-points-in-the-3-d-unit-ball
			float phi = rnd.x * 2 * 3.1415926535;
			float cos_theta = rnd.y * 2 - 1;
			float sin_theta = sqrt(1 - cos_theta);
			float r = rnd.z;
			float4 sincos_phi;
			sincos(phi, sincos_phi.y, sincos_phi.x);
			float3 sampleOffset = viewDirection * i * 256 * rcpSampleCount;
			sampleOffset += float3(r * sin_theta * sincos_phi.x, r * sin_theta * sincos_phi.y, r * cos_theta) * 16;

			uint cascadeIndex = sD.EndSplitDistances.x < GetShadowDepth(positionWS.xyz + viewDirection * (sampleOffset.x + sampleOffset.y), eyeIndex);  // Stochastic cascade sampling
			float3 positionLS = mul(transpose(sD.ShadowMapProj[eyeIndex][cascadeIndex]), float4(positionWS + sampleOffset, 1));

			float4 depths = SharedTexShadowMapSampler.GatherRed(LinearSampler, float3(saturate(positionLS.xy), cascadeIndex), 0);
			shadow += dot(depths > compareValue[cascadeIndex], 0.25);
		}
	} else {
		shadow = 1.0;
	}

	return lerp(1.0, shadow * rcpSampleCount, fadeFactor);
}

float Get2DFilteredShadowCascade(float noise, float2x2 rotationMatrix, float sampleOffsetScale, float2 baseUV, float cascadeIndex, float compareValue, uint eyeIndex)
{
	const uint sampleCount = 8;
	compareValue += 0.001 * (1 + cascadeIndex);

	float layerIndexRcp = rcp(1 + cascadeIndex);

	float visibility = 0;

#if defined(WATER)
	sampleOffsetScale *= 8;
#endif

	for (uint sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
		float2 sampleOffset = mul(Random::SpiralSampleOffsets8[sampleIndex], rotationMatrix);

		float2 sampleUV = layerIndexRcp * sampleOffset * sampleOffsetScale + baseUV;

		float4 depths = SharedTexShadowMapSampler.GatherRed(LinearSampler, float3(saturate(sampleUV), cascadeIndex), 0);
		visibility += dot(depths > (compareValue + noise * 0.001), 0.25);
	}

	return visibility * rcp(sampleCount);
}

float Get2DFilteredShadow(float noise, float2x2 rotationMatrix, float3 positionWS, uint eyeIndex)
{
	PerGeometry sD = SharedPerShadow[0];

	float shadowMapDepth = GetShadowDepth(positionWS, eyeIndex);

	if (sD.EndSplitDistances.z >= shadowMapDepth) {
		float fadeFactor = 1 - pow(saturate(dot(positionWS.xyz, positionWS.xyz) / sD.ShadowLightParam.z), 8);

		float4x3 lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][0];
		float cascadeIndex = 0;

		if (sD.EndSplitDistances.x < shadowMapDepth) {
			lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][1];
			cascadeIndex = 1;
		}

		float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionWS.xyz, 1)).xyz;

		float shadowVisibility = Get2DFilteredShadowCascade(noise, rotationMatrix, sD.ShadowSampleParam.z, positionLS.xy, cascadeIndex, positionLS.z, eyeIndex);

		if (cascadeIndex < 1 && sD.StartSplitDistances.y < shadowMapDepth) {
			float3 cascade1PositionLS = mul(transpose(sD.ShadowMapProj[eyeIndex][1]), float4(positionWS.xyz, 1)).xyz;

			float cascade1ShadowVisibility = Get2DFilteredShadowCascade(noise, rotationMatrix, sD.ShadowSampleParam.z, cascade1PositionLS.xy, 1, cascade1PositionLS.z, eyeIndex);

			float cascade1BlendFactor = smoothstep(0, 1, (shadowMapDepth - sD.StartSplitDistances.y) / (sD.EndSplitDistances.x - sD.StartSplitDistances.y));
			shadowVisibility = lerp(shadowVisibility, cascade1ShadowVisibility, cascade1BlendFactor);
		}

		return lerp(1.0, shadowVisibility, fadeFactor);
	}

	return 1.0;
}

float GetWorldShadow(float3 positionWS, float depth, float3 offset, uint eyeIndex)
{
	float worldShadow = 1.0;
#if defined(TERRAIN_SHADOWS)
	float terrainShadow = TerrainShadows::GetTerrainShadow(positionWS + offset + CameraPosAdjust[eyeIndex].xyz, LinearSampler);
	worldShadow = terrainShadow;
	if (worldShadow == 0.0)
		return 0.0;
#endif

#if defined(CLOUD_SHADOWS)
	worldShadow *= CloudShadows::GetCloudShadowMult(positionWS + offset, LinearSampler);
	if (worldShadow == 0.0)
		return 0.0;
#endif

	return worldShadow;
}

float GetVL(float3 startPosWS, float3 endPosWS, float3 normal, float noise, inout float shadow, uint eyeIndex)
{
	float startDepth = length(startPosWS);

	// Simulate blurring world shadows on the surface
	normal *= 8.0;

	float worldShadow = GetWorldShadow(startPosWS, startDepth, normal, eyeIndex);

	shadow = worldShadow;

	float phase = dot(normalize(startPosWS.xyz), DirLightDirectionShared.xyz) * 0.5 + 0.5;

	worldShadow *= phase;

	PerGeometry sD = SharedPerShadow[0];

	float fadeFactor = 1.0 - saturate(length(endPosWS) / 4096.0);
	uint sampleCount = ceil(4.0 * fadeFactor);

	if (sampleCount == 0)
		return worldShadow;

	float2 rotation;
	sincos(M_2PI * noise, rotation.y, rotation.x);
	float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);

	float stepSize = rcp(sampleCount);

	float3 worldDir = endPosWS - startPosWS;

	// Offset starting position
	startPosWS += worldDir * stepSize * noise;

	sD.EndSplitDistances.x = SharedData::GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = SharedData::GetScreenDepth(sD.EndSplitDistances.y);
	sD.StartSplitDistances.y = SharedData::GetScreenDepth(sD.StartSplitDistances.y);

	float vlShadow = 0;

	for (uint i = 0; i < sampleCount; i++) {
		float3 samplePositionWS = startPosWS + worldDir * saturate(i * stepSize);
		float2 sampleOffset = mul(Random::SpiralSampleOffsets8[(float(i * 2) + noise * 8) % 8].xy, rotationMatrix);

		float cascadeIndex = 0;
		float4x3 lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][0];
		float shadowRange = sD.EndSplitDistances.x;

		if (sD.EndSplitDistances.x < length(samplePositionWS) + 8.0 * dot(sampleOffset, float2(1, 1)))  // Stochastic cascade sampling
		{
			lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][1];
			cascadeIndex = 1;
			shadowRange = sD.EndSplitDistances.y - sD.StartSplitDistances.y;
		}

		float3 samplePositionLS = mul(transpose(lightProjectionMatrix), float4(samplePositionWS.xyz, 1)).xyz;

		samplePositionLS.xy += 8.0 * sampleOffset * rcp(shadowRange);

		float4 depths = SharedTexShadowMapSampler.GatherRed(LinearSampler, float3(saturate(samplePositionLS.xy), cascadeIndex), 0);

		vlShadow += dot(depths > (samplePositionLS.z - 0.0005), 0.25);
	}
	return lerp(worldShadow, min(worldShadow, vlShadow * stepSize), fadeFactor);
}

float GetEffectShadow(float3 worldPosition, float3 viewDirection, float2 screenPosition, uint eyeIndex)
{
	float worldShadow = GetWorldShadow(worldPosition, length(worldPosition), 0.0, eyeIndex);
	if (worldShadow != 0.0) {
		float shadow = Get3DFilteredShadow(worldPosition, viewDirection, screenPosition, eyeIndex);
		return min(worldShadow, shadow);
	}

	return worldShadow;
}

float GetLightingShadow(float noise, float3 worldPosition, uint eyeIndex)
{
	float2 rotation;
	sincos(M_2PI * noise, rotation.y, rotation.x);
	float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
	return Get2DFilteredShadow(noise, rotationMatrix, worldPosition, eyeIndex);
}

float GetWaterShadow(float noise, float3 worldPosition, uint eyeIndex)
{
	float worldShadow = GetWorldShadow(worldPosition, length(worldPosition), 0.0, eyeIndex);
	if (worldShadow != 0.0) {
		float2 rotation;
		sincos(M_2PI * noise, rotation.y, rotation.x);
		float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);
		float shadow = Get2DFilteredShadow(noise, rotationMatrix, worldPosition, eyeIndex);
		return shadow;
	}

	return worldShadow;
}