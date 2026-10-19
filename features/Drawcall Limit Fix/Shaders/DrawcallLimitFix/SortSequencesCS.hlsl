// Drawcall Limit Fix: the scatter of the sort by pipeline (Main/FrameLayout.h, DrawSort). BuildDrawsCS appended this
// segment's colour sequences to the staging buffer, each with its rank among its sort key's and counted per key (a state class's
// keys are contiguous: its first key plus the pipeline's index in it); the prefix sum (RenderGraph/PrefixSumCS.hlsl) turned the
// counts into each key's first slot over every class. A staged sequence carries its class's main sub-range's first slot in its
// instanceCount word and its class's first key in its firstInstance word (GpuLayouts.h, the class table), so sequence i goes to
// its class's first slot plus its key's place within the class plus its rank, and those words are restored (1, 0). Each class's
// call then executes its pipelines' draws together, switching pipeline once per pipeline rather than per draw. Every slot of each
// class's [first, first + its count) is written exactly once.

// Must match SortSequencesConstants (Draws/IndirectDraws/Main/FrameLayout.h).
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
static const uint kInstanceCountOffset = 76;  // the staged class's first slot (BuildDrawsCS)
static const uint kFirstInstanceOffset = 88;  // the staged class's first sort key (BuildDrawsCS)

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
	const uint classFirst = staging.Load(source + kInstanceCountOffset);
	const uint classKeys = offsets.Load(staging.Load(source + kFirstInstanceOffset) * 4);
	const uint target = (classFirst + offsets.Load((keyRank >> kSortRankBits) * 4) - classKeys + (keyRank & kSortRankMask)) * SequenceStride;
	sequences.Store4(target, head);
	[loop] for (uint word = 16; word + 16 <= SequenceStride; word += 16)
		sequences.Store4(target + word, staging.Load4(source + word));
	[loop] for (uint tail = SequenceStride & ~15u; tail < SequenceStride; tail += 4)
		sequences.Store(target + tail, staging.Load(source + tail));
	sequences.Store(target + kInstanceCountOffset, 1u);
	sequences.Store(target + kFirstInstanceOffset, 0u);
}
