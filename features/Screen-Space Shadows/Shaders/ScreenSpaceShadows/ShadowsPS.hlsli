cbuffer SSSData : register(b5)
{
	bool EnableSSS;
	uint FrameCount;
};

Texture2D<float> TexOcclusionSampler : register(t21);

#define LinearSampler SampShadowMaskSampler

float2 SSGetDynamicResolutionAdjustedScreenPosition(float2 uv)
{
	return uv * DynamicResolutionParams1.xy;
}

float PrepassScreenSpaceShadows(float3 positionWS, uint eyeIndex = 0)
{
#if defined(EYE)
	return 1;
#else
	if (EnableSSS && !FrameParams.z) {
		float2 texCoord = ViewToUV(WorldToView(positionWS, true, eyeIndex), true, eyeIndex);
		texCoord = ConvertToStereoUV(texCoord, eyeIndex);
		texCoord = SSGetDynamicResolutionAdjustedScreenPosition(texCoord);
		texCoord /= 2;
		float shadow = TexOcclusionSampler.SampleLevel(LinearSampler, texCoord, 0);
		return shadow;
	}
	return 1;
#endif
}