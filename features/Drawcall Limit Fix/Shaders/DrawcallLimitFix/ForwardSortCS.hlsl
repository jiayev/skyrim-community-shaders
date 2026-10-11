// Drawcall Limit Fix, T7d: the forward range's sort, back to front. The colour segment's BuildDrawsCS appended every visible forward
// draw (Records.h, kObjectForward: blended Lighting geometry) to the staging half of the forward range, each with its view depth (the
// bound centre's clip w) in its firstInstance word, which nothing executes there. Each thread takes one staged draw, counts the draws
// that come before it in the engine's blended order - farther first (FUN_1414e8fc0 sorts the blended list by the view axis distance of
// the bound centre, largest first), ties broken by the object index and then the first index, so the order is the same every frame
// whatever order the atomic append gave - and writes it to that rank of the sorted half, its firstInstance word cleared. Every slot of
// [0, count) of the sorted half is written exactly once; the forward epoch draws the sorted half through the count word.
//
// The rank is O(n^2) in the visible forward draws (tens a frame: the census's blended Lighting class is 1-5 passes a frame), with no
// cap: the range holds every draw the forward slots can produce (IndirectDraws: ReserveMainSequences). A larger class (the Effect port)
// would take a radix or bitonic sort here; the contract (staging half in, sorted half out, the count word) stays.

cbuffer ForwardSortConstants : register(b0)
{
	uint SequencesIndex;  // RWByteAddressBuffer: DrawSequence[], the main sequences
	uint CountIndex;      // ByteAddressBuffer: the count words (kCountForward: the draws appended to the staging half)
	uint StagingBase;     // the staging half's first slot; the sorted half follows it, Capacity slots on
	uint Capacity;        // each half's slots (PassFrame::forwardCapacity)
	uint SequenceStride;  // bytes (sizeof(DrawSequence))
	uint Padding0;
	uint Padding1;
	uint Padding2;
}

static const uint kCountForward = 128;     // GpuLayouts.h, kCountForwardWord
static const uint kObjectWordOffset = 20;  // DrawSequence::objectIndex
static const uint kFirstIndexOffset = 80;  // the DrawIndexed arguments' firstIndex
static const uint kFirstInstanceOffset = 88;
static const uint kObjectIndexMask = (1u << 26) - 1;  // Records.h, kObjectIndexMask

// Whether draw a comes before draw b in the blended order: farther first, then by object index, then by first index.
bool Before(float a_depth, uint a_object, uint a_first, float b_depth, uint b_object, uint b_first)
{
	if (a_depth != b_depth)
		return a_depth > b_depth;
	if (a_object != b_object)
		return a_object < b_object;
	return a_first < b_first;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	ByteAddressBuffer counts = ResourceDescriptorHeap[CountIndex];
	const uint count = min(counts.Load(kCountForward), Capacity);
	const uint index = dispatchID.x;
	if (index >= count)
		return;
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];

	const uint source = (StagingBase + index) * SequenceStride;
	const float depth = asfloat(sequences.Load(source + kFirstInstanceOffset));
	const uint object = sequences.Load(source + kObjectWordOffset) & kObjectIndexMask;
	const uint first = sequences.Load(source + kFirstIndexOffset);
	uint rank = 0;
	[loop] for (uint other = 0; other < count; ++other) {
		if (other == index)
			continue;
		const uint at = (StagingBase + other) * SequenceStride;
		const float otherDepth = asfloat(sequences.Load(at + kFirstInstanceOffset));
		const uint otherObject = sequences.Load(at + kObjectWordOffset) & kObjectIndexMask;
		const uint otherFirst = sequences.Load(at + kFirstIndexOffset);
		rank += Before(otherDepth, otherObject, otherFirst, depth, object, first) ? 1u : 0u;
	}

	const uint target = (StagingBase + Capacity + rank) * SequenceStride;
	[loop] for (uint word = 0; word + 4 <= SequenceStride; word += 4)
		sequences.Store(target + word, word == kFirstInstanceOffset ? 0u : sequences.Load(source + word));
}
