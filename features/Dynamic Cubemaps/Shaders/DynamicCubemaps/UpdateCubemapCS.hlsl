#include "../Common/VR.hlsl"
RWTexture2DArray<float4> DynamicCubemap : register(u0);
RWTexture2DArray<float4> DynamicCubemapRaw : register(u1);
RWTexture2DArray<float4> DynamicCubemapPosition : register(u2);

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> ColorTexture : register(t1);

// Calculate normalized sampling direction vector based on current fragment coordinates.
// This is essentially "inverse-sampling": we reconstruct what the sampling vector would be if we wanted it to "hit"
// this particular fragment in a cubemap.
float3 GetSamplingVector(uint3 ThreadID, in RWTexture2DArray<float4> OutputTexture)
{
	float width = 0.0f;
	float height = 0.0f;
	float depth = 0.0f;
	OutputTexture.GetDimensions(width, height, depth);

	float2 st = ThreadID.xy / float2(width, height);
	float2 uv = 2.0 * float2(st.x, 1.0 - st.y) - 1.0;

	// Select vector based on cubemap face index.
	float3 result = float3(0.0f, 0.0f, 0.0f);
	switch (ThreadID.z) {
	case 0:
		result = float3(1.0, uv.y, -uv.x);
		break;
	case 1:
		result = float3(-1.0, uv.y, uv.x);
		break;
	case 2:
		result = float3(uv.x, 1.0, -uv.y);
		break;
	case 3:
		result = float3(uv.x, -1.0, uv.y);
		break;
	case 4:
		result = float3(uv.x, uv.y, 1.0);
		break;
	case 5:
		result = float3(-uv.x, uv.y, -1.0);
		break;
	}
	return normalize(result);
}

cbuffer PerFrame : register(b0)
{
#if !defined(VR)
	row_major float4x4 CameraView[1] : packoffset(c0);
	row_major float4x4 CameraProj[1] : packoffset(c4);
	row_major float4x4 CameraViewProj[1] : packoffset(c8);
	row_major float4x4 CameraViewProjUnjittered[1] : packoffset(c12);
	row_major float4x4 CameraPreviousViewProjUnjittered[1] : packoffset(c16);
	row_major float4x4 CameraProjUnjittered[1] : packoffset(c20);
	row_major float4x4 CameraProjUnjitteredInverse[1] : packoffset(c24);
	row_major float4x4 CameraViewInverse[1] : packoffset(c28);
	row_major float4x4 CameraViewProjInverse[1] : packoffset(c32);
	row_major float4x4 CameraProjInverse[1] : packoffset(c36);
	float4 CameraPosAdjust[1] : packoffset(c40);
	float4 CameraPreviousPosAdjust[1] : packoffset(c41);  // fDRClampOffset in w
	float4 FrameParams : packoffset(c42);                 // inverse fGamma in x, some flags in yzw
	float4 DynamicResolutionParams1 : packoffset(c43);    // fDynamicResolutionWidthRatio in x,
														  // fDynamicResolutionHeightRatio in y,
														  // fDynamicResolutionPreviousWidthRatio in z,
														  // fDynamicResolutionPreviousHeightRatio in w
	float4 DynamicResolutionParams2 : packoffset(c44);    // inverse fDynamicResolutionWidthRatio in x, inverse
														  // fDynamicResolutionHeightRatio in y,
														  // fDynamicResolutionWidthRatio - fDRClampOffset in z,
														  // fDynamicResolutionPreviousWidthRatio - fDRClampOffset in w
#else
	row_major float4x4 CameraView[2] : packoffset(c0);
	row_major float4x4 CameraProj[2] : packoffset(c8);
	row_major float4x4 CameraViewProj[2] : packoffset(c16);
	row_major float4x4 CameraViewProjUnjittered[2] : packoffset(c24);
	row_major float4x4 CameraPreviousViewProjUnjittered[2] : packoffset(c32);
	row_major float4x4 CameraProjUnjittered[2] : packoffset(c40);
	row_major float4x4 CameraProjUnjitteredInverse[2] : packoffset(c48);
	row_major float4x4 CameraViewInverse[2] : packoffset(c56);
	row_major float4x4 CameraViewProjInverse[2] : packoffset(c64);
	row_major float4x4 CameraProjInverse[2] : packoffset(c72);
	float4 CameraPosAdjust[2] : packoffset(c80);
	float4 CameraPreviousPosAdjust[2] : packoffset(c82);  // fDRClampOffset in w
	float4 FrameParams : packoffset(c84);                 // inverse fGamma in x, some flags in yzw
	float4 DynamicResolutionParams1 : packoffset(c85);    // fDynamicResolutionWidthRatio in x,
														  // fDynamicResolutionHeightRatio in y,
														  // fDynamicResolutionPreviousWidthRatio in z,
														  // fDynamicResolutionPreviousHeightRatio in w
	float4 DynamicResolutionParams2 : packoffset(c86);    // inverse fDynamicResolutionWidthRatio in x, inverse
														  // fDynamicResolutionHeightRatio in y,
														  // fDynamicResolutionWidthRatio - fDRClampOffset in z,
														  // fDynamicResolutionPreviousWidthRatio - fDRClampOffset in w
#endif  // !VR
}

cbuffer UpdateData : register(b1)
{
	float4 CameraData;
	uint Reset;
	float3 CameraPreviousPosAdjust2;
}

float3 WorldToView(float3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	float4 newPosition = float4(x, (float)is_position);
	return mul(CameraView[a_eyeIndex], newPosition).xyz;
}

float2 ViewToUV(float3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	float4 newPosition = float4(x, (float)is_position);
	float4 uv = mul(CameraProj[a_eyeIndex], newPosition);
	return (uv.xy / uv.w) * float2(0.5f, -0.5f) + 0.5f;
}

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

float2 GetDynamicResolutionAdjustedScreenPosition(float2 screenPosition)
{
	float2 adjustedScreenPosition =
		max(0.0.xx, DynamicResolutionParams1.xy * screenPosition);
	return min(float2(DynamicResolutionParams2.z, DynamicResolutionParams1.y),
		adjustedScreenPosition);
}

bool IsSaturated(float value) { return value == saturate(value); }
bool IsSaturated(float2 value) { return IsSaturated(value.x) && IsSaturated(value.y); }

float3 sRGB2Lin(float3 color)
{
	return color > 0.04045 ? pow(color / 1.055 + 0.055 / 1.055, 2.4) : color / 12.92;
}

// Inverse project UV + raw depth into world space.
float3 InverseProjectUVZ(float2 uv, float z)
{
	uv.y = 1 - uv.y;
	float4 cp = float4(float3(uv, z) * 2 - 1, 1);
	float4 vp = mul(CameraViewProjInverse[0], cp);
	return float3(vp.xy, vp.z) / vp.w;
}

[numthreads(32, 32, 1)] void main(uint3 ThreadID
								  : SV_DispatchThreadID) {
	float3 captureDirection = -GetSamplingVector(ThreadID, DynamicCubemap);
	float3 viewDirection = WorldToView(captureDirection, false);
	float2 uv = ViewToUV(viewDirection, false);

	if (Reset) {
		DynamicCubemap[ThreadID] = 0.0;
		DynamicCubemapRaw[ThreadID] = 0.0;
		DynamicCubemapPosition[ThreadID] = 0.0;
		return;
	}

	if (IsSaturated(uv) && viewDirection.z < 0.0) {  // Check that the view direction exists in screenspace and that it is in front of the camera
		uv = GetDynamicResolutionAdjustedScreenPosition(uv);
		uv = ConvertToStereoUV(uv, 0);

		float2 textureDims;
		DepthTexture.GetDimensions(textureDims.x, textureDims.y);

		float depth = DepthTexture[round(uv * textureDims)];
		float linearDepth = GetScreenDepth(depth);

		if (linearDepth > 16.5) {  // First person
			float3 color = ColorTexture[round(uv * textureDims)];
			float4 output = float4(sRGB2Lin(color), 1.0);
			float lerpFactor = 0.1;

			DynamicCubemapRaw[ThreadID] = lerp(DynamicCubemapRaw[ThreadID], output, lerpFactor);
			DynamicCubemap[ThreadID] = lerp(DynamicCubemap[ThreadID], output, lerpFactor);

			float3 position = InverseProjectUVZ(uv, depth);
			DynamicCubemapPosition[ThreadID] = lerp(DynamicCubemapPosition[ThreadID], float4(position * 0.001, 1.0), lerpFactor);
			return;
		}
	}

	float4 position = DynamicCubemapPosition[ThreadID];
	position.xyz = (position.xyz + (CameraPreviousPosAdjust2.xyz * 0.001)) - (CameraPosAdjust[0].xyz * 0.001);  // Remove adjustment, add new adjustment
	DynamicCubemapPosition[ThreadID] = position;

	float4 color = DynamicCubemapRaw[ThreadID];
	color *= lerp(1.0, 0.0, saturate(length(position.xyz)));

	DynamicCubemap[ThreadID] = color;
}
