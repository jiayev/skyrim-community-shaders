#ifndef CLOUD_RELIGHT_HLSLI
#define CLOUD_RELIGHT_HLSLI

#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

namespace CloudRelight
{
	static const float RCP_PI = 1.0 / Math::PI;

	// ----- Helper functions -----

	float Remap(float x, float in_a, float in_b, float out_a, float out_b)
	{
		if (in_a == in_b)
			return out_a;
		return lerp(out_a, out_b, saturate((x - in_a) / (in_b - in_a)));
	}

	// ----- Phase functions -----

	namespace Phase
	{
		// Numerical fit of the full Mie+diffraction cloud phase function.
		// Source: https://www.shadertoy.com/view/tl33Rn
		float smoothstep_unchecked(float x) { return (x * x) * (3.0 - x * 2.0); }
		float smoothbump(float a, float r, float x) { return 1.0 - smoothstep_unchecked(min(abs(x - a), r) / r); }
		float powerful_scurve(float x, float p1, float p2) { return pow(1.0 - pow(1.0 - saturate(x), p2), p1); }

		float3 CloudFit(float cos_theta)
		{
			float x = acos(cos_theta);
			float x2 = max(0., x - 2.45) / (Math::PI - 2.15);
			float x3 = max(0., x - 2.95) / (Math::PI - 2.95);
			float y = (exp(-max(x * 1.5 + 0.0, 0.0) * 30.0) + smoothstep(1.7, 0., x) * 0.45 * 0.8 + smoothbump(0.4, 0.5, cos_theta) * 0.02 - smoothstep(1., 0.2, x) * 0.06 + smoothbump(2.18, 0.20, x) * 0.06 + smoothstep(2.28, 2.45, x) * 0.18 - powerful_scurve(x2 * 4.0, 3.5, 8.) * 0.04 + x2 * -0.085 + x3 * x3 * 0.1);

			float3 ret = y;
			// Spectralize slightly for rainbow fringes / glories
			ret = lerp(ret, ret + 0.008 * 2., smoothstep(0.94, 1., cos_theta) * sin(x * 10. * float3(8, 4, 2)));
			ret = lerp(ret, ret - 0.008 * 2., smoothbump(-0.7, 0.14, cos_theta) * sin(x * 20. * float3(8, 4, 2)));
			ret = lerp(ret, ret - 0.008 * 5., smoothstep(-0.994, -1., cos_theta) * sin(x * 30. * float3(3, 4, 2)));

			// Scale so sphere-integral == 1
			ret += 0.13 * 1.4;
			return ret * 3.9 * 0.25 * RCP_PI;
		}

		// Numerical fit for the Silver-Lining / Bravais-arc phase function.
		// Source: https://www.shadertoy.com/view/4sjBDG
		float ThomasSchander(float costh)
		{
			float bestParams[10];
			bestParams[0] = 9.805233e-06;
			bestParams[1] = -6.500000e+01;
			bestParams[2] = -5.500000e+01;
			bestParams[3] = 8.194068e-01;
			bestParams[4] = 1.388198e-01;
			bestParams[5] = -8.370334e+01;
			bestParams[6] = 7.810083e+00;
			bestParams[7] = 2.054747e-03;
			bestParams[8] = 2.600563e-02;
			bestParams[9] = -4.552125e-12;

			float p1 = costh + bestParams[3];
			float4 expValues = exp(float4(bestParams[1] * costh + bestParams[2], bestParams[5] * p1 * p1, bestParams[6] * costh, bestParams[9] * costh));
			float4 expValWeight = float4(bestParams[0], bestParams[4], bestParams[7], bestParams[8]);
			return dot(expValues, expValWeight) * 0.25;
		}
	}

	// ----- Main relighting function -----

	// Relight a vanilla cloud texel using the directional light.
	//
	// baseColor  - raw cloud rgba sample (alpha = cloud density / opacity mask)
	// viewDir    - normalised view ray direction
	// cloudPosWS - estimated world-space position on the cloud layer
	// sampCube   - sampler used to read the CloudShadows cubemap
	//
	// Requires CLOUD_SHADOWS to be defined; if it is not defined the function
	// is compiled out entirely so callers must guard with the same define.
#if defined(CLOUD_SHADOWS)
	float3 RelightCloud(float4 baseColor, float3 viewDir, float3 cloudPosWS, SamplerState sampCube)
	{
		if (baseColor.w <= 0)
			return baseColor.rgb;

		SharedData::CloudRelightSettings data = SharedData::cloudRelightSettings;

		float3 dirLightDir = SharedData::DirLightDirection.xyz;
		float3 dirLightColor = SharedData::DirLightColor.rgb;

		float u = dot(viewDir, dirLightDir);

		// Build a per-texel phase value that drives the silver-lining effect.
		// The cloud density (baseColor.w) is remapped so thin clouds show the
		// silver-lining and thick clouds fall back to the flat isotropic term.
		float phaseCloud =
			Remap(
				baseColor.w,
				data.silverLiningSpread > 0 ? data.silverLiningSpread : 0,
				data.silverLiningSpread < 0 ? 1 + data.silverLiningSpread : 1,
				lerp(0.25 * RCP_PI, Phase::ThomasSchander(u), data.silverLiningMix),
				0.25 * RCP_PI) *
			2 * Math::PI * data.cloudRelightMix;

		float3 cloudColor = baseColor.rgb * data.cloudOriginalMix;

		// Accumulate directional-light contribution with 4 Poisson-disc
		// cloud-shadow samples along the sun-view great circle.
		{
			float rayStep = 1.0 / 32.0;
			float rayPos = rayStep * 0.5;
			float4 rayShadow = 0.0;

			float3 PoissonDisc[] = {
				float3(0.460921f, 0.615192f, 0.887539f),
				float3(0.757347f, 0.911008f, 0.189581f),
				float3(0.548753f, 0.145482f, 0.0548723f),
				float3(0.90051f, 0.157048f, 0.623493f)
			};

			[unroll] for (int i = 0; i < 4; i++)
			{
				float3 raySample = normalize(lerp(viewDir, SharedData::DirLightDirection.xyz, rayPos));
				raySample += (PoissonDisc[i] * 2.0 - 1.0) * 0.01;

				if (raySample.z < 0.0)
					rayShadow[i % 4] += -raySample.z;  // horizon/ground shadow
				else
					rayShadow[i % 4] = max(rayShadow[i % 4], CloudShadows::CloudShadowsTexture.SampleLevel(sampCube, raySample, 0).x);

				rayPos += rayStep;
			}

			cloudColor += baseColor.a * baseColor.xyz * phaseCloud * (1.0 - saturate(dot(rayShadow, 0.25))) * dirLightColor;
		}

		return cloudColor;
	}
#endif  // CLOUD_SHADOWS

}  // namespace CloudRelight
#endif  // CLOUD_RELIGHT_HLSLI
