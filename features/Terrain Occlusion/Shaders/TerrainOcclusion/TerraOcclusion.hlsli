struct PerPassTerraOcc
{
	uint EnableTerrainShadow;
	uint EnableTerrainAO;

	float HeightBias;

	float ShadowSofteningRadiusAngle;
	float2 ShadowFadeDistance;

	float AOMix;
	float AOPower;
	float AOFadeOutHeightRcp;

	float3 scale;
	float3 invScale;
	float3 offset;
	float2 zRange;
};

#ifdef TERRA_OCC_OUTPUT
StructuredBuffer<PerPassTerraOcc> perPassTerraOcc : register(t1);
Texture2D<float> TexTerraOcc : register(t2);
Texture2D<float> TexNormalisedHeight : register(t3);
Texture2D<float2> TexShadowHeight : register(t4);
#else
StructuredBuffer<PerPassTerraOcc> perPassTerraOcc : register(t1);
Texture2D<float> TexTerraOcc : register(t2);
Texture2D<float> TexNormalisedHeight : register(t3);
Texture2D<float2> TexShadowHeight : register(t4);
#endif

float2 GetTerrainOcclusionUV(float2 xy)
{
	return xy * perPassTerraOcc[0].scale.xy + perPassTerraOcc[0].offset.xy;
}

float2 GetTerrainOcclusionXY(float2 uv)
{
	return (uv - perPassTerraOcc[0].offset.xy) * perPassTerraOcc[0].invScale.xy;
}

float GetTerrainZ(float norm_z)
{
	return lerp(perPassTerraOcc[0].zRange.x, perPassTerraOcc[0].zRange.y, norm_z) + perPassTerraOcc[0].HeightBias;
}

float2 GetTerrainZ(float2 norm_z)
{
	return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
}

void GetTerrainOcclusion(
	const float3 worldPos, const float viewDistance, SamplerState samp,
	out float terrainShadow, out float terrainAo)
{
	terrainShadow = 1;
	terrainAo = 1;

	float2 terraOccUV = GetTerrainOcclusionUV(worldPos.xy);

	if (any(terraOccUV < 0) && any(terraOccUV > 1))
		return;

	if (perPassTerraOcc[0].EnableTerrainShadow && (viewDistance > perPassTerraOcc[0].ShadowFadeDistance.x)) {
		float fadeFactor = saturate((viewDistance - perPassTerraOcc[0].ShadowFadeDistance.x) / (perPassTerraOcc[0].ShadowFadeDistance.y - perPassTerraOcc[0].ShadowFadeDistance.x));
		float2 shadowHeight = GetTerrainZ(TexShadowHeight.SampleLevel(samp, terraOccUV, 0));
		float shadowFraction = saturate((worldPos.z - shadowHeight.y) / (shadowHeight.x - shadowHeight.y));
		terrainShadow = lerp(1, shadowFraction, fadeFactor);
	}
	if (perPassTerraOcc[0].EnableTerrainAO) {
		float terrainHeight = GetTerrainZ(TexNormalisedHeight.SampleLevel(samp, terraOccUV, 0).x);
		terrainAo = TexTerraOcc.SampleLevel(samp, terraOccUV, 0).x;

		// power
		terrainAo = pow(terrainAo, perPassTerraOcc[0].AOPower);

		// height fadeout
		float fadeOut = saturate((worldPos.z - terrainHeight) * perPassTerraOcc[0].AOFadeOutHeightRcp);
		terrainAo = lerp(terrainAo, 1, fadeOut);

		terrainAo = lerp(1, terrainAo, perPassTerraOcc[0].AOMix);
	}
}