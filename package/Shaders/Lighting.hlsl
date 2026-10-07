// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

#define LIGHTING

#include "Common/ColorManagement.hlsli"
#include "Common/DCLFObjects.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/LodLandscape.hlsli"
#include "Common/Math.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"

// The pixel stage's pass descriptor. Under DCLF a pass carries the shadow bits the object can take (StaticShadowBits), and
// the draw decides them as GetRenderPasses' light selection would this frame (BuildDrawsCS): ShadowDir unless its bound
// meets no cascade (DCLFSunMiss), DefShadow when the sun or a local shadow light (DCLFLocalShadowMask) reaches it.
uint PixelDescriptor()
{
#if defined(DCLF_BINDLESS)
	uint descriptor = Permutation::PixelShaderDescriptor;
	if (DCLFSunMiss)
		descriptor &= ~Permutation::LightingFlags::ShadowDir;
	if (!(descriptor & Permutation::LightingFlags::ShadowDir) && DCLFLocalShadowMask == 0)
		descriptor &= ~Permutation::LightingFlags::DefShadow;
	return descriptor;
#else
	return Permutation::PixelShaderDescriptor;
#endif
}

#include "Common/Random.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Skinned.hlsli"
#include "Common/Triplanar.hlsli"

#if defined(DCLF_PULLED_ROWS)
// The permutation block (b4) as the draw's pipeline row holds it (Permutation.hlsli, DCLF_PULLED_ROWS).
void DCLFPermutationStatics()
{
	Permutation::VertexShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock);
	Permutation::PixelShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 4);
	Permutation::ExtraShaderDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 8);
	Permutation::ExtraFeatureDescriptor = DCLFRowUint(DCLFPipelineRowAddress, kDCLFPermutationBlock + 12);
	Permutation::EffectRadius = DCLFRowFloat(DCLFPipelineRowAddress, kDCLFPermutationBlock + 16);
}
#endif  // DCLF_PULLED_ROWS

#if defined(FACEGEN) || defined(FACEGEN_RGB_TINT)
#	define SKIN
#endif

#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if (defined(TREE_ANIM) || defined(LANDSCAPE)) && !defined(VC)
#	define VC
#endif  // TREE_ANIM || LANDSCAPE || !VC

#if defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(LODLANDNOISE) || defined(WORLD_MAP)
#	define LOD
#endif

#if defined(VSHADER)
struct VS_INPUT
{
	float4 Position: POSITION0;
	float2 TexCoord0: TEXCOORD0;
#	if !defined(MODELSPACENORMALS)
	float4 Normal: NORMAL0;
	float4 Bitangent: BINORMAL0;
#	endif  // !MODELSPACENORMALS

#	if defined(VC)
	float4 Color: COLOR0;
#		if defined(LANDSCAPE)
	float4 LandBlendWeights1: TEXCOORD2;
	float4 LandBlendWeights2: TEXCOORD3;
#		endif  // LANDSCAPE
#	endif      // VC
#	if defined(SKINNED)
	float4 BoneWeights: BLENDWEIGHT0;
	float4 BoneIndices: BLENDINDICES0;
#	endif  // SKINNED
#	if defined(EYE)
	float EyeParameter: TEXCOORD2;
#	endif  // EYE
};
#endif

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
#if (defined(PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
	float4
#else
	float2
#endif  // (defined (PROJECTED_UV) && !defined(SKINNED)) || defined(LANDSCAPE)
		TexCoord0: TEXCOORD0;

#if defined(WORLD_MAP)
	float3 InputPosition: TEXCOORD4;
#endif

#if defined(SKINNED) || !defined(MODELSPACENORMALS)
	float3 TBN0: TEXCOORD1;
	float3 TBN1: TEXCOORD2;
	float3 TBN2: TEXCOORD3;
#endif  // defined(SKINNED) || !defined(MODELSPACENORMALS)
#if defined(EYE)
	float3 EyeNormal: TEXCOORD6;
#elif defined(LANDSCAPE)
	float4 LandBlendWeights1: TEXCOORD6;
	float4 LandBlendWeights2: TEXCOORD7;
#elif defined(PROJECTED_UV) && !defined(SKINNED)
	float3 TexProj: TEXCOORD7;
#endif  // EYE

	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
	float4 Color: COLOR0;
	float4 FogParam: COLOR1;

	float3 ModelPosition: TEXCOORD12;
#if defined(DCLF_PULLED)
	// Drawcall Limit Fix's pulled Z-prepass (DCLF_PULLED): the draw's material and pipeline rows' addresses and its object word,
	// which the pixel stage would otherwise have from the draw's push data.
	nointerpolation uint4 DCLFDraw: TEXCOORD15;
	nointerpolation uint DCLFDrawObject: TEXCOORD14;
#endif
};
#ifdef VSHADER

cbuffer PerTechnique : register(b0)
{
	float4 HighDetailRange : packoffset(c0);  // loaded cells center in xy, size in zw
#	if !defined(DCLF_BINDLESS)
	float4 FogParam : packoffset(c1);
	float4 FogNearColor : packoffset(c2);
	float4 FogFarColor : packoffset(c3);
#	endif  // !DCLF_BINDLESS
};

#	if defined(DCLF_BINDLESS)
// The fog: the frame's, the same in every technique and drifting with the time of day, so Drawcall Limit Fix keeps it in a block
// of its own for the whole frame (SceneStore::Tables::frameFog) and no technique row changes with it. No other shader uses VS b13.
cbuffer DCLFFrameFog : register(b13)
{
	float4 FogParam : packoffset(c0);
	float4 FogNearColor : packoffset(c1);
	float4 FogFarColor : packoffset(c2);
};
#	endif  // DCLF_BINDLESS

cbuffer PerMaterial : register(b1)
{
	float4 LeftEyeCenter : packoffset(c0);
	float4 RightEyeCenter : packoffset(c1);
	float4 TexcoordOffset : packoffset(c2);
};

cbuffer PerGeometry : register(b2)
{
#if !defined(DCLF_BINDLESS)
	row_major float3x4 World : packoffset(c0);
	row_major float3x4 PreviousWorld : packoffset(c3);
#endif  // !DCLF_BINDLESS
	float4 EyePosition : packoffset(c6);
#if !defined(DCLF_BINDLESS)
	float4 LandBlendParams : packoffset(c7);  // offset in xy, gridPosition in yw
	float4 TreeParams : packoffset(c8);       // wind magnitude in y, amplitude in z, leaf frequency in w
	float2 WindTimers : packoffset(c9);
	row_major float3x4 TextureProj : packoffset(c10);
#endif  // !DCLF_BINDLESS
	float IndexScale : packoffset(c13);
	float4 WorldMapOverlayParameters : packoffset(c14);
};

cbuffer VS_PerFrame : register(b12)
{
	row_major float3x3 ScreenProj : packoffset(c0);
	row_major float4x4 ViewProj : packoffset(c8);
#	if defined(SKINNED) || defined(DCLF_BINDLESS)
	// posAdjust and its previous value (FrameBuffer's CameraPosAdjust and CameraPreviousPosAdjust): the
	// skinning pivot, and under DCLF_BINDLESS also the eye the absolute object records are made relative to.
	float3 BonesPivot : packoffset(c40);
	float3 PreviousBonesPivot : packoffset(c41);
#	endif  // SKINNED || DCLF_BINDLESS
};

#if defined(DCLF_BINDLESS)
// The placement holds World and PreviousWorld absolute, so one row serves every epoch and every camera;
// the vertex stage makes them relative to this epoch's eye and previous eye, the subtraction the engine
// does on the CPU for its own draws. `precise` keeps it the same single float subtraction (so the depth
// and colour epochs, and the native draws, agree to the bit) rather than something folded into the
// transform that follows.
#	define DCLF_VS_WORLD float3x4(                                         \
		DCLFPlacements[DCLFObjectIndex].World[0] - float4(0, 0, 0, BonesPivot.x), \
		DCLFPlacements[DCLFObjectIndex].World[1] - float4(0, 0, 0, BonesPivot.y), \
		DCLFPlacements[DCLFObjectIndex].World[2] - float4(0, 0, 0, BonesPivot.z))
#	define DCLF_VS_PREVIOUS_WORLD float3x4(                                                 \
		DCLFPlacements[DCLFObjectIndex].PreviousWorld[0] - float4(0, 0, 0, PreviousBonesPivot.x), \
		DCLFPlacements[DCLFObjectIndex].PreviousWorld[1] - float4(0, 0, 0, PreviousBonesPivot.y), \
		DCLFPlacements[DCLFObjectIndex].PreviousWorld[2] - float4(0, 0, 0, PreviousBonesPivot.z))
// Tree animation is per object for the same reason World is: with DCLF_BINDLESS the PerGeometry
// buffer is one block for the whole pipeline, and a tree's wind amplitude and clock are its own.
// Likewise the landscape blend parameters (MTLand) and the ProjectedUV texture matrix, from the object's
// extras rows in the row buffer. An object without extras points at row 0, which nothing reads for it.
#	define DCLF_VS_LAND_BLEND_PARAMS DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 0]
#	define DCLF_VS_TEXTURE_PROJ float3x4(DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 1], \
		DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 2], DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 3])
#	if defined(DCLF_PULLED)
// A pulled draw's object word is its sequence's, which main reads first: these are assigned there (DCLFVertexObjectStatics).
static precise float3x4 World;
static precise float3x4 PreviousWorld;
static float4 TreeParams;
static float2 WindTimers;
static float4 LandBlendParams;
static row_major float3x4 TextureProj;
void DCLFVertexObjectStatics()
{
	World = DCLF_VS_WORLD;
	PreviousWorld = DCLF_VS_PREVIOUS_WORLD;
	TreeParams = DCLFTreeParamsOf(DCLFObjectIndex);
	WindTimers = DCLFWindTimersOf(DCLFObjectIndex);
	LandBlendParams = DCLF_VS_LAND_BLEND_PARAMS;
	TextureProj = DCLF_VS_TEXTURE_PROJ;
}
#	else
static precise float3x4 World = DCLF_VS_WORLD;
static precise float3x4 PreviousWorld = DCLF_VS_PREVIOUS_WORLD;
static float4 TreeParams = DCLFTreeParamsOf(DCLFObjectIndex);
static float2 WindTimers = DCLFWindTimersOf(DCLFObjectIndex);
static float4 LandBlendParams = DCLF_VS_LAND_BLEND_PARAMS;
static row_major float3x4 TextureProj = DCLF_VS_TEXTURE_PROJ;
#	endif
#endif  // DCLF_BINDLESS

// Drawcall Limit Fix draws a pipeline's depth and colour from two vertex modules when the Z-prepass is pulled (DCLF_PULLED):
// the colour pass tests EQUAL against that depth, so the clip position is precise in every DCLF build, to the bit the same.
#if defined(DCLF_BINDLESS)
#	define DCLF_PRECISE precise
#else
#	define DCLF_PRECISE
#endif

#	if defined(TREE_ANIM)
float2 GetTreeShiftVector(float4 position, float4 color)
{
	precise float4 tmp1 = (TreeParams.w * TreeParams.y).xxxx * WindTimers.xxyy;
	precise float4 tmp2 = float4(0.1, 0.25, 0.1, 0.25) * tmp1 + dot(position.xyz, 1.0.xxx).xxxx;
	precise float4 tmp3 = abs(-1.0.xxxx + 2.0.xxxx * frac(0.5.xxxx + tmp2.xyzw));
	precise float4 tmp4 = (tmp3 * tmp3) * (3.0.xxxx - 2.0.xxxx * tmp3);
	return (tmp4.xz + 0.1.xx * tmp4.yw) * (TreeParams.z * color.w).xx;
}
#	endif  // TREE_ANIM

#	if defined(DCLF_PULLED)
VS_OUTPUT DCLFShade(VS_INPUT input)
#	else
VS_OUTPUT main(VS_INPUT input)
#	endif
{
	VS_OUTPUT vsout;

	precise float4 inputPosition = float4(input.Position.xyz, 1.0);

#	if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
#		if defined(DCLF_BINDLESS)
	// Drawcall Limit Fix keeps HighDetailRange's centre absolute (EvaluateTechnique): relative here to the draw's eye, as World is,
	// so the Z-prepass and the colour pass lower the same vertices whichever frame's camera their rows were written with.
	inputPosition = LodLandscape::AdjustLodLandscapeVertexPositionMS(inputPosition, float4x4(World, float4(0, 0, 0, 1)), HighDetailRange - float4(BonesPivot.xy, 0, 0));
#		else
	inputPosition = LodLandscape::AdjustLodLandscapeVertexPositionMS(inputPosition, float4x4(World, float4(0, 0, 0, 1)), HighDetailRange);
#		endif
#	endif  // defined(LODLANDNOISE) || defined(LODLANDSCAPE)                                                                   \

	precise float4 previousInputPosition = inputPosition;

#	if defined(TREE_ANIM)
	precise float2 treeShiftVector = GetTreeShiftVector(input.Position, input.Color);
	float3 normal = -1.0.xxx + 2.0.xxx * input.Normal.xyz;

	inputPosition.xyz += normal.xyz * treeShiftVector.x;
	previousInputPosition.xyz += normal.xyz * treeShiftVector.y;
#	endif

#	if defined(SKINNED)
	precise int4 actualIndices = 765.01.xxxx * input.BoneIndices.xyzw;

#		if defined(DCLF_BINDLESS)
	float3x4 previousWorldMatrix =
		Skinned::GetBoneTransformMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFPreviousBoneOffset, actualIndices, PreviousBonesPivot, input.BoneWeights);
#		else
	float3x4 previousWorldMatrix =
		Skinned::GetBoneTransformMatrix(PreviousBones, actualIndices, PreviousBonesPivot, input.BoneWeights);
#		endif
	precise float4 previousWorldPosition =
		float4(mul(inputPosition, transpose(previousWorldMatrix)), 1);

#		if defined(DCLF_BINDLESS)
	float3x4 worldMatrix = Skinned::GetBoneTransformMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFBoneOffset, actualIndices, BonesPivot, input.BoneWeights);
#		else
	float3x4 worldMatrix = Skinned::GetBoneTransformMatrix(Bones, actualIndices, BonesPivot, input.BoneWeights);
#		endif
	precise float4 worldPosition = float4(mul(inputPosition, transpose(worldMatrix)), 1);

	DCLF_PRECISE float4 viewPos = mul(ViewProj, worldPosition);
#	else   // !SKINNED
	precise float4 previousWorldPosition = float4(mul(PreviousWorld, inputPosition), 1);
	precise float4 worldPosition = float4(mul(World, inputPosition), 1);
	precise float4x4 world4x4 = float4x4(World[0], World[1], World[2], float4(0, 0, 0, 1));
	precise float4x4 modelView = mul(ViewProj, world4x4);
	DCLF_PRECISE float4 viewPos = mul(modelView, inputPosition);
#	endif  // SKINNED

	vsout.Position = viewPos;

#	if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	// Precise like viewPos: Drawcall Limit Fix's Z-prepass and colour pass compile this from two modules, and the colour pass tests
	// EQUAL against the prepass's depth. Contracted differently in one of them (a fused multiply-add), the distant LOD land's depth
	// moved by an ulp and its pixels went unshaded.
	DCLF_PRECISE float lodDepthOffset = min(1, 1e-4 * max(0, viewPos.z - 70000)) * 0.5;
	DCLF_PRECISE float lodDepth = viewPos.z + lodDepthOffset;
	vsout.Position.z = lodDepth;
#	endif

	float2 uv = input.TexCoord0.xy * TexcoordOffset.zw + TexcoordOffset.xy;
#	if defined(LANDSCAPE)
	vsout.TexCoord0.zw = (uv * 0.010416667.xx + LandBlendParams.xy) * float2(1, -1) + float2(0, 1);
#	elif defined(PROJECTED_UV) && !defined(SKINNED)
	vsout.TexCoord0.z = mul(TextureProj[0], inputPosition);
	vsout.TexCoord0.w = mul(TextureProj[1], inputPosition);
#	endif
	vsout.TexCoord0.xy = uv;

#	if defined(WORLD_MAP)
	vsout.InputPosition.xyz = WorldMapOverlayParameters.xyz + worldPosition.xyz;
#	endif

#	if defined(SKINNED)
#		if defined(DCLF_BINDLESS)
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrixBindless(DCLFObjects[DCLFObjectIndex].DCLFBoneOffset, actualIndices, input.BoneWeights);
#		else
	float3x3 boneRSMatrix = Skinned::GetBoneRSMatrix(Bones, actualIndices, input.BoneWeights);
#		endif
#	endif

#	if !defined(MODELSPACENORMALS)
	float3x3 tbn = float3x3(
		float3(input.Position.w, input.Normal.w * 2 - 1, input.Bitangent.w * 2 - 1),
		input.Bitangent.xyz * 2.0.xxx + -1.0.xxx,
		input.Normal.xyz * 2.0.xxx + -1.0.xxx);
	float3x3 tbnTr = transpose(tbn);

#		if defined(SKINNED)
	float3x3 worldTbnTr = transpose(mul(transpose(tbnTr), transpose(boneRSMatrix)));
	float3x3 worldTbnTrTr = transpose(worldTbnTr);
	worldTbnTrTr[0] = normalize(worldTbnTrTr[0]);
	worldTbnTrTr[1] = normalize(worldTbnTrTr[1]);
	worldTbnTrTr[2] = normalize(worldTbnTrTr[2]);
	worldTbnTr = transpose(worldTbnTrTr);
	vsout.TBN0.xyz = worldTbnTr[0];
	vsout.TBN1.xyz = worldTbnTr[1];
	vsout.TBN2.xyz = worldTbnTr[2];
#		else
	vsout.TBN0.xyz = mul(tbn, World[0].xyz);
	vsout.TBN1.xyz = mul(tbn, World[1].xyz);
	vsout.TBN2.xyz = mul(tbn, World[2].xyz);
	float3x3 tempTbnTr = transpose(float3x3(vsout.TBN0.xyz, vsout.TBN1.xyz, vsout.TBN2.xyz));
	tempTbnTr[0] = normalize(tempTbnTr[0]);
	tempTbnTr[1] = normalize(tempTbnTr[1]);
	tempTbnTr[2] = normalize(tempTbnTr[2]);
	tempTbnTr = transpose(tempTbnTr);
	vsout.TBN0.xyz = tempTbnTr[0];
	vsout.TBN1.xyz = tempTbnTr[1];
	vsout.TBN2.xyz = tempTbnTr[2];
#		endif
#	elif defined(SKINNED)
	float3x3 boneRSMatrixTr = transpose(boneRSMatrix);
	float3x3 worldTbnTr = transpose(float3x3(normalize(boneRSMatrixTr[0]),
		normalize(boneRSMatrixTr[1]), normalize(boneRSMatrixTr[2])));

	vsout.TBN0.xyz = worldTbnTr[0];
	vsout.TBN1.xyz = worldTbnTr[1];
	vsout.TBN2.xyz = worldTbnTr[2];
#	endif

#	if defined(LANDSCAPE)
	vsout.LandBlendWeights1 = input.LandBlendWeights1;

	float2 gridOffset = LandBlendParams.zw - input.Position.xy;
	vsout.LandBlendWeights2.w = 1 - saturate(0.000375600968 * (9625.59961 - length(gridOffset)));
	vsout.LandBlendWeights2.xyz = input.LandBlendWeights2.xyz;
#	elif defined(PROJECTED_UV) && !defined(SKINNED)
	float3x3 texProjWorld3x3 = float3x3(World[0].xyz, World[1].xyz, World[2].xyz);
	vsout.TexProj = mul(texProjWorld3x3, TextureProj[2].xyz);
#	endif

#	if defined(EYE)
	precise float4 modelEyeCenter = float4(LeftEyeCenter.xyz + input.EyeParameter.xxx * (RightEyeCenter.xyz - LeftEyeCenter.xyz), 1);
	vsout.EyeNormal.xyz = normalize(worldPosition.xyz - mul(modelEyeCenter, transpose(worldMatrix)));
#	endif  // EYE

	vsout.WorldPosition = worldPosition;
	vsout.PreviousWorldPosition = previousWorldPosition;

#	if defined(VC)
	vsout.Color = input.Color;
#	else
	vsout.Color = 1.0.xxxx;
#	endif  // VC

	float fogColorParam = min(FogParam.w,
		exp2(FogParam.z * log2(saturate(length(viewPos.xyz) * FogParam.y - FogParam.x))));

	vsout.FogParam.xyz = lerp(ColorManagement::SRGBToWorking(FogNearColor.xyz), ColorManagement::SRGBToWorking(FogFarColor.xyz), fogColorParam);
	vsout.FogParam.w = fogColorParam;

	vsout.ModelPosition = input.Position.xyz;

	return vsout;
}

#	if defined(DCLF_PULLED)
// Drawcall Limit Fix's Z-prepass draws with plain indirect draws (MainOpaquePass): one per pipeline, which cannot bind a vertex
// buffer, or push data, per draw. So this stage reads its draw's sequence (BuildDrawsCS: DrawSequence, at DCLFSequencesAddress
// plus its instance's stride), and through it the draw's object word, material row and geometry. The draw is indexed from the
// index pool (IndexPool), with no vertex offset, so the stage's vertex index is the geometry's own, and the stage loads
// the attributes at it, decoded as the engine's input layout for the pipeline's vertex layout has them (VertexInput.cpp,
// BuildVertexElements), then shades them as the input assembler's (DCLFShade).
// The draw's rows (its sequence's: the pipeline row's address, then the material row's), and the blocks the pulled source reads
// from them (ShaderPrograms.cpp, PulledLightingSource), before anything reads them.
void DCLFRowStatics(uint4 a_rows)
{
	DCLFPipelineRowAddress = DCLFAddress(a_rows.xy);
	DCLFMaterialRowAddress = DCLFAddress(a_rows.zw);
	DCLFPerTechniqueStatics();
	DCLFPerMaterialStatics();
	DCLFPerGeometryStatics();
	DCLFPermutationStatics();
}

VS_OUTPUT main(uint index : SV_VertexID, uint a_instance : SV_InstanceID)
{
	const uint64_t sequence = DCLFAddress(DCLFSequencesAddress) + uint64_t(a_instance) * kDCLFSequenceStride;
	const uint4 rows = vk::RawBufferLoad<uint4>(sequence + 4);  // the pipeline row's address, the material row's
	DCLFObjectWord = vk::RawBufferLoad<uint>(sequence + 20);
	DCLFRowStatics(rows);
	DCLFDrawVertexLayout = DCLFPipelineVertexLayout();
	const uint4 vertexBuffer = vk::RawBufferLoad<uint4>(sequence + 24);  // address, size, stride
	const uint4 streamBuffer = vk::RawBufferLoad<uint4>(sequence + 40);
	DCLFStream first, second;
	first.address = DCLFAddress(vertexBuffer.xy);
	first.stride = vertexBuffer.w;
	second.address = DCLFAddress(streamBuffer.xy);
	second.stride = streamBuffer.w;

	VS_INPUT input;
	input.Position = asfloat(vk::RawBufferLoad<uint4>(DCLFAttribute(0, index, first, second)));
	input.TexCoord0 = DCLFHalf2(vk::RawBufferLoad<uint>(DCLFAttribute(1, index, first, second)));
#		if !defined(MODELSPACENORMALS)
	input.Normal = DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(3, index, first, second)));
	input.Bitangent = DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(4, index, first, second)));
#		endif
#		if defined(VC)
	input.Color = DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(5, index, first, second)));
#			if defined(LANDSCAPE)
	const uint64_t landData = DCLFAttribute(7, index, first, second);
	input.LandBlendWeights1 = DCLFUnorm4(vk::RawBufferLoad<uint>(landData));
	input.LandBlendWeights2 = DCLFUnorm4(vk::RawBufferLoad<uint>(landData + 4));
#			endif
#		endif
#		if defined(SKINNED)
	const uint64_t skinning = DCLFAttribute(6, index, first, second);
	const uint2 weights = vk::RawBufferLoad<uint2>(skinning);
	input.BoneWeights = float4(DCLFHalf2(weights.x), DCLFHalf2(weights.y));
	input.BoneIndices = DCLFUnorm4(vk::RawBufferLoad<uint>(skinning + 8));
#		endif
#		if defined(EYE)
	input.EyeParameter = asfloat(vk::RawBufferLoad<uint>(DCLFAttribute(8, index, first, second)));
#		endif

	DCLFVertexObjectStatics();
	VS_OUTPUT vsout = DCLFShade(input);
	vsout.DCLFDraw = rows.zwxy;
	vsout.DCLFDrawObject = DCLFObjectWord;
	return vsout;
}
#	endif  // DCLF_PULLED
#endif  // VSHADER

typedef VS_OUTPUT PS_INPUT;

#if defined(TERRAIN_BLENDING)
// Terrain Blending is loaded; objects only read its settings, so they keep this after the undef below.
#	define TERRAIN_BLENDING_OBJECTS
#endif
#if !defined(LANDSCAPE)
#	undef TERRAIN_BLENDING
#endif

#if defined(DEFERRED)
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
	float4 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Specular: SV_Target4;
	float4 Reflectance: SV_Target5;
	float4 Masks: SV_Target6;
	float4 Masks2: SV_Target7;
};
#else
struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;
	float4 MotionVectors: SV_Target1;
	float4 NormalGlossiness: SV_Target2;
};
#endif

#ifdef PSHADER

#	if defined(TRUE_PBR) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE)
static const bool PBRTextures = true;
#	else
static const bool PBRTextures = false;
#	endif

SamplerState SampTerrainParallaxSampler : register(s1);

#	if defined(LANDSCAPE)

SamplerState SampColorSampler : register(s0);

#		define SampLandColor2Sampler SampColorSampler
#		define SampLandColor3Sampler SampColorSampler
#		define SampLandColor4Sampler SampColorSampler
#		define SampLandColor5Sampler SampColorSampler
#		define SampLandColor6Sampler SampColorSampler
#		define SampNormalSampler SampColorSampler
#		define SampLandNormal2Sampler SampColorSampler
#		define SampLandNormal3Sampler SampColorSampler
#		define SampLandNormal4Sampler SampColorSampler
#		define SampLandNormal5Sampler SampColorSampler
#		define SampLandNormal6Sampler SampColorSampler
#		define SampRMAOSSampler SampColorSampler
#		define SampLandRMAOS2Sampler SampColorSampler
#		define SampLandRMAOS3Sampler SampColorSampler
#		define SampLandRMAOS4Sampler SampColorSampler
#		define SampLandRMAOS5Sampler SampColorSampler
#		define SampLandRMAOS6Sampler SampColorSampler

#	else

SamplerState SampColorSampler : register(s0);

#		define SampNormalSampler SampColorSampler

#		if defined(MODELSPACENORMALS) && !defined(LODLANDNOISE)
SamplerState SampSpecularSampler : register(s2);
#		endif
#		if defined(FACEGEN)
SamplerState SampTintSampler : register(s3);
SamplerState SampDetailSampler : register(s4);
#		elif defined(PARALLAX)
SamplerState SampParallaxSampler : register(s3);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
SamplerState SampProjDiffuseSampler : register(s3);
#		endif

#		if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(SNOW_FLAG) || defined(EYE)) && !defined(FACEGEN)
SamplerState SampEnvSampler : register(s4);
SamplerState SampEnvMaskSampler : register(s5);
#		endif

#		if defined(TRUE_PBR) && !defined(FACEGEN)
SamplerState SampParallaxSampler : register(s4);
#		endif
#		if defined(TRUE_PBR)
SamplerState SampRMAOSSampler : register(s5);
#		endif

SamplerState SampGlowSampler : register(s6);

#		if defined(MULTI_LAYER_PARALLAX)
SamplerState SampLayerSampler : register(s8);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
SamplerState SampProjNormalSampler : register(s8);
#		endif

SamplerState SampBackLightSampler : register(s9);

#		if defined(PROJECTED_UV)
SamplerState SampProjDetailSampler : register(s10);
#		endif

SamplerState SampCharacterLightProjNoiseSampler : register(s11);
SamplerState SampRimSoftLightWorldMapOverlaySampler : register(s12);

#		if defined(WORLD_MAP) && (defined(LODLANDSCAPE) || defined(LODLANDNOISE))
SamplerState SampWorldMapOverlaySnowSampler : register(s13);
#		endif

#	endif

#	if defined(LOD_LAND_BLEND)
SamplerState SampLandLodBlend1Sampler : register(s13);
SamplerState SampLandLodBlend2Sampler : register(s15);
#	elif defined(LODLANDNOISE)
SamplerState SampLandLodNoiseSampler : register(s15);
#	endif

SamplerState SampShadowMaskSampler : register(s14);

#	if defined(LANDSCAPE)

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexLandColor2Sampler : register(t1);
Texture2D<float4> TexLandColor3Sampler : register(t2);
Texture2D<float4> TexLandColor4Sampler : register(t3);
Texture2D<float4> TexLandColor5Sampler : register(t4);
Texture2D<float4> TexLandColor6Sampler : register(t5);
Texture2D<float4> TexNormalSampler : register(t7);
Texture2D<float4> TexLandNormal2Sampler : register(t8);
Texture2D<float4> TexLandNormal3Sampler : register(t9);
Texture2D<float4> TexLandNormal4Sampler : register(t10);
Texture2D<float4> TexLandNormal5Sampler : register(t11);
Texture2D<float4> TexLandNormal6Sampler : register(t12);

Texture2D<float4> TexLandTHDisp0Sampler : register(t92);
Texture2D<float4> TexLandTHDisp1Sampler : register(t93);
Texture2D<float4> TexLandTHDisp2Sampler : register(t94);
Texture2D<float4> TexLandTHDisp3Sampler : register(t95);
Texture2D<float4> TexLandTHDisp4Sampler : register(t96);
Texture2D<float4> TexLandTHDisp5Sampler : register(t97);

#		if defined(TRUE_PBR)

Texture2D<float4> TexLandDisplacement0Sampler : register(t80);
Texture2D<float4> TexLandDisplacement1Sampler : register(t81);
Texture2D<float4> TexLandDisplacement2Sampler : register(t82);
Texture2D<float4> TexLandDisplacement3Sampler : register(t83);
Texture2D<float4> TexLandDisplacement4Sampler : register(t84);
Texture2D<float4> TexLandDisplacement5Sampler : register(t85);

Texture2D<float4> TexRMAOSSampler : register(t86);
Texture2D<float4> TexLandRMAOS2Sampler : register(t87);
Texture2D<float4> TexLandRMAOS3Sampler : register(t88);
Texture2D<float4> TexLandRMAOS4Sampler : register(t89);
Texture2D<float4> TexLandRMAOS5Sampler : register(t90);
Texture2D<float4> TexLandRMAOS6Sampler : register(t91);

#		endif

#	else

Texture2D<float4> TexColorSampler : register(t0);
Texture2D<float4> TexNormalSampler : register(t1);  // normal in xyz, glossiness in w if not modelspacenormal

#		if defined(MODELSPACENORMALS) && !defined(LODLANDNOISE)
Texture2D<float4> TexSpecularSampler : register(t2);
#		endif
#		if defined(FACEGEN)
Texture2D<float4> TexTintSampler : register(t3);
Texture2D<float4> TexDetailSampler : register(t4);
#		elif defined(PARALLAX)
Texture2D<float4> TexParallaxSampler : register(t3);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
Texture2D<float4> TexProjDiffuseSampler : register(t3);
#		endif

#		if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(SNOW_FLAG) || defined(EYE)) && !defined(FACEGEN)
TextureCube<float4> TexEnvSampler : register(t4);
Texture2D<float4> TexEnvMaskSampler : register(t5);
#		endif

#		if defined(TRUE_PBR) && !defined(FACEGEN)
Texture2D<float4> TexParallaxSampler : register(t4);
#		endif
#		if defined(TRUE_PBR)
Texture2D<float4> TexRMAOSSampler : register(t5);
#		endif

Texture2D<float4> TexGlowSampler : register(t6);

#		if defined(MULTI_LAYER_PARALLAX)
Texture2D<float4> TexLayerSampler : register(t8);
#		elif defined(PROJECTED_UV) && !defined(SPARKLE)
Texture2D<float4> TexProjNormalSampler : register(t8);
#		endif

Texture2D<float4> TexBackLightSampler : register(t9);

#		if defined(PROJECTED_UV)
Texture2D<float4> TexProjDetail : register(t10);
#		endif

Texture2D<float4> TexCharacterLightProjNoiseSampler : register(t11);
#		if defined(DCLF_BINDLESS)
// The character light's noise under DCLF (DrawPipelines.h: kCharacterLightRegister). The engine binds it at t11, but it is a
// render target that alternates every frame: the frame record gives it, not the material row. A character-light pass reads it
// here for both of t11's uses.
Texture2D<float4> DCLFCharacterLightNoise : register(t125);
#		endif
Texture2D<float4> TexRimSoftLightWorldMapOverlaySampler : register(t12);

#		if defined(WORLD_MAP) && (defined(LODLANDSCAPE) || defined(LODLANDNOISE))
Texture2D<float4> TexWorldMapOverlaySnowSampler : register(t13);
#		endif

#	endif

#	if defined(LOD_LAND_BLEND)
Texture2D<float4> TexLandLodBlend1Sampler : register(t13);
Texture2D<float4> TexLandLodBlend2Sampler : register(t15);
#	elif defined(LODLANDNOISE)
Texture2D<float4> TexLandLodNoiseSampler : register(t15);
#	endif

Texture2D<float4> TexShadowMaskSampler : register(t14);

#	if defined(SKIN) && defined(CS_SKIN)
Texture2D<float4> TexSkinExtraSampler : register(t71);
Texture2D<float4> TexSkinWetnessSampler : register(t74);
Texture2D<float4> TexSkinWetnessNormalSampler : register(t75);
#	endif

cbuffer PerTechnique : register(b0)
{
	float4 FogColor : packoffset(c0);           // Color in xyz, invFrameBufferRange in w
	float4 ColourOutputClamp : packoffset(c1);  // fLightingOutputColourClampPostLit in x, fLightingOutputColourClampPostEnv in y, fLightingOutputColourClampPostSpec in z
	float4 VPOSOffset : packoffset(c2);         // ???
};

cbuffer PerMaterial : register(b1)
{
	float4 LODTexParams : packoffset(c0);  // TerrainTexOffset in xy, LodBlendingEnabled in z
#	if !(defined(LANDSCAPE) && defined(TRUE_PBR))
	float4 TintColor : packoffset(c1);
	float4 EnvmapData : packoffset(c2);  // fEnvmapScale in x, 1 or 0 in y depending of if has envmask
	float4 ParallaxOccData : packoffset(c3);
	float4 SpecularColor : packoffset(c4);  // Shininess in w, color in xyz
	float4 SparkleParams : packoffset(c5);
	float4 MultiLayerParallaxData : packoffset(c6);  // Layer thickness in x, refraction scale in y, uv scale in zw
#	else
	float4 LandscapeTexture1GlintParameters : packoffset(c1);
	float4 LandscapeTexture2GlintParameters : packoffset(c2);
	float4 LandscapeTexture3GlintParameters : packoffset(c3);
	float4 LandscapeTexture4GlintParameters : packoffset(c4);
	float4 LandscapeTexture5GlintParameters : packoffset(c5);
	float4 LandscapeTexture6GlintParameters : packoffset(c6);
#	endif
	float4 LightingEffectParams : packoffset(c7);  // PBR balance, front/back specular weights, back roughness scale. Otherwise rolloff in x, rim power in y.
	float4 IBLParams : packoffset(c8);

#	if !defined(TRUE_PBR)
	float4 LandscapeTexture1to4IsSnow : packoffset(c9);
	float4 LandscapeTexture5to6IsSnow : packoffset(c10);  // bEnableSnowMask in z, inverse iLandscapeMultiNormalTilingFactor in w
	float4 LandscapeTexture1to4IsSpecPower : packoffset(c11);
	float4 LandscapeTexture5to6IsSpecPower : packoffset(c12);
	float4 SnowRimLightParameters : packoffset(c13);  // fSnowRimLightIntensity in x, fSnowGeometrySpecPower in y, fSnowNormalSpecPower in z, bEnableSnowRimLighting in w
#	endif

#	if defined(TRUE_PBR) && defined(LANDSCAPE)
	float3 LandscapeTexture2PBRParams : packoffset(c9);
	float3 LandscapeTexture3PBRParams : packoffset(c10);
	float3 LandscapeTexture4PBRParams : packoffset(c11);
	float3 LandscapeTexture5PBRParams : packoffset(c12);
	float3 LandscapeTexture6PBRParams : packoffset(c13);
#	endif

	float4 CharacterLightParams : packoffset(c14);

	uint PBRFlags : packoffset(c15.x);
	float3 PBRParams1 : packoffset(c15.y);  // roughness scale, displacement scale, specular level
	float4 PBRParams2 : packoffset(c16);    // subsurface color, subsurface opacity

	float3 MaterialObjectRGBScale : packoffset(c17);  // RGB multipliers for material objects
};

cbuffer PerGeometry : register(b2)
{
#if !defined(DCLF_BINDLESS)
	float3 DirLightDirection : packoffset(c0);
	float3 DirLightColor : packoffset(c1);
#endif  // !DCLF_BINDLESS
	float4 ShadowLightMaskSelect : packoffset(c2);
#if !defined(DCLF_BINDLESS)
	float4 MaterialData : packoffset(c3);  // envmapLODFade in x, specularLODFade in y, alpha in z
#endif  // !DCLF_BINDLESS
	float AlphaTestRef : packoffset(c4);
#if !defined(DCLF_BINDLESS)
	float3 EmitColor : packoffset(c4.y);
#endif  // !DCLF_BINDLESS
#if !defined(DCLF_BINDLESS)
	float4 ProjectedUVParams : packoffset(c6);
#endif  // !DCLF_BINDLESS
	float4 SSRParams : packoffset(c7);
	float4 WorldMapOverlayParametersPS : packoffset(c8);
#if !defined(DCLF_BINDLESS)
	float4 ProjectedUVParams2 : packoffset(c9);
	float4 ProjectedUVParams3 : packoffset(c10);  // fProjectedUVDiffuseNormalTilingScale in x, fProjectedUVNormalDetailTilingScale in y, EnableProjectedNormals in w
#endif  // !DCLF_BINDLESS
#if !defined(DCLF_BINDLESS)
	row_major float3x4 DirectionalAmbient : packoffset(c11);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c14);  // Fresnel power in z, color in xyz
#endif  // !DCLF_BINDLESS
	float4 PointLightPosition[7] : packoffset(c15);  // point light radius in w
	float4 PointLightColor[7] : packoffset(c22);
	float2 NumLightNumShadowLight : packoffset(c29);
};

#if defined(DCLF_BINDLESS)
// The frame's lighting: the same in every pipeline's PerGeometry block, so Drawcall Limit Fix keeps it in one block
// of its own for the whole frame (SceneStore::Tables::frameLighting), and a pipeline's block does not change with the
// sun. No other shader uses b13.
cbuffer DCLFFrameLighting : register(b13)
{
	float3 DirLightDirection : packoffset(c0);
	float3 DirLightColor : packoffset(c1);
	row_major float3x4 DirectionalAmbient : packoffset(c2);
	float4 AmbientSpecularTintAndFresnelPower : packoffset(c5);  // Fresnel power in z, color in xyz
	// The LOD fades' frame inputs (LodFadeFrame): what BSFadeNode::OnVisible's fade value and GetRenderPasses' fade read.
	float4 DCLFLodFadeEye : packoffset(c6);          // the main camera's world position, its lodAdjust in w
	float4 DCLFLodFadeThresholds : packoffset(c7);   // specular start and end, envmap start and end
	float4 DCLFLodFadeMetric : packoffset(c8);       // metric scale, default scale, the override metric, 1 when overridden
	float4 DCLFLodFadeDivisors[4] : packoffset(c9);  // per LOD type
	float4 DCLFLodFadeState : packoffset(c13);       // x: 1 when the engine updates the metric at all (else the property's fades stand)
// CS_DCLF_FOLIAGE_PARITY's draws: the alpha-tested ones, and terrain LOD (whose depth the Z-prepass and the colour pass each
// compute from HighDetailRange); with CS_DCLF_FOLIAGE_PARITY=land every draw (DCLF_FOLIAGE_PARITY_ALL), so that a pixel no draw
// shaded is told from one another object took at the same depth. Each records the pixels it owns in the Z-prepass (those with a
// pixel stage there) and those the colour pass shaded.
#	if defined(DCLF_FOLIAGE_PARITY) && (defined(DO_ALPHA_TEST) || defined(LODLANDNOISE) || defined(LODLANDSCAPE) || defined(DCLF_FOLIAGE_PARITY_ALL))
#		define DCLF_OWNED_PARITY
#	endif
#	if defined(DCLF_FOLIAGE_PARITY)
	// CS_DCLF_FOLIAGE_PARITY (IndirectDraws GpuLayouts.h, FoliageParityConstants): the frame's buffers' addresses (each pixel's
	// object word; its albedo and diffuse), the render size, the frame's tag and the Z-prepass owners' UAV.
	uint4 DCLFFoliageParityBuffers : packoffset(c14);
	uint4 DCLFFoliageParityFrame : packoffset(c15);
#	endif
};
#endif  // DCLF_BINDLESS

#if defined(DCLF_BINDLESS)
// The fade node's LOD metric under the frame's camera (FUN_14147b110's +0x144): the distance to its bound centre, scaled by
// lodAdjust over its LOD type's divisor (the default scale when that is not positive), times the metric scale.
float DCLFLodMetric(float3 a_node, uint a_type)
{
	if (DCLFLodFadeMetric.w != 0)
		return DCLFLodFadeMetric.z;
	const float divisor = DCLFLodFadeDivisors[a_type >> 2][a_type & 3];
	const float scale = divisor > 0 ? DCLFLodFadeEye.w / divisor : DCLFLodFadeMetric.y;
	return length(a_node - DCLFLodFadeEye.xyz) * scale * DCLFLodFadeMetric.x;
}
// GetRenderPasses' fade at a metric (FUN_14147c470): 1 up to the start, 0 past the end, where the engine drops the feature.
float DCLFLodFadeAt(float a_metric, float a_start, float a_end)
{
	if (a_end < a_metric)
		return 0;
	if (a_metric <= a_start)
		return 1;
	return saturate((a_metric - a_end) / (a_start - a_end));
}
#	define DCLF_PS_LOD_FADES (DCLFLodFadeState.x != 0 ? DCLFLodFadeFlagsOf(DCLFObjectIndex) : 0)
#	define DCLF_PS_LOD_METRIC (DCLFLodFades ? DCLFLodMetric(DCLFPlacements[DCLFObjectIndex].LodFadeNode.xyz, DCLFLodFades & 0xF) : 0)
#	define DCLF_PS_SPECULAR_LOD_FADE DCLFLodFadeAt(DCLFLodMetricValue, DCLFLodFadeThresholds.x, DCLFLodFadeThresholds.y)
#	define DCLF_PS_MATERIAL_DATA float4(                                                                                                                 \
		(DCLFLodFades & (1u << 5)) ? DCLFLodFadeAt(DCLFLodMetricValue, DCLFLodFadeThresholds.z, DCLFLodFadeThresholds.w) : DCLFObjects[DCLFObjectIndex].MaterialData.x, \
		(DCLFLodFades & (1u << 4)) ? DCLFSpecularLodFade : DCLFObjects[DCLFObjectIndex].MaterialData.y,                                                             \
		DCLFObjects[DCLFObjectIndex].MaterialData.zw)
// Only the w of SSRParams is per-object; x, y and z stay in the per-pipeline buffer above.
#	define DCLF_PS_SSR_SPECULAR ((DCLFLodFades & (1u << 4)) ? ((DCLFLodFades & (1u << 6)) ? DCLFSpecularLodFade : 0) : DCLFObjects[DCLFObjectIndex].EmitColor.w)
// ProjectedUV's three pixel parameters are per object too (the property's, plus two globals).
#	if defined(DCLF_PULLED)
// A pulled draw's object word is its vertex stage's, which main reads first: these are assigned there (DCLFPixelObjectStatics).
static uint DCLFLodFades;
static float DCLFLodMetricValue;
static float DCLFSpecularLodFade;
static float4 MaterialData;
static float3 EmitColor;
static float DCLFSSRSpecular;
static float4 ProjectedUVParams;
static float4 ProjectedUVParams2;
static float4 ProjectedUVParams3;
#	else
static const uint DCLFLodFades = DCLF_PS_LOD_FADES;
static const float DCLFLodMetricValue = DCLF_PS_LOD_METRIC;
static const float DCLFSpecularLodFade = DCLF_PS_SPECULAR_LOD_FADE;
static float4 MaterialData = DCLF_PS_MATERIAL_DATA;
static float3 EmitColor = DCLFObjects[DCLFObjectIndex].EmitColor.xyz;
static float DCLFSSRSpecular = DCLF_PS_SSR_SPECULAR;
static float4 ProjectedUVParams = DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 4];
static float4 ProjectedUVParams2 = DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 5];
static float4 ProjectedUVParams3 = DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 6];
#	endif
#	define DCLF_SSR_SPECULAR DCLFSSRSpecular
#else
#	define DCLF_SSR_SPECULAR SSRParams.w
#endif  // DCLF_BINDLESS

#if defined(DCLF_BINDLESS_DRAW)
// The reference comes from the per-object record instead of a constant buffer of its own, so that b11
// stops being part of what makes a draw's binding record unique. Same value, same single read below.
#	if defined(DCLF_PULLED)
static float AlphaTestRefRS;  // DCLFPixelObjectStatics
#	else
static const float AlphaTestRefRS = DCLFObjects[DCLFObjectIndex].AlphaTestRef;
#	endif
#else
cbuffer AlphaTestRefBuffer : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}
#endif  // DCLF_BINDLESS_DRAW

float GetSoftLightMultiplier(float angle)
{
	float softLightParam = saturate((LightingEffectParams.x + angle) / (1 + LightingEffectParams.x));
	float arg1 = (softLightParam * softLightParam) * (3 - 2 * softLightParam);
	float clampedAngle = saturate(angle);
	float arg2 = (clampedAngle * clampedAngle) * (3 - 2 * clampedAngle);
	float softLigtMul = saturate(arg1 - arg2);
	return softLigtMul;
}

float GetRimLightMultiplier(float3 L, float3 V, float3 N)
{
	float NdotV = saturate(dot(N, V));
	return exp2(LightingEffectParams.y * log2(1 - NdotV)) * saturate(dot(V, -L));
}

#	if !defined(TRUE_PBR)
float ProcessSparkleColor(float color)
{
	return SparkleParams.y > 0 ? exp2(SparkleParams.y * log2(min(1, abs(color)))) : 1.0;
}
#	endif

float3 TransformNormal(float3 normal)
{
	return normal * 2 + -1.0.xxx;
}

float GetLodLandBlendParameter(float3 color)
{
	float result = saturate(1.6666666 * (dot(color, 0.55.xxx) - 0.4));
	result = ((result * result) * (3 - result * 2));
#	if !defined(WORLD_MAP)
	result *= 0.8;
#	endif
	return result;
}

float GetLodLandBlendMultiplier(float parameter, float mask)
{
	return 0.8333333 * (parameter * (0.37 - mask) + mask) + 0.37;
}

float GetLandSnowMaskValue(float alpha)
{
#	if !defined(TRUE_PBR)
	return alpha * LandscapeTexture5to6IsSnow.z + (1 + -LandscapeTexture5to6IsSnow.z);
#	else
	return 0;
#	endif
}

float3 GetLandNormal(float landSnowMask, float3 normal, float2 uv, SamplerState sampNormal, Texture2D<float4> texNormal)
{
	float3 landNormal = TransformNormal(normal);
#	if defined(SNOW) && !defined(TRUE_PBR)
	if (landSnowMask > 1e-5 && LandscapeTexture5to6IsSnow.w != 1.0) {
		float3 snowNormal =
			float3(-1, -1, 1) *
			TransformNormal(texNormal.Sample(sampNormal, LandscapeTexture5to6IsSnow.ww * uv).xyz);
		landNormal.z += 1;
		float normalProjection = dot(landNormal, snowNormal);
		snowNormal = landNormal * normalProjection.xxx - snowNormal * landNormal.z;
		return normalize(snowNormal);
	} else {
		return landNormal;
	}
#	else
	return landNormal;
#	endif
}

#	if defined(SNOW) && !defined(TRUE_PBR)
float3 GetSnowSpecularColor(PS_INPUT input, float3 worldNormal, float3 viewDirection)
{
	if (SnowRimLightParameters.w > 1e-5) {
#		if defined(MODELSPACENORMALS) && !defined(SKINNED)
		float3 modelGeometryNormal = float3(0, 0, 1);
#		else
		float3 modelGeometryNormal = normalize(float3(input.TBN0.z, input.TBN1.z, input.TBN2.z));
#		endif
		float normalFactor = 1 - saturate(dot(worldNormal, viewDirection));
		float geometryNormalFactor = 1 - saturate(dot(modelGeometryNormal, viewDirection));
		return (SnowRimLightParameters.x * (exp2(SnowRimLightParameters.y * log2(geometryNormalFactor)) * exp2(SnowRimLightParameters.z * log2(normalFactor)))).xxx;
	} else {
		return 0.0.xxx;
	}
}
#	endif

#	if defined(FACEGEN)
float3 GetFacegenBaseColor(float3 rawBaseColor, float2 uv)
{
	float3 detailColor = TexDetailSampler.Sample(SampDetailSampler, uv).xyz;
	detailColor = float3(3.984375, 3.984375, 3.984375) * (float3(0.00392156886, 0, 0.00392156886) + detailColor);
	float3 tintColor = TexTintSampler.Sample(SampTintSampler, uv).xyz;
	tintColor = tintColor * rawBaseColor * 2.0.xxx;
	tintColor = tintColor - tintColor * rawBaseColor;
	return (rawBaseColor * rawBaseColor + tintColor) * detailColor;
}
#	endif

#	if defined(FACEGEN_RGB_TINT)
float3 GetFacegenRGBTintBaseColor(float3 rawBaseColor, float2 uv)
{
	float3 tintColor = TintColor.xyz * rawBaseColor * 2.0.xxx;
	tintColor = tintColor - tintColor * rawBaseColor;
	return float3(1.01171875, 0.99609375, 1.01171875) * (rawBaseColor * rawBaseColor + tintColor);
}
#	endif

#	if defined(WORLD_MAP)
float3 GetWorldMapNormal(PS_INPUT input, float3 rawNormal, float3 baseColor)
{
	float3 normal = normalize(rawNormal);
#		if defined(MODELSPACENORMALS)
	float3 worldMapNormalSrc = normal.xyz;
#		else
	float3 worldMapNormalSrc = float3(input.TBN0.z, input.TBN1.z, input.TBN2.z);
#		endif
	float3 worldMapNormal = 7.0.xxx * (-0.2.xxx + abs(normalize(worldMapNormalSrc)));
	worldMapNormal = max(0.01.xxx, worldMapNormal * worldMapNormal * worldMapNormal);
	worldMapNormal /= dot(worldMapNormal, 1.0.xxx);
	float3 worldMapColor1 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.yz).xyz;
	float3 worldMapColor2 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.xz).xyz;
	float3 worldMapColor3 = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, WorldMapOverlayParametersPS.xx * input.InputPosition.xy).xyz;
#		if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float3 worldMapSnowColor1 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.yz).xyz;
	float3 worldMapSnowColor2 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.xz).xyz;
	float3 worldMapSnowColor3 = TexWorldMapOverlaySnowSampler.Sample(SampWorldMapOverlaySnowSampler, WorldMapOverlayParametersPS.ww * input.InputPosition.xy).xyz;
#		endif
	float3 worldMapColor = worldMapNormal.xxx * worldMapColor1 + worldMapNormal.yyy * worldMapColor2 + worldMapNormal.zzz * worldMapColor3;
#		if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float3 worldMapSnowColor = worldMapSnowColor1 * worldMapNormal.xxx + worldMapSnowColor2 * worldMapNormal.yyy + worldMapSnowColor3 * worldMapNormal.zzz;
	float snowMultiplier = GetLodLandBlendParameter(baseColor);
	worldMapColor = snowMultiplier * (worldMapSnowColor - worldMapColor) + worldMapColor;
#		endif
	worldMapColor = normalize(2.0.xxx * (-0.5.xxx + (worldMapColor)));
#		if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
	float worldMapLandTmp = saturate(19.9999962 * (rawNormal.z - 0.95));
	worldMapLandTmp = saturate(-(worldMapLandTmp * worldMapLandTmp) * (worldMapLandTmp * -2 + 3) + 1.5);
	float3 worldMapLandTmp1 = normalize(normal.zxy * float3(1, 0, 0) - normal.yzx * float3(0, 0, 1));
	float3 worldMapLandTmp2 = normalize(worldMapLandTmp1.yzx * normal.zxy - worldMapLandTmp1.zxy * normal.yzx);
	float3 worldMapLandTmp3 = normalize(worldMapColor.xxx * worldMapLandTmp1 + worldMapColor.yyy * worldMapLandTmp2 + worldMapColor.zzz * normal.xyz);
	float worldMapLandTmp4 = dot(worldMapLandTmp3, worldMapLandTmp3);
	if (worldMapLandTmp4 > 0.999 && worldMapLandTmp4 < 1.001) {
		normal.xyz = worldMapLandTmp * (worldMapLandTmp3 - normal.xyz) + normal.xyz;
	}
#		else
	normal.xyz = normalize(
		WorldMapOverlayParametersPS.zzz * (rawNormal.xyz - worldMapColor.xyz) + worldMapColor.xyz);
#		endif
	return normal;
}

float3 GetWorldMapBaseColor(float3 sampledBaseColor, float3 workingBaseColor, float texProjTmp)
{
#		if defined(LODOBJECTS) && !defined(PROJECTED_UV)
	return workingBaseColor;
#		endif
#		if defined(LODLANDSCAPE) || defined(LODOBJECTSHD) || defined(LODLANDNOISE)
	float lodMultiplier = GetLodLandBlendParameter(sampledBaseColor.xyz);
#		elif defined(LODOBJECTS) && defined(PROJECTED_UV)
	float lodMultiplier = saturate(10 * texProjTmp);
#		else
	float lodMultiplier = 1;
#		endif
#		if defined(LODOBJECTS)
	float4 lodColorMul = lodMultiplier.xxxx * float4(0.269999981, 0.281000018, 0.441000015, 0.441000015) + float4(0.0780000091, 0.09799999, -0.0349999964, 0.465000004);
	float4 lodColor = lodColorMul.xyzw * 2.0.xxxx;
#			if defined(TRUE_PBR)
	lodColor.xyz = Color::Albedo(Color::LinearSRGBToWorking(lodColor.xyz));
#			else
	lodColor.xyz = Color::Albedo(ColorManagement::SRGBToWorking(lodColor.xyz));
#			endif
	bool useLodColorZ = lodColorMul.w > 0.5;
	lodColor.xyz = max(lodColor.xyz, workingBaseColor.xyz);
	lodColor.w = useLodColorZ ? lodColor.z : min(lodColor.w, workingBaseColor.z);
	return (0.5 * lodMultiplier).xxx * (lodColor.xyw - workingBaseColor.xyz) + workingBaseColor;
#		else
	float4 lodColorMul = lodMultiplier.xxxx * float4(0.199999988, 0.441000015, 0.269999981, 0.281000018) + float4(0.300000012, 0.465000004, 0.0780000091, 0.09799999);
	float3 lodColor = lodColorMul.zwy * 2.0.xxx;
#			if defined(TRUE_PBR)
	lodColor.xyz = Color::Albedo(Color::LinearSRGBToWorking(lodColor.xyz));
#			else
	lodColor.xyz = Color::Albedo(ColorManagement::SRGBToWorking(lodColor.xyz));
#			endif
	lodColor.xy = max(lodColor.xy, workingBaseColor.xy);
	lodColor.z = lodColorMul.y > 0.5 ? max((lodMultiplier * 0.441 + -0.0349999964) * 2, workingBaseColor.z) : min(lodColor.z, workingBaseColor.z);
	return lodColorMul.xxx * (lodColor - workingBaseColor.xyz) + workingBaseColor;
#		endif
}
#	endif

float GetSnowParameterY(float texProjTmp, float alpha)
{
	if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::BaseObjectIsSnow) {
		return min(1, texProjTmp + alpha);
	}
	return texProjTmp;
}

#	if defined(LOD)
#		undef EXTENDED_MATERIALS
#		undef WATER_BLENDING
#		undef LIGHT_LIMIT_FIX
#		undef WETNESS_EFFECTS
#		undef DYNAMIC_CUBEMAPS
#		undef WATER_EFFECTS
#	endif

#	if defined(WORLD_MAP)
#		undef CLOUD_SHADOWS
#		undef SKYLIGHTING
#	endif

#	if defined(SKYLIGHTING)
#		define SKYLIGHTING_SHADOW_VIS
#	endif

#	include "Common/LightingCommon.hlsli"

#	if defined(WATER_EFFECTS)
#		include "WaterEffects/WaterCaustics.hlsli"
#	endif

#	if defined(EYE)
#		undef WETNESS_EFFECTS
#	endif

#	if defined(EXTENDED_MATERIALS) && !defined(LOD) && (defined(PARALLAX) || defined(LANDSCAPE) || defined(ENVMAP) || defined(TRUE_PBR))
#		define EMAT
#	endif

#	if defined(EMAT) && (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#		define EMAT_ENVMAP
#	endif

#	if defined(DYNAMIC_CUBEMAPS)
#		include "DynamicCubemaps/DynamicCubemaps.hlsli"
#	endif

#	if defined(TRUE_PBR)
#		include "Common/PBR.hlsli"
#	endif

#	if defined(LANDSCAPE) && defined(LANDSCAPE_SEAMS)
#		include "Common/LandscapeSeams.hlsli"
#	endif

#	if defined(EMAT)
#		include "ExtendedMaterials/ExtendedMaterials.hlsli"
#	endif

#	if defined(SCREEN_SPACE_SHADOWS)
#		include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	endif

#	if defined(LIGHT_LIMIT_FIX)
#		include "LightLimitFix/LightLimitFix.hlsli"
#	endif

#	if defined(TREE_ANIM)
#		undef WETNESS_EFFECTS
#	endif

#	if defined(WETNESS_EFFECTS)
#		include "WetnessEffects/WetnessEffects.hlsli"
#	endif

#	if defined(TERRAIN_BLENDING)
#		include "TerrainBlending/TerrainBlending.hlsli"
#	endif

#	if defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(LODLANDSCAPE)
static const uint RuntimeLightingFlags = 0;
#	elif defined(LANDSCAPE)
static const uint RuntimeLightingFlags = Permutation::LightingFlags::Specular;
#	else
static const uint RuntimeLightingFlags = Permutation::LightingFlags::Specular | Permutation::LightingFlags::SoftLighting | Permutation::LightingFlags::RimLighting | Permutation::LightingFlags::BackLighting;
#	endif

bool HasLightingFlag(uint flag)
{
	uint flags = Permutation::PixelShaderDescriptor & RuntimeLightingFlags;
#	if defined(SPECULAR)
	flags |= Permutation::LightingFlags::Specular;
#	endif
#	if defined(SOFT_LIGHTING)
	flags |= Permutation::LightingFlags::SoftLighting;
#	endif
#	if defined(RIM_LIGHTING)
	flags |= Permutation::LightingFlags::RimLighting;
#	endif
#	if defined(BACK_LIGHTING)
	flags |= Permutation::LightingFlags::BackLighting;
#	endif
	return flags & flag;
}

bool HasSpecular()
{
	return HasLightingFlag(Permutation::LightingFlags::Specular);
}

bool HasSoftLighting()
{
#	if defined(EYE) || (defined(SSS) && defined(SKIN) && defined(DEFERRED))
	return false;
#	else
	return HasLightingFlag(Permutation::LightingFlags::SoftLighting);
#	endif
}

bool HasRimLighting()
{
#	if defined(EYE)
	return false;
#	else
	return HasLightingFlag(Permutation::LightingFlags::RimLighting);
#	endif
}

bool HasBackLighting()
{
#	if defined(EYE)
	return false;
#	else
	return HasLightingFlag(Permutation::LightingFlags::BackLighting);
#	endif
}

bool UseSkylightingShadowVisibility()
{
#	if defined(SKYLIGHTING_SHADOW_VIS)
#		if defined(TRUE_PBR)
	if (PBR::IsFoliageMaterial(PBRFlags) && (PBRFlags & PBR::Flags::Subsurface) != 0)
		return true;
#		endif
	return HasLightingFlag(Permutation::LightingFlags::SoftLighting | Permutation::LightingFlags::RimLighting | Permutation::LightingFlags::BackLighting);
#	else
	return false;
#	endif
}

#	if defined(SKYLIGHTING)
#		include "Skylighting/Skylighting.hlsli"
#	endif

#	if defined(CS_HAIR_SHADING)
#		include "Hair/Hair.hlsli"
#	endif

#	if defined(LANDSCAPE)
#		include "Common/LightingLandscape.hlsli"
#	endif

#	if defined(TERRAIN_VARIATION) && !(defined(LOD) || defined(SKIN) || defined(HAIR) || defined(EYE) || defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS) || defined(DEPTH_WRITE_DECALS))
#		define TERRAIN_VARIATION_MESH
#	endif

#	if defined(TERRAIN_VARIATION) && (defined(LANDSCAPE) || defined(LOD_LAND_BLEND) || (defined(LOD_BLENDING) && defined(LODLANDSCAPE)) || defined(TERRAIN_VARIATION_MESH))
#		include "TerrainVariation/TerrainVariation.hlsli"
#	endif

#	if defined(EXTENDED_TRANSLUCENCY) && !(defined(LOD) || defined(SKIN) || defined(HAIR) || defined(EYE) || defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS) || defined(DEPTH_WRITE_DECALS))
#		include "ExtendedTranslucency/ExtendedTranslucency.hlsli"
#		define ANISOTROPIC_ALPHA
#	endif

#	if defined(CS_SKIN)
#		include "Skin/Skin.hlsli"
#	endif

#	define LinearSampler SampColorSampler

#	include "Common/ShadowSampling.hlsli"

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

#	include "Common/LightingEval.hlsli"

#	if defined(DCLF_PULLED)
PS_OUTPUT DCLFShadePS(PS_INPUT input, bool frontFace)
#	else
#		if defined(DCLF_OWNED_PARITY)
// The parity write below must be the visible fragment's: the colour pass tests EQUAL and writes no depth, so testing first
// changes nothing else.
[earlydepthstencil]
#		endif
PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
#	endif
{
	PS_OUTPUT psout;

	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WorldPosition.xyz, 1)).xyz;
	float3 viewDirection = -normalize(input.WorldPosition.xyz);
#	if defined(TRUE_PBR)
	const bool isFoliage = PBR::IsFoliageMaterial(PBRFlags);
	const bool isThinSurface = PBR::IsThinSurfaceMaterial(PBRFlags);
#		if defined(TREE_ANIM)
	const bool foliageNormals = true;
#		else
	const bool foliageNormals = isFoliage;
#		endif
	const bool thinSurfaceNormals = foliageNormals || isThinSurface;
	float3 thinSurfaceFaceNormal = PBR::GetThinSurfaceFaceNormal(input.WorldPosition.xyz, viewDirection, float3(0, 0, 1));
#	endif

	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

#	if defined(DEFERRED)
	const bool inWorld = true;
#	else
	const bool inWorld = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InWorld);
#	endif
	const bool inReflection = Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection;

	float nearFactor = smoothstep(4096.0 * 2.5, 0.0, viewPosition.z);

#	if defined(SKINNED) || !defined(MODELSPACENORMALS)
	float3x3 tbn = float3x3(input.TBN0.xyz, input.TBN1.xyz, input.TBN2.xyz);

#		if !defined(TREE_ANIM) && !defined(LOD)
	// Fix incorrect vertex normals on double-sided meshes
#			if defined(TRUE_PBR)
	if (!frontFace && !isThinSurface)
#			else
	if (!frontFace)
#			endif
		tbn = lerp(tbn, -tbn, nearFactor);
#		endif

	float3x3 tbnTr = transpose(tbn);

	tbnTr[0] = normalize(tbnTr[0]);
	tbnTr[1] = normalize(tbnTr[1]);
	tbnTr[2] = normalize(tbnTr[2]);

#		if defined(TRUE_PBR)
	// Winding is constant per triangle, so this cannot split bent normals mid-surface like a per-pixel flip.
	if (thinSurfaceNormals && !frontFace)
		tbnTr = -tbnTr;
#		endif

	tbn = transpose(tbnTr);

#	endif  // defined (SKINNED) || !defined (MODELSPACENORMALS)

#	if defined(LANDSCAPE) && defined(LANDSCAPE_SEAMS)
	LandscapeSeams::Load(input.WorldPosition.xy + FrameBuffer::CameraPosAdjust.xy, input.LandBlendWeights1, input.LandBlendWeights2);
#	endif

#	if !defined(TRUE_PBR)
#		if defined(LANDSCAPE)
	float shininess = dot(input.LandBlendWeights1, LandscapeTexture1to4IsSpecPower) + input.LandBlendWeights2.x * LandscapeTexture5to6IsSpecPower.x + input.LandBlendWeights2.y * LandscapeTexture5to6IsSpecPower.y;
#			if defined(LANDSCAPE_SEAMS)
	shininess += dot(LandscapeSeams::ExtraWeights, LandscapeSeams::Data.SpecPower);
#			endif
#		else
	float shininess = HasSpecular() ? SpecularColor.w : 0.0;
#		endif  // defined (LANDSCAPE)
#	endif

#	if defined(TERRAIN_BLENDING)
	float blendFactorTerrain = 0.0;
	[flatten] if (SharedData::terrainBlendingSettings.Enabled)
	{
		float depthSampled = TerrainBlending::TerrainBlendingMaskTexture[input.Position.xy].x;

		float depthSampledLinear = SharedData::GetScreenDepth(depthSampled);
		float depthPixelLinear = SharedData::GetScreenDepth(input.Position.z);

		blendFactorTerrain = saturate((depthSampledLinear - depthPixelLinear) / 10.0);

		if (input.Position.z == depthSampled)
			blendFactorTerrain = 1;
	}
#	endif

	float2 uv = input.TexCoord0.xy;
	float2 uvOriginal = uv;

	// Lattice cell comes from the geometric UV, before the parallax block below rewrites uv.
#	if !defined(LANDSCAPE) && (defined(TERRAIN_VARIATION_MESH) || defined(EMAT))
	StochasticOffsets meshOffset = (StochasticOffsets)0;
#		if defined(TERRAIN_VARIATION_MESH)
	const bool applyMeshTV = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::TVMeshVariation) != 0;
	[branch] if (applyMeshTV)
	{
		meshOffset = ComputeStochasticOffsetsMesh(uvOriginal, input.Position.xy);
	}
#		else
	const bool applyMeshTV = false;
#		endif
#	endif

#	if defined(TERRAIN_VARIATION_MESH) && !defined(LANDSCAPE)
#		define MESH_TV_SAMPLE(DEST, TEX, SAMP, UV)                     \
			{                                                           \
				[branch] if (applyMeshTV)                               \
				{                                                       \
					DEST = StochasticEffect(TEX, SAMP, UV, meshOffset); \
				}                                                       \
				else                                                    \
				{                                                       \
					DEST = TEX.Sample(SAMP, UV);                        \
				}                                                       \
			}
#		define MESH_TV_SAMPLE_BIAS(DEST, TEX, SAMP, UV)                                         \
			{                                                                                    \
				[branch] if (applyMeshTV)                                                        \
				{                                                                                \
					DEST = StochasticEffectBias(TEX, SAMP, UV, meshOffset, SharedData::MipBias); \
				}                                                                                \
				else                                                                             \
				{                                                                                \
					DEST = TEX.SampleBias(SAMP, UV, SharedData::MipBias);                        \
				}                                                                                \
			}
#		define MESH_TV_HEIGHT(DEST, TEX, SAMP, UV, MIP, CHANNEL)                            \
			{                                                                                \
				[branch] if (applyMeshTV)                                                    \
				{                                                                            \
					DEST = StochasticHeightChannel(TEX, SAMP, UV, MIP, CHANNEL, meshOffset); \
				}                                                                            \
				else                                                                         \
				{                                                                            \
					DEST = TEX.SampleLevel(SAMP, UV, MIP)[CHANNEL];                          \
				}                                                                            \
			}
#	else
#		define MESH_TV_SAMPLE(DEST, TEX, SAMP, UV) DEST = TEX.Sample(SAMP, UV)
#		define MESH_TV_SAMPLE_BIAS(DEST, TEX, SAMP, UV) DEST = TEX.SampleBias(SAMP, UV, SharedData::MipBias)
#		define MESH_TV_HEIGHT(DEST, TEX, SAMP, UV, MIP, CHANNEL) DEST = TEX.SampleLevel(SAMP, UV, MIP)[CHANNEL]
#	endif

#	if defined(EMAT)
	float parallaxShadowQuality = viewPosition.z < ExtendedMaterials::ParallaxCheapDistance ? ExtendedMaterials::ParallaxNearShadowQuality : ExtendedMaterials::ParallaxFarShadowQuality;
	float terrainDirectionalShadowQuality = parallaxShadowQuality;
#		define COMPUTE_TERRAIN_SHADOW_BASE(OUT_SH0) ExtendedMaterials::ComputeTerrainParallaxShadowBaseHeight(input, uv, terrainShadowMipLevels, terrainDirectionalShadowQuality, screenNoise, displacementParams, sharedOffset, OUT_SH0)
#		define EVAL_TERRAIN_DIR_SHADOW(BASE_SH0, DIR_TS) ExtendedMaterials::EvaluateTerrainDirectionalParallaxShadowMultiplier(input, uv, terrainShadowMipLevels, DIR_TS, terrainDirectionalShadowQuality, screenNoise, displacementParams, sharedOffset, BASE_SH0)
#		if defined(LANDSCAPE)
#			if defined(TRUE_PBR)
#				define LANDSCAPE_PARALLAX_ENABLED (SharedData::extendedMaterialSettings.EnableParallax)
#			else
#				define LANDSCAPE_PARALLAX_ENABLED                                 \
					(SharedData::extendedMaterialSettings.EnableTerrainParallax || \
						(SharedData::extendedMaterialSettings.EnableParallax && ((Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::THLandHasDisplacement) != 0)))
#			endif
#		endif
#	endif

#	if defined(LANDSCAPE)
#		if defined(TERRAIN_VARIATION) || defined(EMAT)
#			if defined(TERRAIN_VARIATION)
	StochasticOffsets sharedOffset = ComputeStochasticOffsets(input.TexCoord0.zw, input.Position.xy);
#			else
	StochasticOffsets sharedOffset = (StochasticOffsets)0;
#			endif
#		endif
#		if defined(EMAT)
	float mipLevels[6];
	float terrainShadowMipLevels[6];
	float cachedDirectionalTerrainParallaxShadow = 1.0;
	bool hasCachedDirectionalTerrainParallaxShadow = false;
	bool hasCachedTerrainShadowBaseHeight = false;
	bool hasTerrainParallaxShadow = false;
#		endif
#	else
	float mipLevel = 0;
#	endif  // LANDSCAPE
	float sh0 = 0;
	float pixelOffset = 0;

#	if defined(EMAT)
#		if defined(LANDSCAPE)
	DisplacementParams displacementParams[6];
	displacementParams[0].DisplacementScale = 1.f;
	displacementParams[0].DisplacementOffset = 0.f;
	displacementParams[0].HeightScale = 1;
	displacementParams[0].FlattenAmount = 0;
#		else
	DisplacementParams displacementParams;
	displacementParams.DisplacementScale = 1.f;
	displacementParams.DisplacementOffset = 0.f;
	displacementParams.HeightScale = 1;
	displacementParams.FlattenAmount = 0;
#		endif

#	endif

	float curvature = 0;
	float normalSmoothness = 0;

#	if !defined(MODELSPACENORMALS)
	float3 vertexNormal = tbnTr[2];
#		if defined(EMAT)

	if (SharedData::extendedMaterialSettings.EnableParallaxWarpingFix
#			if defined(LANDSCAPE)
	    && LANDSCAPE_PARALLAX_ENABLED
#			endif
	) {
		float3 ndx = ddx(vertexNormal);
		float3 ndy = ddy(vertexNormal);
		float3 fdx = ddx(input.WorldPosition.xyz);
		float3 fdy = ddy(input.WorldPosition.xyz);
		float fragSize = rcp(length(max(abs(fdx), abs(fdy))));
		float normalDelta = length(max(abs(ndx), abs(ndy))) * fragSize;
		float3 flatWorldNormal = normalize(-cross(fdx, fdy));
		normalSmoothness = saturate(1.0 - dot(vertexNormal, flatWorldNormal));
#			if defined(LANDSCAPE)
		curvature = sqrt(normalDelta);
		displacementParams[0].HeightScale = saturate(1.0 - curvature);
		displacementParams[0].FlattenAmount = (normalSmoothness + curvature);
#			else
		// Objects: do not crush HeightScale (that flattens convex faces you are looking at).
		// Only add FlattenAmount past the facing hemisphere (grazing > facing) and only if bent.
		// Flat walls stay at bend ≈ 0. Flatten tracks UV stretch (÷ facing).
		curvature = normalDelta;
		float facing = saturate(dot(vertexNormal, viewDirection));
		float grazing = 1.0 - facing;
		float bend = saturate(normalSmoothness + curvature);
		float silhouette = saturate(grazing - facing);
		// Cap FlattenAmount: ÷facing tracks UV stretch but can explode at grazing (~14× at the 0.0625 floor).
		displacementParams.FlattenAmount = saturate(silhouette * bend * rcp(max(facing, 0.0625)));
#			endif
	}
#		endif
#	endif

	float3 entryNormal = 0;
	float3 entryNormalTS = 0;
	float eta = 1;
	float3 refractedViewDirection = viewDirection;
	float4 sampledCoatColor = PBRParams2;
	float3 complexSpecular = 1.0;  // Declare complexSpecular at a higher scope so it's available throughout the shader (NEEDED FOR STOCH. FIX)

#	if defined(EMAT)
#		if defined(PARALLAX) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
	if (SharedData::extendedMaterialSettings.EnableParallax) {
		mipLevel = ExtendedMaterials::GetMipLevel(uv, TexParallaxSampler);
		uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, viewDirection, tbnTr, screenNoise, TexParallaxSampler, SampParallaxSampler, 0, displacementParams, applyMeshTV, meshOffset, pixelOffset);
		if (SharedData::extendedMaterialSettings.EnableShadows && parallaxShadowQuality > 0.0) {
			MESH_TV_HEIGHT(sh0, TexParallaxSampler, SampParallaxSampler, uv, mipLevel, 0);
		}
	}
#		endif  // defined(PARALLAX) && (defined(SKINNED) || !defined(MODELSPACENORMALS))

	bool complexMaterial = false;
	bool complexMaterialParallax = false;
	float4 complexMaterialColor = 1.0;

#		if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	float4 envMaskSample;
	MESH_TV_SAMPLE(envMaskSample, TexEnvMaskSampler, SampEnvMaskSampler, uv);
	float envMaskBase = envMaskSample.x;
	if (SharedData::extendedMaterialSettings.EnableComplexMaterial) {
		const float kMaskEpsilon = (4.0 / 255.0);

		const float4 mipSample = TexEnvMaskSampler.SampleLevel(SampEnvMaskSampler, uv, 15);
		complexMaterial = mipSample.w < (1.0 - kMaskEpsilon);

		const bool grayscaleMask = (abs(mipSample.x - mipSample.y) < kMaskEpsilon) &&
		                           (abs(mipSample.x - mipSample.z) < kMaskEpsilon) &&
		                           (abs(mipSample.y - mipSample.z) < kMaskEpsilon);
		// Preserve height-only masks while rejecting grayscale environment masks
		const bool solidBlackHeightMask = all(mipSample.xyz < kMaskEpsilon) &&
		                                  mipSample.w > kMaskEpsilon &&
		                                  mipSample.w < (1.0 - kMaskEpsilon);
		if (grayscaleMask && !solidBlackHeightMask)
			complexMaterial = false;

		if (complexMaterial) {
			if (envMaskSample.w > kMaskEpsilon && envMaskSample.w < (1.0 - kMaskEpsilon)) {
				complexMaterialParallax = true;
				mipLevel = ExtendedMaterials::GetMipLevel(uv, TexEnvMaskSampler);
				uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, viewDirection, tbnTr, screenNoise, TexEnvMaskSampler, SampTerrainParallaxSampler, 3, displacementParams, applyMeshTV, meshOffset, pixelOffset);
				if (SharedData::extendedMaterialSettings.EnableShadows && parallaxShadowQuality > 0.0) {
					MESH_TV_HEIGHT(sh0, TexEnvMaskSampler, SampEnvMaskSampler, uv, mipLevel, 3);
				}
				MESH_TV_SAMPLE(complexMaterialColor, TexEnvMaskSampler, SampEnvMaskSampler, uv);
			} else {
				complexMaterialColor = envMaskSample;
			}
			envMaskBase = complexMaterialColor.x;
		}
	}
#		endif  // ENVMAP

#		if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	bool PBRParallax = false;
	[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture0) != 0)
	{
		float4 sampledCoatProperties;
		MESH_TV_SAMPLE(sampledCoatProperties, TexRimSoftLightWorldMapOverlaySampler, SampRimSoftLightWorldMapOverlaySampler, uv);
		sampledCoatColor.rgb *= Color::Albedo(Color::LinearSRGBToWorking(sampledCoatProperties.rgb));
		sampledCoatColor.a *= sampledCoatProperties.a;
	}
#			if !defined(FACEGEN)
	[branch] if (SharedData::extendedMaterialSettings.EnableParallax && (PBRFlags & PBR::Flags::HasDisplacement) != 0)
	{
		PBRParallax = true;
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			displacementParams.HeightScale *= PBRParams1.y;
			displacementParams.DisplacementScale = 0.5;
			displacementParams.DisplacementOffset = -0.25;

			eta = lerp(1.0, (1 - sqrt(MultiLayerParallaxData.y)) / (1 + sqrt(MultiLayerParallaxData.y)), sampledCoatColor.w);
			[branch] if ((PBRFlags & PBR::Flags::CoatNormal) != 0)
			{
				entryNormalTS = normalize(TransformNormal(TexBackLightSampler.Sample(SampBackLightSampler, uvOriginal).xyz));
			}
			else
			{
				entryNormalTS = normalize(TransformNormal(TexNormalSampler.Sample(SampNormalSampler, uvOriginal).xyz));
			}
			entryNormal = normalize(mul(tbn, entryNormalTS));
			refractedViewDirection = -refract(-viewDirection, entryNormal, eta);
		}
		else
		{
			displacementParams.HeightScale *= PBRParams1.y;
		}
		mipLevel = ExtendedMaterials::GetMipLevel(uv, TexParallaxSampler);
		uv = ExtendedMaterials::GetParallaxCoords(viewPosition.z, uv, mipLevel, refractedViewDirection, tbnTr, screenNoise, TexParallaxSampler, SampParallaxSampler, 0, displacementParams, applyMeshTV, meshOffset, pixelOffset);
		if (SharedData::extendedMaterialSettings.EnableShadows && parallaxShadowQuality > 0.0) {
			MESH_TV_HEIGHT(sh0, TexParallaxSampler, SampParallaxSampler, uv, mipLevel, 0);
		}
	}
#			endif  // !FACEGEN
#		endif      // TRUE_PBR

#	elif defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	float4 envMaskSample;
	MESH_TV_SAMPLE(envMaskSample, TexEnvMaskSampler, SampEnvMaskSampler, uv);
	float envMaskBase = envMaskSample.x;
#	endif  // EMAT

#	if defined(SNOW)
	bool useSnowSpecular = true;
#	else
	bool useSnowSpecular = false;
#	endif  // SNOW

#	if defined(SPARKLE) || !defined(PROJECTED_UV)
	bool useSnowDecalSpecular = true;
#	else
	bool useSnowDecalSpecular = false;
#	endif  // defined(SPARKLE) || !defined(PROJECTED_UV)

	float2 diffuseUv = uv;

#	if defined(SPARKLE)
	diffuseUv = ProjectedUVParams2.yy * input.TexCoord0.zw;
#	endif  // SPARKLE

#	if defined(LANDSCAPE)
	// Normalise blend weights
	float totalWeight = input.LandBlendWeights1.x + input.LandBlendWeights1.y + input.LandBlendWeights1.z +
	                    input.LandBlendWeights1.w + input.LandBlendWeights2.x + input.LandBlendWeights2.y;
#		if defined(LANDSCAPE_SEAMS)
	totalWeight += dot(LandscapeSeams::ExtraWeights, 1.0);
	if (totalWeight > 0.0)
		LandscapeSeams::ExtraWeights /= totalWeight;
#		endif
	if (totalWeight > 0.0) {
		input.LandBlendWeights1 /= totalWeight;
		input.LandBlendWeights2.xy /= totalWeight;
	}
	float3 blendedRGB = 0;
	float blendedAlpha = 0;
	float3 blendedNormalRGB = 0;
	float blendedNormalAlpha = 0;

#		if defined(TRUE_PBR)
	float4 blendedRMAOS = 0;
#		endif

#		if defined(EMAT)
	if (LANDSCAPE_PARALLAX_ENABLED) {
		float terrainMaxTexDim = 0.0;
		ExtendedMaterials::InitializeTerrainMipLevels(uv, mipLevels, terrainMaxTexDim);
		[unroll] for (uint terrainMipIndex = 0; terrainMipIndex < 6; terrainMipIndex++)
		{
			terrainShadowMipLevels[terrainMipIndex] = min(mipLevels[terrainMipIndex], ExtendedMaterials::TerrainParallaxShadowMaxMipLevel);
		}

		displacementParams[1] = displacementParams[0];
		displacementParams[2] = displacementParams[0];
		displacementParams[3] = displacementParams[0];
		displacementParams[4] = displacementParams[0];
		displacementParams[5] = displacementParams[0];
#			if defined(TRUE_PBR)
		displacementParams[0].HeightScale *= PBRParams1.y;
		displacementParams[1].HeightScale *= LandscapeTexture2PBRParams.y;
		displacementParams[2].HeightScale *= LandscapeTexture3PBRParams.y;
		displacementParams[3].HeightScale *= LandscapeTexture4PBRParams.y;
		displacementParams[4].HeightScale *= LandscapeTexture5PBRParams.y;
		displacementParams[5].HeightScale *= LandscapeTexture6PBRParams.y;
#			endif

		float weights[6];
		weights[0] = weights[1] = weights[2] = weights[3] = weights[4] = weights[5] = 0.0;

		const bool doTerrainPom = ExtendedMaterials::TerrainHasAnyDisplacement() &&
		                          ExtendedMaterials::TerrainMaxWeightedHeightScale(input, displacementParams) > 0.01;
		[branch] if (doTerrainPom)
		{
			uv = ExtendedMaterials::GetParallaxCoords(input, viewPosition.z, uv, mipLevels, terrainMaxTexDim, viewDirection, tbnTr, screenNoise, displacementParams, sharedOffset, pixelOffset, weights);
		}
		else if (SharedData::extendedMaterialSettings.EnableHeightBlending)
		{
			float unusedHeight;
			unusedHeight = ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, displacementParams, 1.0, input.LandBlendWeights1, input.LandBlendWeights2.xy, sharedOffset, weights);
		}

		if (SharedData::extendedMaterialSettings.EnableHeightBlending) {
			input.LandBlendWeights1.x = weights[0];
			input.LandBlendWeights1.y = weights[1];
			input.LandBlendWeights1.z = weights[2];
			input.LandBlendWeights1.w = weights[3];
			input.LandBlendWeights2.x = weights[4];
			input.LandBlendWeights2.y = weights[5];
#			if defined(LANDSCAPE_SEAMS)
			// Height blending covers only the engine layers; scale them back to their share beside the borrowed ones.
			const float engineWeight = dot(input.LandBlendWeights1, 1.0) + input.LandBlendWeights2.x + input.LandBlendWeights2.y;
			if (engineWeight > 0.0) {
				const float engineScale = saturate(1.0 - dot(LandscapeSeams::ExtraWeights, 1.0)) / engineWeight;
				input.LandBlendWeights1 *= engineScale;
				input.LandBlendWeights2.xy *= engineScale;
			}
#			endif
		}
		hasTerrainParallaxShadow =
			viewPosition.z < ExtendedMaterials::ParallaxCheapDistance &&
			ExtendedMaterials::TerrainHasAnyDisplacement() &&
			ExtendedMaterials::TerrainMaxWeightedHeightScale(input, displacementParams) > 0.01;
		// sh0 feeds point-light terrain shadows (hasTerrainParallaxShadow), not only POM.
		if ((doTerrainPom || hasTerrainParallaxShadow) && SharedData::extendedMaterialSettings.EnableShadows && terrainDirectionalShadowQuality > 0.0) {
			hasCachedTerrainShadowBaseHeight = COMPUTE_TERRAIN_SHADOW_BASE(sh0);
			if (doTerrainPom && hasCachedTerrainShadowBaseHeight) {
				float3 dirLightDirectionTS = mul(DirLightDirection, tbn).xyz;
				cachedDirectionalTerrainParallaxShadow = EVAL_TERRAIN_DIR_SHADOW(sh0, dirLightDirectionTS);
				hasCachedDirectionalTerrainParallaxShadow = true;
			}
		}
	}
#		endif  // EMAT
#	endif      // LANDSCAPE

#	if defined(SPARKLE)
	diffuseUv = ProjectedUVParams2.yy * (input.TexCoord0.zw + (uv - uvOriginal));
#	else
	diffuseUv = uv;
#	endif  // SPARKLE

	float4 baseColor = 0;
	float4 normal = 0;
	float glossiness = 0;
#	if defined(CS_SKIN)
	const bool skinEnabled = SharedData::skinData.skinParams.w > 0.0f;
#		if defined(CS_SKIN_SHADING)
	float skinRoughness = 0;
	float skinSpecular = 0;
	float skinFuzzMask = 1;
	float skinWetMask = 1;
	float skinAO = 1;
	bool skinRoughnessSet = false;
#		endif
#	endif

	float4 rawRMAOS = 0;

	float4 glintParameters = 0;

#	if defined(SNOW)
#		if !defined(TRUE_PBR)
	float landSnowMask = 0.0;
#			if defined(LANDSCAPE)
	landSnowMask = GetLandSnowMaskValue(baseColor.w);
#			endif
#		endif
#	endif

#	if defined(LANDSCAPE)
#		if defined(TERRAIN_VARIATION)
#			define SampleTerrain(TEX, SAMP, UV, OFFSET) StochasticEffectBias(TEX, SAMP, UV, OFFSET, SharedData::MipBias)
#			define SampleTerrainColor(TEX, SAMP, UV, OFFSET, PBR) StochasticEffectBias(TEX, SAMP, UV, OFFSET, SharedData::MipBias, true, PBR)
#		else
#			define SampleTerrain(TEX, SAMP, UV, OFFSET) TEX.SampleBias(SAMP, UV, SharedData::MipBias)
#			define SampleTerrainColor(TEX, SAMP, UV, OFFSET, PBR) ColorManagement::DiffuseToWorking(TEX.SampleBias(SAMP, UV, SharedData::MipBias), PBR)
#		endif
#		if defined(TRUE_PBR)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER_PBR(0, TexColorSampler, SampColorSampler, TexNormalSampler, SampNormalSampler, TexRMAOSSampler, SampRMAOSSampler, PBRParams1, LandscapeTexture1GlintParameters, input.LandBlendWeights1.x)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER_PBR(1, TexLandColor2Sampler, SampLandColor2Sampler, TexLandNormal2Sampler, SampLandNormal2Sampler, TexLandRMAOS2Sampler, SampLandRMAOS2Sampler, LandscapeTexture2PBRParams, LandscapeTexture2GlintParameters, input.LandBlendWeights1.y)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER_PBR(2, TexLandColor3Sampler, SampLandColor3Sampler, TexLandNormal3Sampler, SampLandNormal3Sampler, TexLandRMAOS3Sampler, SampLandRMAOS3Sampler, LandscapeTexture3PBRParams, LandscapeTexture3GlintParameters, input.LandBlendWeights1.z)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER_PBR(3, TexLandColor4Sampler, SampLandColor4Sampler, TexLandNormal4Sampler, SampLandNormal4Sampler, TexLandRMAOS4Sampler, SampLandRMAOS4Sampler, LandscapeTexture4PBRParams, LandscapeTexture4GlintParameters, input.LandBlendWeights1.w)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER_PBR(4, TexLandColor5Sampler, SampLandColor5Sampler, TexLandNormal5Sampler, SampLandNormal5Sampler, TexLandRMAOS5Sampler, SampLandRMAOS5Sampler, LandscapeTexture5PBRParams, LandscapeTexture5GlintParameters, input.LandBlendWeights2.x)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER_PBR(5, TexLandColor6Sampler, SampLandColor6Sampler, TexLandNormal6Sampler, SampLandNormal6Sampler, TexLandRMAOS6Sampler, SampLandRMAOS6Sampler, LandscapeTexture6PBRParams, LandscapeTexture6GlintParameters, input.LandBlendWeights2.y)
#		else
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER(0, TexColorSampler, SampColorSampler, TexNormalSampler, SampNormalSampler, input.LandBlendWeights1.x, LandscapeTexture1to4IsSnow.x)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER(1, TexLandColor2Sampler, SampLandColor2Sampler, TexLandNormal2Sampler, SampLandNormal2Sampler, input.LandBlendWeights1.y, LandscapeTexture1to4IsSnow.y)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER(2, TexLandColor3Sampler, SampLandColor3Sampler, TexLandNormal3Sampler, SampLandNormal3Sampler, input.LandBlendWeights1.z, LandscapeTexture1to4IsSnow.z)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER(3, TexLandColor4Sampler, SampLandColor4Sampler, TexLandNormal4Sampler, SampLandNormal4Sampler, input.LandBlendWeights1.w, LandscapeTexture1to4IsSnow.w)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER(4, TexLandColor5Sampler, SampLandColor5Sampler, TexLandNormal5Sampler, SampLandNormal5Sampler, input.LandBlendWeights2.x, LandscapeTexture5to6IsSnow.x)
	LIGHTING_LANDSCAPE_BLEND_ONE_LAYER(5, TexLandColor6Sampler, SampLandColor6Sampler, TexLandNormal6Sampler, SampLandNormal6Sampler, input.LandBlendWeights2.y, LandscapeTexture5to6IsSnow.y)
#		endif
#		if defined(LANDSCAPE_SEAMS)
	LANDSCAPE_SEAMS_BLEND_EXTRAS
#		endif
#		undef SampleTerrain
#		undef SampleTerrainColor

	float4 sampledBaseColor = float4(blendedRGB, blendedAlpha);
	baseColor = float4(Color::Albedo(blendedRGB), blendedAlpha);
	normal = float4(blendedNormalRGB, blendedNormalAlpha);
#		if defined(TRUE_PBR)
	rawRMAOS = blendedRMAOS;
#		endif
#	else  // Non-landscape code
	float4 sampledBaseColor;
	MESH_TV_SAMPLE_BIAS(sampledBaseColor, TexColorSampler, SampColorSampler, diffuseUv);
#		if defined(ENABLE_LL) && defined(FACEGEN)
	sampledBaseColor.rgb = GetFacegenBaseColor(sampledBaseColor.rgb, uv);
#		elif defined(ENABLE_LL) && defined(FACEGEN_RGB_TINT)
	sampledBaseColor.rgb = GetFacegenRGBTintBaseColor(sampledBaseColor.rgb, uv);
#		endif
	baseColor = Permutation::BaseTextureIsWorking ? sampledBaseColor : ColorManagement::DiffuseToWorking(sampledBaseColor, PBRTextures);
	baseColor.rgb = Color::Albedo(baseColor.rgb);
	float4 normalColor;
	MESH_TV_SAMPLE_BIAS(normalColor, TexNormalSampler, SampNormalSampler, uv);
	normal = normalColor;
#		if defined(TRUE_PBR)
	MESH_TV_SAMPLE_BIAS(rawRMAOS, TexRMAOSSampler, SampRMAOSSampler, diffuseUv);
	rawRMAOS *= float4(PBRParams1.x, 1, 1, PBRParams1.z);
	if ((PBRFlags & PBR::Flags::Glint) != 0) {
		glintParameters = MultiLayerParallaxData;
	}
#		endif
#	endif

#	if defined(LOD_BLENDING)
#		if defined(LODOBJECTS) || defined(LODOBJECTSHD)
	baseColor.xyz = pow(abs(baseColor.xyz), SharedData::lodBlendingSettings.LODObjectGamma) * SharedData::lodBlendingSettings.LODObjectBrightness;
#		elif defined(LODLANDSCAPE)
#			if defined(TERRAIN_VARIATION)
	[branch] if (SharedData::terrainVariationSettings.enableLODTerrainTilingFix)
	{
		float4 sampledLodColor;
		float4 lodStochasticColor = StochasticSampleLOD(screenNoise, TexColorSampler, SampColorSampler, uv, sampledLodColor);
#				if defined(ENABLE_LL)
		sampledBaseColor = sampledLodColor;
#				endif
		baseColor.xyz = Color::Albedo(lodStochasticColor.rgb);
	}
#			endif
	baseColor.xyz = pow(abs(baseColor.xyz), SharedData::lodBlendingSettings.LODTerrainGamma) * SharedData::lodBlendingSettings.LODTerrainBrightness;
#		endif
#	endif  // LOD_BLENDING

#	if defined(CS_SKIN_SHADING)
	float4 skinsk = 0;
	float4 skinExtra = 0;
	float4 skinWetnessSample = 0;
	uint2 skinExtraDimensions = uint2(0, 0);
	uint2 wetnessDimensions = uint2(0, 0);
	bool hasSkinExtra = false;
	bool hasSkinWetness = false;
	if (skinEnabled) {
		skinsk = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, uv);
		TexSkinExtraSampler.GetDimensions(skinExtraDimensions.x, skinExtraDimensions.y);
		TexSkinWetnessSampler.GetDimensions(wetnessDimensions.x, wetnessDimensions.y);
		hasSkinExtra = skinExtraDimensions.x > 32 && skinExtraDimensions.y > 32;
		hasSkinWetness = wetnessDimensions.x > 32 && wetnessDimensions.y > 32;
	}
	float4 skinWetnessNormal = float4(0.f, 0.f, 0.f, 1.f);

	if (hasSkinExtra && SharedData::skinData.skinParams.x > 0.0f) {
		skinExtra = TexSkinExtraSampler.Sample(SampColorSampler, uv);
		skinRoughness = skinExtra.x;
		skinFuzzMask = skinExtra.y;
		skinAO = skinExtra.z;
		skinSpecular = skinExtra.w;
		skinRoughnessSet = true;
	} else {
		skinRoughnessSet = false;
	}
	if (hasSkinWetness && skinEnabled) {
		skinWetnessSample = TexSkinWetnessSampler.Sample(SampColorSampler, uv);
		if ((skinWetnessSample.y == 0 && skinWetnessSample.z == 0) || (skinWetnessSample.x == skinWetnessSample.y && skinWetnessSample.y == skinWetnessSample.z && skinWetnessSample.w >= 0.99f)) {
			skinWetMask = skinWetnessSample.x;
			skinWetnessNormal.xyz = CalculateNormalFromHeight(skinWetMask, SharedData::skinData.wetParams.w * 0.0001, uv) * 0.5 + 0.5;
		} else {
			skinWetnessNormal.xyz = skinWetnessSample.xyz;
			skinWetMask = skinWetnessSample.w;
		}
	} else {
		skinWetMask = 1.0;
	}
#	endif

	float landSnowMask1 = GetLandSnowMaskValue(baseColor.w);

#	if defined(MODELSPACENORMALS)
#		if defined(LODLANDNOISE)
	normal.xyz = normal.xzy - 0.5.xxx;
#			if defined(ENABLE_LL)
	float lodLandNoiseParameter = GetLodLandBlendParameter(sampledBaseColor.xyz);
#			else
	float lodLandNoiseParameter = GetLodLandBlendParameter(baseColor.xyz);
#			endif
	float noise = TexLandLodNoiseSampler.Sample(SampLandLodNoiseSampler, uv * 3.0.xx).x;
	float lodLandNoiseMultiplier = GetLodLandBlendMultiplier(lodLandNoiseParameter, noise);
	baseColor.xyz *= lodLandNoiseMultiplier;
	normal.xyz *= 2;
	normal.w = 1;
	glossiness = 0;
#		elif defined(LODLANDSCAPE)
	normal.xyz = 2.0.xxx * (-0.5.xxx + normal.xzy);
	normal.w = 1;
	glossiness = 0;
#		else
	normal.xyz = normal.xzy * 2.0.xxx + -1.0.xxx;
	normal.w = 1;
	glossiness = TexSpecularSampler.Sample(SampSpecularSampler, uv).x;
#		endif  // LODLANDNOISE
#	elif (defined(SNOW) && defined(LANDSCAPE))
	normal.xyz = GetLandNormal(landSnowMask1, normal.xyz, uv, SampNormalSampler, TexNormalSampler);
	glossiness = normal.w;
#	else
	normal.xyz = TransformNormal(normal.xyz);
	glossiness = normal.w;
#	endif  // MODELSPACENORMALS

#	if defined(WORLD_MAP)
	normal.xyz = GetWorldMapNormal(input, normal.xyz, sampledBaseColor.xyz);
#	endif  // WORLD_MAP

#	if defined(LANDSCAPE)
#		if defined(SNOW) && !defined(TRUE_PBR)
	landSnowMask = LandscapeTexture1to4IsSnow.x * input.LandBlendWeights1.x;
#		endif  // SNOW
#	endif      // LANDSCAPE

#	if defined(EMAT_ENVMAP)
	complexMaterial = complexMaterial && complexMaterialColor.y > (4.0 / 255.0);
	shininess = lerp(shininess, shininess * complexMaterialColor.y, complexMaterial);
	if (complexMaterial) {
		complexSpecular = lerp(1.0, baseColor.xyz, complexMaterialColor.z);
		baseColor.xyz = lerp(baseColor.xyz, 0.0, complexMaterialColor.z);
	}
#	endif  // defined (EMAT) && defined(ENVMAP)

#	if defined(FACEGEN) && !defined(ENABLE_LL)
	baseColor.xyz = GetFacegenBaseColor(baseColor.xyz, uv);
#	elif defined(FACEGEN_RGB_TINT) && !defined(ENABLE_LL)
	baseColor.xyz = GetFacegenRGBTintBaseColor(baseColor.xyz, uv);
#	endif  // FACEGEN

#	if defined(CS_SKIN_SHADING)
	if (skinEnabled) {
		baseColor.xyz = baseColor.xyz * SharedData::skinData.skinParams2.w;
	}
#	endif  // CS_SKIN

#	if defined(CS_HAIR_SHADING)
	float3 hairTint = 0;

	if (SharedData::hairSpecularSettings.Enabled) {
		hairTint = ColorManagement::SRGBToWorking(lerp(1, TintColor.xyz, input.Color.y));
		baseColor.xyz *= hairTint;
		baseColor.xyz = Hair::Saturation(baseColor.xyz, SharedData::hairSpecularSettings.HairSaturation);
		baseColor.xyz *= SharedData::hairSpecularSettings.BaseColorMult;
		baseColor.xyz = SharedData::hairSpecularSettings.HairMode == 1 ? baseColor.xyz * baseColor.xyz : baseColor.xyz;  // To match color for Marschner
	}

	float3 sampledHairFlow = 0;
	bool useHairFlowMap = false;
	if (HasBackLighting() && SharedData::hairSpecularSettings.Enabled) {
		uint2 hairFlowDimensions = uint2(0, 0);
		sampledHairFlow = float3(TexBackLightSampler.Sample(SampBackLightSampler, uv).xy, 0.5f);
		TexBackLightSampler.GetDimensions(hairFlowDimensions.x, hairFlowDimensions.y);
		useHairFlowMap = (sampledHairFlow.x > 0.0 || sampledHairFlow.y > 0.0) && hairFlowDimensions.x > 32 && hairFlowDimensions.y > 32;
		sampledHairFlow = useHairFlowMap ? sampledHairFlow * 2.0f - 1.0f : float3(0.5f, 0.5f, 0.5f);
	}
#	endif

#	if defined(LOD_LAND_BLEND)
	float4 lodLandColor;
	float4 sampledLodLandColor;

#		if defined(TERRAIN_VARIATION)
	float2 blendColorUV = input.TexCoord0.zw;
	[branch] if (SharedData::terrainVariationSettings.enableLODTerrainTilingFix)
		lodLandColor = StochasticSampleLOD(screenNoise, TexLandLodBlend1Sampler, SampLandLodBlend1Sampler, blendColorUV, sampledLodLandColor);
	else
	{
		sampledLodLandColor = TexLandLodBlend1Sampler.SampleBias(SampLandLodBlend1Sampler, blendColorUV, SharedData::MipBias);
		lodLandColor = ColorManagement::DiffuseToWorking(sampledLodLandColor);
	}
#		else
	sampledLodLandColor = TexLandLodBlend1Sampler.Sample(SampLandLodBlend1Sampler, input.TexCoord0.zw);
	lodLandColor = ColorManagement::DiffuseToWorking(sampledLodLandColor);
#		endif
	// Matches the LOD terrain shader, which applies Color::Albedo to its non-PBR base color.
	lodLandColor.xyz = Color::LambertAlbedo(lodLandColor.xyz);

#		if defined(LOD_BLENDING)
	lodLandColor.xyz = pow(abs(lodLandColor.xyz), SharedData::lodBlendingSettings.LODTerrainGamma) * SharedData::lodBlendingSettings.LODTerrainBrightness;
#		endif  // LOD_BLENDING
#		if defined(ENABLE_LL)
	float lodBlendParameter = GetLodLandBlendParameter(sampledLodLandColor.xyz);
#		else
	float lodBlendParameter = GetLodLandBlendParameter(lodLandColor.xyz);
#		endif
	float lodBlendMask = TexLandLodBlend2Sampler.Sample(SampLandLodBlend2Sampler, 3.0.xx * input.TexCoord0.zw).x;
	float lodLandFadeFactor = GetLodLandBlendMultiplier(lodBlendParameter, lodBlendMask);
	float lodLandBlendFactor = LODTexParams.z * input.LandBlendWeights2.w;
	normal.xyz = lerp(normal.xyz, float3(0, 0, 1), lodLandBlendFactor);

#		if !defined(TRUE_PBR)
	baseColor.w = 0;
	baseColor = lerp(baseColor, lodLandColor * lodLandFadeFactor, lodLandBlendFactor);
	glossiness = lerp(glossiness, 0, lodLandBlendFactor);
#		endif
#	endif  // LOD_LAND_BLEND

#	if defined(DO_ALPHA_TEST) && !defined(SPARKLE) && !(defined(LANDSCAPE) && !defined(LOD_LAND_BLEND))
	float alpha = baseColor.w;
#		if defined(EMAT) && !defined(LANDSCAPE)
#			if defined(PARALLAX)
	alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
#			elif defined(TRUE_PBR)
	[branch] if (PBRParallax)
	{
		alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
	}
#			endif
#		endif
#		if defined(DO_ALPHA_TEST)
	[branch] if ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::AdditionalAlphaMask) != 0)
	{
		uint2 alphaMask = input.Position.xy;
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

		float testTmp = 0;
		if (MaterialData.z - maskValues[alphaMask.x] < 0) {
			discard;
		}
	}
	else
#		endif  // defined(DO_ALPHA_TEST)
	{
		alpha *= MaterialData.z;
	}
#		if !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
	alpha *= input.Color.w;
#		endif  // !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
#		if defined(DO_ALPHA_TEST)
#			if defined(DEPTH_WRITE_DECALS)
	if (alpha - 0.0156862754 < 0) {
		discard;
	}
	alpha = saturate(1.05 * alpha);
#			endif  // DEPTH_WRITE_DECALS
	if (alpha - AlphaTestRefRS < 0) {
		discard;
	}
#		endif  // DO_ALPHA_TEST
#	endif

#	if defined(SNOW) && !defined(TRUE_PBR)
	useSnowSpecular = landSnowMask != 0.0;
#	endif  // SNOW

	float4 backLightColor = 0.0;
	float4 rimSoftLightColor = 0.0;
#	if !defined(LANDSCAPE)
	[branch] if (HasBackLighting())
	{
		backLightColor = TexBackLightSampler.Sample(SampBackLightSampler, uv);
#		if defined(CS_HAIR_SHADING)
		if (useHairFlowMap) {
			backLightColor = 0.0f;
		}
#		endif
	}

	[branch] if (HasRimLighting() || HasSoftLighting())
	{
		rimSoftLightColor = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, uv);
	}
#	endif

	uint numLights = min(7, uint(NumLightNumShadowLight.x));
	uint numShadowLights = min(4, uint(NumLightNumShadowLight.y));

#	if defined(MODELSPACENORMALS) && !defined(SKINNED)
	float3 worldNormal = normal.xyz;
	float3x3 tbnTr = ReconstructTBN(input.WorldPosition.xyz, worldNormal, screenUV);
#	else
	float3 worldNormal = normalize(mul(tbn, normal.xyz));
#		if defined(TREE_ANIM) && !defined(TRUE_PBR)
	float3 viewNormal = normalize(FrameBuffer::WorldToView(worldNormal, false));
	viewNormal = float3(viewNormal.xy, -abs(viewNormal.z));
	worldNormal = normalize(FrameBuffer::ViewToWorld(viewNormal, false));
#		endif

#		if defined(SPARKLE)
	float3 projectedNormal = normalize(mul(tbn, float3(ProjectedUVParams2.xx * normal.xy, normal.z)));
#		endif  // SPARKLE
#	endif      // defined (MODELSPACENORMALS) && !defined (SKINNED)

#	if defined(CS_SKIN_SHADING)
#		if defined(WETNESS_EFFECTS)
	float3 skinWetNormal = worldNormal.xyz;
#			if defined(FACEGEN)
	float2 wetUV = uv;
#			else
	float2 wetUV = uv * SharedData::skinData.skinDetailParams.y;
#			endif
	float2 dynamicWet = Skin::GetWetness(input.WorldPosition.z + FrameBuffer::CameraPosAdjust.z, worldNormal.xyz);
	float skinWetness = Skin::PerlinNoise(wetUV, SharedData::skinData.wetParams.x, SharedData::skinData.wetParams.y, SharedData::skinData.wetParams.z, clamp(dynamicWet.x + dynamicWet.y + SharedData::skinData.skinParams2.y, 0.f, 2.f) * (hasSkinWetness ? 1.0 : 0.5));
	if ((SharedData::skinData.skinDetailParams.w > 0.0f || skinWetness > 0.0f) && skinEnabled)
#		else
	if (SharedData::skinData.skinDetailParams.w > 0.0f && skinEnabled)
#		endif
	{
#		if defined(FACEGEN)
		float2 detailUV = input.TexCoord0.xy * SharedData::skinData.skinDetailParams.x;
#		else
		float2 detailUV = input.TexCoord0.xy * SharedData::skinData.skinDetailParams.x * SharedData::skinData.skinDetailParams.y;
#		endif  // FACEGEN
#		if defined(MODELSPACENORMALS)
		const float3x3 tbnTr = ReconstructTBN(input.WorldPosition.xyz, worldNormal, screenUV);
		const float3x3 tbn = transpose(tbnTr);
		const float3 tangentNormal = mul(tbnTr, worldNormal.xyz);
#		else
		const float3 tangentNormal = normal.xyz;
#		endif  // MODELSPACENORMALS
		float3 detailNormal = float3(Skin::TexSkinDetailNormal.SampleBias(SampNormalSampler, detailUV, SharedData::MipBias - 1.0f).xy, 0.5f);
		skinAO *= Skin::TexSkinDetailNormal.Sample(SampNormalSampler, detailUV).w;
		detailNormal = (detailNormal * 2.0 - 1.0) * SharedData::skinData.skinDetailParams.z;
		float3 combinedTangentNormal = normalize(float3(ReorientNormal(detailNormal, tangentNormal).xy, tangentNormal.z));
		float3 combinedNormal = normalize(mul(tbn, combinedTangentNormal));
		if (SharedData::skinData.skinDetailParams.w > 0.0f)
			worldNormal.xyz = combinedNormal;
#		if defined(WETNESS_EFFECTS)
		if (skinWetness > 0.0f) {
			float3 wetNormal = CalculateNormalFromHeight(skinWetness, SharedData::skinData.wetParams.w * 0.0005, uv);
			if (hasSkinWetness) {
				float3 wetMaskNormal = (skinWetnessNormal.xyz * 2.0 - 1.0);
				wetNormal = ReorientNormal(wetMaskNormal, wetNormal);
			}
			if (SharedData::skinData.skinParams2.y > 1.0f) {
				wetNormal = lerp(wetNormal, tangentNormal, saturate(SharedData::skinData.skinParams2.y - 1.0f));
			}
			float3 combinedWetNormal = skinWetMask ? wetNormal : combinedTangentNormal;
			skinWetNormal = normalize(mul(tbn, combinedWetNormal));
			skinWetNormal = lerp(worldNormal.xyz, skinWetNormal, skinWetness > 0 ? 1 : 0);
		}
#		endif
	}
#	endif  // CS_SKIN

	float2 baseShadowUV = 1.0.xx;
	float4 shadowColor = 1.0;
	if ((PixelDescriptor() & Permutation::LightingFlags::DefShadow) && ((PixelDescriptor() & Permutation::LightingFlags::ShadowDir) || inWorld) || numShadowLights > 0) {
		baseShadowUV = input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy;
		float2 adjustedShadowUV = baseShadowUV * VPOSOffset.xy + VPOSOffset.zw;
		float2 shadowUV = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(adjustedShadowUV);
		shadowColor = TexShadowMaskSampler.Sample(SampShadowMaskSampler, shadowUV);
	}

	float projectedMaterialWeight = 0;

	float projWeight = 0;

#	if defined(PROJECTED_UV)
	float3 projWorldPos = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
	float3 triFaceNormal = normalize(-cross(ddx(input.WorldPosition.xyz), ddy(input.WorldPosition.xyz)));
	float3 triWeights = Triplanar::GetWeights(tbnTr[2], triFaceNormal);
#		if defined(DCLF_BINDLESS)
	float projNoise = (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::CharacterLight) ?
	                      Triplanar::Sample(DCLFCharacterLightNoise, SampCharacterLightProjNoiseSampler, projWorldPos, triWeights, ProjectedUVParams.z).x :
	                      Triplanar::Sample(TexCharacterLightProjNoiseSampler, SampCharacterLightProjNoiseSampler, projWorldPos, triWeights, ProjectedUVParams.z).x;
#		else
	float projNoise = Triplanar::Sample(TexCharacterLightProjNoiseSampler, SampCharacterLightProjNoiseSampler, projWorldPos, triWeights, ProjectedUVParams.z).x;
#		endif
	float3 texProj = normalize(input.TexProj);
#		if defined(TREE_ANIM) || defined(LODOBJECTSHD)
	float vertexAlpha = 1;
#		else
	float vertexAlpha = input.Color.w;
#		endif  // defined (TREE_ANIM) || defined (LODOBJECTSHD)
	projWeight = -ProjectedUVParams.x * projNoise + (dot(worldNormal.xyz, texProj) * vertexAlpha - ProjectedUVParams.w);
#		if defined(LODOBJECTSHD)
	projWeight += (-0.5 + input.Color.w) * 2.5;
#		endif  // LODOBJECTSHD
#		if defined(SPARKLE)
	if (projWeight < 0)
		discard;

	sampledBaseColor = Triplanar::SampleStochasticBias(TexColorSampler, SampColorSampler, projWorldPos, triWeights, ProjectedUVParams2.y, SharedData::MipBias, screenNoise);
	baseColor = Permutation::BaseTextureIsWorking ? sampledBaseColor : ColorManagement::DiffuseToWorking(sampledBaseColor, PBRTextures);
	baseColor.rgb = Color::Albedo(baseColor.rgb);
	worldNormal.xyz = projectedNormal;
#		elif !defined(FACEGEN) && !defined(MULTI_LAYER_PARALLAX) && !defined(PARALLAX) && !defined(SPARKLE)
	if (ProjectedUVParams3.w > 0.5) {
		float diffuseNormalScale = ProjectedUVParams3.x * ProjectedUVParams.z;
		float3 projNormal = TransformNormal(Triplanar::SampleStochastic(TexProjNormalSampler, SampProjNormalSampler, projWorldPos, triWeights, diffuseNormalScale, screenNoise).xyz);
		float detailNormalScale = ProjectedUVParams3.y * ProjectedUVParams.z;
		float3 projDetailNormal = Triplanar::SampleStochastic(TexProjDetail, SampProjDetailSampler, projWorldPos, triWeights, detailNormalScale, screenNoise).xyz;
		float3 finalProjNormal = normalize(TransformNormal(projDetailNormal) * float3(1, 1, projNormal.z) + float3(projNormal.xy, 0));
		float3 projBaseColor = Triplanar::SampleStochastic(TexProjDiffuseSampler, SampProjDiffuseSampler, projWorldPos, triWeights, diffuseNormalScale, screenNoise).xyz;
#			if defined(ENABLE_LL)
		bool projectedTextureIsSRGB = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::ProjectedTextureIsSRGB) != 0;
#				if defined(TRUE_PBR)
		projBaseColor = projectedTextureIsSRGB ? projBaseColor : TransferFunctions::SRGBToLinear(projBaseColor);
#				else
		projBaseColor = ColorManagement::DiffuseToWorking(projBaseColor, projectedTextureIsSRGB);
#				endif
#			endif
#			if defined(TRUE_PBR)
#				if defined(ENABLE_LL)
		projBaseColor *= ProjectedUVParams2.xyz * max(0, MaterialObjectRGBScale);
		projBaseColor = Color::LinearSRGBToWorking(max(0, projBaseColor));
#				else
		projBaseColor *= Color::LinearSRGBToWorking(ProjectedUVParams2.xyz);
		projBaseColor = max(0, projBaseColor.xyz * MaterialObjectRGBScale);
#				endif
#			else
		projBaseColor *= ColorManagement::SRGBToWorking(ProjectedUVParams2.xyz);
#			endif
		projectedMaterialWeight = smoothstep(0, 1, 5 * (0.1 + projWeight));
#			if defined(TRUE_PBR)
		rawRMAOS.xyw = lerp(rawRMAOS.xyw, float3(ParallaxOccData.x, 0, ParallaxOccData.y), projectedMaterialWeight);
		float4 projectedGlintParameters = 0;
		if ((PBRFlags & PBR::Flags::ProjectedGlint) != 0) {
			projectedGlintParameters = SparkleParams;
		}
		glintParameters = lerp(glintParameters, projectedGlintParameters, projectedMaterialWeight);
#			endif  // TRUE_PBR
#			if defined(LOD_BLENDING) && (defined(LODOBJECTS) || defined(LODOBJECTSHD))
		projBaseColor.xyz = pow(abs(projBaseColor.xyz), SharedData::lodBlendingSettings.LODObjectSnowGamma) * SharedData::lodBlendingSettings.LODObjectSnowBrightness;
#			endif  // LOD_BLENDING
		float3 projectedWorldNormal = normalize(mul(transpose(tbnTr), finalProjNormal));
		worldNormal = normalize(lerp(worldNormal, projectedWorldNormal, projectedMaterialWeight));
		baseColor.xyz = lerp(baseColor.xyz, projBaseColor, projectedMaterialWeight);

#			if defined(SNOW)
		useSnowDecalSpecular = true;
#			endif  // SNOW
	} else {
		if (projWeight > 0) {
#			if defined(TRUE_PBR)
			baseColor.xyz = Color::Albedo(Color::LinearSRGBToWorking(ProjectedUVParams2.xyz));
#			else
			baseColor.xyz = ColorManagement::SRGBToWorking(ProjectedUVParams2.xyz);
#			endif
#			if defined(SNOW)
			useSnowDecalSpecular = true;
#			endif  // SNOW
		}
	}

	if (HasSpecular())
		useSnowSpecular = useSnowDecalSpecular;
#		endif      // SPARKLE

#	endif  // SNOW

#	if defined(WORLD_MAP)
	baseColor.xyz = GetWorldMapBaseColor(sampledBaseColor.xyz, baseColor.xyz, projWeight);
#	endif  // WORLD_MAP

#	if !defined(DCLF_DEPTH_ONLY)
	// Drawcall Limit Fix's Z-prepass (DCLF_DEPTH_ONLY) needs only the alpha test, and nothing between here
	// and it changes baseColor.w or discards. Compiling the lighting out here, rather than leaving it to the
	// optimizer behind the depth-only return, keeps its reads of the per-frame pixel bindings out of the module
	// even in debug builds (CS_DCLF_SHADER_DEBUG), where DXC keeps them alive for their debug values.
	float3 localShadowPosition = input.WorldPosition.xyz;
#	if defined(DEFERRED) && !defined(LANDSCAPE) && defined(TERRAIN_BLENDING_OBJECTS)
	// Terrain Blending fades terrain over the part of an object below the terrain surface, where the scene
	// depth holds the terrain. Light that part like the terrain it fades into: bend its normal up, and take
	// local shadows at the visible surface, since terrain occludes the buried point in the shadow maps.
	[branch] if (SharedData::terrainBlendingSettings.Enabled)
	{
		const float visibleRawDepth = SharedData::DepthTexture.Load(int3(input.Position.xy, 0)).x;
		const float visibleDepth = SharedData::GetScreenDepth(visibleRawDepth);
		// Objects read the 16-bit blended depth, so ignore differences within one of its steps.
		const float depthStep = SharedData::GetScreenDepth(visibleRawDepth + 1.0 / 65535.0) - visibleDepth;
		const float buriedDepth = max(viewPosition.z - visibleDepth - depthStep, 0.0);
		[branch] if (buriedDepth > 0.0)
		{
			// A downward normal passes through zero halfway, so fall back to up there instead of normalizing it
			const float3 blendedNormal = lerp(worldNormal, float3(0, 0, 1), saturate(buriedDepth / 10.0));
			worldNormal = dot(blendedNormal, blendedNormal) > 1e-6 ? normalize(blendedNormal) : float3(0, 0, 1);
			localShadowPosition *= 1.0 - buriedDepth / viewPosition.z;
		}
	}
#	endif

#	if defined(MODELSPACENORMALS)
	float3 vertexNormal = worldNormal;
#	endif

	float3 screenSpaceNormal = normalize(FrameBuffer::WorldToView(worldNormal, false));
#	if defined(TRUE_PBR)
	// Lighting uses the face-oriented normal. Deferred reflections need it facing the camera.
	if (thinSurfaceNormals)
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(PBR::GetThinSurfaceNormal(worldNormal, viewDirection), false));
#	endif

#	if defined(CS_HAIR_SHADING)
	float3 Bitangent = normalize(float3(input.TBN0.y, input.TBN1.y, input.TBN2.y));
	float3 hairT = 0;
	hairT = useHairFlowMap ? normalize(mul(tbn, sampledHairFlow)) : Bitangent;
	hairT = Hair::ReorientTangent(hairT, worldNormal);

	if (SharedData::hairSpecularSettings.Enabled) {
		if (SharedData::hairSpecularSettings.EnableTangentShift && SharedData::hairSpecularSettings.HairMode != 1) {
			float3 shiftedNormal = Hair::ShiftWorldNormal(hairT, worldNormal, 0, uv);
			screenSpaceNormal = normalize(FrameBuffer::WorldToView(shiftedNormal, false));
		}
	}
#	endif

	MaterialProperties material = (MaterialProperties)0;

	material.F0 = 0;
	material.Roughness = 1;

#	if defined(TRUE_PBR)
	material.Noise = screenNoise;

	material.Roughness = clamp(rawRMAOS.x, PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
	material.ThinSurfaceSpecularWeight = 1.0f;
	if (isThinSurface) {
		material.ThinSurfaceSpecularWeight = saturate(frontFace ? LightingEffectParams.y : LightingEffectParams.z);
		material.Roughness = clamp(rawRMAOS.x * (frontFace ? 1.0f : LightingEffectParams.w), PBR::Constants::MinRoughness, PBR::Constants::MaxRoughness);
	}
	if (foliageNormals)
		material.Roughness = PBR::FilterFoliageRoughness(material.Roughness, ddx(worldNormal), ddy(worldNormal));
	material.Metallic = saturate(rawRMAOS.y);
	material.AO = rawRMAOS.z;

	// Apply vertex color to base color so PBR metals use it. On LANDSCAPE,
	// honor DisableTerrainVertexColors (as the non-PBR path does) by
	// neutralizing the source color so terrain vertex colors don't tint PBR;
	// a white source yields VertexAO == 1, i.e. no AO darkening either.
	float3 pbrVertexColorSrc = input.Color.xyz;
#		if defined(LANDSCAPE)
	if (SharedData::lodBlendingSettings.DisableTerrainVertexColors)
		pbrVertexColorSrc = 1;
#		endif
	float3 pbrVertexColor = ColorManagement::PBRVertexColorToLinear(pbrVertexColorSrc);
	float pbrVertexAO = max(max(pbrVertexColor.x, pbrVertexColor.y), pbrVertexColor.z);
	pbrVertexColor = pbrVertexAO == 0.0f ? 1.0f : pbrVertexColor * lerp(1 / max(pbrVertexAO, 0.001), 1, SharedData::truePBRSettings.VertexAOStrength);
	pbrVertexColor = Color::LinearSRGBToWorking(pbrVertexColor);

	baseColor.xyz = Color::PBRMaterialToLinear(baseColor.xyz) * pbrVertexColor;
	material.F0 = lerp(rawRMAOS.w, baseColor.xyz, material.Metallic);
	baseColor.xyz = Color::LinearToPBRMaterial(baseColor.xyz);

	material.GlintScreenSpaceScale = max(1, glintParameters.x);
	material.GlintLogMicrofacetDensity = clamp(PBR::Constants::MaxGlintDensity - glintParameters.y, PBR::Constants::MinGlintDensity, PBR::Constants::MaxGlintDensity);
	material.GlintMicrofacetRoughness = clamp(glintParameters.z, PBR::Constants::MinGlintRoughness, PBR::Constants::MaxGlintRoughness);
	material.GlintDensityRandomization = clamp(glintParameters.w, PBR::Constants::MinGlintDensityRandomization, PBR::Constants::MaxGlintDensityRandomization);

#		if defined(GLINT)
	float glintNoise = Random::R1Modified(float(SharedData::FrameCount), (Random::pcg2d(uint2(input.Position.xy)) / 4294967296.0).x);
	Glints::PrecomputeGlints(glintNoise, uvOriginal, ddx(uvOriginal), ddy(uvOriginal), material.GlintScreenSpaceScale, material.GlintCache);
#		endif

	baseColor.xyz *= 1 - material.Metallic;

	material.BaseColor = baseColor.xyz;

	float3 coatWorldNormal = worldNormal;

#		if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::Subsurface) != 0)
	{
		material.SubsurfaceColor = PBRParams2.xyz;
		material.Thickness = PBRParams2.w;
		material.SubsurfaceBalance = LightingEffectParams.x;
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture0) != 0)
		{
			float4 sampledSubsurfaceProperties = TexRimSoftLightWorldMapOverlaySampler.Sample(SampRimSoftLightWorldMapOverlaySampler, uv);

			material.SubsurfaceColor *= Color::Albedo(Color::LinearSRGBToWorking(sampledSubsurfaceProperties.xyz));
			material.SubsurfaceColor *= Color::LinearToPBRMaterial(pbrVertexColor);

			material.Thickness *= sampledSubsurfaceProperties.w;
		}
		else if (isThinSurface)
			material.SubsurfaceColor *= material.BaseColor;
		material.Thickness = lerp(material.Thickness, 1, projectedMaterialWeight);
	}
	else if ((PBRFlags & PBR::Flags::TwoLayer) != 0)
	{
		material.CoatColor = sampledCoatColor.xyz;
		material.CoatStrength = sampledCoatColor.w;
		material.CoatRoughness = MultiLayerParallaxData.x;
		material.CoatF0 = MultiLayerParallaxData.y;

		float2 coatUv = uv;
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			coatUv = uvOriginal;
		}
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture1) != 0)
		{
			float4 sampledCoatProperties = TexBackLightSampler.Sample(SampBackLightSampler, coatUv);
			material.CoatRoughness *= sampledCoatProperties.w;
			[branch] if ((PBRFlags & PBR::Flags::CoatNormal) != 0)
			{
				coatWorldNormal = normalize(mul(tbn, TransformNormal(sampledCoatProperties.xyz)));
			}
		}
		material.CoatStrength = lerp(material.CoatStrength, 0, projectedMaterialWeight);
	}

	[branch] if ((PBRFlags & PBR::Flags::Fuzz) != 0)
	{
		material.FuzzColor = MultiLayerParallaxData.xyz;
		material.FuzzWeight = MultiLayerParallaxData.w;
		[branch] if ((PBRFlags & PBR::Flags::HasFeatureTexture1) != 0)
		{
			float4 sampledFuzzProperties = TexBackLightSampler.Sample(SampBackLightSampler, uv);
			material.FuzzColor *= Color::Albedo(Color::LinearSRGBToWorking(sampledFuzzProperties.xyz));
			material.FuzzWeight *= sampledFuzzProperties.w;
		}
		material.FuzzWeight = lerp(material.FuzzWeight, 0, projectedMaterialWeight);
	}
#		endif
#	else
	material.BaseColor = baseColor.xyz;
	const bool hasSpecular = HasSpecular();
	material.Shininess = hasSpecular ? shininess : 0;
	material.Glossiness = hasSpecular ? glossiness : 0;
	material.SpecularColor = hasSpecular ? SpecularColor.xyz : 0;
	material.rimSoftLightColor = ColorManagement::TextureToWorking(rimSoftLightColor.xyz);
	material.backLightColor = ColorManagement::TextureToWorking(backLightColor.xyz);
#	endif  // TRUE_PBR

#	if defined(CS_SKIN_SHADING)
	const float ExtraRoughness = BRDF::F_Schlick(0.04, saturate(dot(worldNormal.xyz, viewDirection))) * SharedData::skinData.fuzzParams.w;
	material.Roughness = SharedData::skinData.skinParams.x;
	material.Roughness = saturate(SharedData::skinData.skinParams.x - SharedData::skinData.skinParams.z * material.Glossiness);
	material.RoughnessSecondary = SharedData::skinData.skinParams.y;
	if (skinRoughnessSet) {
		material.Roughness = skinRoughness * SharedData::skinData.physicalParams.x;
		material.RoughnessSecondary = skinRoughness * SharedData::skinData.physicalParams.y;
	}
	material.Roughness = min(1.0, material.Roughness + ExtraRoughness);
	material.RoughnessSecondary = min(1.0, material.RoughnessSecondary + ExtraRoughness);
	material.SecondarySpecIntensity = SharedData::skinData.skinParams2.x;
	material.Thickness = 1 - skinsk.x;
	material.SubsurfaceColor = skinsk.xyz;
	material.F0 = SharedData::skinData.skinParams2.zzz;
	material.AO = skinAO;
	material.Curvature = Skin::CalculateCurvature(worldNormal.xyz);

	material.FuzzWeight = SharedData::skinData.fuzzParams.x;
	material.FuzzRoughness = SharedData::skinData.fuzzParams.y;
	material.FuzzColor = SharedData::skinData.fuzzParams.zzz;

	if (skinRoughnessSet) {
		material.F0 = 0.08f * skinSpecular * SharedData::skinData.physicalParams.z;
		material.FuzzWeight *= skinFuzzMask;
	}
#	endif  // CS_SKIN

#	if defined(SKIN)
	material.BaseColor = max(material.BaseColor, EPSILON_SKIN_ALBEDO);
#	endif

#	if defined(CS_HAIR_SHADING)
	if (SharedData::hairSpecularSettings.Enabled) {
		material.Shininess = SharedData::hairSpecularSettings.HairGlossiness;
		material.F0 = Hair::HairF0();
		if (SharedData::hairSpecularSettings.HairMode == 1) {
			material.Roughness = 1;
		} else {
			material.Roughness = ShininessToRoughness(material.Shininess * 0.75);
		}
	}
#	endif

	bool dynamicCubemap = false;

#	if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	float envMask = EnvmapData.x * MaterialData.x;

	float viewNormalAngle = dot(worldNormal.xyz, viewDirection);
	float3 envSamplingPoint = (viewNormalAngle * 2) * worldNormal.xyz - viewDirection;

	if (envMask > 0.0) {
		if (EnvmapData.y) {
			envMask *= envMaskBase;
		} else {
			envMask *= glossiness;
		}
	}

	float3 envColor = 0.0;

	if (envMask > 0.0) {
#		if defined(DYNAMIC_CUBEMAPS)
		uint2 envSize;
		TexEnvSampler.GetDimensions(envSize.x, envSize.y);

#			if defined(EMAT)
		if (envSize.x == 1 && envSize.y == 1 || complexMaterial) {
#			else
		if (envSize.x == 1 && envSize.y == 1) {
#			endif

			dynamicCubemap = true;

#			if defined(EMAT)
			if (!complexMaterial)
#			endif
			{
				// Dynamic Cubemap Creator sets this value to black, if it is anything but black it is wrong
				float3 envColorTest = TexEnvSampler.SampleLevel(SampEnvSampler, float3(0.0, 1.0, 0.0), 15).xyz;
				dynamicCubemap = all(envColorTest == 0.0);
			}

#			if defined(CREATOR)
			if (SharedData::cubemapCreatorSettings.Enabled) {
				dynamicCubemap = true;
			}
#			endif

			if (dynamicCubemap) {
				float4 envColorBase = TexEnvSampler.SampleLevel(SampEnvSampler, float3(1.0, 0.0, 0.0), 15);

				if (envColorBase.a < 1.0) {
					material.F0 = TransferFunctions::GameGammaToLinear(envColorBase.rgb);
					material.Roughness = envColorBase.a;
				} else {
					material.F0 = 1.0;
					material.Roughness = 1.0 / 8.0;
				}

#			if defined(CREATOR)
				if (SharedData::cubemapCreatorSettings.Enabled) {
					material.F0 = SharedData::cubemapCreatorSettings.CubemapColor.rgb;
					material.Roughness = SharedData::cubemapCreatorSettings.CubemapColor.a;
				}
#			endif

#			if defined(EMAT)
				float complexMaterialRoughness = 1.0 - complexMaterialColor.y;
				material.Roughness = lerp(material.Roughness, complexMaterialRoughness, complexMaterial);
				material.F0 = lerp(material.F0, complexSpecular, complexMaterial);
#			endif
			}
		}
#		endif

		if (!dynamicCubemap) {
			float3 envColorBase = TexEnvSampler.Sample(SampEnvSampler, envSamplingPoint).xyz;
#		if defined(ENABLE_LL)
			envColorBase = ColorManagement::TextureToWorking(envColorBase);
#		else
			envColorBase = TransferFunctions::GameGammaToLinear(envColorBase);
#		endif
			envColor = envColorBase.xyz * envMask;
		}
	}

#	endif  // defined (ENVMAP) || defined (MULTI_LAYER_PARALLAX) || defined(EYE)

	float porosity = 1.0;

#	if defined(SKYLIGHTING)
	float3 positionMSSkylight = input.WorldPosition.xyz;
#		if defined(SKYLIGHTING_SHADOW_VIS)
	float skylightingShadowVisibility = 1.0;
#		endif
#		if defined(DEFERRED)
#			if defined(SKYLIGHTING_SHADOW_VIS)
	// One probe pass serves both lookups when the height clamp leaves the position unchanged.
	const bool skylightingWantsShadowVis = inWorld && UseSkylightingShadowVisibility();
	const float3 positionMSShadowVis = Skylighting::ClampToArrayHeight(positionMSSkylight);
	const bool skylightingShadowVisShared = skylightingWantsShadowVis && positionMSShadowVis.z == positionMSSkylight.z;
	sh2 skylightingSH = Skylighting::Sample(positionMSSkylight, worldNormal, skylightingShadowVisibility, skylightingShadowVisShared);
	[branch] if (skylightingWantsShadowVis && !skylightingShadowVisShared)
		Skylighting::Sample(positionMSShadowVis, worldNormal, skylightingShadowVisibility);
#			else
	sh2 skylightingSH = Skylighting::Sample(positionMSSkylight, worldNormal);
#			endif
#		else
	sh2 skylightingSH = inWorld ? Skylighting::Sample(positionMSSkylight, worldNormal
#			if defined(SKYLIGHTING_SHADOW_VIS)
									  ,
									  skylightingShadowVisibility,
									  false
#			endif
									  ) :
	                              Skylighting::UNIT_SH;
#		endif
#		if defined(SKYLIGHTING_SHADOW_VIS) && !defined(DEFERRED)
	// Height clamped so shadow visibility stays valid above the volume, unlike the diffuse SH fade
	[branch] if (inWorld && UseSkylightingShadowVisibility())
		Skylighting::Sample(Skylighting::ClampToArrayHeight(positionMSSkylight), worldNormal, skylightingShadowVisibility);
#		endif
	sh2 skylightingSpecularSH = inWorld ? Skylighting::SampleSpecular(skylightingSH, positionMSSkylight) : skylightingSH;
	float skylightingSpecularFadeOut = Skylighting::GetHorizontalFadeOutFactor(positionMSSkylight);
#	endif

	float4 waterData = SharedData::GetWaterData(input.WorldPosition.xyz);
	float waterHeight = waterData.w;

	float waterRoughnessSpecular = 1;

#	if defined(WETNESS_EFFECTS)
	// Initialize wetness parameters
	float wetness = 0.0;
	float3 wetnessNormal = vertexNormal.xyz;

	// Calculate shore wetness factors
	float wetnessDistToWater = abs(input.WorldPosition.z - waterHeight);
	float shoreFactor = saturate(1.0 - (wetnessDistToWater / SharedData::wetnessEffectsSettings.ShoreRange));
	float shoreFactorAlbedo = (input.WorldPosition.z < waterHeight) ? 1.0 : shoreFactor;

	// Calculate wetness angle and occlusion
	float minWetnessValue = SharedData::wetnessEffectsSettings.MinRainWetness;
	float minWetnessAngle = saturate(max(minWetnessValue, vertexNormal.z));
#		if defined(SKYLIGHTING)
	float wetnessOcclusion = inWorld ? saturate(SphericalHarmonics::Unproject(skylightingSH, float3(0, 0, 1))) : 0.0;
#		else
	float wetnessOcclusion = inWorld;
#		endif
	float flatnessAmount = smoothstep(SharedData::wetnessEffectsSettings.PuddleMaxAngle, 1.0, minWetnessAngle);
	// Calculate raindrop effects
	float4 raindropInfo = float4(0, 0, 1, 0);
	bool shouldCalculateRaindrops = (worldNormal.z > 0.0) &&
	                                (SharedData::wetnessEffectsSettings.Raining > 0.0) &&
	                                (SharedData::wetnessEffectsSettings.EnableRaindropFx) &&
	                                (wetnessOcclusion > 0.5);

	if (shouldCalculateRaindrops) {
#		if defined(SKINNED)
		float3 ripplePosition = input.ModelPosition.xyz;
#		elif defined(DEFERRED)
		float3 ripplePosition = input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
#		else
		float3 ripplePosition = !FrameBuffer::FrameParams.y ? input.ModelPosition.xyz : input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz;
#		endif
		raindropInfo = WetnessEffects::GetRainDrops(ripplePosition, SharedData::wetnessEffectsSettings.Time, wetnessNormal, flatnessAmount);
	}

	// Calculate different wetness types
	float rainWetness = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
	rainWetness = max(rainWetness, raindropInfo.w);

#		if defined(SKIN) || defined(HAIR)
	rainWetness = SharedData::wetnessEffectsSettings.SkinWetness * SharedData::wetnessEffectsSettings.Wetness;
#		endif

#		if defined(CS_SKIN) && !defined(SKIN)
	if (skinEnabled) {
		float2 dynamicWetness = Skin::GetWetness(input.WorldPosition.z + FrameBuffer::CameraPosAdjust.z, worldNormal.xyz);
#			if defined(TRUE_PBR)
		dynamicWetness.x = lerp(dynamicWetness.x, 0.0f, material.Metallic);
#			endif
		float dynamicWetnessValue = clamp(dynamicWetness.x + dynamicWetness.y, 0.f, 2.f);
#			if defined(HAIR)
		dynamicWetnessValue = min(SharedData::skinData.skinParams2.y + dynamicWetnessValue, 2.0f);
#			endif
		rainWetness += min(dynamicWetnessValue, 1.f);
	}
#		endif
	float shoreWetness = shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness;
	wetness = max(shoreWetness, rainWetness);

	// Calculate puddle effects
	float puddleWetness = SharedData::wetnessEffectsSettings.PuddleWetness * minWetnessAngle;
	float puddle = wetness;

#		if !defined(SKINNED) && !defined(SKIN)
	if (wetness > 0.0 || puddleWetness > 0.0) {
		float3 puddleCoords = ((input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz) * 0.5 + 0.5) * 0.01 / SharedData::wetnessEffectsSettings.PuddleRadius;
		puddle = Random::perlinNoise(puddleCoords) * 0.5 + 0.5;
		puddle = puddle * ((minWetnessAngle / SharedData::wetnessEffectsSettings.PuddleMaxAngle) * SharedData::wetnessEffectsSettings.MaxPuddleWetness * 0.25) + 0.5;
		puddle *= lerp(wetness, puddleWetness, saturate(puddle - 0.25));
	}
#		endif

	// Apply occlusion and distance factors
	puddle *= saturate(wetnessOcclusion * 2.0) * nearFactor;
	wetnessNormal = lerp(worldNormal.xyz, wetnessNormal, saturate(puddle));

	// Calculate wetness glossiness factors
	float wetnessGlossinessAlbedo = max(puddle, shoreFactorAlbedo * SharedData::wetnessEffectsSettings.MaxShoreWetness);
	wetnessGlossinessAlbedo *= wetnessGlossinessAlbedo;

	float wetnessGlossinessSpecular = puddle;
	if (input.WorldPosition.z < waterHeight) {
		wetnessGlossinessSpecular *= shoreFactor;
	}

	// Update flatness and normal calculations
	flatnessAmount *= smoothstep(SharedData::wetnessEffectsSettings.PuddleMinWetness, 1.0, wetnessGlossinessSpecular);

	// Apply ripple normal effects
	float3 rippleNormal = normalize(lerp(float3(0, 0, 1), raindropInfo.xyz, lerp(flatnessAmount, 1.0, 0.5)));
	wetnessNormal = ReorientNormal(rippleNormal, wetnessNormal);

#		if defined(CS_SKIN_SHADING)
	if (skinEnabled && (skinWetness > 0.0f)) {
		wetnessNormal = skinWetNormal;
		wetnessGlossinessSpecular = saturate(max(wetnessGlossinessSpecular, skinWetness));
	}
#		endif

	// Minimum roughness prevents an extreme retroreflective peak (NdotH→1) for near-zero
	// roughness puddles. Real water has ripples and surface tension that keep it from being
	// optically perfect; the ripple normal map adds micro-variation but GGX still peaks
	// sharply without this floor.
	static const float wetnessMinPuddleRoughness = 0.05;
	waterRoughnessSpecular = max(saturate(1.0 - wetnessGlossinessSpecular), wetnessMinPuddleRoughness);
#	endif

	float3 dirLightColor = Color::DirectionalLight(DirLightColor.xyz);

#	if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		dirLightColor *= ExponentialHeightFog::GetSunlightFogAttenuation(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#	endif

#	if defined(WATER_EFFECTS)
	if (inWorld || inReflection)
		dirLightColor *= WaterEffects::ComputeCaustics(waterData, input.WorldPosition.xyz);
#	endif

	// Apply world shadow (terrain shadows, cloud shadows) directly to light color
	if (inWorld || inReflection)
		dirLightColor *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);

	float dirLightAngle = dot(worldNormal.xyz, DirLightDirection.xyz);

	float3 refractedDirLightDirection = DirLightDirection;
#	if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
	{
		if (dot(DirLightDirection, coatWorldNormal) > 0)
			refractedDirLightDirection = -refract(-DirLightDirection, coatWorldNormal, eta);
	}
#	endif

	float dirSoftShadow = 1.0;
	float dirVSMDetailedShadow = 1.0;

#	if defined(VOLUMETRIC_SHADOWS)
	if (inWorld && !inReflection && ShadowSampling::HasDirectionalShadows()
#		if defined(DEFERRED)
	    && !UseSkylightingShadowVisibility()
#		endif
	) {
		const float lightingShadow = ShadowSampling::GetLightingShadow(input.WorldPosition.xyz, dirVSMDetailedShadow);
		if (!UseSkylightingShadowVisibility())
			dirSoftShadow = lightingShadow;
	}
#	endif

	float dirDetailedShadow = 1.0;
	float dirTransmissionContactShadow = 1.0;

	if ((PixelDescriptor() & Permutation::LightingFlags::DefShadow) && (PixelDescriptor() & Permutation::LightingFlags::ShadowDir)) {
		dirDetailedShadow *= shadowColor.x;

#	if !defined(VOLUMETRIC_SHADOWS)
		if (!UseSkylightingShadowVisibility())
			dirSoftShadow = dirDetailedShadow;
#	endif
	}
#	if !defined(DEFERRED)
	else {
		dirDetailedShadow = dirVSMDetailedShadow;
	}
#	endif

#	if defined(SCREEN_SPACE_SHADOWS) && defined(DEFERRED)
	if (!SharedData::InInterior) {
#		if defined(TRUE_PBR)
		if (thinSurfaceNormals) {
			float faceLightAngle = dot(thinSurfaceFaceNormal, DirLightDirection);
			dirDetailedShadow *= ScreenSpaceShadows::GetThinSurfaceScreenSpaceShadow(input.Position.xyz, screenUV, screenNoise, faceLightAngle);
			float2 screenSpaceShadows = ScreenSpaceShadows::GetScreenSpaceShadows(input.Position.xyz, screenUV, screenNoise);
			dirTransmissionContactShadow = faceLightAngle >= 0.0 ? screenSpaceShadows.x : screenSpaceShadows.y;
		} else
#		endif
		{
			float2 screenSpaceShadows = ScreenSpaceShadows::GetScreenSpaceShadows(input.Position.xyz, screenUV, screenNoise);
			if (dirLightAngle >= 0.0)
				dirDetailedShadow *= screenSpaceShadows.x;
			dirTransmissionContactShadow = dirLightAngle >= 0.0 ? screenSpaceShadows.x : screenSpaceShadows.y;
		}
	}
#	endif

#	if defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
	[branch] if (inWorld && SharedData::extendedMaterialSettings.EnableShadows)
	{
		float3 dirLightDirectionTS = mul(refractedDirLightDirection, tbn).xyz;
#		if defined(LANDSCAPE)
		if (LANDSCAPE_PARALLAX_ENABLED && dirLightAngle > 0.0) {
			if (hasCachedDirectionalTerrainParallaxShadow) {
				dirDetailedShadow *= cachedDirectionalTerrainParallaxShadow;
			} else {
				float sh0;
				bool hasShadowBase = COMPUTE_TERRAIN_SHADOW_BASE(sh0);
				if (hasShadowBase)
					dirDetailedShadow *= EVAL_TERRAIN_DIR_SHADOW(sh0, dirLightDirectionTS);
			}
		}
#		elif defined(PARALLAX)
		[branch] if (SharedData::extendedMaterialSettings.EnableParallax)
			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams, applyMeshTV, meshOffset);
#		elif defined(EMAT_ENVMAP)
		[branch] if (complexMaterialParallax)
			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexEnvMaskSampler, SampEnvMaskSampler, 3, parallaxShadowQuality, screenNoise, displacementParams, applyMeshTV, meshOffset);
#		elif defined(TRUE_PBR) && !defined(LODLANDSCAPE) && !defined(FACEGEN)
		[branch] if (PBRParallax)
			dirDetailedShadow *= ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, dirLightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams, applyMeshTV, meshOffset);
#		endif  // LANDSCAPE
	}
#	endif  // defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))

#	if defined(CS_HAIR_SHADING)
	if (SharedData::hairSpecularSettings.Enabled) {
		vertexNormal.xyz = worldNormal.xyz;
		worldNormal.xyz = hairT;
	}
#	endif

#	if defined(SKYLIGHTING_SHADOW_VIS)
	if (UseSkylightingShadowVisibility())
		dirSoftShadow = skylightingShadowVisibility;
#	endif

#	if defined(TRUE_PBR)
	[branch] if (isFoliage && (PBRFlags & PBR::Flags::Subsurface) != 0)
	{
		dirDetailedShadow = PBR::GetFoliageShadowVisibility(dirDetailedShadow, material.Thickness);
		dirSoftShadow = PBR::GetFoliageShadowVisibility(dirSoftShadow, material.Thickness);
	}
#	endif

	float3 diffuseColor = 0.0.xxx;
	float3 specularColor = 0.0.xxx;
	float3 transmissionColor = 0.0.xxx;

	float3 lightsDiffuseColor = 0.0.xxx;
	float3 coatLightsDiffuseColor = 0.0.xxx;
	float3 lightsSpecularColor = 0.0.xxx;

	float3 lodLandDiffuseColor = 0;

	// Directiontal Lighting
	DirectContext dirLightContext;
	DirectLightingOutput dirLightOutput;
#	if defined(TRUE_PBR)
	dirLightContext = CreateDirectLightingContext(worldNormal.xyz, coatWorldNormal, vertexNormal.xyz, refractedViewDirection, viewDirection, refractedDirLightDirection, DirLightDirection, dirLightColor, dirDetailedShadow, dirSoftShadow);
	if (thinSurfaceNormals)
		dirLightContext.backTransmissionVisibility = PBR::GetThinSurfaceBackTransmissionVisibility(thinSurfaceFaceNormal, DirLightDirection);
#	else
	dirLightContext = CreateDirectLightingContext(worldNormal.xyz, vertexNormal.xyz, viewDirection, DirLightDirection, dirLightColor, dirDetailedShadow, dirSoftShadow);
#		if defined(CS_HAIR_SHADING)
	if (SharedData::hairSpecularSettings.Enabled) {
		float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, DirLightDirection, screenNoise);
		dirLightContext.hairShadow = hairShadow;
	}
#		endif
#	endif

	float2 uvOriginal_ddx = ddx(uvOriginal);
	float2 uvOriginal_ddy = ddy(uvOriginal);
	EvaluateLighting(dirLightContext, material, tbnTr, uvOriginal, uvOriginal_ddx, uvOriginal_ddy, dirLightOutput);
	dirLightOutput.transmission *= dirTransmissionContactShadow;
#	if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		EvaluateWetnessLighting(wetnessNormal, dirLightContext, waterRoughnessSpecular, dirLightOutput);
#	endif

	lightsDiffuseColor += dirLightOutput.diffuse;
	lightsSpecularColor += dirLightOutput.specular;
#	if defined(TRUE_PBR)
	coatLightsDiffuseColor += dirLightOutput.coatDiffuse;
#		if defined(LOD_LAND_BLEND)
	lodLandDiffuseColor += dirLightColor / Math::PI * saturate(dirLightAngle) * dirDetailedShadow;
#		endif
#	endif
	transmissionColor += dirLightOutput.transmission;

#	if !defined(LOD)
#		if !defined(LIGHT_LIMIT_FIX)
	[loop] for (uint lightIndex = 0; lightIndex < numLights; lightIndex++)
	{
		float3 lightDirection = PointLightPosition[lightIndex].xyz - input.WorldPosition.xyz;
		float lightDist = length(lightDirection);
		float intensityFactor = saturate(lightDist / PointLightPosition[lightIndex].w);
		if (intensityFactor == 1)
			continue;

		float intensityMultiplier = 1 - intensityFactor * intensityFactor;
#			if defined(ENABLE_LL)
		intensityMultiplier = pow(intensityMultiplier, TransferFunctions::GAME_GAMMA);
#			endif
		float3 lightColor = Color::PointLight(PointLightColor[lightIndex].xyz) * intensityMultiplier;
		float lightShadow = 1.f;
		if (PixelDescriptor() & Permutation::LightingFlags::DefShadow) {
			if (lightIndex < numShadowLights) {
				lightShadow *= shadowColor[ShadowLightMaskSelect[lightIndex]];
			}
		}

		float3 normalizedLightDirection = normalize(lightDirection);

		DirectContext pointLightContext;
		DirectLightingOutput pointLightOutput;
#			if defined(TRUE_PBR)
		{
			float3 refractedLightDirection = normalizedLightDirection;
#				if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
			[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
			{
				if (dot(normalizedLightDirection, coatWorldNormal) > 0)
					refractedLightDirection = -refract(-normalizedLightDirection, coatWorldNormal, eta);
			}
#				endif
			pointLightContext = CreateDirectLightingContext(worldNormal.xyz, coatWorldNormal, vertexNormal.xyz, refractedViewDirection, viewDirection, refractedLightDirection, normalizedLightDirection, lightColor, lightShadow, lightShadow);
			if (thinSurfaceNormals)
				pointLightContext.backTransmissionVisibility = PBR::GetThinSurfaceBackTransmissionVisibility(thinSurfaceFaceNormal, normalizedLightDirection);
		}
#			else
		pointLightContext = CreateDirectLightingContext(worldNormal.xyz, vertexNormal.xyz, viewDirection, normalizedLightDirection, lightColor, lightShadow, lightShadow);
#				if defined(CS_HAIR_SHADING)
		if (SharedData::hairSpecularSettings.Enabled) {
			float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, normalizedLightDirection, screenNoise);
			pointLightContext.hairShadow = hairShadow;
		}
#				endif
#			endif
		EvaluateLighting(pointLightContext, material, tbnTr, uvOriginal, uvOriginal_ddx, uvOriginal_ddy, pointLightOutput);
#			if defined(WETNESS_EFFECTS)
		if (waterRoughnessSpecular < 1)
			EvaluateWetnessLighting(wetnessNormal, pointLightContext, waterRoughnessSpecular, pointLightOutput);
#			endif
		lightsDiffuseColor += pointLightOutput.diffuse;
		lightsSpecularColor += pointLightOutput.specular;
#			if defined(TRUE_PBR)
		coatLightsDiffuseColor += pointLightOutput.coatDiffuse;
#			endif
		transmissionColor += pointLightOutput.transmission;
	}

#		else

	uint numClusteredLights = 0;
	uint totalLightCount = LightLimitFix::NumStrictLights;
	uint clusterIndex = 0;
	uint lightOffset = 0;
	if (inWorld && LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
		numClusteredLights = LightLimitFix::lightGrid[clusterIndex].lightCount;
		totalLightCount += numClusteredLights;
		lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
	}

#			if defined(SKINNED)
	const bool localShadowSkinned = true;
#			else
	const bool localShadowSkinned = false;
#			endif

	[loop] for (uint lightIndex = 0; lightIndex < totalLightCount; lightIndex++)
	{
		LightLimitFix::Light light;
		if (lightIndex < LightLimitFix::NumStrictLights) {
			light = LightLimitFix::StrictLights[lightIndex];
		} else {
			uint clusteredLightIndex = LightLimitFix::lightList[lightOffset + (lightIndex - LightLimitFix::NumStrictLights)];
			light = LightLimitFix::lights[clusteredLightIndex];

			if (LightLimitFix::IsLightIgnored(light))
				continue;
		}

		float3 lightDirection = light.positionWS.xyz - input.WorldPosition.xyz;
		float lightDist = length(lightDirection);

		float intensityMultiplier = LightLimitFix::GetAttenuation(lightDist, light);
		if (intensityMultiplier < 1e-5)
			continue;

		float3 lightColor = Color::PointLight(light.color.xyz) * intensityMultiplier * light.fade;
		float lightShadow = 1.0;
		float3 normalizedLightDirection = normalize(lightDirection);

		float shadowComponent = 1.0;
		[branch] if (light.lightFlags & LightLimitFix::LightFlags::LocalShadow)
		{
			const float3 localShadowEye = LightLimitFix::FirstPerson ? LightLimitFix::WorldEyePosition.xyz : FrameBuffer::CameraPosAdjust.xyz;
			shadowComponent = LightLimitFix::GetLocalShadow(LinearSampler, light.localShadowIndex, localShadowPosition, localShadowEye, normalizedLightDirection, localShadowSkinned, screenNoise);
			lightShadow *= shadowComponent;
		}
		else if (PixelDescriptor() & Permutation::LightingFlags::DefShadow)
		{
			if (light.lightFlags & LightLimitFix::LightFlags::Shadow) {
				shadowComponent = shadowColor[light.shadowLightIndex];
				lightShadow *= shadowComponent;
			}
		}

		float lightAngle = dot(worldNormal.xyz, normalizedLightDirection.xyz);

		float3 refractedLightDirection = normalizedLightDirection;
#			if defined(TRUE_PBR) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
		[branch] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
		{
			if (dot(normalizedLightDirection, coatWorldNormal) > 0)
				refractedLightDirection = -refract(-normalizedLightDirection, coatWorldNormal, eta);
		}
#			endif

		float parallaxShadow = 1;

#			if defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))
		[branch] if (
			SharedData::extendedMaterialSettings.EnableShadows &&
			!(light.lightFlags & LightLimitFix::LightFlags::Simple) &&
			lightAngle > 0.0 &&
			shadowComponent != 0.0)
		{
			float3 lightDirectionTS = normalize(mul(refractedLightDirection, tbn).xyz);
#				if defined(PARALLAX)
			[branch] if (SharedData::extendedMaterialSettings.EnableParallax)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams, applyMeshTV, meshOffset);
#				elif defined(LANDSCAPE)
			[branch] if (hasTerrainParallaxShadow)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplierTerrain(input, uv, terrainShadowMipLevels, lightDirectionTS, sh0, terrainDirectionalShadowQuality, screenNoise, displacementParams, sharedOffset);
#				elif defined(EMAT_ENVMAP)
			[branch] if (complexMaterialParallax)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexEnvMaskSampler, SampEnvMaskSampler, 3, parallaxShadowQuality, screenNoise, displacementParams, applyMeshTV, meshOffset);
#				elif defined(TRUE_PBR) && !defined(LODLANDSCAPE) && !defined(FACEGEN)
			[branch] if (PBRParallax)
				parallaxShadow = ExtendedMaterials::GetParallaxSoftShadowMultiplier(uv, mipLevel, lightDirectionTS, sh0, TexParallaxSampler, SampParallaxSampler, 0, parallaxShadowQuality, screenNoise, displacementParams, applyMeshTV, meshOffset);
#				endif
		}
#			endif  // defined(EMAT) && (defined(SKINNED) || !defined(MODELSPACENORMALS))

		DirectContext pointLightContext;
		DirectLightingOutput pointLightOutput;
		float pointLightShadow = lightShadow * parallaxShadow;
#			if defined(TRUE_PBR)
		pointLightContext = CreateDirectLightingContext(worldNormal.xyz, coatWorldNormal, vertexNormal.xyz, refractedViewDirection, viewDirection, refractedLightDirection, normalizedLightDirection, lightColor, pointLightShadow, pointLightShadow);
		if (thinSurfaceNormals)
			pointLightContext.backTransmissionVisibility = PBR::GetThinSurfaceBackTransmissionVisibility(thinSurfaceFaceNormal, normalizedLightDirection);
#			else
		pointLightContext = CreateDirectLightingContext(worldNormal.xyz, vertexNormal.xyz, viewDirection, normalizedLightDirection, lightColor, pointLightShadow, pointLightShadow);
#				if defined(CS_HAIR_SHADING)
		if (SharedData::hairSpecularSettings.Enabled) {
			float hairShadow = Hair::HairSelfShadow(input.WorldPosition.xyz, normalizedLightDirection, screenNoise);
			pointLightContext.hairShadow = hairShadow;
		}
#				endif
#			endif
		EvaluateLighting(pointLightContext, material, tbnTr, uvOriginal, uvOriginal_ddx, uvOriginal_ddy, pointLightOutput);
#			if defined(WETNESS_EFFECTS)
		if (waterRoughnessSpecular < 1)
			EvaluateWetnessLighting(wetnessNormal, pointLightContext, waterRoughnessSpecular, pointLightOutput);
#			endif

		lightsDiffuseColor += pointLightOutput.diffuse;
		lightsSpecularColor += pointLightOutput.specular;
#			if defined(TRUE_PBR)
		coatLightsDiffuseColor += pointLightOutput.coatDiffuse;
#			endif
		transmissionColor += pointLightOutput.transmission;
	}
#		endif
#	endif

	diffuseColor += lightsDiffuseColor;
	specularColor += lightsSpecularColor;

#	if !defined(LANDSCAPE)
	if (Permutation::PixelShaderDescriptor & Permutation::LightingFlags::CharacterLight) {
		float charLightMul = saturate(dot(viewDirection, worldNormal.xyz)) * CharacterLightParams.x + CharacterLightParams.y * saturate(dot(float2(0.164398998, -0.986393988), worldNormal.yz));
#		if defined(DCLF_BINDLESS)
		float charLightColor = min(CharacterLightParams.w, max(0, CharacterLightParams.z * DCLFCharacterLightNoise.Sample(SampCharacterLightProjNoiseSampler, baseShadowUV).x));
#		else
		float charLightColor = min(CharacterLightParams.w, max(0, CharacterLightParams.z * TexCharacterLightProjNoiseSampler.Sample(SampCharacterLightProjNoiseSampler, baseShadowUV).x));
#		endif
		diffuseColor += (charLightMul * charLightColor).xxx;
	}
#	endif

#	if defined(EYE) && defined(VANILLA_EYE_NORMAL)
	worldNormal.xyz = input.EyeNormal;
#	endif  // EYE

	// sRGB by default, linear if LL on
	float3 emitColor = EmitColor;
#	if !defined(LANDSCAPE) && !defined(LODLANDSCAPE)
	bool hasEmissive = (0x3F & (Permutation::PixelShaderDescriptor >> 24)) == Permutation::LightingTechnique::Glowmap;
#		if defined(TRUE_PBR)
	hasEmissive = hasEmissive || ((PBRFlags & PBR::Flags::HasEmissive) != 0);
#		endif
	[branch] if (hasEmissive)
	{
		float3 glowColor = ColorManagement::TextureToWorking(TexGlowSampler.Sample(SampGlowSampler, uv).xyz, PBRTextures) * SharedData::linearLightingSettings.glowmapMult;

#		if defined(TRUE_PBR)
		float3 emitVertexColor = ColorManagement::PBRVertexColorToLinear(input.Color.xyz);
		float emitVertexAO = max(max(emitVertexColor.r, emitVertexColor.g), emitVertexColor.b);
		emitVertexColor = emitVertexAO == 0.0f ? 1.0f : emitVertexColor * lerp(1 / max(emitVertexAO, 1e-4), 1, SharedData::truePBRSettings.VertexAOStrength);
		emitVertexColor = Color::LinearSRGBToWorking(emitVertexColor);

		emitColor = emitColor * Color::LinearToPBRMaterial(glowColor) * Color::LinearToPBRMaterial(emitVertexColor);
#		else
		emitColor *= glowColor;
#		endif  // TRUE_PBR
	}
#	endif

#	if !defined(TRUE_PBR)
	diffuseColor += emitColor.xyz;
#	endif

	IndirectContext indirectContext = (IndirectContext)0;
	IndirectLobeWeights indirectLobeWeights;

#	if defined(MULTI_LAYER_PARALLAX)
	float layerValue = MultiLayerParallaxData.x * TexLayerSampler.Sample(SampLayerSampler, uv).w;
	float3 tangentViewDirection = mul(viewDirection, tbn);
	float3 layerNormal = MultiLayerParallaxData.yyy * (normalColor.xyz * 2.0.xxx + float3(-1, -1, -2)) + float3(0, 0, 1);
	float layerViewAngle = dot(-tangentViewDirection.xyz, layerNormal.xyz) * 2;
	float3 layerViewProjection = -layerNormal.xyz * layerViewAngle.xxx - tangentViewDirection.xyz;
	float2 layerUv = uv * MultiLayerParallaxData.zw + (0.0009765625 * (layerValue / abs(layerViewProjection.z))).xx * layerViewProjection.xy;

	float3 layerColor = ColorManagement::DiffuseToWorking(TexLayerSampler.Sample(SampLayerSampler, layerUv).xyz);

	float mlpBlendFactor = saturate(viewNormalAngle) * (1.0 - baseColor.w);

	material.BaseColor = lerp(material.BaseColor, layerColor, mlpBlendFactor);
#	endif  // MULTI_LAYER_PARALLAX

	float3 ambientNormal = worldNormal.xyz;
#	if defined(CS_HAIR_SHADING)
	if (SharedData::hairSpecularSettings.Enabled) {
		if (SharedData::hairSpecularSettings.HairMode == 1)
			ambientNormal = normalize(viewDirection - hairT * dot(viewDirection, hairT));
		else
			ambientNormal = vertexNormal.xyz;
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(ambientNormal, false));
	}
#	endif

	float3 directionalAmbientColor = Color::Ambient(max(0, mul(DirectionalAmbient, float4(ambientNormal, 1.0))));

#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		if (SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection) {
			directionalAmbientColor = ImageBasedLighting::GetStaticDiffuseIBL(ambientNormal, SampColorSampler);
		}
	}
#	endif

#	if defined(LANDSCAPE)
	if (SharedData::lodBlendingSettings.DisableTerrainVertexColors)
		input.Color.xyz = 1;
	else
		input.Color.xyz /= max(max(max(input.Color.x, input.Color.y), input.Color.z), EPSILON_DIVISION);
#	endif

#	if defined(HAIR)
	float3 vertexColor = ColorManagement::SRGBToWorking(lerp(1, TintColor.xyz, input.Color.y));
	float vertexAO = 1;
#		if defined(CS_HAIR_SHADING)
	if (SharedData::hairSpecularSettings.Enabled)
		vertexColor = 1;
#		endif
#		if defined(SKYLIGHTING)
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, input.WorldPosition.xyz, ambientNormal);
#		endif
#	else
	float encodedVertexAO = max(max(input.Color.r, input.Color.g), input.Color.b);
#		if defined(TRUE_PBR)
	float3 vertexColor = 1;
#		else
	float3 vertexColor = ColorManagement::SRGBToWorking(input.Color.xyz);
#		endif
#		if defined(FACEGEN) || defined(FACEGEN_RGB_TINT) || defined(EYE)
	float vertexAO = 1;
#		else
	float vertexAO = ColorManagement::SRGBToWorking(encodedVertexAO);
#		endif
#		if defined(SKYLIGHTING)
#			if defined(TRUE_PBR)
	vertexAO = lerp(1, vertexAO, SharedData::truePBRSettings.VertexAOStrength);
#			endif
	float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, input.WorldPosition.xyz, ambientNormal, vertexAO);
#			if defined(TRUE_PBR)
	[branch] if (isFoliage && (PBRFlags & PBR::Flags::Subsurface) != 0)
		skylightingDiffuse = PBR::GetFoliageShadowVisibility(skylightingDiffuse, material.Thickness);
#			endif
#		endif
#	endif  // defined (HAIR)

#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		if (!(SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection)) {
			directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(directionalAmbientColor, -ambientNormal);
		}
	}
#	endif

	float3 reflectionDiffuseColor = diffuseColor + directionalAmbientColor;

#	if defined(TRUE_PBR) && defined(LOD_LAND_BLEND) && !defined(DEFERRED)
	lodLandDiffuseColor += directionalAmbientColor;
#	endif

	float2 screenMotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);

#	if defined(WETNESS_EFFECTS)
#		if !(defined(FACEGEN) || defined(FACEGEN_RGB_TINT) || defined(EYE)) || defined(TREE_ANIM)
#			if defined(TRUE_PBR)
#				if !defined(LANDSCAPE)
	[branch] if ((PBRFlags & PBR::Flags::TwoLayer) != 0)
	{
		porosity = 0;
	}
	else
#				endif
	{
		porosity = lerp(porosity, 0.0, saturate(sqrt(material.Metallic)));
	}
#			elif defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX)
	porosity = lerp(porosity, 0.0, saturate(sqrt(envMask)));
#			endif
	float wetnessDarkeningAmount = porosity * wetnessGlossinessAlbedo;
	material.BaseColor = lerp(material.BaseColor, pow(abs(material.BaseColor), 1.0 + wetnessDarkeningAmount), 0.5);
#		endif
#	endif

	float4 color = 0;

	indirectContext = CreateIndirectLightingContext(ambientNormal, vertexNormal.xyz, viewDirection);

	GetIndirectLobeWeights(indirectLobeWeights, indirectContext, material, uvOriginal);

#	if defined(WETNESS_EFFECTS)
#		if defined(DYNAMIC_CUBEMAPS)
	float3 wetnessReflectance = GetWetnessIndirectLobeWeights(indirectLobeWeights, wetnessNormal, waterRoughnessSpecular, indirectContext);
#		else
	float3 wetnessReflectance = 0.0;
#		endif
#	endif
#	if defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE)
	indirectLobeWeights.specular *= envMask;
#	endif

#	if !defined(TRUE_PBR)
	if (HasSpecular()) {
		indirectLobeWeights.specular *= MaterialData.yyy;
		specularColor *= MaterialData.yyy;
	}
#	endif

#	if defined(TRUE_PBR)
	{
		float3 directLightsDiffuseInput = diffuseColor * material.BaseColor;
		[branch] if ((PBRFlags & PBR::Flags::ColoredCoat) != 0)
		{
			directLightsDiffuseInput = lerp(directLightsDiffuseInput, material.CoatColor * coatLightsDiffuseColor, material.CoatStrength);
		}

		color.xyz += directLightsDiffuseInput;
	}

	[branch] if ((PBRFlags & PBR::Flags::HasEmissive) != 0)
	{
		color.xyz += emitColor.xyz;
	}
#	else
	color.xyz += diffuseColor * material.BaseColor;
#	endif

	color.xyz += indirectLobeWeights.diffuse * directionalAmbientColor;
	color.xyz += transmissionColor;

#	if defined(TRUE_PBR)
	float3 transmittedAmbientColor = 0.0f;
	[unroll] for (uint side = 0; side < 2; ++side)
	{
		float3 transmissionWeight = side == 0 ? indirectLobeWeights.frontTransmission : indirectLobeWeights.backTransmission;
		[branch] if (any(transmissionWeight > 0.0f))
		{
			float3 transmissionNormal = side == 0 ? ambientNormal : -ambientNormal;
			float3 ambientColor = Color::Ambient(max(0, mul(DirectionalAmbient, float4(transmissionNormal, 1.0))));
			float3 transmittedAmbient;
			float3 indirectTransmissionColor = GetThinSurfaceTransmissionLighting(ambientColor, transmissionWeight, transmissionNormal,
				input.WorldPosition.xyz, vertexAO, isFoliage ? material.Thickness : 1.0f, transmittedAmbient
#		if defined(IBL)
				,
				SampColorSampler, SharedData::iblSettings.UseStaticIBL && !inWorld && !inReflection
#		endif
#		if defined(SKYLIGHTING)
				,
				skylightingSH
#		endif
			);
			color.xyz += indirectTransmissionColor;
			transmittedAmbientColor += transmittedAmbient;
		}
	}
#	endif

	color.xyz *= vertexColor;

#	if defined(SNOW)
	if (useSnowSpecular)
		specularColor = 0;
#	endif

	diffuseColor = reflectionDiffuseColor;

#	if (defined(ENVMAP) || defined(MULTI_LAYER_PARALLAX) || defined(EYE))
#		if defined(DYNAMIC_CUBEMAPS)
	if (!dynamicCubemap)
#		endif
		specularColor += envColor * ColorManagement::SceneToLinear(diffuseColor);
	indirectLobeWeights.diffuse += envColor;
#	endif

#	if defined(EMAT_ENVMAP)
	specularColor *= complexSpecular;
#	endif  // defined (EMAT) && defined(ENVMAP)

#	if defined(LOD_LAND_BLEND) && defined(TRUE_PBR)
	{
		lodLandDiffuseColor += directionalAmbientColor;
		float3 litLodLandColor = vertexColor * lodLandColor.xyz * lodLandFadeFactor * lodLandDiffuseColor;
		color.xyz = lerp(color.xyz * Color::PBRLightingScale, litLodLandColor, lodLandBlendFactor);

		specularColor = lerp(specularColor * Color::PBRLightingScale, 0, lodLandBlendFactor);
		indirectLobeWeights.diffuse = lerp(indirectLobeWeights.diffuse * Color::PBRLightingScale, vertexColor * lodLandColor.xyz * lodLandFadeFactor, lodLandBlendFactor);
		indirectLobeWeights.specular = lerp(indirectLobeWeights.specular, 0, lodLandBlendFactor);
		material.Roughness = lerp(material.Roughness, 1, lodLandBlendFactor);
	}
#	elif defined(TRUE_PBR)
	color.xyz *= Color::PBRLightingScale;
	specularColor *= Color::PBRLightingScale;
	indirectLobeWeights.diffuse *= Color::PBRLightingScale;
#	endif

	float3 outputAlbedo = indirectLobeWeights.diffuse * vertexColor.xyz;

	directionalAmbientColor *= outputAlbedo;

#	if defined(DCLF_SHADOW_DEBUG)
	float dbgBeforeSky = dot(color.xyz, float3(0.2126, 0.7152, 0.0722));
#	endif
#	if defined(SKYLIGHTING)
	float3 skylightingAlbedo = outputAlbedo;
#		if defined(TRUE_PBR)
	skylightingAlbedo /= Color::PBRLightingScale;
#		endif
	Skylighting::ApplySkylighting(color.xyz, directionalAmbientColor, skylightingAlbedo, skylightingDiffuse);
#	endif

#	if !defined(DEFERRED)
	color.xyz = ColorManagement::SceneToLinear(color.xyz);
	color.xyz += specularColor;
	float3 specularNormal = worldNormal;
#		if defined(TRUE_PBR)
	if (thinSurfaceNormals)
		specularNormal = PBR::GetThinSurfaceNormal(specularNormal, viewDirection);
#		endif

	if (any(indirectLobeWeights.specular > 0)
#		if defined(WETNESS_EFFECTS)
	    || any(wetnessReflectance > 0)
#		endif
	)
#		if defined(DYNAMIC_CUBEMAPS)
#			if defined(SKYLIGHTING)
		color.xyz += indirectLobeWeights.specular * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(specularNormal, viewDirection, material.Roughness, skylightingSpecularSH, skylightingSpecularFadeOut);
#				if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		color.xyz += wetnessReflectance * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(wetnessNormal, viewDirection, waterRoughnessSpecular, skylightingSpecularSH, skylightingSpecularFadeOut);
#				endif
#			else
		color.xyz += indirectLobeWeights.specular * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(specularNormal, viewDirection, material.Roughness);
#				if defined(WETNESS_EFFECTS)
	if (waterRoughnessSpecular < 1)
		color.xyz += wetnessReflectance * DynamicCubemaps::GetDynamicCubemapSpecularIrradiance(wetnessNormal, viewDirection, waterRoughnessSpecular);
#				endif
#			endif
#		else
		color.xyz += indirectLobeWeights.specular * directionalAmbientColor;
#		endif

	color.xyz = ColorManagement::LinearToScene(color.xyz);
	float3 fogColor = input.FogParam.xyz;
	float fogFactor = input.FogParam.w;
#		if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		fogColor = ImageBasedLighting::GetFogIBLColor(fogColor);
	}
#		endif
#		if defined(EXP_HEIGHT_FOG)
	float3 vanillaFogColor = fogColor;
	float vanillaFogFactor = fogFactor;
	if (SharedData::exponentialHeightFogSettings.enabled) {
		float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFogInternal(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz, fogColor, true, float4(input.Position.xy * FrameBuffer::DynamicResolutionParams2.xy, input.Position.z, 1), !inReflection);
		fogColor = exponentialHeightFog.xyz;
		fogFactor = exponentialHeightFog.w;
	}
#		endif
	if ((Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::AdditiveLighting) != 0) {
#		if defined(EXP_HEIGHT_FOG)
		vanillaFogColor = 0.0;
#		endif
		fogColor = 0.0;
	}
	if ((FrameBuffer::FrameParams.y && FrameBuffer::FrameParams.z) || inReflection) {
		// ISSAOComposite's fade (FogColor.w = 1 / frame buffer range); reflections skip the composite.
		float fogFade = inReflection ? 1.0 : FogColor.w;
#		if defined(EXP_HEIGHT_FOG)
		if (SharedData::exponentialHeightFogSettings.enabled) {
			fogFade = ExponentialHeightFog::GetVanillaFogFade(fogFade);
			if (!ExponentialHeightFog::ShouldDisableVanillaFog()) {
				color.xyz = lerp(color.xyz, vanillaFogColor, vanillaFogFactor);
			}
			color.xyz = lerp(color.xyz, fogColor, fogFactor);
		} else {
			color.xyz = lerp(color.xyz, fogColor, fogFactor);
		}
#		else
		color.xyz = lerp(color.xyz, fogColor, fogFactor);
#		endif
		color.xyz *= fogFade;
	}
#	endif

#	if defined(TESTCUBEMAP) && defined(ENVMAP) && defined(DYNAMIC_CUBEMAPS)
	baseColor.xyz = 0.0;
	specularColor = 0.0;
	diffuseColor = 0.0;
	dynamicCubemap = true;
	envColor = 1.0;
	material.Roughness = 0.0;
	color.xyz = 0;
#	endif

#	endif  // !DCLF_DEPTH_ONLY

#	if defined(LANDSCAPE) && !defined(LOD_LAND_BLEND)
	psout.Diffuse.w = 0;
#	else
#		if !defined(DO_ALPHA_TEST) || defined(SPARKLE)
	float alpha = baseColor.w;
#			if defined(EMAT) && !defined(LANDSCAPE)
#				if defined(PARALLAX)
	alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
#				elif defined(TRUE_PBR)
	[branch] if (PBRParallax)
	{
		alpha = TexColorSampler.SampleBias(SampColorSampler, uvOriginal, SharedData::MipBias).w;
	}
#				endif
#			endif
#			if defined(DO_ALPHA_TEST)
	[branch] if ((Permutation::PixelShaderDescriptor & Permutation::LightingFlags::AdditionalAlphaMask) != 0)
	{
		uint2 alphaMask = input.Position.xy;
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

		float testTmp = 0;
		if (MaterialData.z - maskValues[alphaMask.x] < 0) {
			discard;
		}
	}
	else
#			endif  // defined(DO_ALPHA_TEST)
	{
		alpha *= MaterialData.z;
	}
#			if !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
	alpha *= input.Color.w;
#			endif  // !(defined(TREE_ANIM) || defined(LODOBJECTSHD) || defined(LODOBJECTS))
#			if defined(DO_ALPHA_TEST)
#				if defined(DEPTH_WRITE_DECALS)
	if (alpha - 0.0156862754 < 0) {
		discard;
	}
	alpha = saturate(1.05 * alpha);
#				endif  // DEPTH_WRITE_DECALS
	if (alpha - AlphaTestRefRS < 0) {
		discard;
	}
#			endif      // DO_ALPHA_TEST
#		endif

#		if !defined(DCLF_DEPTH_ONLY)
#			if defined(ANISOTROPIC_ALPHA)
	// Uniform alpha material settings
	uint AlphaMaterialModel = ExtendedTranslucency::GetMaterialModelFromDescriptor(Permutation::ExtraFeatureDescriptor);
	float AlphaMaterialReduction = 0.f;
	float AlphaMaterialSoftness = 0.f;
	float AlphaMaterialStrength = 0.f;
	[branch] if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::Default)
	{
		AlphaMaterialModel = SharedData::extendedTranslucencySettings.MaterialModel;
		AlphaMaterialReduction = SharedData::extendedTranslucencySettings.Reduction;
		AlphaMaterialSoftness = SharedData::extendedTranslucencySettings.Softness;
		AlphaMaterialStrength = SharedData::extendedTranslucencySettings.Strength;
	}

	[branch] if (ExtendedTranslucency::IsValidMaterial(AlphaMaterialModel))
	{
		if (alpha >= 0.0156862754 && alpha < 1.0) {
			float originalAlpha = alpha;
			alpha = alpha * (1.0 - AlphaMaterialReduction);
			[branch] if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::AnisotropicFabric)
			{
#			if defined(SKINNED) || !defined(MODELSPACENORMALS)
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric2D(alpha, viewDirection, tbnTr);
#			else
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric1D(alpha, viewDirection, worldNormal.xyz);
#			endif
			}
			else if (AlphaMaterialModel == ExtendedTranslucency::MaterialModel::IsotropicFabric)
			{
				alpha = ExtendedTranslucency::GetViewDependentAlphaFabric1D(alpha, viewDirection, worldNormal.xyz);
			}
			else
			{
				alpha = ExtendedTranslucency::GetViewDependentAlphaNaive(alpha, viewDirection, worldNormal.xyz);
			}
			alpha = saturate(ExtendedTranslucency::SoftClamp(alpha, 2.0f - AlphaMaterialSoftness));
			alpha = lerp(alpha, originalAlpha, AlphaMaterialStrength);
		}
	}
#			endif  // ANISOTROPIC_ALPHA

	psout.Diffuse.w = alpha;
#		endif  // !DCLF_DEPTH_ONLY
#	endif

#	if !defined(DCLF_DEPTH_ONLY)

#	if defined(LIGHT_LIMIT_FIX) && defined(LLFDEBUG)
	if (SharedData::lightLimitFixSettings.EnableLightsVisualisation) {
		if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 0) {
			psout.Diffuse.xyz = Color::TurboColormap(LightLimitFix::NumStrictLights >= 7.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 1) {
			psout.Diffuse.xyz = Color::TurboColormap((float)LightLimitFix::NumStrictLights / 15.0);
		} else if (SharedData::lightLimitFixSettings.LightsVisualisationMode == 2) {
			psout.Diffuse.xyz = Color::TurboColormap((float)numClusteredLights / MAX_CLUSTER_LIGHTS);
		} else {
			psout.Diffuse.xyz = shadowColor.xyz;
		}
		baseColor.xyz = 0.0;
	} else {
		psout.Diffuse.xyz = color.xyz;
	}
#	else
	psout.Diffuse.xyz = color.xyz;
#	endif  // defined(LIGHT_LIMIT_FIX)

#	if defined(DEFERRED)
	psout.MotionVectors.xy = screenMotionVector.xy;
	psout.MotionVectors.zw = float2(0, psout.Diffuse.w);

#		if defined(TERRAIN_BLENDING)
	[flatten] if (SharedData::terrainBlendingSettings.Enabled)
	{
		psout.Diffuse.w = blendFactorTerrain;
	}
#		endif

	psout.MotionVectors.zw = float2(0.0, psout.Diffuse.w);
	psout.Specular = float4(specularColor, psout.Diffuse.w);
	psout.Albedo = float4(outputAlbedo, psout.Diffuse.w);

#		if defined(WETNESS_EFFECTS)
	indirectLobeWeights.specular += wetnessReflectance;
	if (waterRoughnessSpecular < 1) {
		// Reflection is from the water film surface; wetnessReflectance scales intensity by wetness amount.
		screenSpaceNormal = normalize(FrameBuffer::WorldToView(wetnessNormal, false));
		material.Roughness = waterRoughnessSpecular;
	}
#		endif

	psout.Reflectance = float4(indirectLobeWeights.specular, psout.Diffuse.w);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(screenSpaceNormal), saturate(1.0 - material.Roughness), psout.Diffuse.w);

#		if defined(SNOW)
#			if defined(TRUE_PBR)
	psout.Parameters.x = Color::RGBToLuminanceAlternative(specularColor);
	psout.Parameters.y = 0;
#			else
	psout.Parameters.x = Color::RGBToLuminanceAlternative(lightsSpecularColor);
#			endif
	psout.Parameters.w = psout.Diffuse.w;
#		endif

#		if defined(TRUE_PBR)
	float masksZ = Color::RGBToYCoCg(directionalAmbientColor + transmittedAmbientColor * Color::PBRLightingScale * vertexColor).x;
#		else
	float masksZ = Color::RGBToYCoCg(directionalAmbientColor).x;
#		endif

#		if defined(SSS) && defined(SKIN)
#			if defined(DCLF_BINDLESS)
	// Drawcall Limit Fix: per actor, from the object record (the permutation is the pipeline's).
	const bool beastRace = (DCLFObjects[DCLFObjectIndex].DCLFRecordFlags & 1u) != 0;
#			else
	const bool beastRace = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::IsBeastRace) != 0;
#			endif
	psout.Masks = float4(saturate(baseColor.a), !beastRace, masksZ, psout.Diffuse.w);
#		else
	psout.Masks = float4(0, 0, masksZ, psout.Diffuse.w);
#		endif

	// Stored as 1 - vertexAO so the cleared default (0) means no occlusion
	// for pixels that do not write to this RT (sky, water, grass, effects).
	psout.Masks2 = float4(1.0 - vertexAO, 0, 0, psout.Diffuse.w);

	// Additive layers (e.g. weapon blood sheen) only add light, so keep the normals and glossiness beneath them
	const bool additiveLighting = Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::AdditiveLighting;
	float stochasticBlend = !additiveLighting && (screenNoise * screenNoise) < psout.Diffuse.w ? 1.0 : 0.0;
	psout.NormalGlossiness.w = stochasticBlend;
#	endif

#	if defined(ENABLE_LL)
	if (Permutation::RenderToUI && !(Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection))
		psout.Diffuse.xyz = ColorManagement::WorkingToUI(psout.Diffuse.xyz);
#	endif

#	if !defined(DEFERRED)
	float3 ssrNormal = screenSpaceNormal;
	ssrNormal.z = max(0.001, sqrt(8.0 - 8.0 * ssrNormal.z));
	ssrNormal.xy /= ssrNormal.zz;

	float4 normalAndSSR;
	normalAndSSR.xy = ssrNormal.xy + 0.5.xx;
	normalAndSSR.z = 0.0;
	normalAndSSR.w = DCLF_SSR_SPECULAR * smoothstep(SSRParams.x - 1e-5, SSRParams.y, normal.w);

	const bool outputColorToAuxiliaryTarget = SSRParams.z > 1e-5;
	psout.NormalGlossiness = outputColorToAuxiliaryTarget ? psout.Diffuse : normalAndSSR;
	psout.MotionVectors = outputColorToAuxiliaryTarget ? float4(1, 0, 0, 1) : float4(screenMotionVector, 0, 1);
#	endif

#		if defined(DCLF_SHADOW_DEBUG) && defined(DEFERRED)
	// [TEMP] Native and DCLF alike: the sun's shadow terms in place of the colour, read back with CS_DCLF_TARGET_PROBE
	// (dclf-open-defects.md, "The VSM soft shadow differs"): x the VSM soft shadow, y its detailed term, z the detailed
	// shadow the lighting uses.
	psout.Diffuse = float4(dirSoftShadow, dirVSMDetailedShadow, dirDetailedShadow, 1);
#		endif
#	endif  // !DCLF_DEPTH_ONLY

#	if defined(EMAT)
#		undef COMPUTE_TERRAIN_SHADOW_BASE
#		undef EVAL_TERRAIN_DIR_SHADOW
#		undef LANDSCAPE_PARALLAX_ENABLED
#	endif

#	if defined(DCLF_DEPTH_ONLY) && defined(DCLF_OWNED_PARITY)
	// CS_DCLF_FOLIAGE_PARITY: the fragment the alpha test kept, as its pixel's owner if it is the closest (its depth inverted
	// above its object): the compare pass checks that the colour pass shaded the owner of each pixel whose depth it is.
	{
		const uint2 pixel = uint2(input.Position.xy);
		if (pixel.x < DCLFFoliageParityFrame.x && pixel.y < DCLFFoliageParityFrame.y) {
			RWStructuredBuffer<uint64_t> owners = ResourceDescriptorHeap[DCLFFoliageParityFrame.w];
			// Bit 31: terrain LOD's, counted on its own by the compare pass.
#		if defined(LODLANDNOISE) || defined(LODLANDSCAPE)
			const uint ownerTag = 0x80000000u;
#		else
			const uint ownerTag = 0;
#		endif
			const uint64_t owner = (uint64_t(0xFFFFFFFFu - asuint(input.Position.z)) << 32) | uint64_t(((DCLFObjectIndex + 1) & 0x00FFFFFFu) | ownerTag);
			InterlockedMax(owners[pixel.y * DCLFFoliageParityFrame.x + pixel.x], owner);
		}
	}
#	endif
#	if defined(DCLF_DEPTH_ONLY)
	// Drawcall Limit Fix's Z-prepass builds this permutation with DCLF_DEPTH_ONLY: the pass has no render
	// targets and only needs the depth, so it keeps the alpha test and nothing else - the lighting before it
	// and the outputs after it are compiled out (see above). That makes the prepass cheap and, more
	// importantly, keeps the shader from reading the per-frame pixel-stage bindings: they are not bound
	// while the native depth pass runs.
	return (PS_OUTPUT)0;
#	else
#		if defined(DCLF_OWNED_PARITY)
	// CS_DCLF_FOLIAGE_PARITY: what this pixel shows of the object, for the compare pass (FoliageParityCS.hlsl).
	{
		const uint2 pixel = uint2(input.Position.xy);
		if (pixel.x < DCLFFoliageParityFrame.x && pixel.y < DCLFFoliageParityFrame.y) {
			const uint index = pixel.y * DCLFFoliageParityFrame.x + pixel.x;
			const uint64_t ids = (uint64_t(DCLFFoliageParityBuffers.y) << 32) | uint64_t(DCLFFoliageParityBuffers.x);
			const uint64_t colours = (uint64_t(DCLFFoliageParityBuffers.w) << 32) | uint64_t(DCLFFoliageParityBuffers.z);
#			if defined(DEFERRED)
			const uint4 albedo = uint4(round(saturate(psout.Albedo) * 255.0));
#			else
			const uint4 albedo = uint4(round(saturate(psout.Diffuse) * 255.0));
#			endif
			const uint4 diffuse = uint4(round(saturate(psout.Diffuse) * 255.0));
			vk::RawBufferStore<uint>(ids + uint64_t(index) * 4, (DCLFFoliageParityFrame.z << 24) | ((DCLFObjectIndex + 1) & 0x00FFFFFFu));
			// The motion vector against a static object's: the same point where the camera saw it the frame before (its absolute
			// position, from this frame's eye, made relative to the frame before's). In pixels, with the motion vector itself.
			float2 motionError = 0;
#			if !defined(SKINNED)
			const float4 staticPrevious = float4(input.WorldPosition.xyz + FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz, 1);
			const float2 renderSize = float2(DCLFFoliageParityFrame.xy);
			motionError = (psout.MotionVectors.xy - MotionBlur::GetSSMotionVector(input.WorldPosition, staticPrevious)) * renderSize;
#			endif
			const float2 motion = psout.MotionVectors.xy * float2(DCLFFoliageParityFrame.xy);
			// The albedo's alpha byte carries the motion error's length (tenths of a pixel); the specular is the pass's.
			const uint error = uint(min(round(length(motionError) * 10.0), 255.0));
#			if defined(DEFERRED)
			const uint4 specular = uint4(round(saturate(psout.Specular) * 255.0));
#			else
			const uint4 specular = 0;
#			endif
			vk::RawBufferStore<uint4>(colours + uint64_t(index) * 16,
				uint4(albedo.x | (albedo.y << 8) | (albedo.z << 16) | (error << 24), diffuse.x | (diffuse.y << 8) | (diffuse.z << 16) | (diffuse.w << 24),
					specular.x | (specular.y << 8) | (specular.z << 16) | (specular.w << 24), f32tof16(motion.x) | (f32tof16(motion.y) << 16)));
		}
	}
#		endif
	return psout;
#	endif
}
#	if defined(DCLF_PULLED)
// The draw's rows (its sequence's: the pipeline row's address, then the material row's), and the blocks the pulled source reads
// from them (ShaderPrograms.cpp, PulledLightingSource), before anything reads them.
void DCLFRowStatics(uint4 a_rows)
{
	DCLFPipelineRowAddress = DCLFAddress(a_rows.xy);
	DCLFMaterialRowAddress = DCLFAddress(a_rows.zw);
	DCLFPerTechniqueStatics();
	DCLFPerMaterialStatics();
	DCLFPerGeometryStatics();
	DCLFPermutationStatics();
}

// The object's values the DCLF_BINDLESS build's statics hold, assigned once this draw's object word is known.
void DCLFPixelObjectStatics()
{
#		if defined(DCLF_BINDLESS)
	DCLFLodFades = DCLF_PS_LOD_FADES;
	DCLFLodMetricValue = DCLF_PS_LOD_METRIC;
	DCLFSpecularLodFade = DCLF_PS_SPECULAR_LOD_FADE;
	MaterialData = DCLF_PS_MATERIAL_DATA;
	EmitColor = DCLFObjects[DCLFObjectIndex].EmitColor.xyz;
	DCLFSSRSpecular = DCLF_PS_SSR_SPECULAR;
	ProjectedUVParams = DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 4];
	ProjectedUVParams2 = DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 5];
	ProjectedUVParams3 = DCLFBones[DCLFObjects[DCLFObjectIndex].DCLFExtraOffset + 6];
#		endif
#		if defined(DCLF_BINDLESS_DRAW)
	AlphaTestRefRS = DCLFObjects[DCLFObjectIndex].AlphaTestRef;
#		endif
}

// Drawcall Limit Fix's pulled Z-prepass (DCLF_PULLED): the draw's object word and rows are its vertex stage's.
PS_OUTPUT main(PS_INPUT input, bool frontFace : SV_IsFrontFace)
{
	DCLFObjectWord = input.DCLFDrawObject;
	DCLFRowStatics(input.DCLFDraw.zwxy);
	DCLFPixelObjectStatics();
	return DCLFShadePS(input, frontFace);
}
#	endif  // DCLF_PULLED
#endif  // PSHADER
