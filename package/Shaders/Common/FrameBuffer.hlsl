#include "Common/VR.hlsl"
cbuffer PerFrame : register(b12)
{
#if !defined(VR)
	row_major float4x4 CameraView : packoffset(c0);
	row_major float4x4 CameraProj : packoffset(c4);
	row_major float4x4 CameraViewProj : packoffset(c8);
	row_major float4x4 CameraViewProjUnjittered : packoffset(c12);
	row_major float4x4 CameraPreviousViewProjUnjittered : packoffset(c16);
	row_major float4x4 CameraProjUnjittered : packoffset(c20);
	row_major float4x4 CameraProjUnjitteredInverse : packoffset(c24);
	row_major float4x4 CameraViewInverse : packoffset(c28);
	row_major float4x4 CameraViewProjInverse : packoffset(c32);
	row_major float4x4 CameraProjInverse : packoffset(c36);
	float4 CameraPosAdjust : packoffset(c40);
	float4 CameraPreviousPosAdjust : packoffset(c41);   // fDRClampOffset in w
	float4 FrameParams : packoffset(c42);               // inverse fGamma in x, some flags in yzw
	float4 DynamicResolutionParams1 : packoffset(c43);  // fDynamicResolutionWidthRatio in x, fDynamicResolutionHeightRatio in y, fDynamicResolutionPreviousWidthRatio in z, fDynamicResolutionPreviousHeightRatio in w
	float4 DynamicResolutionParams2 : packoffset(c44);  // inverse fDynamicResolutionWidthRatio in x, inverse fDynamicResolutionHeightRatio in y, fDynamicResolutionWidthRatio - fDRClampOffset in z, fDynamicResolutionPreviousWidthRatio - fDRClampOffset in w
#else
	row_major float4x4 CameraView : packoffset(c0);
	row_major float4x4 CameraProj : packoffset(c8);
	row_major float4x4 CameraViewProj : packoffset(c16);
	row_major float4x4 CameraViewProjUnjittered : packoffset(c24);
	row_major float4x4 CameraPreviousViewProjUnjittered : packoffset(c32);
	row_major float4x4 CameraProjUnjittered : packoffset(c40);
	row_major float4x4 CameraProjUnjitteredInverse : packoffset(c44);
	row_major float4x4 CameraViewInverse : packoffset(c56);
	row_major float4x4 CameraViewProjInverse : packoffset(c64);
	row_major float4x4 CameraProjInverse : packoffset(c72);
	float4 CameraPosAdjust : packoffset(c80);
	float4 CameraPreviousPosAdjust : packoffset(c82);   // fDRClampOffset in w
	float4 FrameParams : packoffset(c84);               // inverse fGamma in x, some flags in yzw
	float4 DynamicResolutionParams1 : packoffset(c85);  // fDynamicResolutionWidthRatio in x, fDynamicResolutionHeightRatio in y, fDynamicResolutionPreviousWidthRatio in z, fDynamicResolutionPreviousHeightRatio in w
	float4 DynamicResolutionParams2 : packoffset(c86);  // inverse fDynamicResolutionWidthRatio in x, inverse fDynamicResolutionHeightRatio in y, fDynamicResolutionWidthRatio - fDRClampOffset in z, fDynamicResolutionPreviousWidthRatio - fDRClampOffset in w
#endif                                                  // !VR
}

float2 GetDynamicResolutionAdjustedScreenPosition(float2 screenPosition)
{
	float2 adjustedScreenPosition = max(0.0.xx, DynamicResolutionParams1.xy * screenPosition);
	return min(float2(DynamicResolutionParams2.z, DynamicResolutionParams1.y), adjustedScreenPosition);
}

float2 GetPreviousDynamicResolutionAdjustedScreenPosition(float2 screenPosition)
{
	float2 adjustedScreenPosition = max(0.0.xx, DynamicResolutionParams1.zw * screenPosition);
	return min(float2(DynamicResolutionParams2.w, DynamicResolutionParams1.w),
		adjustedScreenPosition);
}

float3 ToSRGBColor(float3 linearColor)
{
	return pow(linearColor, FrameParams.x);
}

float3 WorldToView(float3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	float4 newPosition = float4(x, (float)is_position);
	return NG_mul(CameraView, newPosition, a_eyeIndex).xyz;
}

float2 ViewToUV(float3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	float4 newPosition = float4(x, (float)is_position);
	float4 uv = NG_mul(CameraProj, newPosition, a_eyeIndex);
	return (uv.xy / uv.w) * float2(0.5f, -0.5f) + 0.5f;
}
