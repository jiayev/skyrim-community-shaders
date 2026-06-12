#ifndef COMMON_PHYS_SKY_HLSLI
#define COMMON_PHYS_SKY_HLSLI

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

#ifndef OMIT_PS_NAMESPACE
namespace PhysSky
{
#endif

#ifdef PS_PREPASS_SAMPLERS
	SamplerState SampTr : register(s0);  // in lighting, use shadow
	SamplerState SampSv : register(s1);  // in lighting, use color
	SamplerState SampNoise : register(s2);
#endif

#ifdef PS_SKY_SAMPLERS
	SamplerState SampTr : register(s3);
	SamplerState SampSv : register(s4);
#endif

#ifdef PS_DEFERRED_SAMPLERS
	SamplerState SampSv : register(s1);
#endif

#ifdef PS_PREPASS_RSRCS
#	ifndef PS_NO_RSRCS
	Texture2D<float4> TexTrLut : register(t0);
	Texture2D<float4> TexMsLut : register(t1);
	Texture2D<float4> TexSvLut : register(t2);
	Texture3D<float4> TexApLut : register(t3);
#	endif
#elif defined(PS_DEFERRED_RSRCS)
Texture3D<float4> TexApLut : register(t16);
Texture2D<unorm float> TexApShadow : register(t17);
Texture2D<float4> TexMsLut : register(t20);
#else
Texture2D<float4> TexTrLut : register(t61);
Texture2D<float4> TexSvLut : register(t62);
Texture3D<float4> TexApLut : register(t63);
Texture2D<unorm float> TexApShadow : register(t64);
Texture2D<float4> TexMsLut : register(t113);
#endif

	static const float RCP_PI = 1 / Math::PI;         // PI
	static const float AP_MAX_DIST = 40 / 1.428e-5f;  // 40 km
	static const uint3 CLOUD_DIM = uint3(512, 512, 64);
	static const float3 CLOUD_RANGE = float3(4.f, 4.f, .5f) / 1.428e-5f;
	static const float3 CLOUD_RANGE_M = float3(4.f, 4.f, .5f) * 1e3;

#ifndef ISNAN
#	define ISNAN(x) (!(x < 0.f || x > 0.f || x == 0.f))
#endif

	float Remap(float x, float in_a, float in_b, float out_a, float out_b)
	{
		if (in_a == in_b)
			return out_a;
		return lerp(out_a, out_b, saturate((x - in_a) / (in_b - in_a)));
	}

	float3 PosWs2Planet(float3 posWorld)
	{
		const SharedData::PhysSkyData data = SharedData::physSkyData;
		return posWorld - float3(FrameBuffer::CameraPosAdjust.xy, data.zBottom - data.rPlanet);
	}

	// return distance to sphere surface
	// url: https://viclw17.github.io/2018/07/16/raytracing-ray-sphere-intersection
	float RayIntersectSphere(float3 orig, float3 dir, float3 center, float r)
	{
		float3 oc = orig - center;
		float b = dot(oc, dir);
		float c = dot(oc, oc) - r * r;
		float discr = b * b - c;
		if (discr < 0.0)
			return -1.0;
		// Special case: inside sphere, use far discriminant
		return (discr > b * b) ? (-b + sqrt(discr)) : (-b - sqrt(discr));
	}

	float3 SphericalDir(float azimuth, float zenith)
	{
		float cosZenith, sinZenith, cosAzimuth, sinAzimuth;
		sincos(zenith, sinZenith, cosZenith);
		sincos(azimuth, sinAzimuth, cosAzimuth);
		return float3(sinZenith * cosAzimuth, sinZenith * sinAzimuth, cosZenith);
	}

	float HorizonZenithCos(float r)
	{
		float sinZenith = SharedData::physSkyData.rPlanet / r;
		return -sqrt(1 - sinZenith * sinZenith);
	}

	float2 TrLutUv(float r, float cosSunZenith)
	{
		const SharedData::PhysSkyData data = SharedData::physSkyData;
		// float cosHorZenith = HorizonZenithCos(r);
		const float cosHorZenith = -0.414;
		float2 uv = float2(
			saturate((cosSunZenith - cosHorZenith) / (1 - cosHorZenith)),
			saturate((r - data.rPlanet) / (data.rAtmosphere - data.rPlanet)));
		uv = clamp(uv, float2(0.5 / 256.0, 0.5 / 64.0), float2(1.0 - 0.5 / 256.0, 1.0 - 0.5 / 64.0));
		return uv;
	}

	float2 TrLutUvPlanet(float3 pos, float3 sunDir)
	{
		float r = length(pos);
		float3 up = pos / r;
		return TrLutUv(r, dot(sunDir, up));
	}

	// cylinder map
	float2 SkyViewLutUv(float3 rayDir)
	{
		float azimuth = atan2(rayDir.y, rayDir.x);
		float u = azimuth * .5 * RCP_PI;  // sampler wraps around so ok
		float zenith = asin(rayDir.z);
		float v = 0.5 - 0.5 * sign(zenith) * sqrt(abs(zenith) * 2 * RCP_PI);
		v = max(v, 0.01);
		return frac(float2(u, v));
	}

	// url: http://www.physics.hmc.edu/faculty/esin/a101/limbdarkening.pdf
	float3 LimbDarkenHestroffer(float norm_dist)
	{
		float mu = sqrt(1.0 - norm_dist * norm_dist);

		// coefficient for RGB wavelength (680, 550, 440)
		float3 a0 = float3(0.34685, 0.26073, 0.15248);
		float3 a1 = float3(1.37539, 1.27428, 1.38517);
		float3 a2 = float3(-2.04425, -1.30352, -1.49615);
		float3 a3 = float3(2.70493, 1.47085, 1.99886);
		float3 a4 = float3(-1.94290, -0.96618, -1.48155);
		float3 a5 = float3(0.55999, 0.26384, 0.44119);

		float mu2 = mu * mu;
		float mu3 = mu2 * mu;
		float mu4 = mu2 * mu2;
		float mu5 = mu4 * mu;

		float3 factor = a0 + a1 * mu + a2 * mu2 + a3 * mu3 + a4 * mu4 + a5 * mu5;
		return factor;
	}

	float3 InvSkyViewLutUv(float2 uv)
	{
		float azimuth = uv.x * 2 * Math::PI;
		float vm = 1 - 2 * uv.y;
		float zenith = Math::PI * .5 * (1 - sign(vm) * vm * vm);
		return SphericalDir(azimuth, zenith);
	}

	void SampleAtmosphere(
		float altitude,
		out float rayleighDensity,
		out float aerosolDensity,
		out float ozoneDensity)
	{
		const SharedData::PhysSkyData data = SharedData::physSkyData;
		rayleighDensity = exp(-altitude * data.rayleighFalloff);
		aerosolDensity = exp(-altitude * data.aerosolFalloff);
		ozoneDensity = max(0.f, 1 - abs(altitude - data.ozoneAltitude) / (data.ozoneThickness * 0.5f));
	}

	namespace Phase
	{
		float HG(float cos_theta, float g)
		{
			static const float scale = .25 * RCP_PI;
			const float g2 = g * g;

			float num = (1.0 - g2);
			float denom = pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5);

			return scale * num / denom;
		}

		float HGDualLobe(float cos_theta, float g_0, float g_1, float w)
		{
			return lerp(HG(cos_theta, g_0), HG(cos_theta, g_1), w);
		}

		float CornetteShanks(float cos_theta, float g)
		{
			static const float scale = .375 * RCP_PI;
			const float g2 = g * g;

			float num = (1.0 - g2) * (1.0 + cos_theta * cos_theta);
			float denom = (2.0 + g2) * pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5);

			return scale * num / denom;
		}

		float Draine(float cos_theta, float g, float alpha)
		{
			static const float scale = .25 * RCP_PI;
			const float g2 = g * g;

			float num = (1.0 - g2) * (1.0 + alpha * cos_theta * cos_theta);
			float denom = pow(abs(1.0 + g2 - 2.0 * g * cos_theta), 1.5) * (1.0 + alpha * (1.0 + 2.0 * g2) * .333333333333333);

			return scale * num / denom;
		}

		// https://www.shadertoy.com/view/tl33Rn
		float smoothstep_unchecked(float x) { return (x * x) * (3.0 - x * 2.0); }
		float smoothbump(float a, float r, float x) { return 1.0 - smoothstep_unchecked(min(abs(x - a), r) / r); }
		float powerful_scurve(float x, float p1, float p2) { return pow(1.0 - pow(1.0 - saturate(x), p2), p1); }
		float3 CloudFit(float cos_theta)
		{
			float x = acos(cos_theta);
			float x2 = max(0., x - 2.45) / (Math::PI - 2.15);
			float x3 = max(0., x - 2.95) / (Math::PI - 2.95);
			float y = (exp(-max(x * 1.5 + 0.0, 0.0) * 30.0)         // front peak
					   + smoothstep(1.7, 0., x) * 0.45 * 0.8        // front ramp
					   + smoothbump(0.4, 0.5, cos_theta) * 0.02     // front bump middle
					   - smoothstep(1., 0.2, x) * 0.06              // front ramp damp wave
					   + smoothbump(2.18, 0.20, x) * 0.06           // first trail wave
					   + smoothstep(2.28, 2.45, x) * 0.18           // trailing piece
					   - powerful_scurve(x2 * 4.0, 3.5, 8.) * 0.04  // trail
					   + x2 * -0.085 + x3 * x3 * 0.1);              // trail peak

			float3 ret = y;
			// spectralize a bit
			ret = lerp(ret, ret + 0.008 * 2., smoothstep(0.94, 1., cos_theta) * sin(x * 10. * float3(8, 4, 2)));
			ret = lerp(ret, ret - 0.008 * 2., smoothbump(-0.7, 0.14, cos_theta) * sin(x * 20. * float3(8, 4, 2)));   // fogbow
			ret = lerp(ret, ret - 0.008 * 5., smoothstep(-0.994, -1., cos_theta) * sin(x * 30. * float3(3, 4, 2)));  // glory

			// scale and offset should be tweaked so integral on sphere is 1
			ret += 0.13 * 1.4;
			return ret * 3.9 * 0.25 * RCP_PI;  // Edit: additonal 1/4pi to be consistent with the rest
		}

		// numerical fit https://www.shadertoy.com/view/4sjBDG
		float ThomasSchander(float costh)
		{
			// This function was optimized to minimize (delta*delta)/reference in order to capture
			// the low intensity behavior.
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

		float Rayleigh(float cos_theta)
		{
			const float k = .1875 * RCP_PI;
			return k * (1.0 + cos_theta * cos_theta);
		}
	}

	float GetApShadowedMultiScatterVisibility(float shadow, float3 multiScatter)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;

		float shadowableLum = Color::RGBToLuminance(max(0.0, data.sunlightColor));
		float unshadowedLum = Color::RGBToLuminance(max(0.0, data.masserColor + data.secundaColor + multiScatter));
		return (unshadowedLum + shadowableLum * (1.0 - saturate(shadow))) / max(unshadowedLum + shadowableLum, 1e-6);
	}

#ifndef PS_PREPASS_RSRCS

	float GetApShadow(uint2 pxCoord)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;

		const uint2 apCoord = data.halfResApShadow ? pxCoord / 2 : pxCoord;
		return TexApShadow[apCoord];
	}

#	ifndef PS_DEFERRED_RSRCS
	float3 SampleSky(float3 viewDir, float shadow, SamplerState sampSv)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;

		const float2 skyLutUv = SkyViewLutUv(viewDir);
		float3 skyColor = TexSvLut.SampleLevel(sampSv, skyLutUv, 0).rgb;

		skyColor *= 1 - shadow;

		if (data.tonemapper == 1)
			skyColor = Color::LinearToSkyrimGamma(skyColor);
		else if (data.tonemapper == 2)
			skyColor = skyColor / (1 + skyColor);

		return skyColor;
	}

	float3 SampleSky(float3 viewDir, uint2 pxCoord, SamplerState sampSv)
	{
		return SampleSky(viewDir, GetApShadow(pxCoord), sampSv);
	}

	float3 SampleTr(float3 sunDir, SamplerState sampSv)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;

		if (data.trMix < 1e-8)
			return 1;

		const float2 lutUv = TrLutUv(data.zCameraPlanet, sunDir.z);
		float3 tr = TexTrLut.SampleLevel(sampSv, lutUv, 0).rgb;
		if (sunDir.z <= -0.414)
			tr = 0;
		tr = lerp(1, tr, data.trMix);

		return tr;
	}

#		if defined(CLOUD_SHADOWS) && defined(PS_SKY_SAMPLERS)
	float3 RelightCloud(float4 baseColor, float3 viewDir, float3 cloudPosWS, SamplerState sampTr, SamplerState sampCube)
	{
		if (baseColor.w <= 0)
			return baseColor.rgb;

		SharedData::PhysSkyData data = SharedData::physSkyData;

		// TODO: use proper light Dir
		float3 dirLightDir = SharedData::DirLightDirection.xyz;
		float3 dirLightColor = SharedData::DirLightColor;

		// TODO: planet shadowing

		float2 lutUv = TrLutUvPlanet(cloudPosWS + float3(0, 0, data.zCameraPlanet), dirLightDir);
		float3 trAtmos = TexTrLut.SampleLevel(sampTr, lutUv, 0).rgb;
		trAtmos = lerp(1, trAtmos, data.trMix);
		dirLightColor *= trAtmos;

		float u = dot(viewDir, dirLightDir);
		float phaseCloud = Remap(
							   baseColor.a,
							   data.silverLiningSpread > 0 ? data.silverLiningSpread : 0,
							   data.silverLiningSpread < 0 ? 1 + data.silverLiningSpread : 1,
							   lerp(0.25 * RCP_PI, Phase::ThomasSchander(u), data.silverLiningMix),
							   0.25 * RCP_PI) *
		                   2 * Math::PI * data.cloudRelightMix;

		float3 cloudColor = baseColor.rgb * data.cloudOriginalMix;

		if (baseColor.a > 0.0) {
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
					rayShadow[i % 4] += -raySample.z;  // World shadow
				else
					rayShadow[i % 4] = max(rayShadow[i % 4], CloudShadows::CloudSelfShadowTexture.SampleLevel(sampCube, raySample, 0).x);

				rayPos += rayStep;
			}

			float directVisibility = 1.0 - saturate(dot(rayShadow, 0.25));

			float cloudOpticalDepth = -log(max(1.0 - baseColor.a, 1e-3));
			float forwardG = lerp(0.82, 0.94, saturate(1.0 - abs(data.silverLiningSpread)));
			float forwardMieCore = max(0.0, Phase::HG(u, forwardG) - 0.25 * RCP_PI);
			float forwardMieAureole = max(0.0, Phase::Draine(u, 0.78, 2.0) - 0.25 * RCP_PI);
			float forwardMiePhase = forwardMieCore + 0.45 * forwardMieAureole;
			float silverEdgeMask = smoothstep(0.08, 0.35, baseColor.a) * (1.0 - smoothstep(0.45, 0.85, baseColor.a));
			float silverSingleScatter = 1.35 * silverEdgeMask * (1.0 - exp(-cloudOpticalDepth)) * exp(-0.5 * cloudOpticalDepth);
			float directSingleScatter = 0.9 * cloudOpticalDepth * exp(-0.75 * cloudOpticalDepth);
			cloudColor += baseColor.xyz * dirLightColor * forwardMiePhase * silverSingleScatter * directVisibility * data.silverLiningMix * data.cloudRelightMix;

#			if defined(IBL)
			if (SharedData::iblSettings.EnableIBL) {
				float3 cloudAmbientDir = normalize(-viewDir);
				float skyVisibility = saturate(cloudAmbientDir.z * 0.5 + 0.5) * exp(-cloudOpticalDepth);
				float3 vanillaAmbient = Color::Ambient(max(0.0, SharedData::GetAmbient(cloudAmbientDir)));

				float3 linIblAmbient = 0.0;
				if (SharedData::iblSettings.DALCMode >= 2)
					linIblAmbient += Color::IrradianceToLinear(vanillaAmbient * SharedData::iblSettings.DALCAmount);
				else
					linIblAmbient += ImageBasedLighting::GetEnvIBLColor(cloudAmbientDir);
				linIblAmbient += ImageBasedLighting::GetSkyIBLColorOccluded(cloudAmbientDir, skyVisibility);

				float3 iblAmbient = Color::IrradianceToGamma(linIblAmbient);
				float iblFill = baseColor.a * exp(-0.35 * cloudOpticalDepth) * lerp(0.25, 1.0, 1.0 - directVisibility);
				cloudColor += baseColor.xyz * iblAmbient * iblFill * data.cloudRelightMix;
			}
#			endif

			cloudColor += baseColor.xyz * phaseCloud * directVisibility * directSingleScatter * dirLightColor;
		}

		return cloudColor;
	}
#		endif
#	endif

	float3 SampleApMultiScatter(SamplerState samp)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;

		float3 multiScatter = TexMsLut.SampleLevel(samp, TrLutUv(data.zCameraPlanet, data.sunDir.z), 0).rgb * data.sunlightColor;
		multiScatter += TexMsLut.SampleLevel(samp, TrLutUv(data.zCameraPlanet, data.masserDir.z), 0).rgb * data.masserColor;
		multiScatter += TexMsLut.SampleLevel(samp, TrLutUv(data.zCameraPlanet, data.secundaDir.z), 0).rgb * data.secundaColor;
		return multiScatter;
	}

	float4 SampleAp(float3 viewDir, float dist, float shadow, SamplerState sampSv)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;

		const float2 skyLutUv = SkyViewLutUv(viewDir);

		uint3 apDims;
		TexApLut.GetDimensions(apDims.x, apDims.y, apDims.z);
		const float depth_slice = lerp(.5 / apDims.z, 1 - .5 / apDims.z, saturate(dist / AP_MAX_DIST));
		float4 apColor = TexApLut.SampleLevel(sampSv, float3(skyLutUv, depth_slice), 0);

		apColor.rgb *= GetApShadowedMultiScatterVisibility(shadow, SampleApMultiScatter(sampSv));

		if (data.tonemapper == 1)
			apColor.rgb = Color::LinearToSkyrimGamma(apColor.rgb);
		else if (data.tonemapper == 2)
			apColor.rgb = apColor.rgb / (1 + apColor.rgb);

		apColor.rgb = lerp(0, apColor.rgb, data.apLumMix);
		apColor.a = lerp(1, apColor.a, data.apTrMix);

		return apColor;
	}

	float4 SampleAp(float3 viewDir, uint2 pxCoord, float dist, SamplerState sampSv)
	{
		return SampleAp(viewDir, dist, GetApShadow(pxCoord), sampSv);
	}

#	ifndef PS_PREPASS_RSRCS
	// Volumetric cloud main-view result and shadow volume. Pixel shaders use t110-t112 to avoid feature texture conflicts.
#		if defined(PS_DEFERRED_RSRCS)
	Texture2D<float3> TexVolTr : register(t18);
	Texture2D<float3> TexVolLum : register(t19);
	Texture3D<float> TexShadowVolume : register(t20);
#		else
	Texture2D<float3> TexVolTr : register(t110);
	Texture2D<float3> TexVolLum : register(t111);
	Texture3D<float> TexShadowVolume : register(t112);
	TextureCube<float3> TexVolCubeTr : register(t114);
	TextureCube<float3> TexVolCubeLum : register(t115);
#		endif

	float3 CompositeVolumetricClouds(float3 color, uint2 pxCoord)
	{
		float3 volTr = TexVolTr[pxCoord];
		float3 volLum = TexVolLum[pxCoord];
		return Color::IrradianceToGamma(Color::IrradianceToLinear(color) * volTr + volLum);
	}

	float3 CompositeVolumetricCloudsUvDr(float3 color, float2 screenUvDr, SamplerState samp)
	{
		float3 volTr = TexVolTr.SampleLevel(samp, screenUvDr, 0);
		float3 volLum = TexVolLum.SampleLevel(samp, screenUvDr, 0);
		return Color::IrradianceToGamma(Color::IrradianceToLinear(color) * volTr + volLum);
	}

	float3 CompositeVolumetricCloudsUv(float3 color, float2 screenUv, SamplerState samp)
	{
		return CompositeVolumetricCloudsUvDr(color, FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(screenUv), samp);
	}

	float3 ApplyVolumetricCloudTransmittanceUv(float3 color, float2 screenUv, SamplerState samp)
	{
		const float3 volTr = TexVolTr.SampleLevel(samp, FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(screenUv), 0);
		return Color::IrradianceToGamma(Color::IrradianceToLinear(color) * volTr);
	}

#		ifndef PS_DEFERRED_RSRCS
	float3 CompositeVolumetricCloudsCube(float3 color, float3 viewDir, SamplerState samp)
	{
		float3 volTr = TexVolCubeTr.SampleLevel(samp, viewDir, 0);
		float3 volLum = TexVolCubeLum.SampleLevel(samp, viewDir, 0);
		return Color::IrradianceToGamma(Color::IrradianceToLinear(color) * volTr + volLum);
	}
#		endif

	float3 CompositeAerialPerspective(float3 color, float3 viewDir, uint2 pxCoord, float dist, SamplerState sampSv)
	{
		if (SharedData::physSkyData.enableVolumetricClouds)
			return CompositeVolumetricClouds(color, pxCoord);

		const float4 apSample = SampleAp(viewDir, pxCoord, dist, sampSv);
		return color * apSample.w + apSample.xyz;
	}

	float3 CompositeAerialPerspective(float3 color, float3 viewDir, float2 screenPos, float2 screenUv, float dist, SamplerState sampSv)
	{
		if (SharedData::physSkyData.enableVolumetricClouds)
			return CompositeVolumetricCloudsUv(color, screenUv, sampSv);

		const float4 apSample = SampleAp(viewDir, uint2(screenPos), dist, sampSv);
		return color * apSample.w + apSample.xyz;
	}

#		ifndef PS_DEFERRED_RSRCS
	float3 CompositeAerialPerspectiveReflection(float3 color, float3 viewDir, float dist, SamplerState sampSv)
	{
		if (SharedData::physSkyData.enableVolumetricClouds)
			return CompositeVolumetricCloudsCube(color, viewDir, sampSv);

		const float4 apSample = SampleAp(viewDir, dist, 0.0, sampSv);
		return color * apSample.w + apSample.xyz;
	}
#		endif

#	endif

#endif

#if !defined(PS_DEFERRED_RSRCS) && (!defined(PS_PREPASS_RSRCS) || defined(PS_ENABLE_DIRLIGHT_TRANSMITTANCE))
	float3 GetShadowVolumeUvw(float3 posRelative, float3 sunDir)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;
		float3 boundsMin = float3(FrameBuffer::CameraPosAdjust.xy - 0.5 * data.shadowVolumeRange, data.volCloudBottom);
		float3 boundsMax = float3(FrameBuffer::CameraPosAdjust.xy + 0.5 * data.shadowVolumeRange, data.volCloudBottom + data.volCloudThickness);

		float3 samplePos = posRelative;
		if (any(posRelative < boundsMin) || any(posRelative > boundsMax)) {
			float3 sunSign = float3(sunDir.x < 0 ? -1 : 1, sunDir.y < 0 ? -1 : 1, sunDir.z < 0 ? -1 : 1);
			float3 safeSunDir = sunSign * max(abs(sunDir), 1e-6);
			float3 tMin = (boundsMin - posRelative) / safeSunDir;
			float3 tMax = (boundsMax - posRelative) / safeSunDir;
			float3 t1 = min(tMin, tMax);
			float3 t2 = max(tMin, tMax);
			float tNear = max(max(t1.x, t1.y), t1.z);
			float tFar = min(min(t2.x, t2.y), t2.z);
			if (tNear > tFar || tFar < 0)
				return -1;
			samplePos += (max(tNear, 0) + 128) * sunDir;
		}

		float3 uvw = samplePos - float3(FrameBuffer::CameraPosAdjust.xy, data.volCloudBottom);
		uvw /= float3(data.shadowVolumeRange.xx, data.volCloudThickness);
		uvw.xy += 0.5;
		return uvw;
	}

	float3 GetDirlightTransmittance(float3 worldPosAbs, SamplerState samp)
	{
		SharedData::PhysSkyData data = SharedData::physSkyData;
		float3 transmittance = 1.0;

		if (!data.enableVolumetricClouds || data.volCloudThickness <= 0)
			return transmittance;

		float3 posRelative = worldPosAbs;
		posRelative.z -= data.zBottom;

		float3 uvw = GetShadowVolumeUvw(posRelative, data.sunDir);
		float cloudDensity = 0;
		if (all(uvw > 0) && all(uvw < 1)) {
			cloudDensity = TexShadowVolume.SampleLevel(samp, uvw, 0);
		} else {
			float3 posPlanet = posRelative + float3(-FrameBuffer::CameraPosAdjust.xy, data.rPlanet);
			float rInner = data.rPlanet + data.volCloudBottom;
			float rOuter = rInner + data.volCloudThickness;
			float innerDist = max(RayIntersectSphere(posPlanet, data.sunDir, 0, rInner), 0);
			float outerDist = max(RayIntersectSphere(posPlanet, data.sunDir, 0, rOuter), 0);
			cloudDensity = abs(outerDist - innerDist) * data.volCloudAverageDensity;
		}

		return exp(-(data.volCloudScatter + data.volCloudAbsorption) * cloudDensity);
	}
#endif

#ifndef OMIT_PS_NAMESPACE
}
#endif
#endif
