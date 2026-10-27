// Drawcall Limit Fix: the frame's object rows (src/Features/DrawcallLimitFix/Features/Fades/FadePixelRows.cpp).
//
// Lighting.hlsl's pixel stages computed an object's MaterialData (its LOD fades, from the fade node's metric under the frame's camera, and
// its alpha fade, from its fade root's state), EmitColor and SSRParams.w per pixel, though they are the draw's. Once a frame, first in
// the depth segment (after the latched copies, which put the frame record and the frame constants in place), this writes them for every
// object slot into the frame's pool backing (Common/DCLFObjects.hlsli, DCLFPixelObjectRow), which the pixel stages read at t116
// (DCLFPixelObjects). It reads its inputs as the draws do: their descriptors from the frame record's texture registers, the frame's LOD
// fade inputs from the frame constants (DCLFFrameLighting c6-c13). The backing and its rows are the latch's (BuildDrawsLatch::frameRows),
// so a recording kept across frames writes each frame's.

#define DCLF_BINDLESS
#include "Common/DCLFObjects.hlsli"

// Must match FadePixelRows.cpp's Constants.
cbuffer PixelRowsConstants : register(b0)
{
	uint LatchIndex;       // ByteAddressBuffer: the segment's BuildDrawsLatch block
	uint LatchOffset;      // its latch in this frame slot
	uint2 RecordTextures;  // the frame record's texture registers (DrawBindings::textures): the draws' descriptors
	uint2 LodFadeFrame;    // the frame's LodFadeFrame (DCLFLodFadeFrameData) in the frame constants
	uint2 Padding;
}

// BuildDrawsLatch's frameRowsLo, frameRowsHi, frameRowsCount (GpuLayouts.h).
static const uint kLatchFrameRows = 356;
static const uint kPixelObjectRowBytes = 48;  // sizeof(DCLFPixelObjectRow)
// Must match numthreads times FadePixelRows.cpp's kGroups: the threads stride over the rows.
static const uint kThreads = 64 * 64;

uint64_t Address(uint2 a_words) { return (uint64_t(a_words.y) << 32) | uint64_t(a_words.x); }

// A texture register's descriptor in the frame record (DrawPipelines.h's registers).
uint RecordTexture(uint a_register) { return vk::RawBufferLoad<uint>(Address(RecordTextures) + 4u * a_register, 4); }

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint3 rows = latch.Load3(LatchOffset + kLatchFrameRows);
	const uint64_t base = Address(rows.xy);
	if (base == 0)
		return;
	StructuredBuffer<DCLFObjectRecord> objects = ResourceDescriptorHeap[RecordTexture(127)];
	StructuredBuffer<DCLFPlacement> placements = ResourceDescriptorHeap[RecordTexture(123)];
	StructuredBuffer<DCLFShadingRow> shading = ResourceDescriptorHeap[RecordTexture(121)];
	StructuredBuffer<DCLFFadeNodeState> states = ResourceDescriptorHeap[RecordTexture(120)];
	StructuredBuffer<DCLFFadeRootStatic> roots = ResourceDescriptorHeap[RecordTexture(119)];
	StructuredBuffer<DCLFFadeRootStatic> seeds = ResourceDescriptorHeap[RecordTexture(117)];
	uint objectCount, placementCount, stride;
	objects.GetDimensions(objectCount, stride);
	placements.GetDimensions(placementCount, stride);
	const uint known = min(objectCount, placementCount);
	const DCLFLodFadeFrameData frame = vk::RawBufferLoad<DCLFLodFadeFrameData>(Address(LodFadeFrame), 16);
	[loop] for (uint object = id.x; object < rows.z; object += kThreads) {
		// A slot past the payload's objects or the frame's placements is no draw's: its row is 0.
		DCLFPixelObjectRow row = (DCLFPixelObjectRow)0;
		if (object < known)
			row = DCLFPixelObjectRowOf(objects, shading, placements, roots, states, seeds, frame, object);
		// Three float4s, not the struct: AMD's legacy Windows driver (Polaris, 2.0.279) crashes compiling a struct store to an address.
		const uint64_t at = base + uint64_t(object) * kPixelObjectRowBytes;
		vk::RawBufferStore<float4>(at, row.MaterialData, 16);
		vk::RawBufferStore<float4>(at + 16, row.EmitColorSSR, 16);
		vk::RawBufferStore<float4>(at + 32, float4(row.CopyAlpha, row.AlphaTestRef, row.Padding), 16);
	}
}
