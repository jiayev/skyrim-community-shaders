struct PerPassWetnessEffects
{
	float Wetness;
	row_major float3x4 DirectionalAmbientWS;
	uint EnableWetnessEffects;
	float MaxRainWetness;
	float MaxShoreWetness;
	float MaxDarkness;
	float MaxOcclusion;
	float MinRoughness;
	uint ShoreRange;
	float PuddleMinWetness;
	float PuddleRadius;
	float PuddleMaxAngle;
	float PuddleFlatness;
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

float3 GetPBRAmbientSpecular(float3 N, float3 V, float roughness, float3 F0)
{
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

	float3 specularIrradiance = mul(perPassWetnessEffects[0].DirectionalAmbientWS, float4(R, 1.0)) * 0.75;

#if defined(DYNAMIC_CUBEMAPS)
#	if !defined(GRASS)
	if (!lightingData[0].Reflections)
#	endif
	{
		float level = roughness * 4.0;
#	if defined(GRASS)
		level++;
#	endif
		specularIrradiance = specularTexture.SampleLevel(SampColorSampler, R, level).rgb;
	}
#endif

	specularIrradiance = max(0.01, pow(specularIrradiance, 2.2));

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

float3 FresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1 - F0) * pow(saturate(1 - cosTheta), 5);
}

float DistributionGGX(float NdotH, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH2 = NdotH * NdotH;

	float num = a2;
	float denom = (NdotH2 * (a2 - 1) + 1);
	denom = PI * denom * denom;

	return num / denom;
}

float GeometrySchlickGGX(float cosTheta, float roughness)
{
	float r = (roughness + 1);
	float k = (r * r) / 8;

	float num = cosTheta;
	float denom = cosTheta * (1 - k) + k;

	return num / denom;
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
	float ggxV = GeometrySchlickGGX(NdotV, roughness);
	float ggxL = GeometrySchlickGGX(NdotL, roughness);

	return ggxV * ggxL;
}

float3 GetWetnessSpecular(float3 N, float3 L, float3 V, float3 lightColor, float roughness)
{
	float3 H = normalize(V + L);
	float NdotL = saturate(dot(N, L));
	float NdotV = saturate(dot(N, V));
	float HdotV = saturate(dot(H, V));
	float NdotH = saturate(dot(N, H));

	float NDF = DistributionGGX(NdotH, roughness);
	float G = GeometrySmith(NdotV, NdotL, roughness);
	float3 F = FresnelSchlick(HdotV, 0.02);

	float3 numerator = NDF * G * F;
	float denominator = 4 * NdotV * NdotL + 0.0001;
	float3 specular = numerator / denominator;

	return specular * NdotL * pow(lightColor, 2.2);
}

// Separable SSS Reflectance Pixel Shader
float3 sRGB2Lin(float3 Color)
{
#if defined(ACES)
	return mul(g_sRGBToACEScg, Color);
#else
	return Color > 0.04045 ? pow(Color / 1.055 + 0.055 / 1.055, 2.4) : Color / 12.92;
#endif
}

float3 Lin2sRGB(float3 Color)
{
#if defined(ACES)
	return mul(g_ACEScgToSRGB, Color);
#else
	return Color > 0.0031308 ? 1.055 * pow(Color, 1.0 / 2.4) - 0.055 : 12.92 * Color;
#endif
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

float quintic(float edge0, float edge1, float x)
{
	x = saturate((x - edge0) / (edge1 - edge0));
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}