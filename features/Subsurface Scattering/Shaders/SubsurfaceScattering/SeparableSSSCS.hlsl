RWTexture2D<float4> SSSRW : register(u0);

Texture2D<float4> ColorTexture : register(t0);
Texture2D<float4> DepthTexture : register(t1);

#if defined(HORIZONTAL)
Texture2D<float4> NormalTexture : register(t2);
#else
RWTexture2D<unorm float4> NormalTexture : register(u1);
#endif

#define SSSS_N_SAMPLES 21

struct DiffusionProfile
{
	float BlurRadius;
	float Thickness;
};

cbuffer PerFrame : register(b0)
{
	float4 Kernels[SSSS_N_SAMPLES + SSSS_N_SAMPLES];
	float4 BaseProfile;
	float4 HumanProfile;
	float4 CameraData;
	float2 BufferDim;
	float2 RcpBufferDim;
	uint FrameCount;
	float SSSS_FOVY;
};

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

float3 sRGB2Lin(float3 color)
{
	return color > 0.04045 ? pow(color / 1.055 + 0.055 / 1.055, 2.4) : color / 12.92;
}

float3 Lin2sRGB(float3 color)
{
	return color > 0.0031308 ? 1.055 * pow(color, 1.0 / 2.4) - 0.055 : 12.92 * color;
}

float InterleavedGradientNoise(float2 uv)
{
	// Temporal factor
	float frameStep = float(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

#include "SeparableSSS.hlsli"

[numthreads(32, 32, 1)] void main(uint3 DTid
								  : SV_DispatchThreadID) {
	float2 texCoord = (DTid.xy + 0.5) * RcpBufferDim;
#if defined(HORIZONTAL)
	float4 normals = NormalTexture[DTid.xy];
	float4 color = SSSSBlurCS(DTid.xy, texCoord, float2(1.0, 0.0), normals);
	SSSRW[DTid.xy] = max(0, color);
#else
	float4 normals = NormalTexture[DTid.xy];
	float4 color = SSSSBlurCS(DTid.xy, texCoord, float2(0.0, 1.0), normals);
	color.rgb = Lin2sRGB(color.rgb);
	SSSRW[DTid.xy] = float4(max(0, color.rgb), 1);
	NormalTexture[DTid.xy] = float4(normals.xy, 0.0, normals.w);
#endif
}
