#ifndef XeGTAO_LIGHTING_HLSLI
#define XeGTAO_LIGHTING_HLSLI

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

#include "XeGTAO/XeGTAO.hlsli"

namespace GTAO
{
	Texture2D<uint> XeGTAOTexture : register(t78);
	Texture2D<uint> XeGTAOGeneratedNormal : register(t79);

	float GetVisibility(float2 uv, uint eyeIndex)
	{
		uint2 pixelCoord = SharedData::ConvertUVToSampleCoord(uv, eyeIndex);
		uint packedXeGTAO = XeGTAOTexture[pixelCoord].x;
		float xeGTAOWeight = 1.0;
		xeGTAOWeight = (float)packedXeGTAO / 255.0;
		return xeGTAOWeight;
	}

	float3 GetBentNormals(float2 uv, uint eyeIndex, out float xeGTAO)
    {
        xeGTAO = 1.0;
		uint2 pixelCoord = SharedData::ConvertUVToSampleCoord(uv, eyeIndex);
		uint packedXeGTAO = XeGTAOTexture[pixelCoord].x;
		lpfloat xeGTAOWeight = 1.0;
		lpfloat3 bentNormal = 0.0;

		XeGTAO_DecodeVisibilityBentNormal(packedXeGTAO, xeGTAOWeight, bentNormal);
        xeGTAO = xeGTAOWeight;
		return normalize(bentNormal);
	}

	float3 GetGeneratedNormals(float2 uv, uint eyeIndex)
	{
		uint2 pixelCoord = SharedData::ConvertUVToSampleCoord(uv, eyeIndex);
		uint packedXeGTAONormal = XeGTAOGeneratedNormal[pixelCoord].x;
		return normalize(XeGTAO_R11G11B10_UNORM_to_FLOAT3(packedXeGTAONormal) * 2.0 - 1.0);
	}

	float3 WorldToModel(float3 worldPos, float3x4 world)
	{
		float3x3 rotationMatrix = float3x3(
			world[0].xyz,
			world[1].xyz,
			world[2].xyz);
		float3x3 inverseRotation = float3x3(
			rotationMatrix[0][0], rotationMatrix[1][0], rotationMatrix[2][0],
			rotationMatrix[0][1], rotationMatrix[1][1], rotationMatrix[2][1],
			rotationMatrix[0][2], rotationMatrix[1][2], rotationMatrix[2][2]);
		float3 modelPos = mul(inverseRotation, worldPos);
		return modelPos;
	}

#if !defined(DRAW_IN_WORLDSPACE)
	float3 GetModelSpaceBentNormal(float2 uv, uint eyeIndex, out float xeGTAO, bool worldSpace, float3x4 world)
#else
	float3 GetModelSpaceBentNormal(float2 uv, uint eyeIndex, out float xeGTAO)
#endif
	{
        xeGTAO = 1.0;
		float4x4 inverseView = FrameBuffer::CameraViewInverse[eyeIndex];
		float3 bentNormal = GetBentNormals(uv, eyeIndex, xeGTAO);
		float3 generatedNormal = GetGeneratedNormals(uv, eyeIndex);

		float3 bentNormalWS = normalize(mul(inverseView, float4(bentNormal, 0)).xyz);
		float3 generatedNormalWS = normalize(mul(inverseView, float4(generatedNormal, 0)).xyz);
        float3 diffWS = bentNormalWS - generatedNormalWS;


#if !defined(DRAW_IN_WORLDSPACE)
		if (!worldSpace) {
			bentNormalWS = normalize(WorldToModel(bentNormalWS, world));
			generatedNormalWS = normalize(WorldToModel(generatedNormalWS, world));
            diffWS = bentNormalWS - generatedNormalWS;
		}
#endif
		return diffWS;
	}
}
#endif