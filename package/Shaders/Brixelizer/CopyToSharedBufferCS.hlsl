
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

Texture2D<unorm float> DepthTexture11 : register(t0);
Texture2D<unorm float3> NormalRoughnessTexture : register(t1);

RWTexture2D<unorm float> DepthTexture12 : register(u0);
RWTexture2D<unorm float3> Normal : register(u1);

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

	float3 normalGlossiness = NormalRoughnessTexture[dispatchID.xy];
	float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);

	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);

	DepthTexture12[dispatchID.xy] = DepthTexture11[dispatchID.xy];
	Normal[dispatchID.xy] = normalWS * 0.5 + 0.5;
}