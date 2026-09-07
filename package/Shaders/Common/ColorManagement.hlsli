#ifndef __COLOR_MANAGEMENT_DEPENDENCY_HLSL__
#define __COLOR_MANAGEMENT_DEPENDENCY_HLSL__

#include "Common/Color.hlsli"

namespace ColorManagement
{
#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
	float3 DecodedColorTextureToWorking(float3 decodedTextureColor)
	{
		return Color::LinearSRGBToWorking(decodedTextureColor);
	}

	float3 SRGBToWorking(float3 encodedSRGB)
	{
#	if defined(ENABLE_LL)
		float3 linearSRGB = TransferFunctions::SRGBToLinear(encodedSRGB);
		return Color::LinearSRGBToWorking(linearSRGB);
#	else
		return encodedSRGB;
#	endif
	}

	float SRGBToWorking(float encodedSRGB)
	{
#	if defined(ENABLE_LL)
		return TransferFunctions::SRGBToLinear(encodedSRGB);
#	else
		return encodedSRGB;
#	endif
	}

	float3 ApplyENBDiffuseCurve(float3 color)
	{
#	if defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			color = pow(abs(color), SharedData::enbSettings.ColorPow);
#	endif
		return color;
	}

	float MaterialAlbedoScale()
	{
#	if defined(TRUE_PBR)
		return 1.0f;
#	else
		return SharedData::linearLightingSettings.vanillaDiffuseColorMult;
#	endif
	}

	float3 AlbedoTextureToWorking(float3 decodedTextureColor)
	{
		decodedTextureColor = ApplyENBDiffuseCurve(decodedTextureColor);
		return DecodedColorTextureToWorking(decodedTextureColor) * MaterialAlbedoScale();
	}

	float3 AlbedoValueToWorking(float3 albedo)
	{
		albedo = ApplyENBDiffuseCurve(albedo);
#	if defined(TRUE_PBR)
		return Color::LinearSRGBToWorking(albedo);
#	else
		return SRGBToWorking(albedo) * MaterialAlbedoScale();
#	endif
	}

	float3 EmissiveTextureToWorking(float3 decodedTextureColor)
	{
		return DecodedColorTextureToWorking(decodedTextureColor) * SharedData::linearLightingSettings.glowmapMult;
	}

	float3 PBRVertexColorToLinear(float3 encodedVertexColor)
	{
#	if defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return Color::Gamma22ToLinear(encodedVertexColor);
#	endif
		return SRGBToWorking(encodedVertexColor);
	}

	float3 WorkingToDelivery(float3 workingColor)
	{
#	if defined(ENABLE_ACESCG)
		float3 lin = AP1TosRGB(workingColor);
#	else
		float3 lin = workingColor;
#	endif
		[branch] if (SharedData::linearLightingSettings.deliveryEncoding == 1) return TransferFunctions::LinearToGamma22(lin);
		return lin;
	}

	float StorageToWorking(float storedValue)
	{
#	if defined(ENABLE_LL)
		return storedValue;
#	else
		return Color::GameGammaToLinear(storedValue);
#	endif
	}

	float3 StorageToWorking(float3 storedValue)
	{
#	if defined(ENABLE_LL)
		return storedValue;
#	else
		return Color::GameGammaToLinear(storedValue);
#	endif
	}

	float WorkingToStorage(float workingValue)
	{
#	if defined(ENABLE_LL)
		return workingValue;
#	else
		return Color::LinearToGameGamma(workingValue);
#	endif
	}

	float3 WorkingToStorage(float3 workingValue)
	{
#	if defined(ENABLE_LL)
		return workingValue;
#	else
		return Color::LinearToGameGamma(workingValue);
#	endif
	}

	namespace WorkingColor
	{
		float ScaleByLinear(float workingColor, float linearScale)
		{
#	if defined(ENABLE_LL)
			return workingColor * linearScale;
#	else
			return abs(workingColor) * Color::LinearToGameGamma(linearScale);
#	endif
		}

		float3 ScaleByLinear(float3 workingColor, float linearScale)
		{
#	if defined(ENABLE_LL)
			return workingColor * linearScale;
#	else
			return abs(workingColor) * Color::LinearToGameGamma(linearScale);
#	endif
		}

		float3 ScaleByLinear(float3 workingColor, float3 linearScale)
		{
#	if defined(ENABLE_LL)
			return workingColor * linearScale;
#	else
			return abs(workingColor) * Color::LinearToGameGamma(linearScale);
#	endif
		}

		float3 ScaleAndAddLinear(float3 workingColor, float3 linearScale, float3 linearOffset)
		{
			return WorkingToStorage(StorageToWorking(workingColor) * linearScale + linearOffset);
		}

		float3 Modulate(float3 workingColor, float workingMultiplier)
		{
#	if defined(ENABLE_LL)
			return workingColor * workingMultiplier;
#	else
			return abs(workingColor * workingMultiplier);
#	endif
		}

		float3 LerpInLinear(float3 lhsWorkingColor, float3 rhsWorkingColor, float weight)
		{
			return WorkingToStorage(lerp(StorageToWorking(lhsWorkingColor), StorageToWorking(rhsWorkingColor), weight));
		}
	}

	float BRDFNormalization()
	{
#	if defined(ENABLE_LL)
		return Math::INV_PI;
#	else
		return 1.0f;
#	endif
	}
#endif
}

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
namespace Color
{
	float3 Light(float3 color)
	{
#	if defined(TRUE_PBR)
		return color * PBRLightingCompensation;  // Compensate for traditional Lambertian diffuse
#	else
		return color;
#	endif
	}

	float3 DirectionalLight(float3 color)
	{
		return Light(color) * SharedData::linearLightingSettings.directionalLightMult;
	}

	float3 PointLight(float3 color)
	{
		return Light(color) * SharedData::linearLightingSettings.pointLightMult;
	}

	float3 Ambient(float3 color)
	{
		return color * SharedData::linearLightingSettings.ambientMult;
	}

	float3 EffectMult(float3 color)
	{
#	if defined(MEMBRANE)
		color *= SharedData::linearLightingSettings.membraneEffectMult;
#	elif defined(BLOOD)
		color *= SharedData::linearLightingSettings.bloodEffectMult;
#	elif defined(PROJECTED_UV)
		color *= SharedData::linearLightingSettings.projectedEffectMult;
#	elif defined(DEFERRED)
		color *= SharedData::linearLightingSettings.deferredEffectMult;
#	else
		color *= SharedData::linearLightingSettings.otherEffectMult;
#	endif
		return color;
	}

	float EffectLightingMult()
	{
		return SharedData::linearLightingSettings.effectLightingMult;
	}
}
#endif

#endif
