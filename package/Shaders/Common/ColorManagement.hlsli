#ifndef __COLOR_MANAGEMENT_DEPENDENCY_HLSL__
#define __COLOR_MANAGEMENT_DEPENDENCY_HLSL__

#include "Common/Color.hlsli"

namespace ColorManagement
{
#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
	float DecodeSRGBChannel(float encodedSRGB)
	{
		float magnitude = abs(encodedSRGB);
		float linearSRGB = magnitude <= 0.04045f ? magnitude / 12.92f : pow((magnitude + 0.055f) / 1.055f, 2.4f);
		return linearSRGB * sign(encodedSRGB);
	}

	float3 DecodedColorTextureToWorking(float3 decodedTextureColor)
	{
		return Color::LinearSRGBToWorking(decodedTextureColor);
	}

	float3 SRGBToWorking(float3 encodedSRGB)
	{
		float3 linearSRGB = float3(
			DecodeSRGBChannel(encodedSRGB.r),
			DecodeSRGBChannel(encodedSRGB.g),
			DecodeSRGBChannel(encodedSRGB.b));
		return ENABLE_LL ? Color::LinearSRGBToWorking(linearSRGB) : encodedSRGB;
	}

	float SRGBToWorking(float encodedSRGB)
	{
		return ENABLE_LL ? DecodeSRGBChannel(encodedSRGB) : encodedSRGB;
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
#	if defined(TRUE_PBR) && defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return Color::LinearToGamma22(decodedTextureColor);
#	endif
		return DecodedColorTextureToWorking(decodedTextureColor) * MaterialAlbedoScale();
	}

	float3 AlbedoValueToWorking(float3 albedo)
	{
		albedo = ApplyENBDiffuseCurve(albedo);
#	if defined(TRUE_PBR)
#		if defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return Color::LinearToGamma22(albedo);
#		endif
		return Color::LinearSRGBToWorking(albedo);
#	else
		return SRGBToWorking(albedo) * MaterialAlbedoScale();
#	endif
	}

	float3 EmissiveTextureToWorking(float3 decodedTextureColor)
	{
#	if defined(TRUE_PBR) && defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return Color::LinearToGamma22(decodedTextureColor);
#	endif
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

	float3 PBRMaterialToLinear(float3 materialColor)
	{
#	if defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return Color::Gamma22ToLinear(materialColor);
#	endif
		return materialColor;
	}

	float3 LinearToPBRMaterial(float3 linearColor)
	{
#	if defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return Color::LinearToGamma22(linearColor);
#	endif
		return linearColor;
	}

	float3 ScalePBRMaterialByLinear(float3 materialColor, float3 linearScale)
	{
#	if defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return abs(materialColor) * Color::LinearToGamma22(linearScale);
#	endif
		return materialColor * linearScale;
	}

	float3 ModulatePBRMaterialsByLinear(float3 lhsMaterialColor, float3 rhsMaterialColor, float3 linearScale)
	{
#	if defined(EFFECTS11)
		if (SharedData::enbSettings.Enable)
			return abs(lhsMaterialColor * rhsMaterialColor) * Color::LinearToGamma22(linearScale);
#	endif
		return lhsMaterialColor * rhsMaterialColor * linearScale;
	}

	namespace WorkingColor
	{
		float ToLinear(float workingColor)
		{
			return ENABLE_LL ? workingColor : Color::GameGammaToLinear(workingColor);
		}

		float3 ToLinear(float3 workingColor)
		{
			return ENABLE_LL ? workingColor : Color::GameGammaToLinear(workingColor);
		}

		float FromLinear(float linearValue)
		{
			return ENABLE_LL ? linearValue : Color::LinearToGameGamma(linearValue);
		}

		float3 FromLinear(float3 linearValue)
		{
			return ENABLE_LL ? linearValue : Color::LinearToGameGamma(linearValue);
		}

		float ScaleByLinear(float workingColor, float linearScale)
		{
			return ENABLE_LL ? workingColor * linearScale : abs(workingColor) * Color::LinearToGameGamma(linearScale);
		}

		float3 ScaleByLinear(float3 workingColor, float linearScale)
		{
			return ENABLE_LL ? workingColor * linearScale : abs(workingColor) * Color::LinearToGameGamma(linearScale);
		}

		float3 ScaleByLinear(float3 workingColor, float3 linearScale)
		{
			return ENABLE_LL ? workingColor * linearScale : abs(workingColor) * Color::LinearToGameGamma(linearScale);
		}

		float3 ScaleAndAddLinear(float3 workingColor, float3 linearScale, float3 linearOffset)
		{
			return FromLinear(ToLinear(workingColor) * linearScale + linearOffset);
		}

		float3 Modulate(float3 workingColor, float workingMultiplier)
		{
			return ENABLE_LL ? workingColor * workingMultiplier : abs(workingColor * workingMultiplier);
		}

		float3 LerpInLinear(float3 lhsWorkingColor, float3 rhsWorkingColor, float weight)
		{
			return FromLinear(lerp(ToLinear(lhsWorkingColor), ToLinear(rhsWorkingColor), weight));
		}
	}

	float BRDFNormalization()
	{
		return ENABLE_LL ? Math::INV_PI : 1.0f;
	}
#endif
}

#endif
