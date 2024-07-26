#include "Common/Color.hlsl"
#include "Common/DeferredShared.hlsli"
#include "Common/FrameBuffer.hlsl"
#include "Common/GBuffer.hlsli"
#include "Common/VR.hlsli"

Texture2D<unorm half3> AlbedoTexture : register(t0);
Texture2D<unorm half3> NormalRoughnessTexture : register(t1);

#if defined(SKYLIGHTING)
#	define SL_INCL_STRUCT
#	define SL_INCL_METHODS
#	include "Skylighting/Skylighting.hlsli"

cbuffer SkylightingCB : register(b1)
{
	SkylightingSettings skylightingSettings;
};

Texture2D<unorm float> DepthTexture : register(t2);
Texture3D<sh2> SkylightingProbeArray : register(t3);
#endif

#if defined(SSGI)
Texture2D<half4> SSGITexture : register(t4);
#endif

RWTexture2D<half3> MainRW : register(u0);
#if defined(SSGI)
RWTexture2D<half3> DiffuseAmbientRW : register(u1);
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	half2 uv = half2(dispatchID.xy + 0.5) * BufferDim.zw;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);
	uv = ConvertFromStereoUV(uv, eyeIndex);

	half3 normalGlossiness = NormalRoughnessTexture[dispatchID.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half3 diffuseColor = MainRW[dispatchID.xy];
	half3 albedo = AlbedoTexture[dispatchID.xy];

	half3 normalWS = normalize(mul(CameraViewInverse[eyeIndex], half4(normalVS, 0)).xyz);

	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));

	half3 ambient = directionalAmbientColor;

	diffuseColor = sRGB2Lin(diffuseColor);
	ambient = sRGB2Lin(ambient);
	albedo = sRGB2Lin(albedo);

	half visibility = 1.0;
#if defined(SKYLIGHTING)
	float rawDepth = DepthTexture[dispatchID.xy];
	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, rawDepth, 1);
	float4 positionMS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;
#	if defined(VR)
	if (eyeIndex == 1)
		positionMS.xyz += CameraPosAdjust[1] - CameraPosAdjust[0];
#	endif

	sh2 skylighting = sampleSkylighting(skylightingSettings, SkylightingProbeArray, positionMS.xyz, normalWS);
	half skylightingDiffuse = shHallucinateZH3Irradiance(skylighting, skylightingSettings.DirectionalDiffuse ? normalWS : float3(0, 0, 1));
	skylightingDiffuse = lerp(skylightingSettings.MixParams.x, 1, saturate(skylightingDiffuse * skylightingSettings.MixParams.y));

	visibility = skylightingDiffuse;
#endif

#if defined(SSGI)
	half4 ssgiDiffuse = SSGITexture[dispatchID.xy];
	ssgiDiffuse.rgb *= albedo;

	visibility = min(visibility, ssgiDiffuse.a);

	DiffuseAmbientRW[dispatchID.xy] = albedo * ambient + ssgiDiffuse.rgb;

#	if defined(INTERIOR)
	diffuseColor *= ssgiDiffuse.a;
#	endif
	diffuseColor += ssgiDiffuse.rgb;
#endif

	diffuseColor = Lin2sRGB(diffuseColor);
	ambient = Lin2sRGB(ambient * visibility);
	albedo = Lin2sRGB(albedo);

	diffuseColor += ambient * albedo;

	MainRW[dispatchID.xy] = diffuseColor;
};