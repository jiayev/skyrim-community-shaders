

struct PerPassWaterBlending
{
	bool EnableWaterBlending;
	bool EnableWaterBlendingSSR;
	float WaterBlendRange;
	float SSRBlendRange;
};

#if defined(WATER)
Texture2D<float4> SharedDepthTex : register(t33);
#endif

StructuredBuffer<PerPassWaterBlending> perPassWaterBlending : register(t34);