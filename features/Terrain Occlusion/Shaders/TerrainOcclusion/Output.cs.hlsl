#include "../Common/DeferredShared.hlsli"
#include "../Common/VR.hlsli"

Texture2D<unorm half> TexDepth : register(t0);

#define TERRA_OCC_OUTPUT
#include "TerraOcclusion.hlsli"

RWTexture2D<unorm float> RWTexShadowMask : register(u0);
RWTexture2D<half4> RWTexGI : register(u1);

SamplerState SamplerDefault;

[numthreads(32, 32, 1)] void main(uint2 dtid
								  : SV_DispatchThreadID) {
	float2 uv = (dtid + .5) * RcpBufferDim;
#ifdef VR
	const uint eyeIndex = uv > .5;
#else
	const uint eyeIndex = 0;
#endif

	float3 ndc = float3(ConvertToStereoUV(uv, eyeIndex), 1);
	ndc = ndc * 2 - 1;
	ndc.y = -ndc.y;
	ndc.z = TexDepth[dtid];

	float4 worldPos = mul(InvViewMatrix[eyeIndex], mul(InvProjMatrix[eyeIndex], float4(ndc, 1)));
	worldPos.xyz /= worldPos.w;
	float viewDistance = length(worldPos);
	worldPos.xyz += CamPosAdjust[0].xyz;

	float terrainShadow = 1;
	float terrainAo = 1;

	GetTerrainOcclusion(worldPos, viewDistance, SamplerDefault, terrainShadow, terrainAo);

	half shadow = RWTexShadowMask[dtid];
	RWTexShadowMask[dtid] = min(shadow, terrainShadow);

	float4 gi = RWTexGI[dtid];
	gi.w *= terrainAo;
	RWTexGI[dtid] = gi;
}