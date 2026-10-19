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

// The object's structure (LightingConstants.h, BindlessObject); its placement and its shading are rows of their own
// (DCLFPlacement, DCLFShadingRow, below).
struct DCLFObjectRecord
{
	// The values that used to reach the shader as constant buffers of their own: Light Limit Fix's room
	// index and shadow bit mask (b3) and the alpha test reference (b11). Only read when DCLF_BINDLESS_DRAW is also defined;
	// the record carries them either way so that the two builds share one layout.
	int RoomIndex;
	uint DCLFRecordFlags;  // bit 0: Subsurface Scattering's IsBeastRace, per actor; bit 1: alpha blended (LightingConstants.h, kRecord*)
	float AlphaTestRef;
	// The specular and envmap LOD fades the pass applies (bit 4 specular, 5 envmap, 6 SSRParams.w with specular), made by the
	// draw from the frame's camera (Lighting.hlsl, DCLFFrameLighting c6-c12) while the placement's fade node has them apply
	// (DCLFLodFadeFlagsOf).
	uint DCLFLodFades;
	// Tree animation (technique 12). Per object: under DCLF_BINDLESS the PerGeometry buffer is one
	// block for the whole pipeline, and a tree's wind amplitude and clock are its own.
	float4 DCLFTreeParams;
	float4 DCLFWindTimers;  // xy used; zw the tree slot and its listing's generation (DCLFTreeWind)
	// Skinning: this object's bone palette rows in DCLFPalettes (three float4 rows a bone, current palette
	// at DCLFBoneOffset, the previous frame's at DCLFPreviousBoneOffset), absolute like World.
	uint DCLFBoneOffset;
	uint DCLFPreviousBoneOffset;
	uint DCLFBoneRows;
	// The object's rows of further per-object PerGeometry values in DCLFExtras: LandBlendParams, the three rows of
	// TextureProj, then ProjectedUVParams, 2 and 3.
	uint DCLFExtraOffset;
};

// The object's shading (LightingConstants.h, BindlessShading), by the same index: the frame's (FrameValues, sampled at its start).
struct DCLFShadingRow
{
	float4 MaterialData;
	float4 EmitColor;  // emissive in xyz, the per-object w of SSRParams in w
	// Advanced Skin's SkinPerGeometry (b7): the owning actor's wetness, zero for everything else. Only read when
	// DCLF_BINDLESS_DRAW is also defined (Skin.hlsli).
	float4 SkinPerGeometry;
	// Linear Lighting's emissive multiplier (b8), the sample EmitColor folds in (Color.hlsli).
	float EmissiveMult;
	uint DCLFShadingWritten;  // the CPU's (which components the pass writes)
	uint2 DCLFShadingPadding;
	// T6b1a: TreeParams and WindTimers (xy) for a TreeAnim property, from its tree node as the frame has it: what a member draws with
	// until TreeWindCS's entry of its tree's listing (DCLFTreeParamsOf).
	float4 DCLFShadingTreeParams;
	float4 DCLFShadingWindTimers;
};

// The object's placement (LightingConstants.h, BindlessPlacement), by the same index: what a move changes.
struct DCLFPlacement
{
	float4 World[3];          // row_major float3x4, absolute: the vertex stage subtracts the drawing camera's eye
	float4 PreviousWorld[3];  // likewise, against the previous eye
	// The culling's (BuildDrawsCS): the world bound (centre, radius), and the sun entry's sphere.
	float4 Bound;
	float4 SunEntry;
	// The fade node's world bound centre, and in w its LOD type plus 16 (kLodFadeHeld) when its LOD fades do not apply; w < 0
	// without one.
	float4 LodFadeNode;
	// T7d's ordered-node interleave: the nearest BSOrderedNode's world bound centre over a blended leaf, and its ordinal under it (the
	// bits of a uint) in w; 0 under none. Read by the forward sort alone (BuildDrawsCS.hlsl).
	float4 ForwardSort;
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
// The indirect draw's push data (DrawPipelines.h, kDrawPushWords): its sequence's bytes 4-55 (Records.h, DrawSequence), one
// Constant argument. The first four words are its rows' addresses (the pipeline row's, then the material row's), which the
// pipeline layout consumes to resolve this draw's buffers and descriptors; then the object word and the geometry's two vertex
// buffer views (address, size, stride: D3D12's layout), which the vertex stage pulls its attributes through (DCLF_PULLED_VERTICES:
// no DCLF pipeline has input assembly). No vector straddles a register, so every member sits where the words are.
cbuffer DCLFPushData : register(b190)
{
	uint2 DCLFPipelineRowAddress : packoffset(c0.x);
	uint2 DCLFMaterialRowAddress : packoffset(c0.z);
	uint DCLFObjectWord : packoffset(c1.x);
	uint3 DCLFVertexBuffer : packoffset(c1.y);  // address (low, high), size (unread)
	uint DCLFVertexStride : packoffset(c2.x);
	uint3 DCLFStreamBuffer : packoffset(c2.y);  // slot 1: a face shape's positions, else slot 0 again
	uint DCLFStreamStride : packoffset(c3.x);
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
// The placement rows, by the same index (IndirectDraws: kPlacementBufferRegister).
StructuredBuffer<DCLFPlacement> DCLFPlacements : register(t123);
// The frame's bone palettes (IndirectDraws: kPaletteBufferRegister; FrameValues): a skinned object's block holds its
// current palette, then the previous frame's (DCLFBoneOffset, DCLFPreviousBoneOffset).
StructuredBuffer<float4> DCLFPalettes : register(t122);
// The per-object extras rows (IndirectDraws: kExtrasBufferRegister), at DCLFExtraOffset.
StructuredBuffer<float4> DCLFExtras : register(t126);
// The frame's shading rows (IndirectDraws: kShadingBufferRegister; FrameValues), by the object index: both stages (the vertex stage's
// for a tree's wind until its entry, T6b1a).
StructuredBuffer<DCLFShadingRow> DCLFShading : register(t121);
// The trees' wind (IndirectDraws: kTreeWindRegister; TreeWindCS.hlsl): three rows an entry, TreeParams, WindTimers and the
// listing's generation (x), the
// nodeless entry first and tree slot s at entry s + 1, as the compute queue made them the frame before (it writes this
// frame's into the other buffer). An entry is the object's only for the tree listing its record names (DCLFWindTimers.zw:
// the tree slot and that listing's generation, never 0); otherwise the record's own values, those it joined with, are drawn.
StructuredBuffer<float4> DCLFTreeWind : register(t124);

#	if defined(DCLF_PULLED) || defined(DCLF_PULLED_VERTICES)
// Every DCLF vertex stage pulls its attributes (no DCLF pipeline has input assembly): the pulled builds (DCLF_PULLED) through
// their sequence, the device-generated draws' (DCLF_PULLED_VERTICES) through their push data.
#		if defined(DCLF_PULLED)
// A pulled draw's own (its vertex stage reads them from its sequence, DCLF_PULLED above).
static const uint kDCLFSequenceStride = 92;  // BuildDrawsCS's DrawSequence
#		endif
// The draw's vertex layout, which DCLFAttribute decodes: set by the stage's main before any attribute is read, from the push
// data (Utility.hlsl: a shadow view's call is one pipeline's, so one layout's) or from the draw's pipeline row (Lighting.hlsl:
// a Z-prepass call draws every pipeline slot that shares its depth pipeline, so each draw has its slot's; Lighting.hlsl and
// Effect.hlsl under DCLF_PULLED_VERTICES: an execution set's draw switches pipelines, and so layouts, per draw).
static uint2 DCLFDrawVertexLayout;

uint64_t DCLFAddress(uint2 a_words) { return (uint64_t(a_words.y) << 32) | uint64_t(a_words.x); }

#		if defined(DCLF_PULLED_VERTICES)
// The draw's vertex layout, from its pipeline row (DrawPipelines.h, PipelineRowHeader::vertexLayout, at kPipelineRowHeader + 56;
// DCLFPipelineVertexLayout's read in a pulled build), whose address is the draw's push data's.
uint2 DCLFPushedVertexLayout() { return vk::RawBufferLoad<uint2>(DCLFAddress(DCLFPipelineRowAddress) + 1536 + 56, 8); }
#		endif

// A stream of the geometry: the vertex buffer (stream 0) or a dynamic shape's positions (stream 1).
struct DCLFStream
{
	uint64_t address;
	uint stride;
};

// Whether the draw's vertex layout has attribute a_attribute (BSGraphics::Vertex::Attribute) in either stream (bit 44 + a or
// 54 + a). The input assembler reads an attribute the layout lacks as (0, 0, 0, 1) (VertexInput.cpp, BuildVertexElements gives it no
// element), so the decoders below return that without a load: nothing is read through a stream the attribute is not in.
bool DCLFHasAttribute(uint a_attribute) { return ((DCLFDrawVertexLayout.y >> (12 + a_attribute)) & 0x401u) != 0; }

// The address of attribute a_attribute (BSGraphics::Vertex::Attribute) of vertex a_index: its stream as the layout flags it
// (bit 44 + a in stream 0, else bit 54 + a), and its offset the layout's nibble times four, except the position's (0). As the
// engine's input layout for the vertex layout has them (VertexInput.cpp, BuildVertexElements). The nibble is bits 4a + 4 of the 64-bit
// layout: from the land data's (7) on it is in the high word, and a 32-bit shift by 32 or more wraps (masked to five bits), which read
// the land blend weights at the vertex size's nibble, the next vertex's position.
uint64_t DCLFAttribute(uint a_attribute, uint a_index, DCLFStream a_first, DCLFStream a_second)
{
	const bool first = ((DCLFDrawVertexLayout.y >> (12 + a_attribute)) & 1) != 0;
	const uint shift = 4 * a_attribute + 4;
	const uint nibble = shift < 32 ? (DCLFDrawVertexLayout.x >> shift) : (DCLFDrawVertexLayout.y >> (shift - 32));
	const uint offset = a_attribute == 0 ? 0 : (nibble & 0xF) * 4;
	const uint64_t address = first ? a_first.address : a_second.address;
	const uint stride = first ? a_first.stride : a_second.stride;
	return address + uint64_t(a_index) * stride + offset;
}

// An 8-bit UNORM as the input assembler converts it (the engine's draws read the same attributes through one): c / 255 correctly
// rounded. A plain division is not (it compiles to a reciprocal and a multiply, half the values an ulp off), and a tree's vertex
// moves along its normal by its colour: an ulp of either moves DCLF's depth off the native draws' (and, when the colour pass still
// read its attributes through the input assembler, the Z-prepass's off the colour pass's, whose EQUAL test then failed in stripes
// across the leaves). The product by 1/255 corrected by its fused remainder is exact for all 256 values.
float4 DCLFUnorm4(uint a_word)
{
	precise float4 c = float4(a_word & 0xFF, (a_word >> 8) & 0xFF, (a_word >> 16) & 0xFF, a_word >> 24);
	precise float4 q = c * (1.0 / 255.0);
	precise float4 r = mad(-q, 255.0, c);
	precise float4 result = mad(r, 1.0 / 255.0, q);
	return result;
}
float2 DCLFHalf2(uint a_word) { return float2(f16tof32(a_word), f16tof32(a_word >> 16)); }

// Attribute a_attribute of vertex a_index in its input layout element's format (VertexInput.cpp, BuildVertexElements), a_offset
// bytes into the attribute for its second element (BLENDINDICES, TEXCOORD3), or the input assembler's (0, 0, 0, 1) where the layout
// lacks it (DCLFHasAttribute). An `if`, not `?:`: HLSL 2018's conditional evaluates both sides, and the load would run anyway.
float4 DCLFPullFloat4(uint a_attribute, uint a_index, DCLFStream a_first, DCLFStream a_second)  // R32G32B32A32_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return float4(0, 0, 0, 1);
	return asfloat(vk::RawBufferLoad<uint4>(DCLFAttribute(a_attribute, a_index, a_first, a_second)));
}
float DCLFPullFloat(uint a_attribute, uint a_index, DCLFStream a_first, DCLFStream a_second)  // R32_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return 0;
	return asfloat(vk::RawBufferLoad<uint>(DCLFAttribute(a_attribute, a_index, a_first, a_second)));
}
float2 DCLFPullHalf2(uint a_attribute, uint a_index, DCLFStream a_first, DCLFStream a_second)  // R16G16_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return float2(0, 0);
	return DCLFHalf2(vk::RawBufferLoad<uint>(DCLFAttribute(a_attribute, a_index, a_first, a_second)));
}
float4 DCLFPullHalf4(uint a_attribute, uint a_index, DCLFStream a_first, DCLFStream a_second)  // R16G16B16A16_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return float4(0, 0, 0, 1);
	const uint2 words = vk::RawBufferLoad<uint2>(DCLFAttribute(a_attribute, a_index, a_first, a_second));
	return float4(DCLFHalf2(words.x), DCLFHalf2(words.y));
}
float4 DCLFPullUnorm4(uint a_attribute, uint a_offset, uint a_index, DCLFStream a_first, DCLFStream a_second)  // R8G8B8A8_UNORM
{
	if (!DCLFHasAttribute(a_attribute))
		return float4(0, 0, 0, 1);
	return DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(a_attribute, a_index, a_first, a_second) + a_offset));
}
#	endif  // DCLF_PULLED || DCLF_PULLED_VERTICES

#	if defined(DCLF_PULLED)
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
	// MaterialRowHeader: two addresses, textures[16], samplers[16], features (t71, t74, then TruePBR's t80-t91: kFeatureMaterialRegisters).
	const uint slot = a_register == 71 ? 32 : a_register == 74 ? 33 : a_register >= 80 ? 34 + (a_register - 80) : a_register;
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

// The draw's LOD fade word (LightingDescriptors.h): its fade node's LOD type (bits 0-3) and the fades its pass applies, while
// the node has them apply (LodFadesApply); 0 when nothing fades and MaterialData's fades are the property's.
uint DCLFLodFadeFlagsOf(uint a_object)
{
	const uint fades = DCLFObjects[a_object].DCLFLodFades;
	const float node = DCLFPlacements[a_object].LodFadeNode.w;
	return fades != 0 && node >= 0.0f && node < 16.0f ? fades | (uint(node) & 0xFu) : 0u;
}

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
	return DCLFTreeWindEntry(a_object, entry) ? DCLFTreeWind[entry * 3u] : DCLFShading[a_object].DCLFShadingTreeParams;
}

float2 DCLFWindTimersOf(uint a_object)
{
	uint entry;
	return DCLFTreeWindEntry(a_object, entry) ? DCLFTreeWind[entry * 3u + 1u].xy : DCLFShading[a_object].DCLFShadingWindTimers.xy;
}

// CS_DCLF_BILLBOARDS (Billboards.h): a billboard's effect record (LightingConstants.h, kRecordEffectBillboard: bit 4, its node's face mode
// in bits 5-7). Its placement row holds what no cull writes: World the leaf's unturned world (the node's base times the leaf's transform
// relative to the node), PreviousWorld's rotation part the base rotation R0 (the node's parent's world rotation times its local),
// PreviousWorld[0].w the leaf's world bound radius, Bound.xyz the node's translation (the pivot).
static const uint kDCLFRecordEffectBillboard = 1u << 4;

// A decal's draw (LightingConstants.h, kRecordDecalNudge: bit 8): its clip position moved toward the eye by kDCLFDecalDepthNudge of the
// depth range. The engine's decal depth bias is pipeline state, which a device-generated command's pipeline switch does not carry, and
// where the engine's bias is none (interiors' modes 7 and 11) the decal sits on its host by an exact depth tie, which DCLF's two vertex
// modules (the host's pulled Z-prepass, the decal's colour draw) do not keep to the bit. 2^-20: 16 steps of a D24 depth.
static const uint kDCLFRecordDecalNudge = 1u << 8;
static const float kDCLFDecalDepthNudge = 1.0 / 1048576.0;
float4 DCLFDecalNudged(float4 a_position, uint a_object)
{
	if (DCLFObjects[a_object].DCLFRecordFlags & kDCLFRecordDecalNudge)
		a_position.z -= kDCLFDecalDepthNudge * a_position.w;
	return a_position;
}

// The node's world rotation as NiBillboardNode::OnVisible makes it for a camera (0x140d3e650: R0 times the turn its face mode makes from
// the camera's axes, the base rotation dropped for BSROTATE_ABOUT_UP), a_forward, a_up and a_right the camera's world axes and a_eye its
// position. The constants are the engine's (Billboards.cpp, whose Orient is this function's CPU twin for the observer).
float3x3 DCLFBillboardRotation(float3x3 a_base, float3 a_pivot, uint a_mode, float3 a_forward, float3 a_up, float3 a_right, float3 a_eye)
{
	const float kShort = asfloat(0x358637bdu), kNear = asfloat(0x3a83126fu), kAligned = asfloat(0x3f7fffefu), kPi = asfloat(0x40490fdbu),
				kFlat = asfloat(0x2b8cbcccu);
	float3x3 m = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
	float3x3 base = a_base;
	float3 toward = -a_forward, up = a_up, right = a_right;
	bool turned = true;
	if (a_mode == 3 || a_mode == 4) {
		// FUN_140d3f240: the axes turned by the rotation taking -forward onto the direction from the pivot to the camera; none within
		// kNear (squared) of the pivot.
		float3 d = a_eye - a_pivot;
		const float lengthSq = d.y * d.y + d.x * d.x + d.z * d.z;
		if (lengthSq < kNear) {
			turned = false;
		} else {
			const float length = sqrt(lengthSq);
			d = length <= kShort ? float3(0, 0, 0) : d * (1.0 / length);
			const float cosine = dot(toward, d);
			if (cosine < kAligned) {
				const float angle = -1.0 < cosine ? (1.0 <= cosine ? 0.0 : acos(cosine)) : kPi;
				float3 n = float3(toward.z * d.y - toward.y * d.z, toward.x * d.z - toward.z * d.x, toward.y * d.x - toward.x * d.y);
				const float nLength = sqrt(n.y * n.y + n.x * n.x + n.z * n.z);
				n = nLength <= kShort ? float3(0, 0, 0) : n * (1.0 / nLength);
				float s, c;
				sincos(angle, s, c);
				const float k = 1.0 - c;
				const float xy = n.y * n.x * k, xz = n.z * n.x * k, yz = n.z * n.y * k;
				const float3x3 q = float3x3(n.x * n.x * k + c, n.z * s + xy, xz - n.y * s,
					xy - n.z * s, n.y * n.y * k + c, n.x * s + yz,
					n.y * s + xz, yz - n.x * s, n.z * n.z * k + c);
				toward = d;
				up = mul(q, up);
				right = mul(q, right);
			}
		}
	}
	// The axes in the base frame (R0's transpose times them).
	const float3 a = mul(toward, base), u = mul(up, base), r = mul(right, base);
	if (!turned) {
	} else if (a_mode == 0 || a_mode == 3) {
		// ALWAYS_FACE_CAMERA, ALWAYS_FACE_CENTER: about the toward axis until the right axis has no base-frame y.
		const float length = sqrt(r.y * r.y + u.y * u.y);
		float3 x, y;
		if (length <= kShort) {
			x = -r;
			y = -u;
		} else {
			const float inverse = 1.0 / length;
			const float c = u.y * inverse, s = -(r.y * inverse);
			x = r * c + u * s;
			y = u * c - r * s;
		}
		m = float3x3(x.x, y.x, a.x, x.y, y.y, a.y, x.z, y.z, a.z);
	} else if (a_mode == 2 || a_mode == 4) {
		// RIGID_FACE_CAMERA, RIGID_FACE_CENTER: the camera's axes exactly.
		m = float3x3(r.x, u.x, a.x, r.y, u.y, a.y, r.z, u.z, a.z);
	} else if (a_mode == 1) {
		// ROTATE_ABOUT_UP: about the base's y toward the camera.
		const float3 d = mul(a_eye - a_pivot, base);
		const float length = sqrt(d.z * d.z + d.x * d.x);
		if (kFlat <= length) {
			const float inverse = 1.0 / length;
			m = float3x3(d.z * inverse, 0, d.x * inverse, 0, 1, 0, -(d.x * inverse), 0, d.z * inverse);
		}
	} else if (a_mode == 5) {
		// BSROTATE_ABOUT_UP: the base rotation dropped, about the world's z toward the camera.
		base = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
		const float2 d = a_eye.xy - a_pivot.xy;
		const float length = sqrt(d.y * d.y + d.x * d.x);
		if (kFlat <= length) {
			const float inverse = 1.0 / length;
			m = float3x3(-(d.y * inverse), d.x * inverse, 0, d.x * inverse, d.y * inverse, 0, 0, 0, 1);
		}
	}
	return mul(base, m);
}

// A billboard's world for a camera: its unturned world (World: the node's base times the leaf's transform relative to it) turned about the
// pivot by G = the node's turned rotation (DCLFBillboardRotation) times R0's transpose (the base rotation undone); absolute.
float3x4 DCLFBillboardWorld(uint a_object, float3 a_forward, float3 a_up, float3 a_right, float3 a_eye)
{
	const DCLFPlacement placement = DCLFPlacements[a_object];
	const float3x3 base = float3x3(placement.PreviousWorld[0].xyz, placement.PreviousWorld[1].xyz, placement.PreviousWorld[2].xyz);
	const float3 pivot = placement.Bound.xyz;
	const uint mode = (DCLFObjects[a_object].DCLFRecordFlags >> 5) & 7u;
	const float3x3 g = mul(DCLFBillboardRotation(base, pivot, mode, a_forward, a_up, a_right, a_eye), transpose(base));
	const float3x4 unturned = float3x4(placement.World[0], placement.World[1], placement.World[2]);
	const float3x3 rotation = mul(g, (float3x3)unturned);
	const float3 translation = mul(g, float3(unturned[0].w, unturned[1].w, unturned[2].w) - pivot) + pivot;
	return float3x4(float4(rotation[0], translation.x), float4(rotation[1], translation.y), float4(rotation[2], translation.z));
}

// The world bound radius of an object (Community Shaders' EffectRadius: the geometry's world bound radius): a billboard's Bound covers it
// in every orientation, so its own radius is PreviousWorld[0].w.
float DCLFWorldBoundRadius(uint a_object)
{
	const DCLFPlacement placement = DCLFPlacements[a_object];
	return (DCLFObjects[a_object].DCLFRecordFlags & kDCLFRecordEffectBillboard) ? placement.PreviousWorld[0].w : placement.Bound.w;
}

#	if defined(PSHADER)
// The pixel stages' (Lighting.hlsl's MaterialData.z, Effect.hlsl's PropertyColor.w): the vertex stages bind no t119 or t120.
// FadeStateCS's rows (Records.h, FadeNodeState and FadeRootStatic), the frame record's t120 and t119 (DrawPipelines.h,
// kFadeStatesRegister, kFadeRootsRegister): the states the frame's builds read, and the roots' static rows.
struct DCLFFadeNodeState
{
	uint Flags;
	float CurrentFade;
	float SnapRadius;
	int LastVisible;
	float AmountFade;
	float Metric;
	float PreviousMetric;
	float Blend;
	uint Levels;
	uint Generation;
	uint Verdict;
	uint Frame;
};
struct DCLFFadeRootStatic
{
	DCLFFadeNodeState Initial;
	float Radius;
	float FadeAmount;
	float NearDistance;
	float FarDistance;
	uint Object;
	uint Bits;
	float LodScale;
	uint Generation;
};
StructuredBuffer<DCLFFadeNodeState> DCLFFadeStates : register(t120);
StructuredBuffer<DCLFFadeRootStatic> DCLFFadeRoots : register(t119);
// What GetRenderPasses and SetupGeometry put on a pass's alpha (MaterialData.z): the property's alpha times its fade node's
// currentFade, and for a LOD cross-fade's copy (the draw's object word bit 30: BuildDrawsCS, kObjectCrossfadeCopy) the
// cross-fade factor (+0x14C) instead. T1c: from the object's fade root (DCLFRecordFlags bits 9-31, its slot + 1) in
// FadeStateCS's state; 1 without a root, or with a row the frame does not hold (generation 0: none bound, or freed).
float DCLFAlphaFade(uint a_object, bool a_copy)
{
	const uint root = DCLFObjects[a_object].DCLFRecordFlags >> 9;
	if (root == 0)
		return 1;
	const DCLFFadeRootStatic row = DCLFFadeRoots[root - 1];
	if (row.Generation == 0)
		return 1;
	DCLFFadeNodeState state = DCLFFadeStates[root - 1];
	if (state.Generation != row.Generation)
		state = row.Initial;
	return a_copy ? state.Blend : state.CurrentFade;
}
#	endif  // PSHADER

#endif  // DCLF_BINDLESS
#endif  // __DCLF_OBJECTS_DEPENDENCY_HLSL__
