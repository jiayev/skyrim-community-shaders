#ifndef __IBL_HLSLI__
#define __IBL_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

namespace ImageBasedLighting
{
	const static sh2 IBL_TO_WORLD_SH = float4(1.0, -1.0, -1.0, -1.0);

#if defined(IBL_DEFERRED)
	Texture2D<sh2> EnvIBLTexture : register(t11);
	Texture2D<sh2> SkyIBLTexture : register(t12);
#else
	Texture2D<sh2> EnvIBLTexture : register(t76);
	Texture2D<sh2> SkyIBLTexture : register(t77);
	TextureCube<float4> StaticDiffuseIBLTexture : register(t78);
	TextureCube<float4> StaticSpecularIBLTexture : register(t79);
#endif

	float GetRadianceWeightedVisibility(sh2 radianceSH, sh2 visibilitySH, sh2 referenceVisibilitySH, sh2 responseLobe)
	{
		// Project L(omega) * V(omega) before applying the diffuse/specular
		// response. This retains every radiance/visibility correlation representable
		// by the current L0+L1 basis instead of multiplying by geometric AO later.
		float referenceResponse = max(0.0, SphericalHarmonics::FuncProductIntegral(
											   SphericalHarmonics::Product(radianceSH, referenceVisibilitySH), responseLobe));
		float visibleResponse = max(0.0, SphericalHarmonics::FuncProductIntegral(
											 SphericalHarmonics::Product(radianceSH, visibilitySH), responseLobe));
		return referenceResponse > EPSILON_WEIGHT_SUM ? saturate(visibleResponse / referenceResponse) : 1.0;
	}

	float GetSourceRadianceWeightedVisibility(
		sh2 radianceSHR, sh2 radianceSHG, sh2 radianceSHB,
		sh2 visibilitySH, sh2 referenceVisibilitySH, sh2 responseLobe,
		float fadeOutFactor, float minVisibility)
	{
		// DiffuseIBLCS projects cubemap(-dir) into SH, while Skylighting stores
		// world-space directions. Reflect the L1 band so both functions share the
		// world-space visibility domain before taking their product.
		radianceSHR *= IBL_TO_WORLD_SH;
		radianceSHG *= IBL_TO_WORLD_SH;
		radianceSHB *= IBL_TO_WORLD_SH;

		// A luminance-weighted scalar preserves the source's dominant radiance /
		// visibility correlation at one third of the RGB SH-product cost.
		sh2 luminanceSH = radianceSHR * 0.2126 + radianceSHG * 0.7152 + radianceSHB * 0.0722;
		float visibility = GetRadianceWeightedVisibility(luminanceSH, visibilitySH, referenceVisibilitySH, responseLobe);
		visibility = lerp(1.0, visibility, fadeOutFactor);
		return lerp(minVisibility, 1.0, visibility);
	}

	float GetFullSphereRadianceWeightedVisibility(
		sh2 radianceSHR, sh2 radianceSHG, sh2 radianceSHB,
		sh2 visibilitySH, sh2 responseLobe,
		float fadeOutFactor, float minVisibility)
	{
		radianceSHR *= IBL_TO_WORLD_SH;
		radianceSHG *= IBL_TO_WORLD_SH;
		radianceSHB *= IBL_TO_WORLD_SH;

		sh2 luminanceSH = radianceSHR * 0.2126 + radianceSHG * 0.7152 + radianceSHB * 0.0722;
		// Product(L, FULL_VISIBILITY_SH) is exactly L, so the full-sphere
		// reference needs no SH product.
		float referenceResponse = max(0.0, SphericalHarmonics::FuncProductIntegral(luminanceSH, responseLobe));
		float visibleResponse = max(0.0, SphericalHarmonics::FuncProductIntegral(
											 SphericalHarmonics::Product(luminanceSH, visibilitySH), responseLobe));
		float visibility = referenceResponse > EPSILON_WEIGHT_SUM ? saturate(visibleResponse / referenceResponse) : 1.0;
		visibility = lerp(1.0, visibility, fadeOutFactor);
		return lerp(minVisibility, 1.0, visibility);
	}

	float GetEnvRadianceWeightedVisibility(sh2 skyVisibilitySH, sh2 responseLobe, float fadeOutFactor, float minVisibility)
	{
		sh2 envVisibilitySH = SphericalHarmonics::ExtrapolateZonalCapVisibility(
			skyVisibilitySH, SharedData::skylightingSettings.OpenSkySH);
		return GetFullSphereRadianceWeightedVisibility(
			EnvIBLTexture.Load(int3(0, 0, 0)),
			EnvIBLTexture.Load(int3(1, 0, 0)),
			EnvIBLTexture.Load(int3(2, 0, 0)),
			envVisibilitySH, responseLobe, fadeOutFactor, minVisibility);
	}

	float GetSkyRadianceWeightedVisibility(sh2 skyVisibilitySH, sh2 responseLobe, float fadeOutFactor, float minVisibility)
	{
		return GetSourceRadianceWeightedVisibility(
			SkyIBLTexture.Load(int3(0, 0, 0)),
			SkyIBLTexture.Load(int3(1, 0, 0)),
			SkyIBLTexture.Load(int3(2, 0, 0)),
			skyVisibilitySH, SharedData::skylightingSettings.OpenSkySH, responseLobe, fadeOutFactor, minVisibility);
	}

	// ============================================================================
	// Low-level SH sampling (raw, no user settings applied)
	// ============================================================================
	float3 EvaluateDiffuseIBL(sh2 shR, sh2 shG, sh2 shB, float3 rayDir)
	{
		float colorR = SphericalHarmonics::SHHallucinateZH3Irradiance(shR, rayDir);
		float colorG = SphericalHarmonics::SHHallucinateZH3Irradiance(shG, rayDir);
		float colorB = SphericalHarmonics::SHHallucinateZH3Irradiance(shB, rayDir);
		return float3(colorR, colorG, colorB) / Math::PI;
	}

	/// Get Env IBL color from environment cubemap SH (without sky)
	float3 GetEnvIBL(float3 rayDir)
	{
		sh2 shR = EnvIBLTexture.Load(int3(0, 0, 0));
		sh2 shG = EnvIBLTexture.Load(int3(1, 0, 0));
		sh2 shB = EnvIBLTexture.Load(int3(2, 0, 0));
		return EvaluateDiffuseIBL(shR, shG, shB, rayDir);
	}

	/// Get Sky-only IBL color from source-separated Dynamic Cubemaps SH,
	/// or from the game's native reflections cubemap SH when that source is unavailable.
	float3 GetSkyIBL(float3 rayDir)
	{
		sh2 shR = SkyIBLTexture.Load(int3(0, 0, 0));
		sh2 shG = SkyIBLTexture.Load(int3(1, 0, 0));
		sh2 shB = SkyIBLTexture.Load(int3(2, 0, 0));
		return max(0, EvaluateDiffuseIBL(shR, shG, shB, rayDir));
	}

	float3 GetSkyIBLOccluded(float3 rayDir, float visibility)
	{
		return GetSkyIBL(rayDir) * visibility;
	}

	// ============================================================================
	// Ratio / settings helpers
	// ============================================================================

	float3 GetIBLRatio(sh2 iblSHR, sh2 iblSHG, sh2 iblSHB)
	{
		float3 dalc0 = Color::IrradianceToLinear(Color::Ambient(SharedData::GetAmbient(0.f)));

		// The L0 coefficient is the spherical mean radiance times sqrt(4 PI).
		// Evaluating the hallucinated irradiance at direction zero is undefined
		// and made exposure matching depend on the SH dominant direction.
		float3 ibl0 = max(0, float3(iblSHR.x, iblSHG.x, iblSHB.x) / sqrt(4.0 * Math::PI));

		if (SharedData::iblSettings.DALCMode == 1) {
			float3 ratio = dalc0 / max(ibl0, 0.001);
			return lerp(1.0, ratio, SharedData::iblSettings.DALCAmount);
		} else {
			float dalcLum = Color::RGBToLuminance(dalc0);
			float iblLum = Color::RGBToLuminance(ibl0);
			float ratio = (iblLum > 0.001) ? (dalcLum / iblLum) : 1.0;
			return lerp(1.0, ratio, SharedData::iblSettings.DALCAmount);
		}
	}

	/// Compute ratio between DALC and IBL for brightness/color matching.
	float3 GetIBLRatio()
	{
		return GetIBLRatio(
			EnvIBLTexture.Load(int3(0, 0, 0)),
			EnvIBLTexture.Load(int3(1, 0, 0)),
			EnvIBLTexture.Load(int3(2, 0, 0)));
	}

	// ============================================================================
	// Mid-level: individual components with user settings applied
	// ============================================================================

	float3 GetEnvIBLColor(float3 rayDir)
	{
		float3 ratio = GetIBLRatio();
		return Color::Saturation(GetEnvIBL(rayDir), SharedData::iblSettings.EnvIBLSaturation) * SharedData::iblSettings.EnvIBLScale * ratio;
	}

	float3 GetSkyIBLColor(float3 rayDir)
	{
		if (SharedData::InInterior) {
			return 0;
		}
		return Color::Saturation(GetSkyIBL(rayDir), SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
	}

	float3 GetSkyIBLColorOccluded(float3 rayDir, float visibility)
	{
		if (SharedData::InInterior) {
			return 0;
		}
		return Color::Saturation(GetSkyIBLOccluded(rayDir, visibility), SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
	}

	// ============================================================================
	// High-level: compute the full diffuse ambient replacement
	// ============================================================================

	/// Compute diffuse IBL ambient (gamma-space) without directional occlusion.
	float3 GetDiffuseIBL(float3 vanillaDALC, float3 rayDir)
	{
		float3 linEnv, linSky;
		if (SharedData::iblSettings.DALCMode >= 2) {
			linEnv = Color::IrradianceToLinear(vanillaDALC * SharedData::iblSettings.DALCAmount);
			linSky = GetSkyIBLColor(rayDir);
		} else {
			linEnv = GetEnvIBLColor(rayDir);
			linSky = GetSkyIBLColor(rayDir);
		}
		return Color::IrradianceToGamma(linEnv + linSky);
	}

	/// Compute diffuse IBL ambient (gamma-space) with source-specific visibility.
	/// DALC and Env IBL are full-environment sources; Sky IBL is an upper-cap source.
	float3 GetDiffuseIBLOccluded(float3 vanillaDALC, float3 rayDir, float skyVisibility, float envVisibility)
	{
		float3 linEnv, linSky;
		if (SharedData::iblSettings.DALCMode >= 2) {
			linEnv = Color::IrradianceToLinear(vanillaDALC * SharedData::iblSettings.DALCAmount);
		} else {
			linEnv = GetEnvIBLColor(rayDir);
		}
		if (!SharedData::InInterior)
			linEnv *= envVisibility;
		linSky = GetSkyIBLColorOccluded(rayDir, skyVisibility);
		return Color::IrradianceToGamma(linEnv + linSky);
	}

	/// Source-specific diffuse visibility with the IBL radiance inside the SH
	/// integral. visibilityScale preserves the existing vertex-AO compensation.
	float3 GetDiffuseIBLRadianceWeighted(
		float3 vanillaDALC, float3 rayDir, sh2 skyVisibilitySH,
		float fadeOutFactor = 1.0, float visibilityScale = 1.0)
	{
		// Skylighting has no measured role in interiors. Keep the original IBL
		// path there and avoid the visibility SH products entirely.
		if (SharedData::InInterior)
			return GetDiffuseIBL(vanillaDALC, rayDir);

		float3 normalWS = -normalize(rayDir);
		sh2 cosineLobe = SphericalHarmonics::EvaluateCosineLobe(normalWS);
		sh2 skySHR = SkyIBLTexture.Load(int3(0, 0, 0));
		sh2 skySHG = SkyIBLTexture.Load(int3(1, 0, 0));
		sh2 skySHB = SkyIBLTexture.Load(int3(2, 0, 0));

		float skyVisibility = GetSourceRadianceWeightedVisibility(
			skySHR, skySHG, skySHB,
			skyVisibilitySH, SharedData::skylightingSettings.OpenSkySH, cosineLobe, fadeOutFactor,
			SharedData::skylightingSettings.MinDiffuseVisibility);
		skyVisibility = saturate(skyVisibility * visibilityScale);

		float3 linEnv;
		if (SharedData::iblSettings.DALCMode >= 2) {
			// DALC is already a directional ambient result rather than incident
			// radiance SH, so use the full-Env geometric response for this path.
			sh2 envVisibilitySH = SphericalHarmonics::ExtrapolateZonalCapVisibility(
				skyVisibilitySH, SharedData::skylightingSettings.OpenSkySH);
			float envVisibility = SphericalHarmonics::FuncProductIntegral(envVisibilitySH, cosineLobe) / Math::PI;
			envVisibility = lerp(1.0, saturate(envVisibility), fadeOutFactor);
			envVisibility = lerp(SharedData::skylightingSettings.MinDiffuseVisibility, 1.0, envVisibility);
			envVisibility = saturate(envVisibility * visibilityScale);
			linEnv = Color::IrradianceToLinear(vanillaDALC * SharedData::iblSettings.DALCAmount);
			linEnv *= envVisibility;
		} else {
			sh2 envSHR = EnvIBLTexture.Load(int3(0, 0, 0));
			sh2 envSHG = EnvIBLTexture.Load(int3(1, 0, 0));
			sh2 envSHB = EnvIBLTexture.Load(int3(2, 0, 0));
			sh2 envVisibilitySH = SphericalHarmonics::ExtrapolateZonalCapVisibility(
				skyVisibilitySH, SharedData::skylightingSettings.OpenSkySH);
			float envVisibility = GetFullSphereRadianceWeightedVisibility(
				envSHR, envSHG, envSHB,
				envVisibilitySH, cosineLobe, fadeOutFactor,
				SharedData::skylightingSettings.MinDiffuseVisibility);
			envVisibility = saturate(envVisibility * visibilityScale);
			linEnv = Color::Saturation(EvaluateDiffuseIBL(envSHR, envSHG, envSHB, rayDir), SharedData::iblSettings.EnvIBLSaturation) *
			         SharedData::iblSettings.EnvIBLScale * GetIBLRatio(envSHR, envSHG, envSHB);
			linEnv *= envVisibility;
		}

		float3 linSky = Color::Saturation(
							max(0.0, EvaluateDiffuseIBL(skySHR, skySHG, skySHB, rayDir)), SharedData::iblSettings.SkyIBLSaturation) *
		                SharedData::iblSettings.SkyIBLScale * skyVisibility;
		return Color::IrradianceToGamma(linEnv + linSky);
	}

	// ============================================================================
	// Convenience: combined IBL (for simple contexts)
	// ============================================================================

	float3 GetIBLColor(float3 rayDir)
	{
		float3 envColor = SharedData::iblSettings.DALCMode >= 2 ?
		                      Color::IrradianceToLinear(Color::Ambient(max(0, SharedData::GetAmbient(rayDir))) * SharedData::iblSettings.DALCAmount) :
		                      GetEnvIBLColor(rayDir);
		return envColor + GetSkyIBLColor(rayDir);
	}

	float3 GetIBLColorOccluded(float3 rayDir, float skyVisibility, float envVisibility)
	{
		float3 envColor = SharedData::iblSettings.DALCMode >= 2 ?
		                      Color::IrradianceToLinear(Color::Ambient(max(0, SharedData::GetAmbient(rayDir))) * SharedData::iblSettings.DALCAmount) :
		                      GetEnvIBLColor(rayDir);
		if (!SharedData::InInterior)
			envColor *= envVisibility;
		return envColor + GetSkyIBLColorOccluded(rayDir, skyVisibility);
	}

#if defined(LIGHTING)
	float3 GetStaticDiffuseIBL(float3 N, SamplerState samp)
	{
		return StaticDiffuseIBLTexture.SampleLevel(samp, N.xzy, 0).xyz / Math::PI;
	}
#endif

	float3 GetFogIBLColor(float3 fogColor)
	{
		float3 iblColor;
		if (SharedData::iblSettings.DALCMode >= 2) {
			float3 dalc0 = Color::Ambient(SharedData::GetAmbient(0.f));
			iblColor = Color::IrradianceToLinear(dalc0 * SharedData::iblSettings.DALCAmount) + GetSkyIBLColor(float3(0, 0, 0));
		} else {
			iblColor = GetEnvIBLColor(float3(0, 0, 0)) + GetSkyIBLColor(float3(0, 0, 0));
		}
		if (SharedData::iblSettings.PreserveFogLuminance) {
			const float fogLuminance = Color::RGBToLuminance(fogColor);
			const float iblLuminance = Color::RGBToLuminance(iblColor);
			if (iblLuminance > 0) {
				const float scale = fogLuminance / iblLuminance;
				iblColor *= scale;
			} else {
				iblColor = fogColor;
			}
		}
		return lerp(fogColor, iblColor, SharedData::iblSettings.FogAmount);
	}
}

#endif  // __IBL_HLSLI__
