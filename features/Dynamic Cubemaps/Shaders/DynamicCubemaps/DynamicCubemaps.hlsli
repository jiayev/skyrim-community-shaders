#ifndef DYNAMICCUBEMAPS_HLSLI
#define DYNAMICCUBEMAPS_HLSLI

#include "Common/BRDF.hlsli"

#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"
#endif

#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

namespace DynamicCubemaps
{
	TextureCube<float3> EnvReflectionsTexture : register(t30);
	TextureCube<float3> EnvTexture : register(t31);

#if !defined(WATER)

	float3 GetSourceSeparatedIrradiance(float3 R, float level, float3 directionalAmbientColorSpecular, float skyVisibility, float envVisibility)
	{
		float3 linEnvSample = Color::IrradianceToLinear(EnvTexture.SampleLevel(SampColorSampler, R, level));
		float3 linFullSample = Color::IrradianceToLinear(EnvReflectionsTexture.SampleLevel(SampColorSampler, R, level));
		float3 linSkySample = max(0.0, linFullSample - linEnvSample);
		float linEnvLum = Color::RGBToLuminance(Color::IrradianceToLinear(EnvTexture.SampleLevel(SampColorSampler, R, 15)));
		float3 linDALC = Color::IrradianceToLinear(directionalAmbientColorSpecular);

		float3 envSpecular;
		float3 skySpecular;
#	if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			if (SharedData::iblSettings.DALCMode >= 2) {
				envSpecular = (linEnvSample / max(linEnvLum, 0.001)) * linDALC * SharedData::iblSettings.DALCAmount;
			} else {
				float3 ratio = ImageBasedLighting::GetIBLRatio();
				envSpecular = Color::Saturation(linEnvSample, SharedData::iblSettings.EnvIBLSaturation) * ratio * SharedData::iblSettings.EnvIBLScale;
			}
			skySpecular = Color::Saturation(linSkySample, SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
		} else
#	endif
		{
			// DALC is a full-environment exposure for the source-separated fallback.
			float3 exposure = linDALC / max(linEnvLum, 0.001);
			envSpecular = linEnvSample * exposure;
			skySpecular = linSkySample * exposure;
		}

		if (SharedData::InInterior) {
			skySpecular = 0.0;
		} else {
			envSpecular *= envVisibility;
			skySpecular *= skyVisibility;
		}
		return envSpecular + skySpecular;
	}
#	if defined(SKYLIGHTING)
	float3 GetDynamicCubemapSpecularIrradiance(float3 N, float3 V, float roughness, sh2 skylighting)
#	else
	float3 GetDynamicCubemapSpecularIrradiance(float3 N, float3 V, float roughness)
#	endif
	{
#	if defined(DEFERRED)
		return 1.0;
#	else
		float3 R = reflect(-V, N);
		float NoV = saturate(dot(N, V));

		float level = roughness * 7.0;

		float3 finalIrradiance = 0;

		float3 directionalAmbientColorSpecular = Color::Ambient(max(0, SharedData::GetAmbient(R))) *
		                                         Color::ReflectionNormalisationScale;

#		if defined(IBL) && defined(LIGHTING)
		const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);
		const bool inReflection = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection);
		const bool useStaticIBL = SharedData::iblSettings.EnableIBL && SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection;
#		else
		const bool useStaticIBL = false;
#		endif

		if (!useStaticIBL) {
#		if defined(SKYLIGHTING)
			float skySpecularVisibility = 1.0;
			float envSpecularVisibility = 1.0;
			if (!SharedData::InInterior) {
				sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(N, V, roughness);
#			if defined(IBL)
				if (SharedData::iblSettings.EnableIBL) {
					skySpecularVisibility = ImageBasedLighting::GetSkyRadianceWeightedVisibility(
						skylighting, specularLobe, 1.0, SharedData::skylightingSettings.MinSpecularVisibility);
					envSpecularVisibility = ImageBasedLighting::GetEnvRadianceWeightedVisibility(
						skylighting, specularLobe, 1.0, SharedData::skylightingSettings.MinSpecularVisibility);
				} else {
					skySpecularVisibility = Skylighting::EvaluateSkySpecular(skylighting, specularLobe);
					envSpecularVisibility = Skylighting::EvaluateEnvironmentSpecular(skylighting, specularLobe);
				}
#			else
				skySpecularVisibility = Skylighting::EvaluateSkySpecular(skylighting, specularLobe);
				envSpecularVisibility = Skylighting::EvaluateEnvironmentSpecular(skylighting, specularLobe);
#			endif
			}
#		else
			float skySpecularVisibility = 1.0;
			float envSpecularVisibility = 1.0;
#		endif
			finalIrradiance = GetSourceSeparatedIrradiance(R, level, directionalAmbientColorSpecular, skySpecularVisibility, envSpecularVisibility);
		} else {
#		if defined(IBL) && defined(LIGHTING)
			float3 specularIrradiance = ImageBasedLighting::StaticSpecularIBLTexture.SampleLevel(SampColorSampler, R.xzy, level).xyz;
			finalIrradiance = specularIrradiance;
#		endif
		}

		return finalIrradiance;
#	endif
	}

#	if defined(SKYLIGHTING)
	float3 GetDynamicCubemap(float3 N, float3 V, float roughness, float3 F0, sh2 skylighting)
#	else
	float3 GetDynamicCubemap(float3 N, float3 V, float roughness, float3 F0)
#	endif
	{
#	if defined(DEFERRED)
		return 1.0;
#	else
		float3 R = reflect(-V, N);
		float NoV = saturate(dot(N, V));

		float level = roughness * 7.0;

		float2 specularBRDF = BRDF::EnvBRDF(roughness, NoV);

		float3 finalIrradiance = 0;
		float3 directionalAmbientColorSpecular = Color::Ambient(max(0, SharedData::GetAmbient(R))) * Color::ReflectionNormalisationScale;

#		if defined(IBL) && defined(LIGHTING)
		const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);
		const bool inReflection = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection);
		if (SharedData::iblSettings.EnableIBL && SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection) {
			float3 specularIrradiance = ImageBasedLighting::StaticSpecularIBLTexture.SampleLevel(SampColorSampler, R.xzy, level).xyz;
			return (F0 * specularBRDF.x + specularBRDF.y) * specularIrradiance;
		}
#		endif

		float skySpecularVisibility = 1.0;
		float envSpecularVisibility = 1.0;
#		if defined(SKYLIGHTING)
		if (!SharedData::InInterior) {
			sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(N, V, roughness);
#			if defined(IBL)
			if (SharedData::iblSettings.EnableIBL) {
				skySpecularVisibility = ImageBasedLighting::GetSkyRadianceWeightedVisibility(
					skylighting, specularLobe, 1.0, SharedData::skylightingSettings.MinSpecularVisibility);
				envSpecularVisibility = ImageBasedLighting::GetEnvRadianceWeightedVisibility(
					skylighting, specularLobe, 1.0, SharedData::skylightingSettings.MinSpecularVisibility);
			} else {
				skySpecularVisibility = Skylighting::EvaluateSkySpecular(skylighting, specularLobe);
				envSpecularVisibility = Skylighting::EvaluateEnvironmentSpecular(skylighting, specularLobe);
			}
#			else
			skySpecularVisibility = Skylighting::EvaluateSkySpecular(skylighting, specularLobe);
			envSpecularVisibility = Skylighting::EvaluateEnvironmentSpecular(skylighting, specularLobe);
#			endif
		}
#		endif
		finalIrradiance = GetSourceSeparatedIrradiance(R, level, directionalAmbientColorSpecular, skySpecularVisibility, envSpecularVisibility);

		return (F0 * specularBRDF.x + specularBRDF.y) * finalIrradiance;
#	endif
	}
#endif  // !WATER
}
#endif  // DYNAMICCUBEMAPS_HLSLI
