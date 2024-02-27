RWTexture2D<float4> SSSRW : register(u0);

Texture2D<float4> ColorTexture : register(t0);
Texture2D<float4> DepthTexture : register(t1);

#	if defined(HORIZONTAL)
Texture2D<float4> NormalTexture : register(t2);
#	else
RWTexture2D<unorm float4> NormalTexture : register(u1);
#	endif

cbuffer PerFrame : register(b0)
{
    float4 DynamicRes;
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

#include "SeparableSSS.hlsli"

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	float2 texCoord = (DTid.xy + 0.5) * RcpBufferDim;
#	if defined(HORIZONTAL)
		float4 normals = NormalTexture[DTid.xy];
		float4 color = SSSSBlurCS(DTid.xy, texCoord, float2(0.0, 1.0), normals);
	    SSSRW[DTid.xy] = color;
#	else
		float4 normals = NormalTexture[DTid.xy];
	    float4 color = SSSSBlurCS(DTid.xy, texCoord, float2(0.0, 1.0), normals);
		color.rgb = Lin2sRGB(color.rgb);		

		SSSRW[DTid.xy] = color;

		NormalTexture[DTid.xy] = float4(normals.xy, normals.z < 1 ? normals.z : 0.0, normals.w);
#	endif
}
