TextureCube<float4> specularTexture : register(t64);
Texture2D<float4> specularBRDF_LUT : register(t65);

#define LinearSampler SampShadowMaskSampler
// Separable SSS Reflectance Pixel Shader
float3 sRGB2Lin2(float3 Color)
{
#if defined(ACES)
	return mul(g_sRGBToACEScg, Color);
#else
	return Color > 0.04045 ? pow(Color / 1.055 + 0.055 / 1.055, 2.4) : Color / 12.92;
#endif
}

float3 Lin2sRGB2(float3 Color)
{
#if defined(ACES)
	return mul(g_ACEScgToSRGB, Color);
#else
	return Color > 0.0031308 ? 1.055 * pow(Color, 1.0 / 2.4) - 0.055 : 12.92 * Color;
#endif
}

float3 GetDynamicCubemap(float3 N, float3 V, float roughness, float3 F0)
{
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

	float level = roughness * 10.0;

	float3 specularIrradiance = specularTexture.SampleLevel(SampColorSampler, R, level).rgb;

	// Split-sum approximation factors for Cook-Torrance specular BRDF.
	float2 specularBRDF = specularBRDF_LUT.SampleLevel(LinearSampler, float2(NoV, roughness), 0);

	// Roughness dependent fresnel
	// https://www.jcgt.org/published/0008/01/03/paper.pdf
	float3 Fr = max(1.0.xxx - roughness.xxx, F0) - F0;
	float3 S = F0 + Fr * pow(1.0 - NoV, 5.0);

	return specularIrradiance * (S * specularBRDF.x + specularBRDF.y);
}