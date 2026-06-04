#ifndef PHYSICAL_LIGHTING_AREA_LIGHTS_HLSLI
#define PHYSICAL_LIGHTING_AREA_LIGHTS_HLSLI

namespace AreaLights
{
	float3 SphereAreaLightDir(float radius, float3 lightPos, float3 worldPos, float3 R)
	{
		float3 L = lightPos - worldPos;
		float centerToRayLen = length(dot(L, R) * R - L);
		float3 centerToRay = dot(L, R) * R - L;
		float3 closestPoint = L + centerToRay * saturate(radius / max(centerToRayLen, 1e-4f));
		return normalize(closestPoint);
	}

	float3 DiscAreaLightDir(float radius, float3 lightPos, float3 worldPos, float3 N, float3 R)
	{
		return SphereAreaLightDir(radius, lightPos, worldPos, R);
	}

	float3 TubeAreaLightDir(float length, float3 lightPos, float3 worldPos, float3 R)
	{
		return normalize(lightPos - worldPos);
	}

	float3 RectAreaLightDir(float width, float height, float3 lightPos, float3 worldPos, float3 N, float3 R)
	{
		return normalize(lightPos - worldPos);
	}

	float EnergyNormalization(float roughness, float areaRadius, float lightDistance)
	{
		float a = max(roughness, 0.045f);
		float sphereAngle = saturate(areaRadius / max(lightDistance, 1e-4f));
		float normalization = a / saturate(a + 0.5f * sphereAngle);
		return normalization * normalization;
	}
}

#endif
