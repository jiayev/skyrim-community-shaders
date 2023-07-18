cbuffer SSSData : register(b5)
{
	bool EnableSSS;
	uint FrameCount;
};

Texture2D<float> TexDepthSampler : register(t20);
Texture2D<float> TexOcclusionSampler : register(t21);

#if defined(DEFSHADOW)
#	define LinearSampler SampShadowMaskSampler
#else
SamplerState LinearSampler : register(s14);
#endif

float2 SSGetDynamicResolutionAdjustedScreenPosition(float2 uv)
{
	return uv * DynamicResolutionParams1.xy;
}

float PrepassScreenSpaceShadows(float3 positionWS, uint eyeIndex = 0)
{
#if defined(EYE)
	return 1;
#else
	if (EnableSSS) {
		float2 texCoord = ViewToUV(WorldToView(positionWS, true, eyeIndex), true, eyeIndex);
		float2 coords = SSGetDynamicResolutionAdjustedScreenPosition(texCoord) / 2;
		float shadow = TexOcclusionSampler.SampleLevel(LinearSampler, coords, 0);
		return shadow;
	}
	return 1;
#endif
}