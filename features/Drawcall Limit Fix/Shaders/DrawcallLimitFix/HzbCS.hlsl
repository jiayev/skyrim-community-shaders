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
// The chain is built by AMD's single-pass downsampler (FidelityFX/ffx_spd.h): one dispatch writes every
// mip, each group reducing a 64x64 tile of the source through mip 5 and the last group to finish carrying
// on from mip 5 to the end. That needs a source whose every level halves evenly, which the depth is not.
// The HZB is defined over a power-of-two domain instead (Resources.cpp: mip 0 is half the next power of two
// of the depth), and the loads answer for the texels the depth does not have:
//
//   - inside the domain but past the rendered area (the padding): the far plane, 1.0, which suppresses
//     culling rather than causing it;
//   - outside the domain, or outside a level that is not square once its short side has reached one
//     texel: 0.0, the reduction's identity. Such a texel covers no area at all, so it must not count.
//
// Stores outside a level are dropped (a non-square chain's levels are narrower than SPD's square tiles).
//
// A domain of more than 4096 texels a side (a render target wider than 4096) is more than one dispatch can
// take: its mip 5 is wider than the last group's 64x64. HzbPass then splits the chain, each dispatch reading
// the previous one's last level (FromDepth 0).

cbuffer HzbConstants : register(b0)
{
	uint SourceIndex;   // FromDepth: Texture2D<float> of the scene depth. Otherwise RWTexture2D<float> of an HZB level.
	uint FromDepth;
	uint2 ValidSize;    // the source texels holding depth (the rendered area, or the whole level)
	uint2 DomainSize;   // the source's extent, of which [ValidSize, DomainSize) is padding
	uint2 TargetSize;   // the first written level's size; level i is max(1, TargetSize >> i)
	uint Mips;          // the levels this dispatch writes
	uint WorkGroups;    // the dispatch's group count, for the last-group test
	uint CounterIndex;  // RWStructuredBuffer<uint>: the groups that have finished; the last one zeroes it again
	uint Padding;
	uint4 TargetIndices[3];  // RWTexture2D<float> per written level, 12 at most
}

static const float kFarPlane = 1.0;
static const float kNoArea = 0.0;

#define A_GPU
#define A_HLSL
#include "DrawcallLimitFix/FidelityFX/ffx_a.h"

groupshared AU1 spdCounter;
groupshared AF1 spdIntermediate[16][16];

uint TargetIndex(uint a_level)
{
	return TargetIndices[a_level >> 2][a_level & 3];
}

bool InsideLevel(int2 a_position, uint a_level)
{
	const int2 size = int2(max(TargetSize >> a_level, uint2(1, 1)));
	return all(a_position >= 0) && all(a_position < size);
}

AF4 SpdLoadSourceImage(ASU2 p, AU1 slice)
{
	if (any(p < 0) || any(p >= int2(DomainSize)))
		return kNoArea.xxxx;
	if (any(p >= int2(ValidSize)))
		return kFarPlane.xxxx;
	if (FromDepth != 0) {
		Texture2D<float> depth = ResourceDescriptorHeap[SourceIndex];
		return depth.Load(int3(p, 0)).xxxx;
	}
	RWTexture2D<float> source = ResourceDescriptorHeap[SourceIndex];
	return source[p].xxxx;
}

// The last group's reads of level 5, written by every other group.
AF4 SpdLoad(ASU2 p, AU1 slice)
{
	if (!InsideLevel(p, 5))
		return kNoArea.xxxx;
	globallycoherent RWTexture2D<float> level = ResourceDescriptorHeap[TargetIndex(5)];
	return level[p].xxxx;
}

void SpdStore(ASU2 p, AF4 value, AU1 mip, AU1 slice)
{
	if (!InsideLevel(p, mip))
		return;
	if (mip == 5) {
		globallycoherent RWTexture2D<float> level = ResourceDescriptorHeap[TargetIndex(5)];
		level[p] = value.x;
		return;
	}
	RWTexture2D<float> level = ResourceDescriptorHeap[NonUniformResourceIndex(TargetIndex(mip))];
	level[p] = value.x;
}

AF4 SpdReduce4(AF4 v0, AF4 v1, AF4 v2, AF4 v3)
{
	return max(max(v0.x, v1.x), max(v2.x, v3.x)).xxxx;
}

void SpdIncreaseAtomicCounter(AU1 slice)
{
	globallycoherent RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CounterIndex];
	// This group's level 5 texel (stored by this same thread) before its arrival; the arrival before the
	// last group's reads of everyone's.
	DeviceMemoryBarrier();
	InterlockedAdd(counter[0], 1, spdCounter);
	DeviceMemoryBarrier();
}

AU1 SpdGetAtomicCounter()
{
	return spdCounter;
}

void SpdResetAtomicCounter(AU1 slice)
{
	globallycoherent RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[CounterIndex];
	counter[0] = 0;
}

AF4 SpdLoadIntermediate(AU1 x, AU1 y)
{
	return spdIntermediate[x][y].xxxx;
}

void SpdStoreIntermediate(AU1 x, AU1 y, AF4 value)
{
	spdIntermediate[x][y] = value.x;
}

#include "DrawcallLimitFix/FidelityFX/ffx_spd.h"

[numthreads(256, 1, 1)] void main(uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	SpdDownsample(AU2(groupID.xy), AU1(localIndex), AU1(Mips), AU1(WorkGroups), AU1(0));
}
