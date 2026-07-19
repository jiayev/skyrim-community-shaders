#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "NRD/NRDReblurSH.hlsli"

Texture2D<unorm float3> AlbedoTexture : register(t0);
Texture2D<unorm float3> NormalRoughnessTexture : register(t1);

// 24/32-bit depth: TerrainBlending ON -> R32_FLOAT (no unorm),
// OFF -> R24_UNORM_X8_TYPELESS game depth (unorm).
#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t2);
#else
Texture2D<unorm float> DepthTexture : register(t2);
#endif

Texture2D<float4> SsgiTexture : register(t3);
#if defined(SSGI_SH)
Texture2D<float4> SsgiSH1Texture : register(t4);
#endif

RWTexture2D<float4> MainRW : register(u0);

float3 SampleSSGIIL(uint2 pixCoord, float3 normalWS, float3 viewWS)
{
#if defined(SSGI_SH)
	NRD_SG sg = REBLUR_BackEnd_UnpackSh(SsgiTexture[pixCoord], SsgiSH1Texture[pixCoord]);
	float3 radiance = NRD_SG_ResolveDiffuse(sg, normalWS, viewWS, 1.0);
#else
	float normHitDist;
	float3 radiance;
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SsgiTexture[pixCoord], radiance, normHitDist);
#endif
	return max(0, radiance);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)) || SharedData::ssgiSettings.EnableIL == 0)
		return;

	float depth = DepthTexture[dispatchID.xy];
	if (depth >= 1.0 - 1e-6)
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;

	float3 normalVS = GBuffer::DecodeNormal(NormalRoughnessTexture[dispatchID.xy].xy);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse, float4(normalVS, 0)).xyz);

	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse, positionWS);
	positionWS.xyz /= positionWS.w;

	float3 linAlbedo = Color::IrradianceToLinear(AlbedoTexture[dispatchID.xy] / Color::PBRLightingScale);
	float3 ssgiIl = SampleSSGIIL(dispatchID.xy, normalWS, -normalize(positionWS.xyz));

	float4 mainColor = MainRW[dispatchID.xy];
	float3 linDiffuseColor = Color::IrradianceToLinear(mainColor.xyz);
	linDiffuseColor += ssgiIl * linAlbedo;
	MainRW[dispatchID.xy] = float4(Color::IrradianceToGamma(linDiffuseColor), mainColor.a);
}
