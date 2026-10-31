// Drawcall Limit Fix: the frame's texture transforms into the frame's copy of the animated material rows
// (src/Features/DrawcallLimitFix/Features/TextureTransforms).
//
// A material's TexcoordOffset (offset.xy, scale.xy) is what BSShaderMaterial holds in the texture-transform buffer the frame reads
// (Main::Update flips it; the controllers write the other). The installed payload's material rows hold their records' captured value
// and are never written here (immutable versions the frames in flight share). Every row a frame patch may write is an animated row
// (MaterialRowHeader::animatedRow, word 0 of its header): phase 0 copies each one whole into its row of the frame's copy (the latch's
// animatedRows, a pooled backing of the frame's), phase 1 stores the frame's values into the copies, first in the depth epoch, before
// any of the frame's draws reads them (BuildDraws points an animated row's draws at its copy; Lighting.hlsl and Effect.hlsl's PerMaterial
// blocks read it by address). Every animated row is copied and every entry stored every frame, so the copy is the frame's whatever
// backing the pool handed it.

// Must match TextureTransformsPass.cpp's Constants.
cbuffer TextureTransformsConstants : register(b0)
{
	uint LatchIndex;   // ByteAddressBuffer: the depth segment's BuildDrawsLatch block
	uint LatchOffset;  // its latch in this frame slot
	uint Phase;        // 0: the animated rows copied; 1: the entries stored into the copies
	uint Padding;
}

// TextureTransformRow (TextureTransforms.h): row 0 the header (RowOffset the entry count E, Mask the animated rows' count A), entry i at
// row i + 1, animated row k at row E + 1 + k (RowOffset its installed row's bytes past the material rows, ~0 a free row).
struct TextureTransformRow
{
	uint RowOffset;  // bytes past the material rows' address
	uint Mask;       // the components to store (bit c)
	uint2 RowPadding;
	float4 Values;
};

// BuildDrawsLatch's words (GpuLayouts.h: materialRowsLo at 272, materialRowsHi at 276, stageWord at 300: the pool's rows, animatedRowsLo
// at 348, animatedRowsHi at 352: the frame's copy).
static const uint kLatchMaterialRows = 272;
static const uint kLatchStageWord = 300;
static const uint kLatchAnimatedRows = 348;
// DrawPipelines.h: kMaterialRowBytes, kMaterialRowHeader (MaterialRowHeader::animatedRow its first word).
static const uint kMaterialRowBytes = 1024;
static const uint kMaterialRowHeader = 768;
static const uint kNoAnimatedRow = 0xFFFFFFFFu;
// Must match numthreads times TextureTransformsPass.cpp's kGroups: the threads stride over the rows' 16-byte words and the entries.
static const uint kThreads = 64 * 32;

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint transformsIndex = latch.Load(LatchOffset + kLatchStageWord);
	if (transformsIndex == 0)
		return;  // no producer this frame (and no copy: the draws read the installed rows)
	const uint2 rows = latch.Load2(LatchOffset + kLatchMaterialRows);
	const uint2 copyRows = latch.Load2(LatchOffset + kLatchAnimatedRows);
	const uint64_t base = (uint64_t(rows.y) << 32) | uint64_t(rows.x);
	const uint64_t copyBase = (uint64_t(copyRows.y) << 32) | uint64_t(copyRows.x);
	if (base == 0 || copyBase == 0)
		return;
	StructuredBuffer<TextureTransformRow> transforms = ResourceDescriptorHeap[transformsIndex];
	const uint count = transforms[0].RowOffset;
	const uint animated = transforms[0].Mask;
	if (Phase == 0) {
		// Each animated row whole, a 16-byte word a thread.
		const uint kWords = kMaterialRowBytes / 16;
		[loop] for (uint w = id.x; w < animated * kWords; w += kThreads) {
			const uint k = w / kWords;
			const uint installed = transforms[count + 1 + k].RowOffset;
			if (installed == kNoAnimatedRow)
				continue;  // a free row: no header names it
			const uint at = (w % kWords) * 16;
			vk::RawBufferStore<uint4>(copyBase + uint64_t(k) * kMaterialRowBytes + at, vk::RawBufferLoad<uint4>(base + installed + at, 16), 16);
		}
		return;
	}
	[loop] for (uint i = id.x; i < count; i += kThreads) {
		const TextureTransformRow row = transforms[i + 1];
		// The entry's row's copy (its installed header names it); a row with none is not listed (MainRows::AnimatedRows::Animated).
		const uint rowBase = row.RowOffset / kMaterialRowBytes * kMaterialRowBytes;
		const uint k = vk::RawBufferLoad<uint>(base + rowBase + kMaterialRowHeader, 4);
		// kNoAnimatedRow is past every count. One unsigned compare, no equality test: AMD's RDNA2 Windows compiler took
		// `k == kNoAnimatedRow || k >= animated` as k == kNoAnimatedRow on the path past it, and stored every entry ~4 TB past the copy.
		if (k >= animated)
			continue;
		const uint64_t at = copyBase + uint64_t(k) * kMaterialRowBytes + (row.RowOffset - rowBase);
		[unroll] for (uint c = 0; c < 4; ++c)
			if ((row.Mask >> c) & 1)
				vk::RawBufferStore<uint>(at + c * 4, asuint(row.Values[c]), 4);
	}
}
