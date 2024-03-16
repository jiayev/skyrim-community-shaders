TextureCube<float4> specularTexture : register(t64);

// https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile
half2 EnvBRDFApprox(half Roughness, half NoV)
{
	const half4 c0 = { -1, -0.0275, -0.572, 0.022 };
	const half4 c1 = { 1, 0.0425, 1.04, -0.04 };
	half4 r = Roughness * c0 + c1;
	half a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	half2 AB = half2(-1.04, 1.04) * a004 + r.zw;
	return AB;
}

#if !defined(WATER)

#	if defined(CREATOR)
struct CreatorSettingsCB
{
	uint Enabled;
	uint pad0[3];
	float4 CubemapColor;
};

StructuredBuffer<CreatorSettingsCB> perFrameCreator : register(t65);
#	endif

float3 GetDynamicCubemap(float2 uv, float3 N, float3 VN, float3 V, float roughness, float3 F0, float3 diffuseColor, float distance)
{
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

	float level = roughness * 9.0;

	float3 specularIrradiance = specularTexture.SampleLevel(SampColorSampler, R, level);
	specularIrradiance = sRGB2Lin(specularIrradiance);
	diffuseColor = sRGB2Lin(diffuseColor);

	// Local lighting contribution
	specularIrradiance += specularIrradiance * diffuseColor;

	// Fade into only local lighting
	specularIrradiance = lerp(specularIrradiance, diffuseColor, smoothstep(1000, 2000, distance));

	// Darken under hemisphere
	specularIrradiance *= lerp(1.0, saturate(R.z), 0.5);

	float2 specularBRDF = EnvBRDFApprox(roughness, NoV);

	// Horizon specular occlusion
	// https://marmosetco.tumblr.com/post/81245981087
	float horizon = min(1.0 + dot(R, VN), 1.0);
	specularIrradiance *= horizon * horizon;

	// Roughness dependent fresnel
	// https://www.jcgt.org/published/0008/01/03/paper.pdf
	float3 Fr = max(1.0.xxx - roughness.xxx, F0) - F0;
	float3 S = Fr * pow(1.0 - NoV, 5.0);

	return specularIrradiance * ((F0 + S) * specularBRDF.x + specularBRDF.y);
}

float3 GetDynamicCubemapFresnel(float2 uv, float3 N, float3 VN, float3 V, float roughness, float level, float3 diffuseColor, float distance)
{
	float NoV = saturate(dot(N, V));
	float2 specularBRDF = EnvBRDFApprox(roughness, NoV);
	if (specularBRDF.y > 0.001) {
		float3 R = reflect(-V, N);

		float3 specularIrradiance = specularTexture.SampleLevel(SampColorSampler, R, level);
		specularIrradiance = sRGB2Lin(specularIrradiance);
		diffuseColor = sRGB2Lin(diffuseColor);

		// Local lighting contribution
		specularIrradiance += specularIrradiance * diffuseColor;

		// Fade into only local lighting
		specularIrradiance = lerp(specularIrradiance, diffuseColor, smoothstep(1000, 2000, distance));

		// Darken under hemisphere
		specularIrradiance *= lerp(1.0, saturate(R.z), 0.5);

		// Horizon specular occlusion
		// https://marmosetco.tumblr.com/post/81245981087
		float horizon = min(1.0 + dot(R, VN), 1.0);
		specularIrradiance *= horizon * horizon;

		return specularIrradiance * specularBRDF.y;
	}
	return 0.0;
}
#endif
