#ifndef PHYSICAL_LIGHTING_HLSLI
#define PHYSICAL_LIGHTING_HLSLI

#include "PhysicalLighting/AreaLights.hlsli"
#include "PhysicalLighting/ColorTemperature.hlsli"

namespace PhysicalLighting
{
	struct PhysicalLightExt
	{
		float luminousIntensity;
		float luminousPower;
		uint unitType;
		float unitScale;

		float colorTemperature;
		float tint;
		uint pad0;
		uint pad1;

		uint areaType;
		float areaWidth;
		float areaHeight;
		float areaNormalize;

		uint pad2;
		uint pad3;
		uint pad4;
		uint pad5;
	};

	StructuredBuffer<PhysicalLightExt> physicalLightData : register(t38);

	bool IsPhysicalLight(uint lightFlags)
	{
		return (lightFlags & LightLimitFix::LightFlags::Physical) != 0;
	}

	float GetIntensity(uint lightIndex, LightLimitFix::Light light)
	{
		PhysicalLightExt ext = physicalLightData[lightIndex];
		float intensity = ext.luminousIntensity * SharedData::physicalLightingSettings.globalIntensityScale;
		if ((light.lightFlags & LightLimitFix::LightFlags::AreaLight) != 0)
			intensity *= ext.areaNormalize;
		return intensity;
	}

	float3 GetLightColor(uint lightIndex, LightLimitFix::Light light)
	{
		PhysicalLightExt ext = physicalLightData[lightIndex];
		float3 colorTemperatureRgb = ColorTemperature::KelvinToRGB(ext.colorTemperature, ext.tint);
		return ((light.lightFlags & LightLimitFix::LightFlags::UseColorTemp) != 0) ? colorTemperatureRgb : light.color;
	}

	float3 GetAreaLightDirection(uint lightIndex, float3 worldPos, float3 normal, float3 reflection, float3 lightPos)
	{
		PhysicalLightExt ext = physicalLightData[lightIndex];

		switch (ext.areaType) {
		case 1:
			return AreaLights::SphereAreaLightDir(ext.areaWidth * 0.5f, lightPos, worldPos, reflection);
		case 2:
			return AreaLights::DiscAreaLightDir(ext.areaWidth * 0.5f, lightPos, worldPos, normal, reflection);
		case 3:
			return AreaLights::TubeAreaLightDir(ext.areaWidth, lightPos, worldPos, reflection);
		case 4:
			return AreaLights::RectAreaLightDir(ext.areaWidth, ext.areaHeight, lightPos, worldPos, normal, reflection);
		default:
			return normalize(lightPos - worldPos);
		}
	}
}

#endif
