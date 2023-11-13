
struct PerPassParallax
{
	bool EnableComplexMaterial;

	bool EnableParallax;
	bool EnableTerrainParallax;
	bool EnableHighQuality;

	uint MaxDistance;
	float CRPMRange;
	float BlendRange;
	float Height;

	bool EnableShadows;
	uint ShadowsStartFade;
	uint ShadowsEndFade;
};

StructuredBuffer<PerPassParallax> perPassParallax : register(t30);

#include "ComplexParallaxMaterials/CRPM.hlsli"
