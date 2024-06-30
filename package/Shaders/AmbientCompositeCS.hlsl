#include "Common/DeferredShared.hlsli"
#include "Common/FrameBuffer.hlsl"
#include "Common/GBuffer.hlsli"
#include "Common/VR.hlsli"

Texture2D<unorm half3> AlbedoTexture : register(t0);
Texture2D<unorm half3> NormalRoughnessTexture : register(t1);

#if defined(SKYLIGHTING)
Texture2D<half2> SkylightingTexture : register(t2);
#endif

RWTexture2D<half3> MainRW : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	half2 uv = half2(dispatchID.xy + 0.5) * BufferDim.zw;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[dispatchID.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half3 diffuseColor = MainRW[dispatchID.xy];
	half3 albedo = AlbedoTexture[dispatchID.xy];

	half3 normalWS = normalize(mul(CameraViewInverse[eyeIndex], half4(normalVS, 0)));

	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));

#if defined(SKYLIGHTING)
	half skylightingDiffuse = SkylightingTexture[dispatchID.xy].x;
	diffuseColor += albedo * directionalAmbientColor * skylightingDiffuse;
#else
	diffuseColor += albedo * directionalAmbientColor;
#endif

	MainRW[dispatchID.xy] = diffuseColor;
};