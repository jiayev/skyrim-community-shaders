#include "Common/Constants.hlsli"
#include "Common/FrameBuffer.hlsl"
#include "Common/LodLandscape.hlsli"
#include "Common/Skinned.hlsli"
#include "Common/VR.hlsli"

#if defined(RENDER_SHADOWMASK) || defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB) || defined(RENDER_SHADOWMASKDPB)
#	define RENDER_SHADOWMASK_ANY
#endif

struct VS_INPUT
{
	float4 PositionMS : POSITION0;

#if defined(TEXTURE)
	float2 TexCoord : TEXCOORD0;
#endif

#if defined(NORMALS)
	float4 Normal : NORMAL0;
	float4 Bitangent : BINORMAL0;
#endif
#if defined(VC)
	float4 Color : COLOR0;
#endif
#if defined(SKINNED)
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
#endif
#if defined(VR)
	uint InstanceID : SV_INSTANCEID;
#endif  // VR
};

struct VS_OUTPUT
{
	float4 PositionCS : SV_POSITION0;

#if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#	if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 TexCoord0 : TEXCOORD0;
#	endif

#	if defined(RENDER_NORMAL)
	float4 Normal : TEXCOORD1;
#	endif

#	if defined(RENDER_SHADOWMAP_PB)
	float3 TexCoord1 : TEXCOORD2;
#	elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 TexCoord1 : TEXCOORD2;
#	elif defined(ADDITIONAL_ALPHA_MASK)
	float2 TexCoord1 : TEXCOORD2;
#	endif

#	if defined(LOCALMAP_FOGOFWAR)
	float Alpha : TEXCOORD3;
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	float4 PositionMS : TEXCOORD5;
#	endif

#	if defined(ALPHA_TEST) && defined(VC) && defined(RENDER_SHADOWMASK_ANY)
	float2 Alpha : TEXCOORD4;
#	elif (defined(ALPHA_TEST) && defined(VC) && !defined(TREE_ANIM)) || defined(RENDER_SHADOWMASK_ANY)
	float Alpha : TEXCOORD4;
#	endif

#	if defined(DEBUG_SHADOWSPLIT)
	float Depth : TEXCOORD2;
#	endif
#endif
#if defined(VR)
	float ClipDistance : SV_ClipDistance0;  // o11
	float CullDistance : SV_CullDistance0;  // p11
	uint EyeIndex : EYEIDX0;
#endif  // VR
};

#ifdef VSHADER
cbuffer PerTechnique : register(b0)
{
#	if !defined(VR)
	float4 HighDetailRange[1] : packoffset(c0);  // loaded cells center in xy, size in zw
	float2 ParabolaParam : packoffset(c1);       // inverse radius in x, y is 1 for forward hemisphere or -1 for backward hemisphere
#	else
	float4 HighDetailRange[2] : packoffset(c0);  // loaded cells center in xy, size in zw
	float2 ParabolaParam : packoffset(c2);       // inverse radius in x, y is 1 for forward hemisphere or -1 for backward hemisphere
#	endif  // VR
};

cbuffer PerMaterial : register(b1)
{
	float4 TexcoordOffset : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
#	if !defined(VR)
	float4 ShadowFadeParam : packoffset(c0);
	row_major float4x4 World[1] : packoffset(c1);
	float4 EyePos[1] : packoffset(c5);
	float4 WaterParams : packoffset(c6);
	float4 TreeParams : packoffset(c7);
#	else
	float4 ShadowFadeParam : packoffset(c0);
	row_major float4x4 World[2] : packoffset(c1);
	float4 EyePos[2] : packoffset(c9);
	float4 WaterParams : packoffset(c11);
	float4 TreeParams : packoffset(c12);
#	endif  // VR
};

float2 SmoothSaturate(float2 value)
{
	return value * value * (3 - 2 * value);
}

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;

	uint eyeIndex = GetEyeIndexVS(
#	if defined(VR)
		input.InstanceID
#	endif
	);

#	if (defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) || SHADOWFILTER == 2
	vsout.PositionCS.xy = input.PositionMS.xy;
#		if defined(RENDER_SHADOWMASKDPB)
	vsout.PositionCS.z = ShadowFadeParam.z;
#		else
	vsout.PositionCS.z = HighDetailRange[eyeIndex].x;
#		endif
	vsout.PositionCS.w = 1;
#	elif defined(STENCIL_ABOVE_WATER)
	vsout.PositionCS.y = WaterParams.x * 2 + input.PositionMS.y;
	vsout.PositionCS.xzw = input.PositionMS.xzw;
#	else

	precise float4 positionMS = float4(input.PositionMS.xyz, 1.0);
	float4 positionCS = float4(0, 0, 0, 0);

	float3 normalMS = float3(1, 1, 1);
#		if defined(NORMALS)
	normalMS = input.Normal.xyz * 2 - 1;
#		endif

#		if defined(VC) && defined(NORMALS) && defined(TREE_ANIM)
	float2 treeTmp1 = SmoothSaturate(abs(2 * frac(float2(0.1, 0.25) * (TreeParams.w * TreeParams.y * TreeParams.x) + dot(input.PositionMS.xyz, 1.0.xxx) + 0.5) - 1));
	float normalMult = (treeTmp1.x + 0.1 * treeTmp1.y) * (input.Color.w * TreeParams.z);
	positionMS.xyz += normalMS.xyz * normalMult;
#		endif

#		if defined(LOD_LANDSCAPE)
	positionMS = AdjustLodLandscapeVertexPositionMS(positionMS, World[eyeIndex], HighDetailRange[eyeIndex]);
#		endif

#		if defined(SKINNED)
	precise int4 boneIndices = 765.01.xxxx * input.BoneIndices.xyzw;

	float3x4 worldMatrix = GetBoneTransformMatrix(Bones, boneIndices, CameraPosAdjust[eyeIndex].xyz, input.BoneWeights);
	precise float4 positionWS = float4(mul(positionMS, transpose(worldMatrix)), 1);

	positionCS = mul(CameraViewProj[eyeIndex], positionWS);
#		else
	precise float4x4 modelViewProj = mul(CameraViewProj[eyeIndex], World[eyeIndex]);
	positionCS = mul(modelViewProj, positionMS);
#		endif

#		if defined(RENDER_SHADOWMAP) && defined(RENDER_SHADOWMAP_CLAMPED)
	positionCS.z = max(0, positionCS.z);
#		endif

#		if defined(LOD_LANDSCAPE)
	vsout.PositionCS = AdjustLodLandscapeVertexPositionCS(positionCS);
#		elif defined(RENDER_SHADOWMAP_PB)
	float3 positionCSPerspective = positionCS.xyz / positionCS.w;
	float3 shadowDirection = normalize(normalize(positionCSPerspective) + float3(0, 0, ParabolaParam.y));
	vsout.PositionCS.xy = shadowDirection.xy / shadowDirection.z;
	vsout.PositionCS.z = ParabolaParam.x * length(positionCSPerspective);
	vsout.PositionCS.w = positionCS.w;
#		else
	vsout.PositionCS = positionCS;
#		endif

#		if defined(RENDER_NORMAL)
	float3 normalVS = float3(1, 1, 1);
#			if defined(SKINNED)
	float3x3 boneRSMatrix = GetBoneRSMatrix(Bones, boneIndices, input.BoneWeights);
	normalMS = normalize(mul(normalMS, transpose(boneRSMatrix)));
	normalVS = mul(CameraView[eyeIndex], float4(normalMS, 0)).xyz;
#			else
	normalVS = mul(mul(CameraView[eyeIndex], World[eyeIndex]), float4(normalMS, 0)).xyz;
#			endif
#			if defined(RENDER_NORMAL_CLAMP)
	normalVS = max(min(normalVS, 0.1), -0.1);
#			endif
	vsout.Normal.xyz = normalVS;

#			if defined(VC)
	vsout.Normal.w = input.Color.w;
#			else
	vsout.Normal.w = 1;
#			endif

#		endif

#		if (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	float4 texCoord = float4(0, 0, 1, 1);
	texCoord.xy = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;

#			if defined(RENDER_NORMAL)
	texCoord.z = max(1, 0.0013333333 * positionCS.z + 0.8);

	float falloff = 1;
#				if defined(RENDER_NORMAL_FALLOFF)
#					if defined(SKINNED)
	falloff = dot(normalMS, normalize(EyePos[eyeIndex].xyz - positionWS.xyz));
#					else
	falloff = dot(normalMS, normalize(EyePos[eyeIndex].xyz - positionMS.xyz));
#					endif
#				endif
	texCoord.w = EyePos[eyeIndex].w * falloff;
#			endif

	vsout.TexCoord0 = texCoord;
#		endif

#		if defined(RENDER_SHADOWMAP_PB)
	vsout.TexCoord1.x = ParabolaParam.x * length(positionCSPerspective);
	vsout.TexCoord1.y = positionCS.w;
	precise float parabolaParam = ParabolaParam.y * positionCS.z;
	vsout.TexCoord1.z = parabolaParam * 0.5 + 0.5;
#		elif defined(ALPHA_TEST) && (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP))
	float4 texCoord1 = float4(0, 0, 0, 0);
	texCoord1.xy = positionCS.zw;
	texCoord1.zw = input.TexCoord * TexcoordOffset.zw + TexcoordOffset.xy;

	vsout.TexCoord1 = texCoord1;
#		elif defined(ADDITIONAL_ALPHA_MASK)
	vsout.TexCoord1 = positionCS.zw;
#		elif defined(DEBUG_SHADOWSPLIT)
	vsout.Depth = positionCS.z;
#		endif

#		if defined(RENDER_SHADOWMASK_ANY)
	vsout.Alpha.x = 1 - pow(saturate(dot(positionCS.xyz, positionCS.xyz) / ShadowFadeParam.x), 8);

#			if defined(SKINNED)
	vsout.PositionMS.xyz = positionWS.xyz;
#			else
	vsout.PositionMS.xyz = positionMS.xyz;
#			endif
	vsout.PositionMS.w = positionCS.z;
#		endif

#		if (defined(ALPHA_TEST) && defined(VC)) || defined(LOCALMAP_FOGOFWAR)
#			if defined(RENDER_SHADOWMASK_ANY)
	vsout.Alpha.y = input.Color.w;
#			elif !defined(TREE_ANIM)
	vsout.Alpha.x = input.Color.w;
#			endif
#		endif

#	endif

#	if defined(OFFSET_DEPTH)
	if (vsout.PositionCS.z < 4096)
		vsout.PositionCS.z += 5.0;
#	endif

#	ifdef VR
	vsout.EyeIndex = eyeIndex;
	VR_OUTPUT VRout = GetVRVSOutput(vsout.PositionCS, eyeIndex);
	vsout.PositionCS = VRout.VRPosition;
	vsout.ClipDistance.x = VRout.ClipDistance;
	vsout.CullDistance.x = VRout.CullDistance;
#	endif  // VR
	return vsout;
}
#endif

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#ifdef PSHADER

SamplerState SampBaseSampler : register(s0);
SamplerState SampNormalSampler : register(s1);
SamplerState SampDepthSampler : register(s2);
SamplerState SampShadowMapSampler : register(s3);
SamplerComparisonState SampShadowMapSamplerComp : register(s4);
SamplerState SampStencilSampler : register(s5);
SamplerComparisonState SampFocusShadowMapSamplerComp : register(s6);
SamplerState SampGrayscaleSampler : register(s7);

Texture2D<float4> TexBaseSampler : register(t0);
Texture2D<float4> TexNormalSampler : register(t1);
Texture2D<float4> TexDepthSampler : register(t2);
Texture2DArray<float4> TexShadowMapSampler : register(t3);
Texture2DArray<float4> TexShadowMapSamplerComp : register(t4);
Texture2D<uint4> TexStencilSampler : register(t5);
Texture2DArray<float4> TexFocusShadowMapSamplerComp : register(t6);
Texture2D<float4> TexGrayscaleSampler : register(t7);

cbuffer PerTechnique : register(b0)
{
	float4 VPOSOffset : packoffset(c0);
	float4 ShadowSampleParam : packoffset(c1);    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances : packoffset(c2);    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances : packoffset(c3);  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam : packoffset(c4);
}

cbuffer PerMaterial : register(b1)
{
	float RefractionPower : packoffset(c0);
	float4 BaseColor : packoffset(c1);
}

cbuffer PerGeometry : register(b2)
{
	float4 DebugColor : packoffset(c0);
	float4 PropertyColor : packoffset(c1);
	float4 AlphaTestRef : packoffset(c2);
	float4 ShadowLightParam : packoffset(c3);  // Falloff in x, ShadowDistance squared in z
#	if !defined(VR)
	float4x3 FocusShadowMapProj[4] : packoffset(c4);
#		if defined(RENDER_SHADOWMASK)
	float4x3 ShadowMapProj[1][3] : packoffset(c16);  // 16, 19, 22
#		elif defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB) || defined(RENDER_SHADOWMASKDPB)
	float4x4 ShadowMapProj[1][3] : packoffset(c16);
#		endif
#	else
	float4 VRUnknown : packoffset(c4);  // used to multiply by identity matrix, see e.g., 4202499.ps.bin.hlsl
										/*
										r1.x = dot(cb2[4].xz, icb[r0.w+0].xz);
										r1.x = r0.x * cb12[86].x + -r1.x;
										r0.w = (int)r0.w + 1;
										r0.w = (int)r0.w + -1;
										r0.w = dot(cb2[4].yw, icb[r0.w+0].xz);
										*/
	float4x3 FocusShadowMapProj[4] : packoffset(c5);
#		if defined(RENDER_SHADOWMASK)
	float4x3 ShadowMapProj[2][3] : packoffset(c29);  // VR has a couple of offsets of 3, e.g., {29, 32, 35} and {38, 41, 44}, compare to Flat which does [16, 19, 22]
#		elif defined(RENDER_SHADOWMASKSPOT) || defined(RENDER_SHADOWMASKPB) || defined(RENDER_SHADOWMASKDPB)
	float4x4 ShadowMapProj[2][3] : packoffset(c29);
#		endif
#	endif  // VR
}

#	if !defined(VR)
cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#	endif  // !VR

float GetPoissonDiskFilteredShadowVisibility(Texture2DArray<float4> tex, SamplerComparisonState samp, float sampleOffsetShift, float2 baseUv, float layerIndex, float compareValue, bool asymmetric)
{
	const int SampleCount = 8;

	const static float2 PoissonDiskSampleOffsets[] = {
		{ 0.493393, 0.394269 },
		{ 0.798547, 0.885922 },
		{ 0.247322, 0.926450 },
		{ 0.051454, 0.140782 },
		{ 0.831843, 0.009552 },
		{ 0.428632, 0.017151 },
		{ 0.015656, 0.749779 },
		{ 0.758385, 0.496170 },
		{ 0.223487, 0.562151 },
		{ 0.011628, 0.406995 },
		{ 0.241462, 0.304636 },
		{ 0.430311, 0.727226 },
		{ 0.981811, 0.278359 },
		{ 0.407056, 0.500534 },
		{ 0.123478, 0.463546 },
		{ 0.809534, 0.682272 },
		{ 0.675802, 0.653920 },
		{ 0.238014, 0.069338 },
		{ 0.000671, 0.611103 },
		{ 0.621876, 0.499039 },
		{ 0.712882, 0.115299 },
		{ 0.913663, 0.819391 },
		{ 0.295450, 0.809687 },
		{ 0.985015, 0.117801 },
		{ 0.630757, 0.313211 },
		{ 0.362621, 0.185705 },
		{ 0.164464, 0.787591 },
		{ 0.003845, 0.938841 },
		{ 0.522752, 0.146275 },
		{ 0.987518, 0.938994 },
		{ 0.770104, 0.315531 },
		{ 0.044832, 0.268838 },
		{ 0.972320, 0.438551 },
		{ 0.690359, 0.974731 },
		{ 0.582720, 0.833552 },
		{ 0.496780, 0.998993 },
		{ 0.498215, 0.603473 },
		{ 0.916440, 0.590777 },
		{ 0.851131, 0.219520 },
		{ 0.413465, 0.893124 },
		{ 0.004425, 0.015687 },
		{ 0.580889, 0.027406 },
		{ 0.090854, 0.364971 },
		{ 0.907498, 0.387829 },
		{ 0.107364, 0.746513 },
		{ 0.987091, 0.183691 },
		{ 0.304147, 0.542741 },
		{ 0.769127, 0.022675 },
		{ 0.895444, 0.058382 },
		{ 0.709677, 0.203162 },
		{ 0.420392, 0.414716 },
		{ 0.833094, 0.157628 },
		{ 0.290963, 0.195532 },
		{ 0.484420, 0.908933 },
		{ 0.760491, 0.956145 },
		{ 0.037172, 0.551775 },
		{ 0.142003, 0.195105 },
		{ 0.950560, 0.749626 },
		{ 0.364116, 0.790643 },
		{ 0.229011, 0.857936 },
		{ 0.742729, 0.732231 },
		{ 0.712851, 0.378796 },
		{ 0.346324, 0.442183 },
		{ 0.481765, 0.222877 },
		{ 0.673299, 0.566820 },
		{ 0.000641, 0.323008 },
		{ 0.875179, 0.328135 },
		{ 0.390515, 0.324442 },
		{ 0.990417, 0.650136 },
		{ 0.356212, 0.951842 },
		{ 0.432020, 0.796564 },
		{ 0.670186, 0.449019 },
		{ 0.607288, 0.721427 },
		{ 0.137700, 0.531968 },
		{ 0.707694, 0.841395 },
		{ 0.806696, 0.820704 },
		{ 0.643727, 0.101413 },
		{ 0.251747, 0.000122 },
		{ 0.558489, 0.412549 },
		{ 0.507645, 0.006348 },
		{ 0.823817, 0.408032 },
		{ 0.301706, 0.359844 },
		{ 0.300729, 0.626392 },
		{ 0.951323, 0.535203 },
		{ 0.116581, 0.878201 },
		{ 0.367748, 0.046083 },
		{ 0.256172, 0.740318 },
		{ 0.162297, 0.983001 },
		{ 0.532121, 0.497971 },
		{ 0.206732, 0.239875 },
		{ 0.102481, 0.626576 },
		{ 0.346812, 0.688284 },
		{ 0.903531, 0.672231 },
		{ 0.174078, 0.120060 },
		{ 0.317240, 0.285318 },
		{ 0.097934, 0.979614 },
		{ 0.195441, 0.385968 },
		{ 0.113987, 0.300943 },
		{ 0.830744, 0.585620 },
		{ 0.562853, 0.662038 },
		{ 0.373516, 0.114170 },
		{ 0.887936, 0.978851 },
		{ 0.978912, 0.849574 },
		{ 0.502640, 0.068697 },
		{ 0.168676, 0.050172 },
		{ 0.865932, 0.530351 },
		{ 0.923643, 0.164739 },
		{ 0.049654, 0.205237 },
		{ 0.823176, 0.079989 },
		{ 0.024812, 0.686422 },
		{ 0.872066, 0.871883 },
		{ 0.613880, 0.991363 },
		{ 0.096072, 0.094943 },
		{ 0.825892, 0.283456 },
		{ 0.188910, 0.449202 },
		{ 0.625172, 0.902493 },
		{ 0.587634, 0.563860 },
		{ 0.012055, 0.487930 },
		{ 0.326548, 0.891964 },
		{ 0.932279, 0.891201 },
		{ 0.493728, 0.695975 },
		{ 0.656850, 0.780145 },
		{ 0.470565, 0.461470 },
		{ 0.372662, 0.251411 },
		{ 0.873684, 0.452925 },
		{ 0.174749, 0.654347 },
		{ 0.695212, 0.302133 },
		{ 0.095615, 0.813196 },
		{ 0.034150, 0.076601 },
		{ 0.066805, 0.916013 },
		{ 0.234016, 0.628071 },
		{ 0.616596, 0.376446 },
		{ 0.563982, 0.229011 },
		{ 0.010224, 0.865444 },
		{ 0.414991, 0.607349 },
		{ 0.631397, 0.242103 },
		{ 0.071108, 0.002564 },
		{ 0.877804, 0.738487 },
		{ 0.997192, 0.036164 },
		{ 0.436110, 0.114231 },
		{ 0.959197, 0.337962 },
		{ 0.705130, 0.046754 },
		{ 0.180090, 0.324412 },
		{ 0.610218, 0.162175 },
		{ 0.547166, 0.300943 },
		{ 0.185034, 0.918699 },
		{ 0.446608, 0.960387 },
		{ 0.954070, 0.994873 },
		{ 0.334971, 0.166936 },
		{ 0.333384, 0.212806 },
		{ 0.466170, 0.509629 },
		{ 0.363659, 0.343303 },
		{ 0.171911, 0.171087 },
		{ 0.759880, 0.460891 },
		{ 0.291208, 0.983428 },
		{ 0.758629, 0.107456 },
		{ 0.045076, 0.596759 },
		{ 0.902982, 0.507401 },
		{ 0.596515, 0.791589 },
		{ 0.174993, 0.242531 },
		{ 0.468581, 0.553545 },
		{ 0.866451, 0.633686 },
		{ 0.672384, 0.069369 },
		{ 0.239937, 0.185553 },
		{ 0.691885, 0.735557 },
		{ 0.644398, 0.734977 },
		{ 0.419385, 0.558153 },
		{ 0.496017, 0.434584 },
		{ 0.665212, 0.913816 },
		{ 0.277963, 0.765435 },
		{ 0.085482, 0.171514 },
		{ 0.305826, 0.394971 },
		{ 0.708029, 0.574419 },
		{ 0.728782, 0.160558 },
		{ 0.186102, 0.289041 },
		{ 0.677938, 0.143620 },
		{ 0.140324, 0.707785 },
		{ 0.059084, 0.760308 },
		{ 0.610981, 0.451796 },
		{ 0.036256, 0.007630 },
		{ 0.947325, 0.402966 },
		{ 0.195929, 0.682913 },
		{ 0.057710, 0.468093 },
		{ 0.591754, 0.277749 },
		{ 0.733970, 0.621052 },
		{ 0.928220, 0.774071 },
		{ 0.890194, 0.187719 },
		{ 0.740806, 0.080538 },
		{ 0.770898, 0.566759 },
		{ 0.480087, 0.133763 },
		{ 0.339000, 0.016968 },
		{ 0.854793, 0.809992 },
		{ 0.514420, 0.259651 },
		{ 0.136448, 0.623341 },
		{ 0.369060, 0.503616 },
		{ 0.335429, 0.655782 },
		{ 0.545000, 0.610279 },
		{ 0.991882, 0.728324 },
		{ 0.276376, 0.897610 },
		{ 0.628834, 0.948363 },
		{ 0.538102, 0.784417 },
		{ 0.067751, 0.057588 },
		{ 0.097201, 0.035279 },
		{ 0.439436, 0.642598 },
		{ 0.250923, 0.370464 },
		{ 0.452895, 0.174413 },
		{ 0.206427, 0.739555 },
		{ 0.459212, 0.349132 },
		{ 0.864681, 0.020020 },
		{ 0.632069, 0.186712 },
		{ 0.792352, 0.211158 },
		{ 0.091342, 0.401563 },
		{ 0.308298, 0.242561 },
		{ 0.573046, 0.331858 },
		{ 0.503067, 0.641316 },
		{ 0.665059, 0.876003 },
		{ 0.258187, 0.548265 },
		{ 0.873135, 0.699484 },
		{ 0.342296, 0.110782 },
		{ 0.269173, 0.218574 },
		{ 0.074465, 0.548357 },
		{ 0.351573, 0.403607 },
		{ 0.660298, 0.341380 },
		{ 0.862850, 0.099155 },
		{ 0.208136, 0.952818 },
		{ 0.193701, 0.029023 },
		{ 0.408826, 0.153020 },
		{ 0.710440, 0.796258 },
		{ 0.782678, 0.736534 },
		{ 0.830714, 0.772332 },
		{ 0.494888, 0.497055 },
		{ 0.714866, 0.698050 },
		{ 0.931791, 0.199164 },
		{ 0.637837, 0.808985 },
		{ 0.665700, 0.599597 },
		{ 0.753105, 0.672658 },
		{ 0.593707, 0.412366 },
		{ 0.228889, 0.489761 },
		{ 0.559099, 0.108554 },
		{ 0.674917, 0.394726 },
		{ 0.710440, 0.467666 },
		{ 0.105411, 0.228523 },
		{ 0.517258, 0.108036 },
		{ 0.980590, 0.546953 },
		{ 0.394665, 0.804224 },
		{ 0.867000, 0.162847 },
		{ 0.822565, 0.640614 },
		{ 0.802850, 0.257210 },
		{ 0.441450, 0.221259 },
		{ 0.959319, 0.707572 },
		{ 0.628254, 0.685202 },
		{ 0.132237, 0.245064 },
		{ 0.041200, 0.870968 },
		{ 0.826044, 0.497482 },
		{ 0.246254, 0.673513 },
		{ 0.025544, 0.437849 },
		{ 0.238136, 0.222419 },
		{ 0.168432, 0.887051 },
		{ 0.274239, 0.336375 },
		{ 0.350566, 0.827570 },
		{ 0.202063, 0.510300 },
		{ 0.313425, 0.922208 },
		{ 0.050264, 0.709799 },
		{ 0.369396, 0.584307 },
		{ 0.077639, 0.441420 },
		{ 0.402142, 0.709250 },
		{ 0.223914, 0.787591 },
		{ 0.997986, 0.905545 },
		{ 0.492080, 0.768212 },
		{ 0.464400, 0.046205 },
		{ 0.512070, 0.944639 },
		{ 0.635639, 0.648274 },
		{ 0.878689, 0.250649 },
		{ 0.275338, 0.132237 },
		{ 0.899655, 0.786828 },
		{ 0.305826, 0.728813 },
		{ 0.981323, 0.809107 },
		{ 0.323313, 0.790887 },
		{ 0.543931, 0.382153 },
		{ 0.218726, 0.656484 },
		{ 0.593158, 0.630024 },
		{ 0.148289, 0.291757 },
		{ 0.924284, 0.038453 },
		{ 0.515458, 0.330790 },
		{ 0.161443, 0.087741 },
		{ 0.922300, 0.551134 },
		{ 0.076693, 0.326762 },
		{ 0.183874, 0.589160 },
		{ 0.438704, 0.387707 },
		{ 0.807672, 0.607990 },
		{ 0.933439, 0.949278 },
		{ 0.745628, 0.283700 },
		{ 0.846095, 0.906888 },
		{ 0.860775, 0.067690 },
		{ 0.585406, 0.138218 },
		{ 0.869564, 0.932554 },
		{ 0.761864, 0.405621 },
		{ 0.313517, 0.679189 },
		{ 0.533555, 0.051424 },
		{ 0.206854, 0.118076 },
		{ 0.385022, 0.753960 },
		{ 0.406507, 0.987213 },
		{ 0.793908, 0.097232 },
		{ 0.539293, 0.993133 },
		{ 0.359355, 0.727134 },
		{ 0.917508, 0.336222 },
		{ 0.038362, 0.934446 },
		{ 0.803247, 0.942930 },
		{ 0.098911, 0.513077 },
		{ 0.635151, 0.026551 },
		{ 0.022126, 0.180822 },
		{ 0.468795, 0.094516 },
		{ 0.614032, 0.585864 },
		{ 0.470229, 0.314646 },
		{ 0.707053, 0.430525 },
		{ 0.033662, 0.791345 },
		{ 0.449355, 0.898190 },
		{ 0.876736, 0.282205 },
		{ 0.172796, 0.538408 },
		{ 0.258003, 0.476577 },
		{ 0.589251, 0.881191 },
		{ 0.343333, 0.541551 },
		{ 0.250710, 0.414838 },
		{ 0.272286, 0.839778 },
		{ 0.555834, 0.954711 },
		{ 0.013550, 0.654073 },
		{ 0.335215, 0.605396 },
		{ 0.109043, 0.564043 },
		{ 0.124210, 0.167272 },
		{ 0.390393, 0.948820 },
		{ 0.811823, 0.035310 },
		{ 0.665914, 0.192175 },
		{ 0.786615, 0.528886 },
		{ 0.838008, 0.994110 },
		{ 0.903409, 0.440809 },
		{ 0.818995, 0.733634 },
		{ 0.526902, 0.888485 },
		{ 0.915342, 0.630024 },
		{ 0.807154, 0.439070 },
		{ 0.958556, 0.153081 },
		{ 0.055086, 0.828028 },
		{ 0.551622, 0.004120 },
		{ 0.618946, 0.839534 },
		{ 0.462294, 0.783990 },
		{ 0.826228, 0.124424 },
		{ 0.927702, 0.093295 },
		{ 0.553758, 0.267678 },
		{ 0.037446, 0.352184 },
		{ 0.380169, 0.544328 },
		{ 0.846034, 0.671957 },
		{ 0.760002, 0.228309 },
		{ 0.457747, 0.930570 },
		{ 0.839839, 0.436598 },
		{ 0.259163, 0.802271 },
		{ 0.954344, 0.924192 },
		{ 0.739128, 0.037904 },
		{ 0.195593, 0.832575 },
		{ 0.333659, 0.856227 },
		{ 0.572771, 0.456954 },
		{ 0.259102, 0.036317 },
		{ 0.979492, 0.603259 },
		{ 0.261574, 0.960295 },
		{ 0.663778, 0.524461 },
		{ 0.701010, 0.882778 },
		{ 0.181707, 0.760033 },
		{ 0.114811, 0.917600 },
		{ 0.910886, 0.251411 },
		{ 0.437178, 0.764916 },
		{ 0.148747, 0.577258 },
		{ 0.041383, 0.308023 },
		{ 0.064211, 0.659017 },
		{ 0.065920, 0.984985 },
		{ 0.966155, 0.022553 },
		{ 0.221259, 0.428205 },
		{ 0.942259, 0.298471 },
		{ 0.566363, 0.526109 },
		{ 0.417615, 0.059603 },
		{ 0.029572, 0.982360 },
		{ 0.766320, 0.911466 },
		{ 0.000122, 0.902341 },
		{ 0.697806, 0.526231 },
		{ 0.598804, 0.533586 },
		{ 0.278085, 0.572466 },
		{ 0.593860, 0.691305 },
		{ 0.394940, 0.188635 },
		{ 0.222297, 0.705710 },
		{ 0.798578, 0.993896 },
		{ 0.480667, 0.818751 },
		{ 0.123722, 0.367077 },
		{ 0.710044, 0.648671 },
		{ 0.943052, 0.264138 },
		{ 0.760826, 0.603351 },
		{ 0.653005, 0.278542 },
		{ 0.525803, 0.409986 },
		{ 0.133427, 0.677175 },
		{ 0.790246, 0.289163 },
		{ 0.256630, 0.704428 },
		{ 0.589129, 0.077334 },
		{ 0.981109, 0.231422 },
		{ 0.208319, 0.897427 },
		{ 0.568438, 0.747307 },
		{ 0.130406, 0.048402 },
		{ 0.523850, 0.846065 },
		{ 0.549486, 0.695883 },
		{ 0.376965, 0.422010 },
		{ 0.410871, 0.662252 },
		{ 0.293588, 0.055849 },
		{ 0.488205, 0.965453 },
		{ 0.726463, 0.953734 },
		{ 0.983764, 0.070681 },
		{ 0.799066, 0.148076 },
		{ 0.692679, 0.242744 },
		{ 0.810755, 0.370586 },
		{ 0.761345, 0.773553 },
		{ 0.584399, 0.197119 },
		{ 0.382244, 0.649586 },
		{ 0.536119, 0.562609 },
		{ 0.106937, 0.780877 },
		{ 0.047212, 0.502060 },
		{ 0.767052, 0.259743 },
		{ 0.219031, 0.269570 },
		{ 0.999908, 0.761010 },
		{ 0.740135, 0.005493 },
		{ 0.911496, 0.000702 },
		{ 0.005493, 0.113590 },
		{ 0.746940, 0.536424 },
		{ 0.830988, 0.858119 },
		{ 0.940001, 0.466842 },
		{ 0.464156, 0.742759 },
		{ 0.305216, 0.116062 },
		{ 0.265206, 0.625690 },
		{ 0.350414, 0.757683 },
		{ 0.526353, 0.220649 },
		{ 0.432020, 0.835536 },
		{ 0.459395, 0.679708 },
		{ 0.751244, 0.825312 },
		{ 0.988922, 0.369793 },
		{ 0.554765, 0.922208 },
		{ 0.138188, 0.335002 },
		{ 0.063692, 0.625874 },
		{ 0.729637, 0.996338 },
		{ 0.876949, 0.573473 },
		{ 0.300638, 0.152409 },
		{ 0.414106, 0.278359 },
		{ 0.404706, 0.443525 },
		{ 0.592639, 0.965606 },
		{ 0.165929, 0.398114 },
		{ 0.407575, 0.382946 },
		{ 0.141026, 0.939268 },
		{ 0.310709, 0.447249 },
		{ 0.950591, 0.820704 },
		{ 0.185247, 0.620716 },
		{ 0.421674, 0.469985 },
		{ 0.995727, 0.460372 },
		{ 0.635975, 0.060243 },
		{ 0.667562, 0.703818 },
		{ 0.992676, 0.509690 },
		{ 0.226997, 0.826533 },
		{ 0.089236, 0.274056 },
		{ 0.122257, 0.427473 },
		{ 0.008759, 0.216468 },
		{ 0.969146, 0.964812 },
		{ 0.527390, 0.744041 },
		{ 0.376446, 0.462325 },
		{ 0.266396, 0.273721 },
		{ 0.724723, 0.504654 },
		{ 0.330180, 0.056947 },
		{ 0.102756, 0.708152 },
		{ 0.203894, 0.077975 },
		{ 0.272744, 0.098819 },
		{ 0.838313, 0.547685 },
		{ 0.136418, 0.002106 },
		{ 0.144749, 0.857418 },
		{ 0.366314, 0.300699 },
		{ 0.869045, 0.369304 },
		{ 0.337107, 0.362285 },
		{ 0.648000, 0.992248 },
		{ 0.157323, 0.462966 },
		{ 0.257912, 0.516190 },
		{ 0.756401, 0.363323 },
		{ 0.079257, 0.863277 },
		{ 0.497085, 0.872829 },
		{ 0.733421, 0.248238 },
		{ 0.838557, 0.336741 },
		{ 0.701865, 0.612568 },
		{ 0.891507, 0.135166 },
		{ 0.929167, 0.859157 },
		{ 0.163854, 0.823298 },
		{ 0.295785, 0.868709 },
		{ 0.143651, 0.138249 },
		{ 0.687368, 0.000153 },
		{ 0.192724, 0.198370 },
		{ 0.476119, 0.005982 },
		{ 0.002960, 0.053652 },
		{ 0.375958, 0.876553 },
		{ 0.489425, 0.178533 },
		{ 0.449019, 0.275124 },
		{ 0.059206, 0.106876 },
		{ 0.216712, 0.338511 },
		{ 0.430891, 0.331309 },
		{ 0.396985, 0.018189 },
		{ 0.786920, 0.349437 },
		{ 0.462966, 0.593738 },
		{ 0.320719, 0.484634 },
		{ 0.535874, 0.182348 },
		{ 0.557115, 0.075289 },
		{ 0.352245, 0.918882 },
		{ 0.533219, 0.530320 },
		{ 0.004028, 0.535691 },
		{ 0.105899, 0.141392 },
		{ 0.387829, 0.839930 },
		{ 0.910215, 0.737083 },
		{ 0.926664, 0.979583 },
		{ 0.608081, 0.812220 },
		{ 0.964965, 0.276315 },
		{ 0.142888, 0.448897 },
		{ 0.778283, 0.929777 },
		{ 0.866573, 0.979675 },
		{ 0.621143, 0.036714 },
		{ 0.148350, 0.321085 },
		{ 0.397626, 0.525315 },
		{ 0.090060, 0.532243 },
		{ 0.518265, 0.557848 },
		{ 0.917234, 0.647511 },
		{ 0.432173, 0.568926 },
		{ 0.203558, 0.285257 },
		{ 0.724021, 0.097598 },
		{ 0.739830, 0.652974 },
		{ 0.026338, 0.463179 },
		{ 0.344890, 0.582018 },
		{ 0.616565, 0.259987 },
		{ 0.898984, 0.297555 },
		{ 0.628895, 0.873867 },
		{ 0.264229, 0.773553 },
		{ 0.270211, 0.056642 },
		{ 0.251595, 0.260659 },
		{ 0.410901, 0.352153 },
		{ 0.266579, 0.881191 },
		{ 0.800623, 0.576525 },
		{ 0.044221, 0.971587 },
		{ 0.753655, 0.796167 },
		{ 0.981567, 0.663778 },
		{ 0.614215, 0.195776 },
		{ 0.974517, 0.465957 },
		{ 0.725944, 0.772729 },
		{ 0.109409, 0.184362 },
		{ 0.393506, 0.967254 },
		{ 0.630879, 0.158727 },
		{ 0.078036, 0.711447 },
		{ 0.281655, 0.355815 },
		{ 0.676260, 0.126865 },
		{ 0.525742, 0.029328 },
		{ 0.666494, 0.379192 },
		{ 0.096255, 0.385571 },
		{ 0.905515, 0.986328 },
		{ 0.794031, 0.769677 },
		{ 0.593982, 0.664602 },
		{ 0.242378, 0.730735 },
		{ 0.342418, 0.787835 },
		{ 0.272805, 0.972472 },
		{ 0.784173, 0.975738 },
		{ 0.083224, 0.750969 },
		{ 0.153935, 0.902554 },
		{ 0.586993, 0.298685 },
		{ 0.650838, 0.465407 },
		{ 0.930784, 0.414533 },
		{ 0.048646, 0.158727 },
		{ 0.234840, 0.978545 },
		{ 0.929014, 0.012238 },
		{ 0.807459, 0.338267 },
		{ 0.279092, 0.462294 },
		{ 0.897885, 0.320536 },
		{ 0.162297, 0.507675 },
		{ 0.024140, 0.854091 },
		{ 0.323222, 0.640492 },
		{ 0.774529, 0.444960 },
		{ 0.583361, 0.587817 },
		{ 0.230201, 0.528245 },
		{ 0.717795, 0.528428 },
		{ 0.895779, 0.917417 },
		{ 0.053316, 0.087863 },
		{ 0.632710, 0.261635 },
		{ 0.740593, 0.101321 },
		{ 0.726890, 0.815638 },
		{ 0.493576, 0.739860 },
		{ 0.191107, 0.256233 },
		{ 0.709769, 0.334605 },
		{ 0.304636, 0.473830 },
		{ 0.169988, 0.844569 },
		{ 0.162206, 0.138371 },
		{ 0.637562, 0.527390 },
		{ 0.968688, 0.522263 },
		{ 0.643757, 0.295144 },
		{ 0.876278, 0.063784 },
		{ 0.849086, 0.630696 },
		{ 0.315256, 0.319742 },
		{ 0.639912, 0.001648 },
		{ 0.738914, 0.853816 },
		{ 0.834651, 0.695364 },
		{ 0.218421, 0.585925 },
		{ 0.203131, 0.319956 },
		{ 0.260506, 0.935057 },
		{ 0.503220, 0.224097 },
		{ 0.980651, 0.686911 },
		{ 0.044923, 0.291421 },
		{ 0.631855, 0.457564 },
		{ 0.626881, 0.783715 },
		{ 0.419599, 0.699362 },
		{ 0.040071, 0.055971 },
		{ 0.030519, 0.951201 },
		{ 0.928678, 0.929502 },
		{ 0.827204, 0.747765 },
		{ 0.111057, 0.018342 },
		{ 0.533738, 0.962615 },
		{ 0.906827, 0.610279 },
		{ 0.582293, 0.250740 },
		{ 0.057466, 0.805597 },
		{ 0.711356, 0.309549 },
		{ 0.340831, 0.988342 },
		{ 0.865688, 0.190863 },
		{ 0.281075, 0.854976 },
		{ 0.673696, 0.684225 },
		{ 0.485122, 0.850215 },
		{ 0.126835, 0.116977 },
		{ 0.382397, 0.772485 },
		{ 0.140782, 0.465651 },
		{ 0.399640, 0.734916 },
		{ 0.925962, 0.693472 },
		{ 0.003296, 0.452498 },
		{ 0.018677, 0.052797 },
		{ 0.315043, 0.617206 },
		{ 0.968352, 0.295724 },
		{ 0.140446, 0.214606 },
		{ 0.824458, 0.911588 },
		{ 0.670156, 0.263558 },
		{ 0.940886, 0.073824 },
		{ 0.560991, 0.855434 },
		{ 0.908811, 0.405316 },
		{ 0.043184, 0.030610 },
		{ 0.886563, 0.546739 },
		{ 0.560259, 0.393536 },
		{ 0.786584, 0.695700 },
		{ 0.981323, 0.310892 },
		{ 0.741691, 0.807001 },
		{ 0.792749, 0.955565 },
		{ 0.037965, 0.109134 },
		{ 0.542772, 0.326609 },
		{ 0.256813, 0.229926 },
		{ 0.529069, 0.125370 },
		{ 0.157231, 0.219245 },
		{ 0.511368, 0.680380 },
		{ 0.881954, 0.082369 },
		{ 0.778802, 0.331614 },
		{ 0.893155, 0.237068 },
		{ 0.998657, 0.253914 },
		{ 0.554003, 0.355266 },
		{ 0.344768, 0.322794 },
		{ 0.062502, 0.523698 },
		{ 0.486343, 0.799615 },
		{ 0.096927, 0.886990 },
		{ 0.100772, 0.348186 },
		{ 0.308573, 0.374401 },
		{ 0.118473, 0.693503 },
		{ 0.274148, 0.178106 },
		{ 0.101016, 0.604480 },
		{ 0.447707, 0.811640 },
		{ 0.660939, 0.843959 },
		{ 0.814875, 0.705985 },
		{ 0.195318, 0.060243 },
		{ 0.170324, 0.352580 },
		{ 0.145940, 0.264046 },
		{ 0.651479, 0.897610 },
		{ 0.512101, 0.045259 },
		{ 0.942289, 0.645466 },
		{ 0.837123, 0.566057 },
		{ 0.693319, 0.709952 },
		{ 0.223182, 0.298776 },
		{ 0.776666, 0.991424 },
		{ 0.646290, 0.878719 },
		{ 0.013794, 0.067965 },
		{ 0.575549, 0.890713 },
		{ 0.682913, 0.596149 },
		{ 0.677908, 0.176458 },
		{ 0.383862, 0.204138 },
		{ 0.039155, 0.653859 },
		{ 0.722098, 0.929624 },
		{ 0.405560, 0.644368 },
		{ 0.646992, 0.756157 },
		{ 0.916593, 0.110935 },
		{ 0.779870, 0.586291 },
		{ 0.578875, 0.703970 },
		{ 0.162206, 0.686422 },
		{ 0.545366, 0.593524 },
		{ 0.600818, 0.981628 },
		{ 0.491806, 0.328043 },
		{ 0.334422, 0.036592 },
		{ 0.437483, 0.464339 },
		{ 0.111423, 0.259926 },
		{ 0.816157, 0.555986 },
		{ 0.565630, 0.576373 },
		{ 0.459578, 0.611927 },
		{ 0.717063, 0.068697 },
		{ 0.824152, 0.528916 },
		{ 0.091922, 0.241768 },
		{ 0.077090, 0.377941 },
		{ 0.816218, 0.966430 },
		{ 0.078372, 0.592273 },
		{ 0.108097, 0.064913 },
		{ 0.664205, 0.952849 },
		{ 0.688955, 0.580676 },
		{ 0.067354, 0.272530 },
		{ 0.023866, 0.733177 },
		{ 0.841487, 0.949278 },
		{ 0.958342, 0.499130 },
		{ 0.295297, 0.907590 },
		{ 0.426069, 0.655141 },
		{ 0.374493, 0.803705 },
		{ 0.452437, 0.726310 },
		{ 0.770684, 0.692251 },
		{ 0.240913, 0.562304 },
		{ 0.837489, 0.141789 },
		{ 0.947295, 0.668355 },
		{ 0.653188, 0.502762 },
		{ 0.799341, 0.490768 },
		{ 0.044374, 0.901883 },
		{ 0.232795, 0.084109 },
		{ 0.039277, 0.244819 },
		{ 0.673574, 0.050630 },
		{ 0.428938, 0.625904 },
		{ 0.850215, 0.028046 },
		{ 0.139348, 0.973601 },
		{ 0.709738, 0.138676 },
		{ 0.710501, 0.244667 },
		{ 0.806207, 0.457747 },
		{ 0.352672, 0.480117 },
		{ 0.019013, 0.577441 },
		{ 0.306345, 0.709952 },
		{ 0.613300, 0.860805 },
		{ 0.870693, 0.671255 },
		{ 0.193884, 0.108249 },
		{ 0.863796, 0.118809 },
		{ 0.612873, 0.651936 },
		{ 0.740776, 0.434431 },
		{ 0.774773, 0.891110 },
		{ 0.973418, 0.872066 },
		{ 0.296670, 0.075045 },
		{ 0.677541, 0.766472 },
		{ 0.727409, 0.737327 },
		{ 0.231330, 0.113468 },
		{ 0.865566, 0.513749 },
		{ 0.027833, 0.392315 },
		{ 0.676351, 0.086520 },
		{ 0.081973, 0.124485 },
		{ 0.766045, 0.939970 },
		{ 0.413587, 0.813929 },
		{ 0.384625, 0.919187 },
		{ 0.914396, 0.853694 },
		{ 0.833308, 0.888699 },
		{ 0.503464, 0.533647 },
		{ 0.948820, 0.042115 },
		{ 0.889920, 0.516709 },
		{ 0.029145, 0.199164 },
		{ 0.696127, 0.953795 },
		{ 0.680929, 0.841365 },
		{ 0.475967, 0.423627 },
		{ 0.882504, 0.403119 },
		{ 0.487197, 0.622395 },
		{ 0.291543, 0.283273 },
		{ 0.417707, 0.873592 },
		{ 0.569781, 0.559954 },
		{ 0.709037, 0.819941 },
		{ 0.945189, 0.779107 },
		{ 0.206641, 0.559832 },
		{ 0.154515, 0.800501 },
		{ 0.761559, 0.583667 },
		{ 0.843623, 0.515061 },
		{ 0.906461, 0.368480 },
		{ 0.763756, 0.173132 },
		{ 0.339702, 0.240455 },
		{ 0.973571, 0.129124 },
		{ 0.591479, 0.325022 },
		{ 0.714438, 0.222907 },
		{ 0.281564, 0.014985 },
		{ 0.375866, 0.157384 },
		{ 0.816950, 0.065004 },
		{ 0.592730, 0.043763 },
		{ 0.333232, 0.962218 },
		{ 0.809503, 0.112583 },
		{ 0.892697, 0.869930 },
		{ 0.275399, 0.683096 },
		{ 0.687643, 0.778405 },
		{ 0.047426, 0.685568 },
		{ 0.838679, 0.796716 },
		{ 0.003296, 0.960204 },
		{ 0.474776, 0.701254 },
		{ 0.698996, 0.407636 },
		{ 0.467055, 0.952391 },
		{ 0.009064, 0.784478 },
		{ 0.790155, 0.842616 },
		{ 0.742668, 0.926084 },
		{ 0.953948, 0.087863 },
		{ 0.784112, 0.386578 },
		{ 0.467910, 0.636006 },
		{ 0.277505, 0.746330 },
		{ 0.996277, 0.017731 },
		{ 0.693625, 0.015412 },
		{ 0.161809, 0.277139 },
		{ 0.748070, 0.571795 },
		{ 0.380322, 0.309305 },
		{ 0.263192, 0.072237 },
		{ 0.123417, 0.736503 },
		{ 0.294565, 0.095950 },
		{ 0.400586, 0.262856 },
		{ 0.744652, 0.020508 },
		{ 0.236061, 0.433851 },
		{ 0.506699, 0.356182 },
		{ 0.763390, 0.748802 },
		{ 0.187170, 0.531571 },
		{ 0.528336, 0.651662 },
		{ 0.875362, 0.896054 },
		{ 0.303446, 0.963256 },
		{ 0.286264, 0.613849 },
		{ 0.418104, 0.305338 },
		{ 0.644917, 0.386822 },
		{ 0.471999, 0.253975 },
		{ 0.962889, 0.581683 },
		{ 0.438795, 0.184179 },
		{ 0.650746, 0.633198 },
		{ 0.893033, 0.038545 },
		{ 0.853206, 0.003906 },
		{ 0.392743, 0.160588 },
		{ 0.200537, 0.406568 },
		{ 0.384167, 0.616199 },
		{ 0.676351, 0.105930 },
		{ 0.061190, 0.737632 },
		{ 0.523881, 0.634205 },
		{ 0.074862, 0.880245 },
		{ 0.279519, 0.636982 },
		{ 0.977569, 0.564104 },
		{ 0.657979, 0.724143 },
		{ 0.587603, 0.432844 },
		{ 0.153539, 0.428449 },
		{ 0.544694, 0.400006 },
		{ 0.775689, 0.483352 },
		{ 0.220740, 0.041963 },
		{ 0.084658, 0.291299 },
		{ 0.236427, 0.712485 },
		{ 0.002991, 0.349590 },
		{ 0.907407, 0.708029 },
		{ 0.865780, 0.761803 },
		{ 0.764824, 0.808618 },
		{ 0.328318, 0.346294 },
		{ 0.386883, 0.096194 },
		{ 0.635487, 0.511307 },
		{ 0.210852, 0.533403 },
		{ 0.895169, 0.944121 },
		{ 0.905240, 0.470595 },
		{ 0.244362, 0.999634 },
		{ 0.653218, 0.062136 },
		{ 0.182501, 0.144658 },
		{ 0.940245, 0.113926 },
		{ 0.835231, 0.833613 },
		{ 0.731864, 0.890011 },
		{ 0.468276, 0.851009 },
		{ 0.725944, 0.050417 },
		{ 0.030000, 0.513871 },
		{ 0.642689, 0.846675 },
		{ 0.115665, 0.636769 },
		{ 0.860012, 0.317698 },
		{ 0.651295, 0.150121 },
		{ 0.146672, 0.483291 },
		{ 0.002594, 0.080691 },
		{ 0.274270, 0.947356 },
		{ 0.856563, 0.884457 },
		{ 0.615345, 0.525895 },
		{ 0.616169, 0.065035 },
		{ 0.044649, 0.754021 },
		{ 0.621570, 0.610614 },
		{ 0.909909, 0.128025 },
		{ 0.385815, 0.710044 },
		{ 0.960814, 0.843471 },
		{ 0.258614, 0.174017 },
		{ 0.329539, 0.087741 },
		{ 0.362072, 0.010987 },
		{ 0.577105, 0.794458 },
		{ 0.924894, 0.616077 },
		{ 0.351482, 0.871487 },
		{ 0.145207, 0.087130 },
		{ 0.263710, 0.918088 },
		{ 0.930418, 0.217902 },
		{ 0.038392, 0.840999 },
		{ 0.363842, 0.965911 },
		{ 0.711875, 0.909482 },
		{ 0.546220, 0.903623 },
		{ 0.772271, 0.548479 },
		{ 0.068636, 0.239692 },
		{ 0.510727, 0.662648 },
		{ 0.822840, 0.200873 },
		{ 0.847102, 0.728782 },
		{ 0.803949, 0.309091 },
		{ 0.165929, 0.999207 },
		{ 0.017029, 0.258339 },
		{ 0.144047, 0.691153 },
		{ 0.306345, 0.882595 },
		{ 0.285745, 0.823756 },
		{ 0.900418, 0.215857 },
		{ 0.790826, 0.652181 },
		{ 0.542619, 0.093509 },
		{ 0.987152, 0.404187 },
		{ 0.586779, 0.546159 },
		{ 0.001221, 0.148381 },
		{ 0.388684, 0.041261 },
		{ 0.303323, 0.654195 },
		{ 0.229987, 0.413099 },
		{ 0.089267, 0.055513 },
		{ 0.050783, 0.441877 },
		{ 0.165960, 0.925779 },
		{ 0.369152, 0.439344 },
		{ 0.994201, 0.842128 },
		{ 0.601642, 0.237831 },
		{ 0.259865, 0.319346 },
		{ 0.311930, 0.094302 },
		{ 0.850368, 0.584216 },
		{ 0.444411, 0.943205 },
		{ 0.841090, 0.613819 },
		{ 0.752647, 0.868892 },
		{ 0.842311, 0.476882 },
		{ 0.998779, 0.556658 },
		{ 0.728843, 0.299997 },
		{ 0.419080, 0.511399 },
		{ 0.006531, 0.190100 },
		{ 0.777184, 0.865688 },
		{ 0.552416, 0.808313 },
		{ 0.915708, 0.267495 },
		{ 0.238685, 0.957274 },
		{ 0.177679, 0.673330 },
		{ 0.420118, 0.755242 },
		{ 0.690115, 0.375103 },
		{ 0.646199, 0.485824 },
		{ 0.147954, 0.874203 },
		{ 0.453780, 0.558763 },
		{ 0.876095, 0.776513 },
		{ 0.097812, 0.453566 },
		{ 0.288797, 0.782952 },
		{ 0.518113, 0.927244 },
		{ 0.534715, 0.804621 },
		{ 0.611652, 0.132572 },
		{ 0.395886, 0.367779 },
		{ 0.074587, 0.781732 },
		{ 0.097903, 0.731346 },
		{ 0.478469, 0.659139 },
		{ 0.746086, 0.178991 },
		{ 0.882809, 0.659932 },
		{ 0.502335, 0.552293 },
		{ 0.618427, 0.745140 },
		{ 0.033296, 0.215278 },
		{ 0.820429, 0.939238 },
		{ 0.218726, 0.918638 },
		{ 0.965300, 0.902982 },
		{ 0.412519, 0.327219 },
		{ 0.575793, 0.116550 },
		{ 0.498398, 0.813318 },
		{ 0.800562, 0.804651 },
		{ 0.014649, 0.716178 },
		{ 0.281167, 0.252632 },
		{ 0.460189, 0.991974 },
		{ 0.858425, 0.487960 },
		{ 0.405591, 0.226936 },
		{ 0.619953, 0.344584 },
		{ 0.225959, 0.730461 },
		{ 0.876553, 0.384533 },
		{ 0.740471, 0.556719 },
		{ 0.335887, 0.937803 },
		{ 0.644612, 0.585192 },
		{ 0.090213, 0.833979 },
		{ 0.772027, 0.838069 },
		{ 0.631825, 0.548631 },
		{ 0.592944, 0.926115 },
		{ 0.686911, 0.899899 },
		{ 0.655538, 0.811243 },
		{ 0.508652, 0.126316 },
		{ 0.732109, 0.202979 },
		{ 0.587268, 0.010498 },
		{ 0.123875, 0.837397 },
		{ 0.554979, 0.207617 },
		{ 0.063143, 0.175512 },
		{ 0.594287, 0.775018 },
		{ 0.201819, 0.809259 },
		{ 0.236732, 0.134098 },
		{ 0.987640, 0.088839 },
		{ 0.818842, 0.427076 },
		{ 0.110019, 0.473128 },
		{ 0.725730, 0.838618 },
		{ 0.752953, 0.201086 },
		{ 0.549883, 0.498978 },
		{ 0.399152, 0.783258 },
		{ 0.449843, 0.307382 },
		{ 0.303385, 0.014557 },
		{ 0.442976, 0.371654 },
		{ 0.449446, 0.846675 }
	};

	float visibility = 0;
	for (int sampleIndex = 0; sampleIndex < SampleCount; ++sampleIndex) {
		float sampleOffsetScaleMultiplier = !asymmetric;
		if (asymmetric) {
			float maxSampleOffsetY = max(abs(2 * PoissonDiskSampleOffsets[2 * sampleIndex].y + sampleOffsetShift), abs(2 * PoissonDiskSampleOffsets[2 * sampleIndex + 1].y + sampleOffsetShift));
			float uvScale = -1;
			float uvThreshold = -0.5;
			if (baseUv.y >= 0.5) {
				uvScale = 1;
				uvThreshold = 0.5;
			}
			if (uvScale * (baseUv.y + maxSampleOffsetY) >= uvThreshold) {
				sampleOffsetScaleMultiplier = 1;
			}
		}
		float2 sampleOffset1 = 2 * PoissonDiskSampleOffsets[2 * sampleIndex] + sampleOffsetShift;
		float2 sampleOffset2 = 2 * PoissonDiskSampleOffsets[2 * sampleIndex + 1] + sampleOffsetShift;
		float2 sampleOffsetScale = float2(ShadowSampleParam.z, ShadowSampleParam.z * sampleOffsetScaleMultiplier);
		float2 sampleUv1 = sampleOffset1 * sampleOffsetScale + baseUv;
		float2 sampleUv2 = sampleOffset2 * sampleOffsetScale + baseUv;
		visibility += tex.SampleCmpLevelZero(samp, float3(sampleUv1, layerIndex), compareValue).x;
		visibility += tex.SampleCmpLevelZero(samp, float3(sampleUv2, layerIndex), compareValue).x;
	}
	return visibility / (2 * SampleCount);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if !defined(VR)
	uint eyeIndex = 0;
#	else
	uint eyeIndex = input.EyeIndex;
#	endif  // !VR
#	if defined(ADDITIONAL_ALPHA_MASK)
	uint2 alphaMask = input.PositionCS.xy;
	alphaMask.x = ((alphaMask.x << 2) & 12);
	alphaMask.x = (alphaMask.y & 3) | (alphaMask.x & ~3);
	const float maskValues[16] = {
		0.003922,
		0.533333,
		0.133333,
		0.666667,
		0.800000,
		0.266667,
		0.933333,
		0.400000,
		0.200000,
		0.733333,
		0.066667,
		0.600000,
		0.996078,
		0.466667,
		0.866667,
		0.333333,
	};

	if (AlphaTestRef.w - maskValues[alphaMask.x] < 0) {
		discard;
	}
#	endif

#	if defined(FOLIAGE)
	// Randomly skips rendering foliage to depth to simulate transparency
	float2 depthUV = (input.PositionCS.xy / 512.0);

	float4 positionCS = float4(2 * float2(depthUV.x, -depthUV.y + 1) - 1, input.PositionCS.z, 1);
	float4 positionMS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	positionMS.xyz += CameraPosAdjust[eyeIndex];

	// https://www.shadertoy.com/view/mts3zN
	float checkerboard = frac(positionMS.xyz + float3(0.180827486604, 0.328956393296, 0.450299522098));

	if (checkerboard > 0.25)
		discard;
#	endif

	float2 baseTexCoord = 0;
#	if !(defined(RENDER_DEPTH) && defined(RENDER_SHADOWMASK_ANY)) && SHADOWFILTER != 2
#		if (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP)) && defined(ALPHA_TEST) && !defined(RENDER_SHADOWMAP_PB)
	baseTexCoord = input.TexCoord1.zw;
#		elif (defined(ALPHA_TEST) && ((!defined(RENDER_DEPTH) && !defined(RENDER_SHADOWMAP)) || defined(RENDER_SHADOWMAP_PB))) || defined(RENDER_NORMAL) || defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	baseTexCoord = input.TexCoord0.xy;
#		endif
#	endif
	float4 baseColor = TexBaseSampler.Sample(SampBaseSampler, baseTexCoord);

#	if defined(RENDER_SHADOWMAP_PB)
	if (input.TexCoord1.z < 0) {
		discard;
	}
#	endif

	float alpha = 1;
#	if defined(ALPHA_TEST) && !(defined(RENDER_SHADOWMASK_ANY) && defined(RENDER_DEPTH))
	alpha = baseColor.w;
#		if defined(DEPTH_WRITE_DECALS) && !defined(GRAYSCALE_MASK)
	alpha = saturate(1.05 * alpha);
#		endif
#		if defined(OPAQUE_EFFECT)
	alpha *= BaseColor.w * PropertyColor.w;
#		endif
#		if (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP) || ((defined(RENDER_NORMAL) || defined(RENDER_NORMAL_CLEAR)) && defined(DEBUG_COLOR))) && defined(VC) && !defined(TREE_ANIM) && !defined(LOD_OBJECT)
	alpha *= input.Alpha;
#		elif defined(RENDER_SHADOWMASK_ANY) && defined(VC)
	alpha *= input.Alpha.y;
#		endif
#		if defined(GRAYSCALE_TO_ALPHA)
	float grayScaleColor = TexGrayscaleSampler.Sample(SampGrayscaleSampler, float2(baseColor.w, alpha)).w;
	if (grayScaleColor - AlphaTestRef.x < 0) {
		discard;
	}
#		endif
#		if defined(GRAYSCALE_MASK)
	if (baseColor.y - AlphaTestRef.x < 0) {
		discard;
	}
#		elif !defined(RENDER_SHADOWMAP)
	if (alpha - AlphaTestRef.x < 0) {
		discard;
	}
#		endif
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	float4 shadowColor = 1;

	uint stencilValue = 0;
	float shadowMapDepth = 0;
#		if defined(RENDER_DEPTH)
	float2 depthUv = input.PositionCS.xy * VPOSOffset.xy + VPOSOffset.zw;
	float depth = TexDepthSampler.Sample(SampDepthSampler, depthUv).x;

	shadowMapDepth = depth;

#			if defined(FOCUS_SHADOW)
	uint3 stencilDimensions;
	TexStencilSampler.GetDimensions(0, stencilDimensions.x, stencilDimensions.y, stencilDimensions.z);
	stencilValue = TexStencilSampler.Load(float3(stencilDimensions.xy * depthUv, 0)).x;
#			endif
	depthUv = ConvertFromStereoUV(depthUv, eyeIndex);
	float4 positionCS = float4(2 * float2(DynamicResolutionParams2.x * depthUv.x, -depthUv.y * DynamicResolutionParams2.y + 1) - 1, depth, 1);
	float4 positionMS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	float fadeFactor = 1 - pow(saturate(dot(positionMS.xyz, positionMS.xyz) / ShadowLightParam.z), 8);
#		else
	float4 positionMS = input.PositionMS.xyzw;

	shadowMapDepth = positionMS.w;

	float fadeFactor = input.Alpha.x;
#		endif

#		if defined(RENDER_SHADOWMASK)
	if (EndSplitDistances.z >= shadowMapDepth) {
		float4x3 lightProjectionMatrix = ShadowMapProj[eyeIndex][0];
		float shadowMapThreshold = AlphaTestRef.y;
		float cascadeIndex = 0;
		if (2.5 < EndSplitDistances.w && EndSplitDistances.y < shadowMapDepth) {
			lightProjectionMatrix = ShadowMapProj[eyeIndex][2];
			shadowMapThreshold = AlphaTestRef.z;
			cascadeIndex = 2;
		} else if (EndSplitDistances.x < shadowMapDepth) {
			lightProjectionMatrix = ShadowMapProj[eyeIndex][1];
			shadowMapThreshold = AlphaTestRef.z;
			cascadeIndex = 1;
		}

		float shadowVisibility = 0;

		float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionMS.xyz, 1)).xyz;

#			if SHADOWFILTER == 0
		float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(positionLS.xy, cascadeIndex)).x;
		if (shadowMapValue >= positionLS.z - shadowMapThreshold) {
			shadowVisibility = 1;
		}
#			elif SHADOWFILTER == 1
		shadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(positionLS.xy, cascadeIndex), positionLS.z - shadowMapThreshold).x;
#			elif SHADOWFILTER == 3
		shadowVisibility = GetPoissonDiskFilteredShadowVisibility(TexShadowMapSamplerComp, SampShadowMapSamplerComp, -1, positionLS.xy, cascadeIndex, positionLS.z - shadowMapThreshold, false);
#			endif

		if (cascadeIndex < 1 && StartSplitDistances.y < shadowMapDepth) {
			float cascade1ShadowVisibility = 0;

			float3 cascade1PositionLS = mul(transpose(ShadowMapProj[eyeIndex][1]), float4(positionMS.xyz, 1)).xyz;

#			if SHADOWFILTER == 0
			float cascade1ShadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(cascade1PositionLS.xy, 1)).x;
			if (cascade1ShadowMapValue >= cascade1PositionLS.z - AlphaTestRef.z) {
				cascade1ShadowVisibility = 1;
			}
#			elif SHADOWFILTER == 1
			cascade1ShadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(cascade1PositionLS.xy, 1), cascade1PositionLS.z - AlphaTestRef.z).x;
#			elif SHADOWFILTER == 3
			cascade1ShadowVisibility = GetPoissonDiskFilteredShadowVisibility(TexShadowMapSamplerComp, SampShadowMapSamplerComp, -1, cascade1PositionLS.xy, 1, cascade1PositionLS.z - AlphaTestRef.z, false);
#			endif

			float cascade1BlendFactor = smoothstep(0, 1, (shadowMapDepth - StartSplitDistances.y) / (EndSplitDistances.x - StartSplitDistances.y));
			shadowVisibility = lerp(shadowVisibility, cascade1ShadowVisibility, cascade1BlendFactor);

			shadowMapThreshold = AlphaTestRef.z;
		}

		if (stencilValue != 0) {
			uint focusShadowIndex = stencilValue - 1;
			float3 focusShadowMapPosition = mul(transpose(FocusShadowMapProj[focusShadowIndex]), float4(positionMS.xyz, 1));
			float3 focusShadowMapUv = float3(focusShadowMapPosition.xy, StartSplitDistances.w + focusShadowIndex);
			float focusShadowMapCompareValue = focusShadowMapPosition.z - 3 * shadowMapThreshold;
#			if SHADOWFILTER == 3
			float focusShadowVisibility = GetPoissonDiskFilteredShadowVisibility(TexFocusShadowMapSamplerComp, SampFocusShadowMapSamplerComp, -0.5, focusShadowMapUv.xy, focusShadowMapUv.z, focusShadowMapCompareValue, false).x;
#			else
			float focusShadowVisibility = TexFocusShadowMapSamplerComp.SampleCmpLevelZero(SampFocusShadowMapSamplerComp, focusShadowMapUv, focusShadowMapCompareValue).x;
#			endif
			float focusShadowFade = FocusShadowFadeParam[focusShadowIndex];
			shadowVisibility = min(shadowVisibility, lerp(1, focusShadowVisibility, focusShadowFade));
		}

		shadowColor.xyzw = fadeFactor * (shadowVisibility - 1) + 1;
	}
#		elif defined(RENDER_SHADOWMASKSPOT)
	float4 positionLS = mul(transpose(ShadowMapProj[eyeIndex][0]), float4(positionMS.xyz, 1));
	positionLS.xyz /= positionLS.w;
	float2 shadowMapUv = positionLS.xy * 0.5 + 0.5;
	float shadowBaseVisibility = 0;
#			if SHADOWFILTER == 0
	float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(shadowMapUv, EndSplitDistances.x)).x;
	if (shadowMapValue >= positionLS.z - AlphaTestRef.y) {
		shadowBaseVisibility = 1;
	}
#			elif SHADOWFILTER == 1
	shadowBaseVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(shadowMapUv, EndSplitDistances.x), positionLS.z - AlphaTestRef.y).x;
#			elif SHADOWFILTER == 3
	shadowBaseVisibility = GetPoissonDiskFilteredShadowVisibility(TexShadowMapSamplerComp, SampShadowMapSamplerComp, -1, shadowMapUv.xy, EndSplitDistances.x, positionLS.z - AlphaTestRef.y, false);
#			endif
	float shadowVisibilityFactor = pow(2 * length(0.5 * positionLS.xy), ShadowLightParam.x);
	float shadowVisibility = shadowBaseVisibility - shadowVisibilityFactor * shadowBaseVisibility;

	if (stencilValue != 0) {
		uint focusShadowIndex = stencilValue - 1;
		float3 focusShadowMapPosition = mul(transpose(FocusShadowMapProj[focusShadowIndex]), float4(positionMS.xyz, 1));
		float3 focusShadowMapUv = float3(focusShadowMapPosition.xy, StartSplitDistances.w + focusShadowIndex);
		float focusShadowMapCompareValue = focusShadowMapPosition.z - 3 * AlphaTestRef.y;
		float focusShadowVisibility = 0;
#			if SHADOWFILTER == 0
		float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, focusShadowMapUv).x;
		if (shadowMapValue >= focusShadowMapCompareValue) {
			focusShadowVisibility = 1;
		}
#			elif SHADOWFILTER == 1
		focusShadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, focusShadowMapUv, focusShadowMapCompareValue).x;
#			elif SHADOWFILTER == 3
		focusShadowVisibility = GetPoissonDiskFilteredShadowVisibility(TexShadowMapSamplerComp, SampShadowMapSamplerComp, -1, focusShadowMapUv.xy, focusShadowMapUv.z, focusShadowMapCompareValue, false);
#			endif
		shadowVisibility = min(shadowVisibility, lerp(1, focusShadowVisibility, FocusShadowFadeParam[focusShadowIndex]));
	}

	shadowColor.xyzw = fadeFactor * shadowVisibility;
#		elif defined(RENDER_SHADOWMASKPB)
	float4 unadjustedPositionLS = mul(transpose(ShadowMapProj[eyeIndex][0]), float4(positionMS.xyz, 1));

	float shadowVisibility = 0;

	if (unadjustedPositionLS.z * 0.5 + 0.5 >= 0) {
		float3 positionLS = unadjustedPositionLS.xyz / unadjustedPositionLS.w;
		float3 lightDirection = normalize(normalize(positionLS) + float3(0, 0, 1));
		float2 shadowMapUv = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
		float shadowMapCompareValue = saturate(length(positionLS) / ShadowLightParam.x) - AlphaTestRef.y;
#			if SHADOWFILTER == 0
		float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(shadowMapUv, EndSplitDistances.x)).x;
		if (shadowMapValue >= shadowMapCompareValue) {
			shadowVisibility = 1;
		}
#			elif SHADOWFILTER == 1
		shadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(shadowMapUv, EndSplitDistances.x), shadowMapCompareValue).x;
#			elif SHADOWFILTER == 3
		shadowVisibility = GetPoissonDiskFilteredShadowVisibility(TexShadowMapSamplerComp, SampShadowMapSamplerComp, -1, shadowMapUv.xy, EndSplitDistances.x, shadowMapCompareValue, false);
#			endif
	} else {
		shadowVisibility = 1;
	}

	shadowColor.xyzw = fadeFactor * shadowVisibility;
#		elif defined(RENDER_SHADOWMASKDPB)
	float4 unadjustedPositionLS = mul(transpose(ShadowMapProj[eyeIndex][0]), float4(positionMS.xyz, 1));
	float3 positionLS = unadjustedPositionLS.xyz / unadjustedPositionLS.w;
	bool lowerHalf = unadjustedPositionLS.z * 0.5 + 0.5 < 0;
	float3 normalizedPositionLS = normalize(positionLS);

	float shadowMapCompareValue = saturate(length(positionLS) / ShadowLightParam.x) - AlphaTestRef.y;
	float3 positionOffset = lowerHalf ? float3(0, 0, -1) : float3(0, 0, 1);
	float3 lightDirection = normalize(normalizedPositionLS + positionOffset);
	float2 shadowMapUv = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
	shadowMapUv.y = lowerHalf ? 1 - 0.5 * shadowMapUv.y : 0.5 * shadowMapUv.y;

	float shadowVisibility = 0;
#			if SHADOWFILTER == 0
	float shadowMapValue = TexShadowMapSampler.Sample(SampShadowMapSampler, float3(shadowMapUv, EndSplitDistances.x)).x;
	if (shadowMapValue >= shadowMapCompareValue) {
		shadowVisibility = 1;
	}
#			elif SHADOWFILTER == 1
	shadowVisibility = TexShadowMapSamplerComp.SampleCmpLevelZero(SampShadowMapSamplerComp, float3(shadowMapUv, EndSplitDistances.x), shadowMapCompareValue).x;
#			elif SHADOWFILTER == 3
	shadowVisibility = GetPoissonDiskFilteredShadowVisibility(TexShadowMapSamplerComp, SampShadowMapSamplerComp, -1, shadowMapUv.xy, EndSplitDistances.x, shadowMapCompareValue, true);
#			endif

	shadowColor.xyzw = fadeFactor * shadowVisibility;
#		endif
#	endif

#	if defined(TEXTURE)
	float testAlpha = 1;
#		if defined(RENDER_SHADOWMASK_ANY)
	testAlpha = shadowColor.w;
#		elif defined(DEBUG_SHADOWSPLIT) || defined(RENDER_BASE_TEXTURE)
	testAlpha = baseColor.w;
#		elif defined(LOCALMAP_FOGOFWAR)
	testAlpha = input.Alpha;
#		elif (defined(RENDER_DEPTH) || defined(RENDER_SHADOWMAP)) && defined(ALPHA_TEST)
	testAlpha = alpha;
#		elif defined(STENCIL_ABOVE_WATER)
	testAlpha = 0.5;
#		elif defined(RENDER_NORMAL_CLEAR) && !defined(DEBUG_COLOR)
	testAlpha = 0;
#		endif
	if (-AlphaTestRefRS + testAlpha < 0) {
		discard;
	}
#	endif

#	if defined(RENDER_SHADOWMASK_ANY)
	psout.Color.xyzw = shadowColor;
#	elif defined(DEBUG_SHADOWSPLIT)

	float3 splitFactor = 0;

	if (input.Depth < EndSplitDistances.x) {
		splitFactor.y += 1;
	}
	if (input.Depth < EndSplitDistances.w && input.Depth > EndSplitDistances.z) {
		splitFactor.y += 1;
		splitFactor.z += 1;
	}
	if (input.Depth < EndSplitDistances.y && input.Depth > EndSplitDistances.x) {
		splitFactor.z += 1;
	}
	if (input.Depth < EndSplitDistances.z && input.Depth > EndSplitDistances.y) {
		splitFactor.x += 1;
		splitFactor.y += 1;
	}

#		if defined(DEBUG_COLOR)
	psout.Color.xyz = (-splitFactor.xyz + DebugColor.xyz) * 0.9 + splitFactor.xyz;
#		else
	psout.Color.xyz = (-splitFactor.xyz + baseColor.xyz) * 0.9 + splitFactor.xyz;
#		endif

	psout.Color.w = baseColor.w;
#	elif defined(DEBUG_COLOR)
	psout.Color = float4(DebugColor.xyz, 1);
#	elif defined(RENDER_BASE_TEXTURE)
	psout.Color.xyzw = baseColor;
#	elif defined(LOCALMAP_FOGOFWAR)
	psout.Color = float4(0, 0, 0, input.Alpha);
#	elif defined(GRAYSCALE_MASK)
	psout.Color = float4(input.TexCoord1.x / input.TexCoord1.y, baseColor.yz, alpha);
#	elif (defined(RENDER_SHADOWMAP) && defined(ALPHA_TEST)) || defined(RENDER_SHADOWMAP_PB)
	psout.Color = float4(input.TexCoord1.xxx / input.TexCoord1.yyy, alpha);
#	elif defined(RENDER_DEPTH) && (defined(ALPHA_TEST) || defined(ADDITIONAL_ALPHA_MASK))
	psout.Color = float4(input.TexCoord1.x / input.TexCoord1.y, 1, 1, alpha);
#	elif defined(STENCIL_ABOVE_WATER)
	psout.Color = float4(1, 0, 0, 0.5);
#	elif defined(RENDER_NORMAL_CLEAR)
	psout.Color = float4(0.5, 0.5, 0, 0);
#	elif defined(RENDER_NORMAL)
	float2 normal = 2 * (-0.5 + TexNormalSampler.Sample(SampNormalSampler, input.TexCoord0.xy).xy);
#		if defined(RENDER_NORMAL_CLAMP)
	normal = clamp(normal, -0.1, 0.1);
#		endif
	psout.Color.xy = ((normal * 0.9 + input.Normal.xy) / input.TexCoord0.z) * 0.5 + 0.5;
	psout.Color.z = input.Normal.w * (RefractionPower * input.TexCoord0.w);
	psout.Color.w = 1;
#	else
	psout.Color = float4(1, 1, 1, 1);
#	endif

	return psout;
}

#endif
