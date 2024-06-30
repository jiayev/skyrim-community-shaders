Texture2D<unorm half> ScreenSpaceShadowsTexture : register(t17);

float GetScreenSpaceShadow(float2 a_uv, float a_noise, float3 a_viewPosition, uint a_eyeIndex)
{
	a_noise *= 2.0 * M_PI;

	half2x2 rotationMatrix = half2x2(cos(a_noise), sin(a_noise), -sin(a_noise), cos(a_noise));

	float weight = 0;
	float shadow = 0;

	static const float2 BlurOffsets[8] = {
		float2(-0.4706069, -0.4427112),
		float2(-0.9057375, +0.3003471),
		float2(-0.3487388, +0.4037880),
		float2(+0.1023042, +0.6439373),
		float2(+0.5699277, +0.3513750),
		float2(+0.2939128, -0.1131226),
		float2(+0.7836658, -0.4208784),
		float2(+0.1564120, -0.8198990)
	};

	for (uint i = 0; i < 8; i++) {
		float2 offset = mul(BlurOffsets[i], rotationMatrix) * 0.001;

		float2 sampleUV = a_uv + offset;
		sampleUV = ConvertToStereoUV(sampleUV, a_eyeIndex);
		sampleUV = GetDynamicResolutionAdjustedScreenPosition(sampleUV);
		uint2 sampleCoord = sampleUV * BufferDim.xy;

		float rawDepth = TexDepthSampler.Load(int3(sampleCoord, 0)).x;
		float linearDepth = GetScreenDepth(rawDepth);

		float attenuation = 1.0 - saturate(128.0 * abs(linearDepth - a_viewPosition.z) / a_viewPosition.z);
		if (attenuation > 0.0) {
			shadow += ScreenSpaceShadowsTexture.Load(int3(sampleCoord, 0)).x * attenuation;
			weight += attenuation;
		}
	}

	if (weight > 0.0)
		shadow /= weight;
	else
		shadow = ScreenSpaceShadowsTexture.Load(int3(a_uv * BufferDim.xy, 0)).x;

	return shadow;
}