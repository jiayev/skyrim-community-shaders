// Exclusive prefix sum of a uint buffer, in two dispatches (BasicRenderer's materialPrefixSum.hlsl, which the material and
// raster-bucket binning use): BlockScan scans each block of kBlockSize elements in its own group and writes the block's sum,
// then BlockOffsets scans the block sums in one group and adds each block's prefix to its elements. The one group bounds a
// scan to kBlockSize * kBlockSize elements. Buffers are fetched from the descriptor heap by index (BasicRHI's ABI); the host
// side is src/RenderGraph/PrefixSum.cpp, whose PrefixSumConstants these must match.

cbuffer PrefixSumConstants : register(b0)
{
	uint CountsIndex;     // RWByteAddressBuffer (or ByteAddressBuffer without kClearCounts): uint[Elements]
	uint OffsetsIndex;    // RWByteAddressBuffer: uint[Elements], the exclusive scan
	uint BlockSumsIndex;  // RWByteAddressBuffer: uint[Blocks]
	uint TotalIndex;      // RWByteAddressBuffer: uint, the sum of every count; kNoBuffer for none
	uint Elements;
	uint Blocks;
	uint Flags;
	uint Padding;
}

static const uint kBlockSize = 1024;  // power of two
static const uint kThreads = 256;
static const uint kNoBuffer = 0xFFFFFFFFu;
// The counts are zeroed as the scan reads them, leaving them ready for the next accumulation without a clear of their own.
static const uint kClearCounts = 1;

groupshared uint s_data[kBlockSize];
groupshared uint s_scan[kBlockSize];
groupshared uint s_blockSum;

// Exclusive (Blelloch) scan of s_data[0, n) into s_scan[0, n), padded with zeros to the next power of two (at most kBlockSize).
void ExclusiveScanBlock(uint n, uint thread)
{
	if (n == 0)
		return;
	uint p = 1;
	while (p < n)
		p <<= 1;

	for (uint i = thread; i < n; i += kThreads)
		s_scan[i] = s_data[i];
	for (uint i = thread + n; i < p; i += kThreads)
		s_scan[i] = 0;
	GroupMemoryBarrierWithGroupSync();

	// Up-sweep: partial sums.
	for (uint stride = 1; stride < p; stride <<= 1) {
		const uint step = stride << 1;
		for (uint index = (thread + 1) * step - 1; index < p; index += kThreads * step)
			s_scan[index] += s_scan[index - stride];
		GroupMemoryBarrierWithGroupSync();
	}

	if (thread == 0)
		s_scan[p - 1] = 0;
	GroupMemoryBarrierWithGroupSync();

	// Down-sweep.
	for (uint stride = p >> 1; stride >= 1; stride >>= 1) {
		const uint step = stride << 1;
		for (uint index = (thread + 1) * step - 1; index < p; index += kThreads * step) {
			const uint left = s_scan[index - stride];
			s_scan[index - stride] = s_scan[index];
			s_scan[index] += left;
		}
		GroupMemoryBarrierWithGroupSync();
		if (stride == 1)
			break;
	}
}

// One group per block: the block's local offsets and its sum.
[numthreads(kThreads, 1, 1)] void BlockScan(uint3 groupThread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
	RWByteAddressBuffer offsets = ResourceDescriptorHeap[OffsetsIndex];
	RWByteAddressBuffer blockSums = ResourceDescriptorHeap[BlockSumsIndex];

	const uint thread = groupThread.x;
	const uint block = group.x;
	const uint base = block * kBlockSize;
	const uint n = Elements > base ? min(kBlockSize, Elements - base) : 0;

	if ((Flags & kClearCounts) != 0) {
		RWByteAddressBuffer counts = ResourceDescriptorHeap[CountsIndex];
		for (uint i = thread; i < kBlockSize; i += kThreads) {
			s_data[i] = i < n ? counts.Load((base + i) * 4) : 0;
			if (i < n)
				counts.Store((base + i) * 4, 0);
		}
	} else {
		ByteAddressBuffer counts = ResourceDescriptorHeap[CountsIndex];
		for (uint i = thread; i < kBlockSize; i += kThreads)
			s_data[i] = i < n ? counts.Load((base + i) * 4) : 0;
	}
	GroupMemoryBarrierWithGroupSync();

	ExclusiveScanBlock(n, thread);

	for (uint i = thread; i < n; i += kThreads)
		offsets.Store((base + i) * 4, s_scan[i]);

	uint sum = 0;
	for (uint i = thread; i < n; i += kThreads)
		sum += s_data[i];
	if (thread == 0)
		s_blockSum = 0;
	GroupMemoryBarrierWithGroupSync();
	InterlockedAdd(s_blockSum, sum);
	GroupMemoryBarrierWithGroupSync();

	if (thread == 0 && block < Blocks)
		blockSums.Store(block * 4, s_blockSum);
}

// One group: the block sums' exclusive scan added to every element's local offset, and the total.
[numthreads(kThreads, 1, 1)] void BlockOffsets(uint3 groupThread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
	if (any(group != 0))
		return;
	RWByteAddressBuffer offsets = ResourceDescriptorHeap[OffsetsIndex];
	RWByteAddressBuffer blockSums = ResourceDescriptorHeap[BlockSumsIndex];

	const uint thread = groupThread.x;
	const uint blocks = min(Blocks, kBlockSize);

	for (uint i = thread; i < blocks; i += kThreads)
		s_data[i] = blockSums.Load(i * 4);
	GroupMemoryBarrierWithGroupSync();

	ExclusiveScanBlock(blocks, thread);
	GroupMemoryBarrierWithGroupSync();

	for (uint element = thread; element < Elements; element += kThreads) {
		const uint prefix = s_scan[element / kBlockSize];
		if (prefix != 0)
			offsets.Store(element * 4, offsets.Load(element * 4) + prefix);
	}

	if (thread == 0 && blocks > 0 && TotalIndex != kNoBuffer) {
		RWByteAddressBuffer total = ResourceDescriptorHeap[TotalIndex];
		total.Store(0, s_scan[blocks - 1] + s_data[blocks - 1]);
	}
}
