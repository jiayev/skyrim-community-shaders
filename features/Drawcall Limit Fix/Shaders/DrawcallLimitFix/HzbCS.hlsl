// Drawcall Limit Fix: builds the hierarchical depth buffer the occlusion culling tests against. Compiled
// to SPIR-V for the render graph (BasicRHI's descriptor-heap ABI); textures come from the descriptor heap.
//
// The HZB holds raw NDC depth, not linear view depth. Skyrim's projection is z_ndc = A - B/w, which is
// monotonic in distance, and the test only ever compares two depths for order, so linearising would add a
// step that can be got wrong without making any comparison more correct. Every level is the MAXIMUM of the
// level below, so a texel holds the FARTHEST surface anywhere under it. That is the direction that keeps
// the test conservative: a footprint that is partly empty sky keeps a large value and nothing under it is
// ever culled.
//
// Mip 0 is half the padded power-of-two size, so that the chain is a clean sequence of halvings, and it is
// itself a max over the depth texels it covers rather than a point sample. Texels outside the real depth
// buffer (the padding, and the area past the edge at odd sizes) read 1.0, the far plane: that is the value
// that suppresses culling rather than causing it.

cbuffer HzbConstants : register(b0)
{
	uint SourceIndex;  // mip 0: Texture2D<float> of the scene depth. Other mips: RWTexture2D<float> mip-1.
	uint TargetIndex;  // RWTexture2D<float> of the mip being written
	uint2 TargetSize;  // dimensions of the mip being written
	uint2 SourceSize;  // dimensions of what is being read
	uint FromDepth;    // 1 for mip 0 (read the depth SRV), 0 for the rest (read the mip above)
	uint Padding;
}

static const float kFarPlane = 1.0;

float SourceTexel(int2 position)
{
	// Past the edge of the real data: the far plane, which never causes a rejection.
	if (any(position < 0) || any(position >= int2(SourceSize)))
		return kFarPlane;
	if (FromDepth != 0) {
		Texture2D<float> depth = ResourceDescriptorHeap[SourceIndex];
		return depth.Load(int3(position, 0));
	}
	RWTexture2D<float> source = ResourceDescriptorHeap[SourceIndex];
	return source[position];
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const int2 target = int2(dispatchID.xy);
	if (any(target >= int2(TargetSize)))
		return;

	// Each target texel covers a 2x2 block of the source. Where the source is odd-sized the block reaches
	// past the edge, and SourceTexel answers the far plane there, so the reduction stays conservative.
	const int2 source = target * 2;
	const float depth = max(
		max(SourceTexel(source), SourceTexel(source + int2(1, 0))),
		max(SourceTexel(source + int2(0, 1)), SourceTexel(source + int2(1, 1))));

	RWTexture2D<float> destination = ResourceDescriptorHeap[TargetIndex];
	destination[target] = depth;
}
