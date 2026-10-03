#ifndef __DCLF_OBJECTS_DEPENDENCY_HLSL__
#define __DCLF_OBJECTS_DEPENDENCY_HLSL__

// Drawcall Limit Fix draws its permutations indirectly, and the handful of values that differ between the
// objects sharing one pipeline come from a GPU-resident table indexed by the draw's own object index
// rather than from per-draw constant buffers. That is what lets the PerGeometry buffer become
// per-pipeline and, once nothing else in the binding record is per-object, lets the record itself
// deduplicate to one per (material, pipeline) pair.
//
// This lives in its own header rather than in Lighting.hlsl because Color.hlsli consumes the emissive
// multiplier and is included before the point where the block used to sit.
#if defined(DCLF_BINDLESS)

struct DCLFObjectRecord
{
	float4 World[3];          // row_major float3x4, absolute: the vertex stage subtracts the drawing camera's eye
	float4 PreviousWorld[3];  // likewise, against the previous eye
	float4 MaterialData;
	float4 EmitColor;         // emissive in xyz, the per-object w of SSRParams in w
	// The values that used to reach the shader as constant buffers of their own: Light Limit Fix's room
	// index and shadow bit mask (b3), the alpha test reference (b11) and Linear Lighting's emissive
	// multiplier (b8). Only read when DCLF_BINDLESS_DRAW is also defined; the record carries them either
	// way so that the two builds share one layout.
	int RoomIndex;
	uint ShadowBitMask;
	float AlphaTestRef;
	float EmissiveMult;
	// Tree animation (technique 12). Per object: under DCLF_BINDLESS the PerGeometry buffer is one
	// block for the whole pipeline, and a tree's wind amplitude and clock are its own.
	float4 DCLFTreeParams;
	float4 DCLFWindTimers;  // xy used; zw the tree slot and its listing's generation (DCLFTreeWind)
	// Skinning: this object's bone palette rows in DCLFBones (three float4 rows a bone, current palette
	// at DCLFBoneOffset, the previous frame's at DCLFPreviousBoneOffset), absolute like World.
	uint DCLFBoneOffset;
	uint DCLFPreviousBoneOffset;
	uint DCLFBoneRows;
	// The object's rows of further per-object PerGeometry values in the same buffer, after every
	// palette: LandBlendParams, the three rows of TextureProj, then ProjectedUVParams, 2 and 3.
	uint DCLFExtraOffset;
	// Advanced Skin's SkinPerGeometry (b7): the owning actor's wetness, zero for everything else. Only read
	// when DCLF_BINDLESS_DRAW is also defined (Skin.hlsli).
	float4 DCLFSkinPerGeometry;
	// The specular and envmap LOD fades, made by the draw from the frame's camera (Lighting.hlsl, DCLFFrameLighting c6-c12):
	// the fade node's world bound centre, and its LOD type (bits 0-3) and which fades apply (bit 4 specular, 5 envmap, 6
	// SSRParams.w with specular) in the word; 0 when nothing fades and MaterialData's fades are the property's.
	float3 DCLFLodFadeNode;
	uint DCLFLodFadeFlags;
	// The culling's (BuildDrawsCS): the world bound (centre, radius), and the sun entry's sphere.
	float4 DCLFBound;
	float4 DCLFSunEntry;
};

#	if defined(DCLF_PULLED)
// A plain indirect draw's push data (DrawPipelines.h, kPulledPush*): pushed once per draw call, so nothing in it is a draw's
// own. The draw's sequence (its first one's address here, indexed by the draw's instance) holds its object word, its rows'
// addresses and its geometry's buffers, which the vertex stage reads (Utility.hlsl, the shadow views; Lighting.hlsl, the
// Z-prepass). The second address is the layout's: the shadow views' material rows, the Z-prepass's pipeline row.
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFSequencesAddress : packoffset(c0.x);
	uint2 DCLFMaterialRowsAddress : packoffset(c0.z);
	uint2 DCLFVertexLayout : packoffset(c1.x);
};

// The object word (below), set by the stage's main from the draw's sequence (the vertex stage) or its vertex stage's output
// (the pixel stage) before anything reads it.
static uint DCLFObjectWord;
#		define DCLFObjectIndex (DCLFObjectWord & 0x03FFFFFFu)
#		define DCLFLocalShadowMask ((DCLFObjectWord >> 26) & 0xFu)
#		define DCLFSunMiss ((DCLFObjectWord & 0x80000000u) != 0)
#	else
// The indirect draw's push data (DrawPipelines.h, kDrawPushWords). The first four words are its rows' addresses (the
// pipeline row's, then the material row's), which the pipeline layout consumes to resolve this draw's buffers and
// descriptors; only the object word is read here.
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFPipelineRowAddress : packoffset(c0.x);
	uint2 DCLFMaterialRowAddress : packoffset(c0.z);
	uint DCLFObjectWord : packoffset(c1.x);
};

// The object word (Records.h): the object's index in bits 0-25; the local shadow lights BuildDrawsCS selected for the draw
// in bits 26-29 (Light Limit Fix's ShadowBitMask, whose lights are shadow mask channels 0-3); and in the top bit
// kObjectSunMiss, which BuildDrawsCS sets on a pass carrying the sun's bits whose bound meets none of this frame's cascades.
static const uint DCLFObjectIndex = DCLFObjectWord & 0x03FFFFFFu;
static const uint DCLFLocalShadowMask = (DCLFObjectWord >> 26) & 0xFu;
static const bool DCLFSunMiss = (DCLFObjectWord & 0x80000000u) != 0;
#	endif

// The object rows, by the draw's object index. Read as a structured buffer: reading the row as a constant buffer at its
// address (a per-draw push address) measured no faster on NVIDIA (dclf-architecture.md, "Object rows").
StructuredBuffer<DCLFObjectRecord> DCLFObjects : register(t127);
// The epoch's row buffer (IndirectDraws: kBonesBufferRegister): every skinned object's bone palette
// rows end to end, current then previous, and after them the per-object extras rows (DCLFExtraOffset).
StructuredBuffer<float4> DCLFBones : register(t126);
// The trees' wind (IndirectDraws: kTreeWindRegister; TreeWindCS.hlsl): three rows an entry, TreeParams, WindTimers and the
// listing's generation (x), the
// nodeless entry first and tree slot s at entry s + 1, as the compute queue made them the frame before (it writes this
// frame's into the other buffer). An entry is the object's only for the tree listing its record names (DCLFWindTimers.zw:
// the tree slot and that listing's generation, never 0); otherwise the record's own values, those it joined with, are drawn.
StructuredBuffer<float4> DCLFTreeWind : register(t124);

#	if defined(DCLF_PULLED)
// A pulled draw's own (its vertex stage reads them from its sequence, DCLF_PULLED above).
static const uint kDCLFSequenceStride = 92;  // BuildDrawsCS's DrawSequence
// The draw's vertex layout, which DCLFAttribute decodes: set by the stage's main before any attribute is read, from the push
// data (Utility.hlsl: a shadow view's call is one pipeline's, so one layout's) or from the draw's pipeline row (Lighting.hlsl:
// a Z-prepass call draws every pipeline slot that shares its depth pipeline, so each draw has its slot's).
static uint2 DCLFDrawVertexLayout;

uint64_t DCLFAddress(uint2 a_words) { return (uint64_t(a_words.y) << 32) | uint64_t(a_words.x); }

// A stream of the geometry: the vertex buffer (stream 0) or a dynamic shape's positions (stream 1).
struct DCLFStream
{
	uint64_t address;
	uint stride;
};

// The address of attribute a_attribute (BSGraphics::Vertex::Attribute) of vertex a_index: its stream as the layout flags it
// (bit 44 + a in stream 0, else bit 54 + a), and its offset the layout's nibble times four, except the position's (0). As the
// engine's input layout for the vertex layout has them (VertexInput.cpp, BuildVertexElements).
uint64_t DCLFAttribute(uint a_attribute, uint a_index, DCLFStream a_first, DCLFStream a_second)
{
	const bool first = ((DCLFDrawVertexLayout.y >> (12 + a_attribute)) & 1) != 0;
	const uint offset = a_attribute == 0 ? 0 : ((DCLFDrawVertexLayout.x >> (4 * a_attribute + 4)) & 0xF) * 4;
	const uint64_t address = first ? a_first.address : a_second.address;
	const uint stride = first ? a_first.stride : a_second.stride;
	return address + uint64_t(a_index) * stride + offset;
}

float4 DCLFUnorm4(uint a_word) { return float4(a_word & 0xFF, (a_word >> 8) & 0xFF, (a_word >> 16) & 0xFF, a_word >> 24) / 255.0; }
float2 DCLFHalf2(uint a_word) { return float2(f16tof32(a_word), f16tof32(a_word >> 16)); }

// The draw's rows (DrawPipelines.h, kMaterialRow*, kPipelineRow*), for a pulled Lighting stage: the material row's PerMaterial
// block (b1), textures and samplers, and the pipeline row's PerTechnique (b0), PerGeometry template (b2) and permutation (b4)
// blocks and its shadow mask (t14, s14), which a device-generated draw's layout would map from the rows' addresses in the
// draw's push data. A pulled build reads them itself, from the rows its sequence names (ShaderPrograms.cpp,
// PulledLightingSource, which turns Lighting.hlsl's declarations of them into these reads; Permutation.hlsli, DCLF_PULLED_ROWS).
static uint64_t DCLFMaterialRowAddress;
static uint64_t DCLFPipelineRowAddress;
static const uint kDCLFMaterialRowHeader = 768;
static const uint kDCLFPipelineRowHeader = 1536;
static const uint kDCLFPermutationBlock = 1280;  // one block, both stages
#		if defined(VSHADER)
static const uint kDCLFMaterialBlock = 0;
static const uint kDCLFTechniqueBlock = 0;
static const uint kDCLFGeometryBlock = 512;
#		else
static const uint kDCLFMaterialBlock = 256;
static const uint kDCLFTechniqueBlock = 256;
static const uint kDCLFGeometryBlock = 768;
#		endif
uint DCLFMaterialTextureIndex(uint a_register)
{
	// MaterialRowHeader: two addresses, textures[16], samplers[16], features (t71, t74).
	const uint slot = a_register == 71 ? 32 : a_register == 74 ? 33 : a_register;
	return vk::RawBufferLoad<uint>(DCLFMaterialRowAddress + kDCLFMaterialRowHeader + 16 + 4 * slot, 4);
}
uint DCLFMaterialSamplerIndex(uint a_register)
{
	return vk::RawBufferLoad<uint>(DCLFMaterialRowAddress + kDCLFMaterialRowHeader + 80 + 4 * a_register, 4);
}
// PipelineRowHeader: six addresses, the shadow mask's texture and sampler, the vertex layout.
uint DCLFPipelineTextureIndex() { return vk::RawBufferLoad<uint>(DCLFPipelineRowAddress + kDCLFPipelineRowHeader + 48, 4); }
uint DCLFPipelineSamplerIndex() { return vk::RawBufferLoad<uint>(DCLFPipelineRowAddress + kDCLFPipelineRowHeader + 52, 4); }
uint2 DCLFPipelineVertexLayout() { return vk::RawBufferLoad<uint2>(DCLFPipelineRowAddress + kDCLFPipelineRowHeader + 56, 8); }
float4 DCLFRowFloat4(uint64_t a_row, uint a_offset) { return asfloat(vk::RawBufferLoad<uint4>(a_row + a_offset, 4)); }
float3 DCLFRowFloat3(uint64_t a_row, uint a_offset) { return asfloat(vk::RawBufferLoad<uint3>(a_row + a_offset, 4)); }
float2 DCLFRowFloat2(uint64_t a_row, uint a_offset) { return asfloat(vk::RawBufferLoad<uint2>(a_row + a_offset, 4)); }
float DCLFRowFloat(uint64_t a_row, uint a_offset) { return asfloat(vk::RawBufferLoad<uint>(a_row + a_offset, 4)); }
uint DCLFRowUint(uint64_t a_row, uint a_offset) { return vk::RawBufferLoad<uint>(a_row + a_offset, 4); }
// A row_major matrix: a register per row.
float3x4 DCLFRowFloat3x4(uint64_t a_row, uint a_offset)
{
	return float3x4(DCLFRowFloat4(a_row, a_offset), DCLFRowFloat4(a_row, a_offset + 16), DCLFRowFloat4(a_row, a_offset + 32));
}
float4x4 DCLFRowFloat4x4(uint64_t a_row, uint a_offset)
{
	return float4x4(DCLFRowFloat4(a_row, a_offset), DCLFRowFloat4(a_row, a_offset + 16), DCLFRowFloat4(a_row, a_offset + 32), DCLFRowFloat4(a_row, a_offset + 48));
}
float3x3 DCLFRowFloat3x3(uint64_t a_row, uint a_offset)
{
	return float3x3(DCLFRowFloat3(a_row, a_offset), DCLFRowFloat3(a_row, a_offset + 16), DCLFRowFloat3(a_row, a_offset + 32));
}
#	endif  // DCLF_PULLED
static const uint kDCLFNodelessTree = 0xFFFFFFFEu;

bool DCLFTreeWindEntry(uint a_object, out uint a_entry)
{
	const float4 joined = DCLFObjects[a_object].DCLFWindTimers;
	const uint slot = asuint(joined.z);
	a_entry = slot == kDCLFNodelessTree ? 0u : slot + 1u;
	return asuint(joined.w) != 0u && asuint(DCLFTreeWind[a_entry * 3u + 2u].x) == asuint(joined.w);
}

float4 DCLFTreeParamsOf(uint a_object)
{
	uint entry;
	return DCLFTreeWindEntry(a_object, entry) ? DCLFTreeWind[entry * 3u] : DCLFObjects[a_object].DCLFTreeParams;
}

float2 DCLFWindTimersOf(uint a_object)
{
	uint entry;
	return DCLFTreeWindEntry(a_object, entry) ? DCLFTreeWind[entry * 3u + 1u].xy : DCLFObjects[a_object].DCLFWindTimers.xy;
}

#endif  // DCLF_BINDLESS
#endif  // __DCLF_OBJECTS_DEPENDENCY_HLSL__
