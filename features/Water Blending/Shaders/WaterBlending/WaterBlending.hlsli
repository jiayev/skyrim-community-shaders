

struct PerPassWaterBlending
{
	float WaterHeight;
	bool EnableWaterBlending;
	bool EnableWaterBlendingSSR;
	float WaterBlendRange;
	float SSRBlendRange;
	float pad[3];
};

#if defined(WATER)
Texture2D<float4> SharedDepthTex : register(t31);
#endif

StructuredBuffer<PerPassWaterBlending> perPassWaterBlending : register(t32);