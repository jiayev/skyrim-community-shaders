#ifndef XeGTAO_BENTNORMALS_HLSLI
#define XeGTAO_BENTNORMALS_HLSLI

#include "Common/FastMath.hlsli"
#include "Common/Math.hlsli"

namespace BentNormals
{
	float FastAcosPositive(float x)
	{
		float p = -0.1565827f * x + 1.570796f;
		return p * sqrt(1.0 - x);
	}

	float sphericalCapsIntersection(float cosCap1, float cosCap2, float cosDistance)
	{
		// Oat and Sander 2007, "Ambient Aperture Lighting"
		// Approximation mentioned by Jimenez et al. 2016
		float r1 = FastAcosPositive(cosCap1);
		float r2 = FastAcosPositive(cosCap2);
		float d = FastMath::ACos(cosDistance);

		// We work with cosine angles, replace the original paper's use of
		// cos(min(r1, r2)) with max(cosCap1, cosCap2)
		// We also remove a multiplication by 2 * PI to simplify the computation
		// since we divide by 2 * PI in computeBentSpecularAO()

		if (min(r1, r2) <= max(r1, r2) - d) {
			return 1.0 - max(cosCap1, cosCap2);
		} else if (r1 + r2 <= d) {
			return 0.0;
		}

		float delta = abs(r1 - r2);
		float x = 1.0 - saturate((d - delta) / max(r1 + r2 - delta, 1e-4));
		// simplified smoothstep()
		float area = x * x * (-2.0 * x + 3.0);
		return area * (1.0 - max(cosCap1, cosCap2));
	}

	float SpecularAO_Cones(float3 BN, float3 N, float3 V, float visibility, float roughness)
	{
		// Jimenez et al. 2016, "Practical Realtime Strategies for Accurate Indirect Occlusion"
		const float3 R = reflect(-V, N);
		roughness = max(roughness, 0.01);

		// aperture from ambient occlusion
		float cosAv = sqrt(1.0 - visibility);
		// aperture from roughness, log(10) / log(2) = 3.321928
		float cosAs = exp2(-3.321928 * roughness * roughness);
		// angle betwen bent normal and reflection direction
		float cosB = dot(BN, R);

		// Remove the 2 * PI term from the denominator, it cancels out the same term from
		// sphericalCapsIntersection()
		float ao = sphericalCapsIntersection(cosAv, cosAs, cosB) / (1.0 - cosAs);
		// Smoothly kill specular AO when entering the perceptual roughness range [0.1..0.3]
		// Without this, specular AO can remove all reflections, which looks bad on metals
		return lerp(1.0, ao, smoothstep(0.01, 0.09, roughness));
	}

	// a contact shadow approximation, totally not physically correct; a riff on "Chan 2018, "Material Advances in Call of Duty: WWII" and "The Technical Art of Uncharted 4" http://advances.realtimerendering.com/other/2016/naughty_dog/NaughtyDog_TechArt_Final.pdf (microshadowing)"
	float ApproximateDirectVisibility(float aoVisibility, float3 N, float3 L)
	{
		// Could use bent normal instead of normal
		float NdotL = saturate(dot(N, L));
		float aperture = rsqrt(1.0000001 - aoVisibility);
		NdotL += 0.1;  // when using bent normals, avoids overshadowing - bent normals are just approximation anyhow
		return saturate(NdotL * aperture);
	}
}
#endif