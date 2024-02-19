
Texture2D<float4> TerrainBlendingMaskTexture : register(t35);

float2 ShiftTerrain(float blendFactor, float2 coords, float3 viewDir, float3x3 tbn)
{
	float3 viewDirTS = mul(viewDir, tbn).xyz;
	return viewDirTS.xy * ((1.0 - blendFactor) * 0.02) + coords.xy;
}