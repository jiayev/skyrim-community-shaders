namespace TerrainShadows
{
	Texture2D<float2> ShadowHeightTexture : register(t60);

	float2 GetTerrainShadowUV(float2 xy)
	{
		return xy * SharedData::terraOccSettings.Scale.xy + SharedData::terraOccSettings.Offset.xy;
	}

	float GetTerrainZ(float norm_z)
	{
		return lerp(SharedData::terraOccSettings.ZRange.x, SharedData::terraOccSettings.ZRange.y, norm_z) - 256;
	}

	float2 GetTerrainZ(float2 norm_z)
	{
		return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
	}

	float GetTerrainShadow(const float3 worldPos, SamplerState samp)
	{
		if (!SharedData::terraOccSettings.EnableTerrainShadow)
			return 1.0;
		float2 uv = GetTerrainShadowUV(worldPos.xy);
		if (any(uv < 0.0) || any(uv > 1.0))
			return 1.0;
		float2 shadowHeight = GetTerrainZ(ShadowHeightTexture.SampleLevel(samp, uv, 0));
		float penumbraHeight = shadowHeight.x - shadowHeight.y;
		if (penumbraHeight <= 0.0)
			return worldPos.z >= shadowHeight.x ? 1.0 : 0.0;
		return saturate((worldPos.z - shadowHeight.y) / penumbraHeight);
	}
}
