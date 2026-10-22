// SPDX-License-Identifier: MIT AND (LicenseRef-GPL-3.0-with-Modding-Exception WITH GPL-3.0-linking-source-exception)
// Original code Copyright (c) 2022 Ilya Perapechka (MIT), modifications GPL-3.0 with Modding Exception (see EXCEPTIONS.md)

// A pulled draw (DCLF_PULLED, below) has no permutation block to bind: Permutation.hlsli's values are statics, zero (the
// main view's draws, never a reflection's).
#if defined(DCLF_PULLED) && !defined(DCLF_PULLED_ROWS)
#	define DCLF_PULLED_ROWS
#endif
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/Permutation.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#if !defined(DYNAMIC_CUBEMAPS) && defined(IBL)
#	undef IBL
#endif

#if defined(DCLF_PULLED)
// Drawcall Limit Fix's tree LOD draws (dclf-architecture.md, "Tree LOD: the draws"; Scene/TreeLod.h): one non-indexed instanced draw
// per pass, whose instances are the culled list's records. The vertex stage fetches its record, its shape's row, its mesh's
// index and vertex through the device addresses in the draw row; the pixel stages take the texture, the sampler and the
// alpha reference from it. The draw's push data (DrawPipelines.h, kDrawPushBinding) holds the draw row's address, and whether
// the draw is the Z-prepass's second phase, whose instances follow phase 1's in the list (TreeLodCullCS.hlsl).
cbuffer DCLFTreePush : register(b190)
{
	uint2 DCLFTreeDrawAddress : packoffset(c0.x);
	uint DCLFTreePhaseTwo : packoffset(c0.z);
	uint DCLFTreeDebugTint : packoffset(c0.w);  // CS_DCLF_TREE_LOD_TINT: the colour draw writes solid magenta
	uint2 DCLFTreePushUnused1 : packoffset(c1.x);
};
uint64_t DCLFTreeAddress(uint2 a_words) { return (uint64_t(a_words.y) << 32) | uint64_t(a_words.x); }
uint64_t DCLFTreeDraw() { return DCLFTreeAddress(DCLFTreeDrawAddress); }
// TreeLod::DrawRow
uint64_t DCLFTreeTable(uint a_offset) { return DCLFTreeAddress(vk::RawBufferLoad<uint2>(DCLFTreeDraw() + a_offset, 8)); }
static const uint kDCLFTreeInstances = 0, kDCLFTreeShapes = 8, kDCLFTreeMeshes = 16, kDCLFTreeVisible = 24;
static const uint kDCLFTreeTexture = 32, kDCLFTreeSampler = 36, kDCLFTreeAlphaRef = 40;
static const uint kDCLFTreeGroupInstances = 75;
// TreeLod::VisibleHeader, before the visible list: phase 1's instance count, where phase 2's instances start.
static const uint kDCLFTreeHeaderBytes = 64, kDCLFTreePhaseOneInstances = 4;
// CS_DCLF_TREE_LOD_STAGES (TreeInstancesLayout.h, kStageSlots): the draw row's stamps buffer (0: none) and frame; a stage stores the
// frame in its word of the frame's slot (every invocation the same value: no atomics).
static const uint kDCLFTreeStats = 52, kDCLFTreeFrame = 60;
static const uint kDCLFStageDepthVS = 0, kDCLFStagePhaseTwoVS = 1, kDCLFStageDepthPS = 2, kDCLFStageColourVS = 3, kDCLFStageColourPS = 4,
				  kDCLFStageColourSaw = 5, kDCLFStageDepthClip = 8, kDCLFStageColourClip = 12, kDCLFStageColourEntry = 16, kDCLFStageDepthEntry = 17,
				  kDCLFStageColourProbe = 18, kDCLFStageDepthProbe = 25, kDCLFStageWords = 32;
uint64_t DCLFTreeStageWord(uint a_word)
{
	const uint64_t stats = DCLFTreeAddress(vk::RawBufferLoad<uint2>(DCLFTreeDraw() + kDCLFTreeStats, 4));
	if (stats == 0)
		return 0;
	return stats + 4 * uint64_t((vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeFrame, 4) & 7u) * kDCLFStageWords + a_word);
}
void DCLFTreeStamp(uint a_stage)
{
	const uint64_t word = DCLFTreeStageWord(a_stage);
	if (word != 0)
		vk::RawBufferStore<uint>(word, vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeFrame, 4), 4);
}
#endif

#if defined(VSHADER)
struct VS_INPUT
{
	float3 Position: POSITION0;
	float2 TexCoord0: TEXCOORD0;
	float4 InstanceData1: TEXCOORD4;
	float4 InstanceData2: TEXCOORD5;
	float4 InstanceData3: TEXCOORD6;
	float4 InstanceData4: TEXCOORD7;
};
#endif

struct VS_OUTPUT
{
	float4 Position: SV_POSITION0;
	float3 TexCoord: TEXCOORD0;

#if defined(RENDER_DEPTH)
	float4 Depth: TEXCOORD3;
#else
	float4 WorldPosition: POSITION1;
	float4 PreviousWorldPosition: POSITION2;
#endif  // RENDER_DEPTH
	float4 ViewPosition: POSITION3;
};

#ifdef VSHADER
#	if defined(DCLF_PULLED)
// The record's instance data (TEXCOORD4, TEXCOORD5 of the engine's draw), its mesh's vertex, and its shape's translation: the
// engine's World is the shape's, a translation. Precise throughout: the Z-prepass and the colour pass each compute the
// position, and the colour pass tests EQUAL against the Z-prepass's depth.
VS_OUTPUT main(uint a_vertex : SV_VertexID, uint a_instance : SV_InstanceID)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;
	const uint64_t visible = DCLFTreeTable(kDCLFTreeVisible);
	const uint first = DCLFTreePhaseTwo != 0 ? vk::RawBufferLoad<uint>(visible - kDCLFTreeHeaderBytes + kDCLFTreePhaseOneInstances, 4) : 0;
#		if defined(RENDER_DEPTH)
	DCLFTreeStamp(DCLFTreePhaseTwo != 0 ? kDCLFStagePhaseTwoVS : kDCLFStageDepthVS);
#		else
	DCLFTreeStamp(kDCLFStageColourVS);
	if (a_instance == 0 && a_vertex == 0) {
		const uint64_t saw = DCLFTreeStageWord(kDCLFStageColourSaw);
		if (saw != 0) {
			// The list header's phase 1, phase 2 and colour instance counts as this draw read them.
			const uint64_t header = visible - kDCLFTreeHeaderBytes;
			vk::RawBufferStore<uint3>(saw, uint3(vk::RawBufferLoad<uint>(header + 4, 4), vk::RawBufferLoad<uint>(header + 20, 4), vk::RawBufferLoad<uint>(header + 36, 4)), 4);
		}
	}
#		endif
	const uint index = vk::RawBufferLoad<uint>(visible + 4 * uint64_t(first + a_instance), 4);
	const uint64_t shape = DCLFTreeTable(kDCLFTreeShapes) + 32 * uint64_t(index / kDCLFTreeGroupInstances);
	const float3 translate = asfloat(vk::RawBufferLoad<uint3>(shape, 4));
	const uint meshSlot = vk::RawBufferLoad<uint>(shape + 12, 4);
	if (meshSlot == 0xFFFFFFFFu)
		return vsout;
	const uint64_t mesh = DCLFTreeTable(kDCLFTreeMeshes) + 32 * uint64_t(meshSlot);
	const uint4 meshWords = vk::RawBufferLoad<uint4>(mesh, 8);  // vertex address, index address
	const uint3 meshLayout = vk::RawBufferLoad<uint3>(mesh + 16, 4);  // stride, index count, texcoord offset
	// Past the mesh's indices (the draw draws the largest mesh's count): a degenerate triangle.
	if (a_vertex >= meshLayout.y)
		return vsout;
	const uint indexWord = vk::RawBufferLoad<uint>(DCLFTreeAddress(meshWords.zw) + 4 * uint64_t(a_vertex / 2), 4);
	const uint vertexIndex = (a_vertex & 1) ? (indexWord >> 16) : (indexWord & 0xFFFF);
	const uint64_t vertex = DCLFTreeAddress(meshWords.xy) + uint64_t(vertexIndex) * meshLayout.x;
	VS_INPUT input = (VS_INPUT)0;
	input.Position = asfloat(vk::RawBufferLoad<uint3>(vertex, 4));
	const uint texcoord = vk::RawBufferLoad<uint>(vertex + meshLayout.z, 4);
	input.TexCoord0 = float2(f16tof32(texcoord), f16tof32(texcoord >> 16));
	const uint4 record = vk::RawBufferLoad<uint4>(DCLFTreeTable(kDCLFTreeInstances) + 32 * uint64_t(index), 16);
	input.InstanceData1 = float4(f16tof32(record.x), f16tof32(record.x >> 16), f16tof32(record.y), f16tof32(record.y >> 16));
	input.InstanceData2 = float4(f16tof32(record.z), f16tof32(record.z >> 16), f16tof32(record.w), f16tof32(record.w >> 16));

	precise float3 scaledModelPosition = input.InstanceData1.www * input.Position.xyz;
	precise float3 adjustedModelPosition = 0.0.xxx;
	adjustedModelPosition.x = dot(float2(1, -1) * input.InstanceData2.xy, scaledModelPosition.xy);
	adjustedModelPosition.y = dot(input.InstanceData2.yx, scaledModelPosition.xy);
	adjustedModelPosition.z = scaledModelPosition.z;
	precise float3 modelPosition = input.InstanceData1.xyz + adjustedModelPosition.xyz;
	precise float3 worldPosition = modelPosition + translate;
	precise float4 relativePosition = float4(worldPosition - FrameBuffer::CameraPosAdjust.xyz, 1.0);
	precise float4 viewPosition = mul(FrameBuffer::CameraViewProj, relativePosition);

#		ifdef RENDER_DEPTH
	vsout.Depth.xy = viewPosition.zw;
	vsout.Depth.zw = input.InstanceData2.zw;
#		else
	vsout.WorldPosition = relativePosition;
	vsout.PreviousWorldPosition = float4(worldPosition - FrameBuffer::CameraPreviousPosAdjust.xyz, 1.0);
	vsout.ViewPosition = viewPosition;
#		endif  // RENDER_DEPTH

	vsout.Position = viewPosition;
	// CS_DCLF_TREE_LOD_STAGES: the clip position of the list's first instance's first vertex, depth's (phase 1) and colour's.
	if (DCLFTreePhaseTwo == 0 && a_instance == 0 && a_vertex == 0) {
#		ifdef RENDER_DEPTH
		const uint64_t clip = DCLFTreeStageWord(kDCLFStageDepthClip);
#		else
		const uint64_t clip = DCLFTreeStageWord(kDCLFStageColourClip);
#		endif
		if (clip != 0)
			vk::RawBufferStore<uint4>(clip, asuint(viewPosition), 4);
	}
	vsout.TexCoord = float3(input.TexCoord0.xy, 0.0);

	return vsout;
}
#	else
cbuffer PerTechnique : register(b0)
{
	float4 FogParam : packoffset(c0);
};

cbuffer PerGeometry : register(b2)
{
	row_major float4x4 WorldViewProj : packoffset(c0);
	row_major float4x4 World : packoffset(c4);
	row_major float4x4 PreviousWorld : packoffset(c8);
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout = (VS_OUTPUT)0;

	float3 scaledModelPosition = input.InstanceData1.www * input.Position.xyz;
	float3 adjustedModelPosition = 0.0.xxx;
	adjustedModelPosition.x = dot(float2(1, -1) * input.InstanceData2.xy, scaledModelPosition.xy);
	adjustedModelPosition.y = dot(input.InstanceData2.yx, scaledModelPosition.xy);
	adjustedModelPosition.z = scaledModelPosition.z;
	float4 finalModelPosition = float4(input.InstanceData1.xyz + adjustedModelPosition.xyz, 1.0);
	float4 viewPosition = mul(WorldViewProj, finalModelPosition);

#	ifdef RENDER_DEPTH
	vsout.Depth.xy = viewPosition.zw;
	vsout.Depth.zw = input.InstanceData2.zw;
#	else
	vsout.WorldPosition = mul(World, finalModelPosition);
	vsout.PreviousWorldPosition = mul(PreviousWorld, finalModelPosition);
	vsout.ViewPosition = viewPosition;
#	endif  // RENDER_DEPTH

	vsout.Position = viewPosition;
	vsout.TexCoord = float3(input.TexCoord0.xy, FogParam.z);

	return vsout;
}
#	endif  // DCLF_PULLED
#endif  // VSHADER

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Diffuse: SV_Target0;

#if !defined(RENDER_DEPTH)
#	if defined(DEFERRED)
	float2 MotionVector: SV_Target1;
	float4 Normal: SV_Target2;
	float4 Albedo: SV_Target3;
	float4 Masks: SV_Target6;
#	endif  // DEFERRED
#endif      // !RENDER_DEPTH
};

#ifdef PSHADER
#	if defined(DCLF_PULLED)
SamplerState DCLFTreeSampler()
{
	SamplerState s = SamplerDescriptorHeap[vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeSampler, 4)];
	return s;
}
Texture2D<float4> DCLFTreeTexture()
{
	Texture2D<float4> t = ResourceDescriptorHeap[vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeTexture, 4)];
	return t;
}
#		define SampDiffuse DCLFTreeSampler()
#		define TexDiffuse DCLFTreeTexture()
#		define AlphaTestRefRS asfloat(vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeAlphaRef, 4))
// CS_DCLF_TREE_LOD_STAGES: the stage's entry stamp and what its texture resolves to (TreeInstancesLayout.h, kStageProbeWords).
void DCLFTreeProbe(uint a_entry, uint a_probe, float2 a_texCoord)
{
	DCLFTreeStamp(a_entry);
	const uint64_t probe = DCLFTreeStageWord(a_probe);
	if (probe == 0)
		return;
	uint width, height, mips;
	DCLFTreeTexture().GetDimensions(0, width, height, mips);
	const float alpha = DCLFTreeTexture().SampleBias(DCLFTreeSampler(), a_texCoord, SharedData::MipBias).w;
	vk::RawBufferStore<uint4>(probe, uint4(vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeTexture, 4), vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeSampler, 4), width, height), 4);
	vk::RawBufferStore<uint3>(probe + 16, uint3(mips, vk::RawBufferLoad<uint>(DCLFTreeDraw() + kDCLFTreeAlphaRef, 4), asuint(alpha)), 4);
}
static const float4 DiffuseColor = 0.0;
static const float4 AmbientColor = 0.0;
#	else
SamplerState SampDiffuse : register(s0);

Texture2D<float4> TexDiffuse : register(t0);

cbuffer AlphaTestRefCB : register(b11)
{
	float AlphaTestRefRS : packoffset(c0);
}

cbuffer PerFrame : register(b12)
{
	float4 UnknownPerFrame1[12] : packoffset(c0);
	row_major float4x4 ScreenProj : packoffset(c12);
	row_major float4x4 PreviousScreenProj : packoffset(c16);
}

cbuffer PerTechnique : register(b0)
{
	float4 DiffuseColor : packoffset(c0);
	float4 AmbientColor : packoffset(c1);
};
#	endif  // DCLF_PULLED

const static float DepthOffsets[16] = {
	0.003921568,
	0.533333361,
	0.133333340,
	0.666666687,
	0.800000000,
	0.266666681,
	0.933333337,
	0.400000000,
	0.200000000,
	0.733333349,
	0.066666670,
	0.600000000,
	0.996078432,
	0.466666669,
	0.866666675,
	0.333333343
};

#	if defined(SCREEN_SPACE_SHADOWS)
#		include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	endif

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

#	if defined(EXP_HEIGHT_FOG)
#		define SampColorSampler SampDiffuse
#		include "ExponentialHeightFog/ExponentialHeightFog.hlsli"
#	endif

#	define LinearSampler SampDiffuse

#	include "Common/ShadowSampling.hlsli"

#	if defined(EXP_HEIGHT_FOG)
void ApplyReflectionExponentialHeightFog(inout float3 color, float3 positionWS, float4 screenPosition)
{
	float3 fogColor = ColorManagement::SRGBToWorking(AmbientColor.xyz);
	float4 exponentialHeightFog = ExponentialHeightFog::GetExponentialHeightFogNoVolumetric(positionWS, FrameBuffer::CameraPosAdjust.xyz, fogColor, float4(screenPosition.xy * FrameBuffer::DynamicResolutionParams2.xy, screenPosition.z, 1));
	color = lerp(color, exponentialHeightFog.xyz, exponentialHeightFog.w);
}
#	endif

#	if defined(DCLF_PULLED) && defined(RENDER_DEPTH)
void main(PS_INPUT input)
{
	DCLFTreeProbe(kDCLFStageDepthEntry, kDCLFStageDepthProbe, input.TexCoord.xy);
	uint2 temp = uint2(input.Position.xy);
	uint index = ((temp.x << 2) & 12) | (temp.y & 3);

	float depthOffset = 0.5 - DepthOffsets[index];
	float depthModifier = (input.Depth.w * depthOffset) + input.Depth.z - 0.5;

	if (depthModifier < 0) {
		discard;
	}

	float alpha = TexDiffuse.SampleBias(SampDiffuse, input.TexCoord.xy, SharedData::MipBias).w;

	if ((alpha - AlphaTestRefRS) < 0) {
		discard;
	}
	DCLFTreeStamp(kDCLFStageDepthPS);
}
#	else
#		if defined(DCLF_PULLED) && defined(DEFERRED)
// CS_DCLF_TREE_LOD_STAGES' entry stamp counts fragments that passed EQUAL (no depth writes: early depth changes nothing drawn).
[earlydepthstencil]
#		endif
PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
#		if defined(DCLF_PULLED)
	DCLFTreeProbe(kDCLFStageColourEntry, kDCLFStageColourProbe, input.TexCoord.xy);
#		endif

#	if defined(EXP_HEIGHT_FOG)
	const bool inReflection = (Permutation::ExtraShaderDescriptor & Permutation::ExtraFlags::InReflection) != 0;
#	endif

#	if defined(RENDER_DEPTH)
	uint2 temp = uint2(input.Position.xy);
	uint index = ((temp.x << 2) & 12) | (temp.y & 3);

	float depthOffset = 0.5 - DepthOffsets[index];
	float depthModifier = (input.Depth.w * depthOffset) + input.Depth.z - 0.5;

	if (depthModifier < 0) {
		discard;
	}

	float alpha = TexDiffuse.SampleBias(SampDiffuse, input.TexCoord.xy, SharedData::MipBias).w;

	if ((alpha - AlphaTestRefRS) < 0) {
		discard;
	}

	psout.Diffuse.xyz = input.Depth.xxx / input.Depth.yyy;
	psout.Diffuse.w = 0;
#	else
	float4 baseColor = TexDiffuse.SampleBias(SampDiffuse, input.TexCoord.xy, SharedData::MipBias);
	baseColor.xyz = Color::Albedo(ColorManagement::DiffuseToWorking(baseColor.xyz));

	if ((baseColor.w - AlphaTestRefRS) < 0) {
		discard;
	}

	float3 ddx = ddx_coarse(input.WorldPosition.xyz);
	float3 ddy = ddy_coarse(input.WorldPosition.xyz);
	float3 normal = -normalize(cross(ddx, ddy));

	float3 directionalAmbientColor = max(0, Color::Ambient(SharedData::GetAmbient(normal)));
#		if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		directionalAmbientColor = ImageBasedLighting::GetDiffuseIBL(directionalAmbientColor, -normal);
	}
#		endif

#		if defined(DEFERRED)
	float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WorldPosition.xyz, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);

	float dirShadow = 1;

#			if defined(SCREEN_SPACE_SHADOWS)
	dirShadow = lerp(1.0, ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, screenUV, screenNoise), 0.8);
#			endif

	if (dirShadow != 0.0)
		dirShadow *= ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);

	float3 diffuseColor = Color::DirectionalLight(SharedData::DirLightColor.xyz) * dirShadow * 0.5 * Color::BRDFScale;

#			if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		diffuseColor *= ExponentialHeightFog::GetSunlightFogAttenuation(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#			endif

	diffuseColor += directionalAmbientColor;

	psout.Diffuse.xyz = diffuseColor * baseColor.xyz;
	psout.Diffuse.w = 1;

#			if defined(EXP_HEIGHT_FOG)
	if (inReflection && SharedData::exponentialHeightFogSettings.enabled) {
		ApplyReflectionExponentialHeightFog(psout.Diffuse.xyz, input.WorldPosition.xyz, input.Position);
	}
#			endif

	psout.MotionVector = MotionBlur::GetSSMotionVector(input.WorldPosition, input.PreviousWorldPosition);

	psout.Normal.xy = GBuffer::EncodeNormal(FrameBuffer::WorldToView(normal, false));
	psout.Normal.zw = 0;

	psout.Albedo = float4(baseColor.xyz, 1);
	psout.Masks = float4(0, 0, 1, 0);
#		else
	float dirShadow = ShadowSampling::GetWorldShadow(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);

	float3 diffuseColor = Color::DirectionalLight(SharedData::DirLightColor.xyz) * dirShadow * 0.5 * Color::BRDFScale;

#			if defined(EXP_HEIGHT_FOG)
	if (SharedData::exponentialHeightFogSettings.enabled) {
		diffuseColor *= ExponentialHeightFog::GetSunlightFogAttenuation(input.WorldPosition.xyz, FrameBuffer::CameraPosAdjust.xyz);
	}
#			endif

	diffuseColor += directionalAmbientColor;

	float3 color = diffuseColor * baseColor.xyz;
#			if defined(EXP_HEIGHT_FOG)
	if (inReflection && SharedData::exponentialHeightFogSettings.enabled) {
		ApplyReflectionExponentialHeightFog(color, input.WorldPosition.xyz, input.Position);
	}
#			endif
	psout.Diffuse = float4(color, 1.0);
#		endif  // DEFERRED
#		if defined(DCLF_PULLED)
	DCLFTreeStamp(kDCLFStageColourPS);
	if (DCLFTreeDebugTint != 0) {
		psout.Diffuse.xyz = float3(4, 0, 4);
#			if defined(DEFERRED)
		psout.Albedo = float4(1, 0, 1, 1);
#			endif
	}
#		endif
#	endif      // RENDER_DEPTH

	return psout;
}
#	endif  // DCLF_PULLED && RENDER_DEPTH
#endif  // PSHADER
