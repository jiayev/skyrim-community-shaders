// Drawcall Limit Fix: the scatter of the sort by pipeline (IndirectDraws.cpp, SortDraws). BuildDrawsCS appended this
// segment's sequences to the staging buffer, each with its rank among its pipeline's, and counted them per pipeline; the
// prefix sum (RenderGraph/PrefixSumCS.hlsl) turned the counts into each pipeline's first slot. Sequence i goes to its
// pipeline's first slot plus its rank, so the draw executes each pipeline's draws together and switches pipeline once per
// pipeline rather than per draw. Every slot of [0, count) is written exactly once.

cbuffer SortSequencesConstants : register(b0)
{
	uint StagingIndex;    // ByteAddressBuffer: DrawSequence[DrawLimit], as BuildDrawsCS appended them
	uint RanksIndex;      // ByteAddressBuffer: uint[DrawLimit], each the sequence's sort key << 20 | its rank among the key's
	uint OffsetsIndex;    // ByteAddressBuffer: uint[kSortKeys], each key's first slot
	uint CountIndex;      // ByteAddressBuffer: the draw count words (kCountDrawn, the sequences appended)
	uint SequencesIndex;  // RWByteAddressBuffer: DrawSequence[], what the draw executes
	uint SequenceStride;  // bytes: DrawSequence (BuildDrawsCS's kSequenceStride)
	uint DrawLimit;       // the draws' range: BuildDrawsCS's PhaseTwoBase
	uint Padding;
}

static const uint kCountDrawn = 0;
static const uint kSortRankBits = 20;  // BuildDrawsCS's
static const uint kSortRankMask = (1u << kSortRankBits) - 1;

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	ByteAddressBuffer count = ResourceDescriptorHeap[CountIndex];
	const uint index = dispatchID.x;
	if (index >= min(count.Load(kCountDrawn), DrawLimit))
		return;
	ByteAddressBuffer staging = ResourceDescriptorHeap[StagingIndex];
	ByteAddressBuffer ranks = ResourceDescriptorHeap[RanksIndex];
	ByteAddressBuffer offsets = ResourceDescriptorHeap[OffsetsIndex];
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];

	const uint source = index * SequenceStride;
	const uint4 head = staging.Load4(source);  // the pipeline, then the root constants
	const uint keyRank = ranks.Load(index * 4);
	const uint target = (offsets.Load((keyRank >> kSortRankBits) * 4) + (keyRank & kSortRankMask)) * SequenceStride;
	sequences.Store4(target, head);
	[loop] for (uint word = 16; word + 16 <= SequenceStride; word += 16)
		sequences.Store4(target + word, staging.Load4(source + word));
	[loop] for (uint tail = SequenceStride & ~15u; tail < SequenceStride; tail += 4)
		sequences.Store(target + tail, staging.Load(source + tail));
}
