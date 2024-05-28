
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
Texture2D<unorm half3> Masks2Texture : register(t7);
Texture2D<unorm half4> GITexture : register(t8);

Texture2D<unorm half2> SkylightingTexture : register(t10);

RWTexture2D<half4> MainRW : register(u0);
RWTexture2D<half4> NormalTAAMaskSpecularMaskRW : register(u1);
RWTexture2D<half2> SnowParametersRW : register(u2);

TextureCube<unorm half3> EnvTexture : register(t12);
TextureCube<unorm half3> ReflectionTexture : register(t13);

SamplerState LinearSampler : register(s0);

struct PerGeometry
{
	float4 VPOSOffset;
	float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam;
	float4 DebugColor;
	float4 PropertyColor;
	float4 AlphaTestRef;
	float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
#if !defined(VR)
	float4x3 ShadowMapProj[1][3];
	float4x4 CameraViewProjInverse[1];
#else
	float4x3 ShadowMapProj[2][3];
	float4x4 CameraViewProjInverse[2];
#endif  // VR
};

Texture2DArray<float4> TexShadowMapSampler : register(t10);
StructuredBuffer<PerGeometry> perShadow : register(t11);

// #	define DEBUG

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

[numthreads(8, 8, 1)] void DirectionalPass(uint3 globalId
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

[numthreads(8, 8, 1)] void DirectionalShadowPass(uint3 globalId
												 : SV_DispatchThreadID, uint3 localId
												 : SV_GroupThreadID, uint3 groupId
												 : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	half NdotL = dot(normalVS, DirLightDirectionVS[eyeIndex].xyz);
	half shadow = 1.0;

	half3 albedo = AlbedoTexture[globalId.xy];
	half3 masks = MasksTexture[globalId.xy];

	if (NdotL > 0.0 || masks.z > 0.0) {
		shadow = 0.0;
		half weight = 0.0;

		// Approximation of PCF in screen-space
		for (half i = -1.5; i < 1.5; i++) {
			for (half k = -1.5; k < 1.5; k++) {
				half2 offset = half2(i, k) * RcpBufferDim.xy;
				float sampleDepth = GetScreenDepth(DepthTexture.SampleLevel(LinearSampler, uv + offset, 0));
				half attenuation = 1.0 - saturate(abs(sampleDepth - depth) * 0.1);
				shadow += ShadowMaskTexture.SampleLevel(LinearSampler, uv + offset, 0) * attenuation;
				weight += attenuation;
			}
		}
		if (weight > 0.0) {
			shadow /= weight;
		} else {
			shadow = ShadowMaskTexture[globalId.xy];
		}
	}

	half3 color = MainRW[globalId.xy].rgb;
	color += albedo * lerp(max(0, NdotL), 1.0, masks.z) * DirLightColor.xyz * shadow;

	half3 normalWS = normalize(mul(InvViewMatrix[eyeIndex], half4(normalVS, 0)));
	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));

	float skylighting = SkylightingTexture[globalId.xy];
	skylighting *= saturate(dot(normalWS, float3(0, 0, 1)) * 0.5 + 0.5);
	half3 masks2 = Masks2Texture[globalId.xy];

	color += albedo * directionalAmbientColor * skylighting * (1.0 - masks2.zzz);

	MainRW[globalId.xy] = half4(color, 1.0);
};

float random(float2 uv)
{
	return frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453123);
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

[numthreads(8, 8, 1)] void AmbientCompositePass(uint3 globalId
												: SV_DispatchThreadID, uint3 localId
												: SV_GroupThreadID, uint3 groupId
												: SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);

	half3 diffuseColor = MainRW[globalId.xy];

	half3 albedo = AlbedoTexture[globalId.xy];

	half4 giAo = GITexture[globalId.xy];
	half3 gi = giAo.rgb;
	half ao = giAo.w;

	half3 masks = MasksTexture[globalId.xy];

	half3 normalWS = normalize(mul(InvViewMatrix[eyeIndex], half4(normalVS, 0)));

	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));

	float skylighting = SkylightingTexture[globalId.xy];

	half weight = 1.0;

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	gi *= (rawDepth < 1.0);
	ao = lerp(ao, 1.0, rawDepth == 1.0);

	diffuseColor *= ao;
	diffuseColor += gi;

	skylighting = lerp(skylighting, 1.0, 0.5);

	diffuseColor += albedo * directionalAmbientColor * ao * skylighting * lerp(saturate(dot(normalWS, float3(0, 0, -1)) * 0.5 + 0.5), 1.0, 0.5);

	float3 giao = (skylighting * directionalAmbientColor * ao) + gi;

	MainRW[globalId.xy] = half4(diffuseColor, 1.0);
};

float3 sRGB2Lin(float3 color)
{
	return color > 0.04045 ? pow(color / 1.055 + 0.055 / 1.055, 2.4) : color / 12.92;
}

float3 Lin2sRGB(float3 color)
{
	return color > 0.0031308 ? 1.055 * pow(color, 1.0 / 2.4) - 0.055 : 12.92 * color;
}

[numthreads(8, 8, 1)] void MainCompositePass(uint3 globalId
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
	half3 reflectance = ReflectanceTexture[globalId.xy];

	half3 normalWS = normalize(mul(InvViewMatrix[eyeIndex], half4(normalVS, 0)));

	half glossiness = normalGlossiness.z;
	half3 color = sRGB2Lin(diffuseColor) + sRGB2Lin(specularColor);

	half2 skylighting = SkylightingTexture[globalId.xy];
	half4 giAo = GITexture[globalId.xy];

	half rawDepth = DepthTexture[globalId.xy];

	half4 positionCS = half4(2 * half2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];

	half4 positionMS = mul(InvViewMatrix[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	float3 V = normalize(positionMS.xyz);
	float3 R = reflect(V, normalWS);

	float roughness = 1.0 - glossiness;
	float level = roughness * 9.0;

	float3 specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level).xyz;
	specularIrradiance = sRGB2Lin(specularIrradiance);

	float3 specularIrradiance2 = ReflectionTexture.SampleLevel(LinearSampler, R, level).xyz;
	specularIrradiance2 = sRGB2Lin(specularIrradiance2);

	color += reflectance * lerp(specularIrradiance, specularIrradiance2, skylighting.y * giAo.w) * giAo.w;

	color = Lin2sRGB(color);

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

	half2 snowParamaters = Masks2Texture[globalId.xy];

	MainRW[globalId.xy] = half4(color, 1.0);
	NormalTAAMaskSpecularMaskRW[globalId.xy] = half4(EncodeNormalVanilla(normalVS), 0.0, glossiness);
	SnowParametersRW[globalId.xy] = snowParamaters * lerp(skylighting.x, 1.0, 0.5);
}