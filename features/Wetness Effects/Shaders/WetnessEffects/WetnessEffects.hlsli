#include "WetnessEffects/optimized-ggx.hlsl"

struct PerPassWetnessEffects
{
	float Wetness;
	row_major float3x4 DirectionalAmbientWS;
	uint EnableWetnessEffects;
	float MaxRainWetness;
	float MaxShoreWetness;
	uint ShoreRange;
	float PuddleRadius;
	float PuddleMaxAngle;
	float PuddleMinWetness;
};

StructuredBuffer<PerPassWetnessEffects> perPassWetnessEffects : register(t22);

#define LinearSampler SampShadowMaskSampler

// https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile
float2 EnvBRDFApprox(float3 F0, float Roughness, float NoV)
{
	const float4 c0 = { -1, -0.0275, -0.572, 0.022 };
	const float4 c1 = { 1, 0.0425, 1.04, -0.04 };
	float4 r = Roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
	return AB;
}

float3 GetWetnessAmbientSpecular(float3 N, float3 V, float roughness, float3 F0)
{
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

	float3 specularIrradiance = sRGB2Lin(mul(perPassWetnessEffects[0].DirectionalAmbientWS, float4(R, 1.0)));

#if defined(DYNAMIC_CUBEMAPS)
#	if !defined(GRASS)
	if (!lightingData[0].Reflections)
#	endif
	{
		float level = roughness * 9.0;
#	if defined(GRASS)
		level++;
#	endif
		specularIrradiance = specularTexture.SampleLevel(SampColorSampler, R, level).rgb;
	}
#endif

	// Split-sum approximation factors for Cook-Torrance specular BRDF.
#if defined(DYNAMIC_CUBEMAPS)
	float2 specularBRDF = specularBRDF_LUT.Sample(LinearSampler, float2(NoV, roughness));
#else
	float2 specularBRDF = EnvBRDFApprox(F0, roughness, NoV);
#endif

	// Roughness dependent fresnel
	// https://www.jcgt.org/published/0008/01/03/paper.pdf
	float3 Fr = max(1.0.xxx - roughness.xxx, F0) - F0;
	float3 S = F0 + Fr * pow(1.0 - NoV, 5.0);

	return specularIrradiance * (S * specularBRDF.x + specularBRDF.y);
}

float3 GetWetnessSpecular(float3 N, float3 L, float3 V, float3 lightColor, float roughness)
{
	return LightingFuncGGX_OPT3(N, V, L, roughness, 0.02) * pow(lightColor, 2.2);
}

float3 GetWetnessSpecular(float3 N, float3 L, float3 V, float3 lightColor, float roughness, float3 f0)
{
	return LightingFuncGGX_OPT3(N, V, L, roughness, f0) * pow(lightColor, 2.2);
}

// https://github.com/BelmuTM/Noble/blob/master/LICENSE.txt

const float fbmLacunarity = 2.0;
const float fbmPersistance = 0.5;

float hash11(float p)
{
	return frac(sin(p) * 1e4);
}

float noise(float3 pos)
{
	const float3 step = float3(110.0, 241.0, 171.0);
	float3 i = floor(pos);
	float3 f = frac(pos);
	float n = dot(i, step);

	float3 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(hash11(n + dot(step, float3(0.0, 0.0, 0.0))), hash11(n + dot(step, float3(1.0, 0.0, 0.0))), u.x),
					lerp(hash11(n + dot(step, float3(0.0, 1.0, 0.0))), hash11(n + dot(step, float3(1.0, 1.0, 0.0))), u.x), u.y),
		lerp(lerp(hash11(n + dot(step, float3(0.0, 0.0, 1.0))), hash11(n + dot(step, float3(1.0, 0.0, 1.0))), u.x),
			lerp(hash11(n + dot(step, float3(0.0, 1.0, 1.0))), hash11(n + dot(step, float3(1.0, 1.0, 1.0))), u.x), u.y),
		u.z);
}

float FBM(float3 pos, int octaves, float frequency)
{
	float height = 0.0;
	float amplitude = 1.0;

	for (int i = 0; i < octaves; i++) {
		height += noise(pos * frequency) * amplitude;
		frequency *= fbmLacunarity;
		amplitude *= fbmPersistance;
	}
	return height;
}