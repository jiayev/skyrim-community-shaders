TextureCube<float4> specularTexture : register(t64);
Texture2D<float4> specularBRDF_LUT : register(t65);

float3 GetDynamicCubemap(float3 N, float3 V, float roughness, float3 F0)
{
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

	float level = roughness * 10.0;

	float3 specularIrradiance = specularTexture.SampleLevel(SampColorSampler, R, level).rgb;
	specularIrradiance = sRGB2Lin(specularIrradiance);

	// Split-sum approximation factors for Cook-Torrance specular BRDF.
	float2 specularBRDF = specularBRDF_LUT.SampleLevel(SampColorSampler, float2(NoV, roughness), 0);

	F0 = sRGB2Lin(F0);

	// Roughness dependent fresnel
	// https://www.jcgt.org/published/0008/01/03/paper.pdf
	float3 Fr = max(1.0.xxx - roughness.xxx, F0) - F0;
	float3 S = F0 + Fr * pow(1.0 - NoV, 5.0);

	return specularIrradiance * (S * specularBRDF.x + specularBRDF.y);
}