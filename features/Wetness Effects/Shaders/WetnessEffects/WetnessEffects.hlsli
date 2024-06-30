#include "WetnessEffects/optimized-ggx.hlsl"

// https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile
float2 EnvBRDFApproxWater(float3 F0, float Roughness, float NoV)
{
	const float4 c0 = { -1, -0.0275, -0.572, 0.022 };
	const float4 c1 = { 1, 0.0425, 1.04, -0.04 };
	float4 r = Roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
	return AB;
}

// https://github.com/BelmuTM/Noble/blob/master/LICENSE.txt

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

// https://www.pcg-random.org/
uint pcg(uint v)
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint3 pcg3d(uint3 v)
{
	v = v * 1664525u + 1013904223u;

	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;

	v ^= v >> 16u;

	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;

	return v;
}

uint iqint3(uint2 x)
{
	uint2 q = 1103515245U * ((x >> 1U) ^ (x.yx));
	uint n = 1103515245U * ((q.x) ^ (q.y >> 3U));

	return n;
}

float SmoothstepDeriv(float x)
{
	return 6.0 * x * (1. - x);
}

float RainFade(float normalised_t)
{
	const float rain_stay = .5;

	if (normalised_t < rain_stay)
		return 1.0;

	float val = lerp(1.0, 0.0, (normalised_t - rain_stay) / (1.0 - rain_stay));
	return val * val;
}

// https://blog.selfshadow.com/publications/blending-in-detail/
// geometric normal s, a base normal t and a secondary (or detail) normal u
float3 ReorientNormal(float3 u, float3 t, float3 s)
{
	// Build the shortest-arc quaternion
	float4 q = float4(cross(s, t), dot(s, t) + 1) / sqrt(2 * (dot(s, t) + 1));

	// Rotate the normal
	return u * (q.w * q.w - dot(q.xyz, q.xyz)) + 2 * q.xyz * dot(q.xyz, u) + 2 * q.w * cross(q.xyz, u);
}

// for when s = (0,0,1)
float3 ReorientNormal(float3 n1, float3 n2)
{
	n1 += float3(0, 0, 1);
	n2 *= float3(-1, -1, 1);

	return n1 * dot(n1, n2) / n1.z - n2;
}

// xyz - ripple normal, w - splotches
float4 GetRainDrops(float3 worldPos, float t, float3 normal)
{
	const static float uintToFloat = rcp(4294967295.0);
	const float rippleBreadthRcp = rcp(wetnessEffects.RippleBreadth);

	float2 gridUV = worldPos.xy * wetnessEffects.RaindropGridSizeRcp;
	gridUV += normal.xy * 0.5;
	int2 grid = floor(gridUV);
	gridUV -= grid;

	float3 rippleNormal = float3(0, 0, 1);
	float wetness = 0;

	if (wetnessEffects.EnableSplashes || wetnessEffects.EnableRipples)
		for (int i = -1; i <= 1; i++)
			for (int j = -1; j <= 1; j++) {
				int2 gridCurr = grid + int2(i, j);
				float tOffset = float(iqint3(gridCurr)) * uintToFloat;

				// splashes
				if (wetnessEffects.EnableSplashes) {
					float residual = t * wetnessEffects.RaindropIntervalRcp / wetnessEffects.SplashesLifetime + tOffset + worldPos.z * 0.001;
					uint timestep = residual;
					residual = residual - timestep;

					uint3 hash = pcg3d(uint3(gridCurr, timestep));
					float3 floatHash = float3(hash) * uintToFloat;

					if (floatHash.z < (wetnessEffects.RaindropChance)) {
						float2 vec2Centre = int2(i, j) + floatHash.xy - gridUV;
						float distSqr = dot(vec2Centre, vec2Centre);
						float drop_radius = lerp(wetnessEffects.SplashesMinRadius, wetnessEffects.SplashesMaxRadius,
							float(iqint3(hash.yz)) * uintToFloat);
						if (distSqr < drop_radius * drop_radius)
							wetness = max(wetness, RainFade(residual));
					}
				}

				// ripples
				if (wetnessEffects.EnableRipples) {
					float residual = t * wetnessEffects.RaindropIntervalRcp + tOffset + worldPos.z * 0.001;
					uint timestep = residual;
					residual = residual - timestep;

					uint3 hash = pcg3d(uint3(gridCurr, timestep));
					float3 floatHash = float3(hash) * uintToFloat;

					if (floatHash.z < (wetnessEffects.RaindropChance)) {
						float2 vec2Centre = int2(i, j) + floatHash.xy - gridUV;
						float distSqr = dot(vec2Centre, vec2Centre);
						float rippleT = residual * wetnessEffects.RippleLifetimeRcp;
						if (rippleT < 1.) {
							float ripple_r = lerp(0., wetnessEffects.RippleRadius, rippleT);
							float ripple_inner_radius = ripple_r - wetnessEffects.RippleBreadth;

							float band_lerp = (sqrt(distSqr) - ripple_inner_radius) * rippleBreadthRcp;
							if (band_lerp > 0. && band_lerp < 1.) {
								float deriv = (band_lerp < .5 ? SmoothstepDeriv(band_lerp * 2.) : -SmoothstepDeriv(2. - band_lerp * 2.)) *
								              lerp(wetnessEffects.RippleStrength, 0, rippleT * rippleT);

								float3 grad = float3(normalize(vec2Centre), -deriv);
								float3 bitangent = float3(-grad.y, grad.x, 0);
								float3 normal = normalize(cross(grad, bitangent));

								rippleNormal = ReorientNormal(normal, rippleNormal);
							}
						}
					}
				}
			}

	if (wetnessEffects.EnableChaoticRipples) {
		float3 turbulenceNormal = noise(float3(worldPos.xy * wetnessEffects.ChaoticRippleScaleRcp, t * wetnessEffects.ChaoticRippleSpeed));
		turbulenceNormal.z = turbulenceNormal.z * .5 + 5;
		turbulenceNormal = normalize(turbulenceNormal);
		rippleNormal = normalize(rippleNormal + float3(turbulenceNormal.xy * wetnessEffects.ChaoticRippleStrength, 0));
	}

	wetness *= wetnessEffects.SplashesStrength;

	return float4(rippleNormal, wetness);
}

float3 GetWetnessAmbientSpecular(float2 uv, float3 N, float3 VN, float3 V, float roughness)
{
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

#if defined(DYNAMIC_CUBEMAPS)
#	if defined(DEFERRED)
	float level = roughness * 9.0;
	float3 specularIrradiance = 1.0;
#	else
	float level = roughness * 9.0;
	float3 specularIrradiance = sRGB2Lin(specularTexture.SampleLevel(SampColorSampler, R, level));
#	endif
#else
	float3 specularIrradiance = 1.0;
#endif

	float2 specularBRDF = EnvBRDFApproxWater(0.02, roughness, NoV);

	// Horizon specular occlusion
	// https://marmosetco.tumblr.com/post/81245981087
	float horizon = min(1.0 + dot(R, VN), 1.0);
	specularIrradiance *= horizon * horizon;

	// Roughness dependent fresnel
	// https://www.jcgt.org/published/0008/01/03/paper.pdf
	float3 Fr = max(1.0.xxx - roughness.xxx, 0.02) - 0.02;
	float3 S = 0.02 + Fr * pow(1.0 - NoV, 5.0);

	return specularIrradiance * (S * specularBRDF.x + specularBRDF.y);
}

float3 GetWetnessSpecular(float3 N, float3 L, float3 V, float3 lightColor, float roughness)
{
	lightColor *= 0.1;
	return LightingFuncGGX_OPT3(N, V, L, roughness, 1.0 - roughness) * lightColor;
}
