// Drawcall Limit Fix: the shadow views' index pool (IndirectDraws: ShadowIndexPool). The shadow commit lists the index buffers
// it gave a range of the pool this frame; a group copies one, word by word, from the buffer's device address into its range.
// The shadow views' plain indexed draws then bind the pool alone (ShadowViewPass).

cbuffer IndexPoolConstants : register(b0)
{
	uint LatchIndex;   // ByteAddressBuffer: the shadow latch block
	uint LatchOffset;  // the copies' dispatch in it (ShadowLatchLayout::PoolOffset): groups x, y, z, then the copy count
	uint CopiesIndex;  // StructuredBuffer<uint4>: per copy, the source's address (low, high), the range's first index, its words
	uint PoolIndex;    // RWByteAddressBuffer: the pool's 16-bit indices
}

static const uint kGroupsX = 65535;  // the dispatch's x limit: a copy past it is in the next row (y)

[numthreads(64, 1, 1)] void main(uint3 group : SV_GroupID, uint thread : SV_GroupIndex)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint copy = group.y * kGroupsX + group.x;
	if (copy >= latch.Load(LatchOffset + 12))
		return;
	StructuredBuffer<uint4> copies = ResourceDescriptorHeap[CopiesIndex];
	RWByteAddressBuffer pool = ResourceDescriptorHeap[PoolIndex];
	const uint4 job = copies[copy];
	const uint64_t source = (uint64_t(job.y) << 32) | uint64_t(job.x);
	// A range starts at an even index (4-byte aligned); the source's last word may hold two bytes past its indices, which
	// land in the range's own padding.
	[loop] for (uint word = thread; word < job.w; word += 64)
		pool.Store(job.z * 2 + word * 4, vk::RawBufferLoad<uint>(source + uint64_t(word) * 4));
}
