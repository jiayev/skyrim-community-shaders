
#include "Common/DeferredShared.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/VR.hlsli"

Texture2D<half3> SpecularTexture : register(t0);
Texture2D<unorm half3> AlbedoTexture : register(t1);
Texture2D<unorm half3> ReflectanceTexture : register(t2);
Texture2D<unorm half3> NormalRoughnessTexture : register(t3);
Texture2D<unorm half> ShadowMaskTexture : register(t4);
Texture2D<unorm half> DepthTexture : register(t5);
Texture2D<unorm half3> MasksTexture : register(t6);
Texture2D<unorm half4> GITexture : register(t7);

RWTexture2D<half4> MainRW : register(u0);
RWTexture2D<half4> NormalTAAMaskSpecularMaskRW : register(u1);

SamplerState LinearSampler : register(s0);

// #	define DEBUG

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

[numthreads(32, 32, 1)] void DirectionalPass(uint3 globalId
											 : SV_DispatchThreadID, uint3 localId
											 : SV_GroupThreadID, uint3 groupId
											 : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	half weight = 1.0;

	half NdotL = dot(normalVS, DirLightDirectionVS[eyeIndex].xyz);

	half3 albedo = AlbedoTexture[globalId.xy];
	half3 masks = MasksTexture[globalId.xy];

	half3 color = MainRW[globalId.xy].rgb;
	color += albedo * lerp(max(0, NdotL), 1.0, masks.z) * DirLightColor.xyz;

	MainRW[globalId.xy] = half4(color.xyz, 1.0);
};

[numthreads(32, 32, 1)] void DirectionalShadowPass(uint3 globalId
												   : SV_DispatchThreadID, uint3 localId
												   : SV_GroupThreadID, uint3 groupId
												   : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	half weight = 1.0;

	half NdotL = dot(normalVS, DirLightDirectionVS[eyeIndex].xyz);
	half shadow = 1.0;

	half3 albedo = AlbedoTexture[globalId.xy];
	half3 masks = MasksTexture[globalId.xy];

	if (NdotL > 0.0 || masks.z > 0.0) {
		shadow = ShadowMaskTexture[globalId.xy];

		if (shadow != 0) {
			// Approximation of PCF in screen-space
			for (int i = -1; i < 1; i++) {
				for (int k = -1; k < 1; k++) {
					if (i == 0 && k == 0)
						continue;
					float2 offset = float2(i, k) * RcpBufferDim.xy * 1.5;
					float sampleDepth = GetScreenDepth(DepthTexture.SampleLevel(LinearSampler, uv + offset, 0));
					float attenuation = 1.0 - saturate(abs(sampleDepth - depth));
					shadow += ShadowMaskTexture.SampleLevel(LinearSampler, uv + offset, 0) * attenuation;
					weight += attenuation;
				}
			}
			shadow /= weight;
		}
	}

	half3 color = MainRW[globalId.xy].rgb;

	color += albedo * lerp(max(0, NdotL), 1.0, masks.z) * DirLightColor.xyz * shadow;

	MainRW[globalId.xy] = half4(color.xyz, 1.0);
};

[numthreads(32, 32, 1)] void AmbientCompositePass(uint3 globalId
												  : SV_DispatchThreadID, uint3 localId
												  : SV_GroupThreadID, uint3 groupId
												  : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half4 diffuseColor = MainRW[globalId.xy];

	half3 normalWS = normalize(mul(InvViewMatrix[eyeIndex], half4(normalVS, 0)));

	half3 albedo = AlbedoTexture[globalId.xy];

	half4 giAo = GITexture[globalId.xy];
	half3 gi = giAo.rgb;
	half ao = giAo.w;

	half3 masks = MasksTexture[globalId.xy];

	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));
	diffuseColor.rgb += albedo * directionalAmbientColor * ao + gi;

	MainRW[globalId.xy] = half4(diffuseColor.xyz, 1.0);
};

[numthreads(32, 32, 1)] void MainCompositePass(uint3 globalId
											   : SV_DispatchThreadID, uint3 localId
											   : SV_GroupThreadID, uint3 groupId
											   : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half3 diffuseColor = MainRW[globalId.xy];
	half3 specularColor = SpecularTexture[globalId.xy];
	half3 albedo = AlbedoTexture[globalId.xy];
	half3 masks = MasksTexture[globalId.xy];

	half3 normalWS = normalize(mul(InvViewMatrix[eyeIndex], half4(normalVS, 0)));

	half glossiness = normalGlossiness.z;
	half3 color = diffuseColor + specularColor;

#if defined(DEBUG)
	half2 texCoord = half2(globalId.xy) / BufferDim.xy;

	if (texCoord.x < 0.5 && texCoord.y < 0.5) {
		color = color;
	} else if (texCoord.x < 0.5) {
		color = albedo;
	} else if (texCoord.y < 0.5) {
		color = normalWS;
	} else {
		color = glossiness;
	}
#endif

	MainRW[globalId.xy] = half4(color.xyz, 1.0);
	NormalTAAMaskSpecularMaskRW[globalId.xy] = half4(EncodeNormalVanilla(normalVS), 0.0, glossiness);
}