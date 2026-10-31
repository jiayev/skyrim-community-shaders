#ifndef __DCLF_VERTEX_PULL_DEPENDENCY_HLSL__
#define __DCLF_VERTEX_PULL_DEPENDENCY_HLSL__

// Drawcall Limit Fix's vertex pulling: no DCLF pipeline has input assembly, so every DCLF vertex stage loads its vertex's attributes
// itself, decoded as the engine's input layout for the vertex layout has them (VertexInput.cpp, BuildVertexElements). Its own header,
// apart from the object rows (DCLFObjects.hlsli, which includes it), so that a draw family's stage that binds none of those rows and
// declares its own push data (RunGrass.hlsl) can pull its vertices too.
#if defined(DCLF_PULLED) || defined(DCLF_PULLED_VERTICES)
// The draw's vertex layout, which DCLFAttribute decodes: set by the stage's main before any attribute is read, from the push
// data (Utility.hlsl: a shadow view's call is one pipeline's, so one layout's) or from the draw's pipeline row (Lighting.hlsl:
// a Z-prepass call draws every pipeline slot that shares its depth pipeline, so each draw has its slot's; Lighting.hlsl and
// Effect.hlsl under DCLF_PULLED_VERTICES: an execution set's draw switches pipelines, and so layouts, per draw) or from the
// instance's mesh row (RunGrass.hlsl).
static uint2 DCLFDrawVertexLayout;

uint64_t DCLFAddress(uint2 a_words) { return (uint64_t(a_words.y) << 32) | uint64_t(a_words.x); }

// A stream of the geometry: the vertex buffer (stream 0) or a dynamic shape's positions (stream 1).
struct DCLFStream
{
	uint64_t address;
	uint stride;
};

// The streams at vertex a_index (DCLFAttribute's): each stream's address moved to the vertex once, so that an attribute adds only its
// offset. The product is 32-bit: a geometry's buffer is under 4 GB (its sequence's and push data's size words are 32-bit).
void DCLFStreamsAt(uint a_index, inout DCLFStream a_first, inout DCLFStream a_second)
{
	a_first.address += uint64_t(a_index * a_first.stride);
	a_second.address += uint64_t(a_index * a_second.stride);
}

// Whether the draw's vertex layout has attribute a_attribute (BSGraphics::Vertex::Attribute) in either stream (bit 44 + a or
// 54 + a). The input assembler reads an attribute the layout lacks as (0, 0, 0, 1) (VertexInput.cpp, BuildVertexElements gives it no
// element), so the decoders below return that without a load: nothing is read through a stream the attribute is not in.
bool DCLFHasAttribute(uint a_attribute) { return ((DCLFDrawVertexLayout.y >> (12 + a_attribute)) & 0x401u) != 0; }

// The address of attribute a_attribute (BSGraphics::Vertex::Attribute) of the streams' vertex (DCLFStreamsAt): its stream as the
// layout flags it (bit 44 + a in stream 0, else bit 54 + a), and its offset the layout's nibble times four, except the position's (0).
// As the engine's input layout for the vertex layout has them (VertexInput.cpp, BuildVertexElements). The nibble is bits 4a + 4 of the
// 64-bit layout: from the land data's (7) on it is in the high word, and a 32-bit shift by 32 or more wraps (masked to five bits), which
// read the land blend weights at the vertex size's nibble, the next vertex's position.
uint64_t DCLFAttribute(uint a_attribute, DCLFStream a_first, DCLFStream a_second)
{
	const bool first = ((DCLFDrawVertexLayout.y >> (12 + a_attribute)) & 1) != 0;
	const uint shift = 4 * a_attribute + 4;
	const uint nibble = shift < 32 ? (DCLFDrawVertexLayout.x >> shift) : (DCLFDrawVertexLayout.y >> (shift - 32));
	const uint offset = a_attribute == 0 ? 0 : (nibble & 0xF) * 4;
	return (first ? a_first.address : a_second.address) + offset;
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

// Attribute a_attribute of the streams' vertex (DCLFStreamsAt) in its input layout element's format (VertexInput.cpp,
// BuildVertexElements), a_offset bytes into the attribute for its second element (BLENDINDICES, TEXCOORD3), or the input assembler's
// (0, 0, 0, 1) where the layout lacks it (DCLFHasAttribute). An `if`, not `?:`: HLSL 2018's conditional evaluates both sides, and the
// load would run anyway.
float4 DCLFPullFloat4(uint a_attribute, DCLFStream a_first, DCLFStream a_second)  // R32G32B32A32_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return float4(0, 0, 0, 1);
	return asfloat(vk::RawBufferLoad<uint4>(DCLFAttribute(a_attribute, a_first, a_second)));
}
float DCLFPullFloat(uint a_attribute, DCLFStream a_first, DCLFStream a_second)  // R32_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return 0;
	return asfloat(vk::RawBufferLoad<uint>(DCLFAttribute(a_attribute, a_first, a_second)));
}
float2 DCLFPullHalf2(uint a_attribute, DCLFStream a_first, DCLFStream a_second)  // R16G16_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return float2(0, 0);
	return DCLFHalf2(vk::RawBufferLoad<uint>(DCLFAttribute(a_attribute, a_first, a_second)));
}
float4 DCLFPullHalf4(uint a_attribute, DCLFStream a_first, DCLFStream a_second)  // R16G16B16A16_FLOAT
{
	if (!DCLFHasAttribute(a_attribute))
		return float4(0, 0, 0, 1);
	const uint2 words = vk::RawBufferLoad<uint2>(DCLFAttribute(a_attribute, a_first, a_second));
	return float4(DCLFHalf2(words.x), DCLFHalf2(words.y));
}
float4 DCLFPullUnorm4(uint a_attribute, uint a_offset, DCLFStream a_first, DCLFStream a_second)  // R8G8B8A8_UNORM
{
	if (!DCLFHasAttribute(a_attribute))
		return float4(0, 0, 0, 1);
	return DCLFUnorm4(vk::RawBufferLoad<uint>(DCLFAttribute(a_attribute, a_first, a_second) + a_offset));
}
#endif  // DCLF_PULLED || DCLF_PULLED_VERTICES

#endif  // __DCLF_VERTEX_PULL_DEPENDENCY_HLSL__
