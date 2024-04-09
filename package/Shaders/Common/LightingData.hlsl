#include "Common/Constants.hlsli"
#include "Common/VR.hlsli"

#if defined(PSHADER)

struct LightingData
{
	float WaterHeight[25];
	bool Reflections;
	float4 CameraData;
	float2 BufferDim;
	float Timer;
};

StructuredBuffer<LightingData> lightingData : register(t126);

Texture2D<float4> TexDepthSampler : register(t20);

// Get a raw depth from the depth buffer. [0,1] in uv space
float GetDepth(float2 uv, uint a_eyeIndex = 0)
{
	uv = ConvertToStereoUV(uv, a_eyeIndex);
	uv = GetDynamicResolutionAdjustedScreenPosition(uv);
	return TexDepthSampler.Load(int3(uv * lightingData[0].BufferDim, 0));
}

float GetScreenDepth(float depth)
{
	return (lightingData[0].CameraData.w / (-depth * lightingData[0].CameraData.z + lightingData[0].CameraData.x));
}

float GetScreenDepth(float2 uv, uint a_eyeIndex = 0)
{
	float depth = GetDepth(uv, a_eyeIndex);
	return GetScreenDepth(depth);
}

float GetWaterHeight(float3 worldPosition)
{
	float2 cellF = (((worldPosition.xy + CameraPosAdjust[0].xy)) / 4096.0) + 64.0;  // always positive
	int2 cellInt;
	float2 cellFrac = modf(cellF, cellInt);

	cellF = worldPosition.xy / float2(4096.0, 4096.0);  // remap to cell scale
	cellF += 2.5;                                       // 5x5 cell grid
	cellF -= cellFrac;                                  // align to cell borders
	cellInt = round(cellF);

	uint waterTile = (uint)clamp(cellInt.x + (cellInt.y * 5), 0, 24);  // remap xy to 0-24
	float waterHeight = -2147483648;                                   // lowest 32-bit integer

	[flatten] if (cellInt.x < 5 && cellInt.x >= 0 && cellInt.y < 5 && cellInt.y >= 0)
		waterHeight = lightingData[0].WaterHeight[waterTile];
	return waterHeight;
}

#endif