// Drawcall Limit Fix: the frame's texture transforms into the main material rows (TextureTransforms.h, FrameValues).
//
// A material's TexcoordOffset (offset.xy, scale.xy) is what BSShaderMaterial holds in the texture-transform buffer the frame reads
// (Main::Update flips it; the controllers write the other). The material rows hold their records' captured value; the frame values
// sample every row's material and this pass stores the frame's into the rows of the installed payload, first in the depth epoch,
// before any of the frame's draws reads them (through the rows' addresses: Lighting.hlsl and Effect.hlsl's PerMaterial blocks).
// Every row that carries one is written every frame, so a pooled backing holds the frame's value whatever its history.

// Must match TextureTransformsConstants (Draws/IndirectDraws/Core/GpuLayouts.h).
cbuffer TextureTransformsConstants : register(b0)
{
	uint LatchIndex;   // ByteAddressBuffer: the depth segment's BuildDrawsLatch block
	uint LatchOffset;  // its latch in this frame slot
	uint2 Padding;
}

// TextureTransformRow (Draws/Gpu/TextureTransforms.h): row 0 the header (RowOffset the entry count), entry i at row i + 1.
struct TextureTransformRow
{
	uint RowOffset;  // bytes past the material rows' address
	uint Mask;       // the components to store (bit c)
	uint2 RowPadding;
	float4 Values;
};

// BuildDrawsLatch's words (GpuLayouts.h: materialRowsLo at 272, materialRowsHi at 276, textureTransformsIndex at 300).
static const uint kLatchMaterialRows = 272;
static const uint kLatchTextureTransforms = 300;
// Must match kTextureTransformsGroup * kTextureTransformsGroups (GpuLayouts.h): the threads stride over the entries.
static const uint kThreads = 64 * 32;

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint transformsIndex = latch.Load(LatchOffset + kLatchTextureTransforms);
	if (transformsIndex == 0)
		return;  // no producer this frame
	const uint2 rows = latch.Load2(LatchOffset + kLatchMaterialRows);
	const uint64_t base = (uint64_t(rows.y) << 32) | uint64_t(rows.x);
	if (base == 0)
		return;
	StructuredBuffer<TextureTransformRow> transforms = ResourceDescriptorHeap[transformsIndex];
	const uint count = transforms[0].RowOffset;
	[loop] for (uint i = id.x; i < count; i += kThreads) {
		const TextureTransformRow row = transforms[i + 1];
		const uint64_t at = base + row.RowOffset;
		[unroll] for (uint c = 0; c < 4; ++c)
			if ((row.Mask >> c) & 1)
				vk::RawBufferStore<uint>(at + c * 4, asuint(row.Values[c]), 4);
	}
}
