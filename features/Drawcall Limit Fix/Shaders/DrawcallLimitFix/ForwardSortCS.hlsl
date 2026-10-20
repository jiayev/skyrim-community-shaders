// Drawcall Limit Fix: the forward range's sort, back to front. The colour segment's BuildDrawsCS appended every visible forward
// draw (Records.h, kObjectForward: blended Lighting geometry and the Effect port's meshes) to the staging half of the forward range,
// each with its sort key in three words nothing executes there: its view depth (the clip w of its bound centre, or of its BSOrderedNode's
// centre for a leaf of an ordered group) in firstInstance, and for such a leaf its group (a hash of that centre) in instanceCount and its
// ordinal under the node in vertexOffset. Each thread takes one staged draw, counts the draws that come before it in the engine's blended
// order, and writes it to that rank of the sorted half with those three words put back (instanceCount 1, vertexOffset 0, firstInstance
// 0). Every slot of [0, count) of the sorted half is written exactly once; the forward epoch draws the sorted half through the count word.
//
// The engine's order (FUN_1414b39d0): the blended list sorted by the view axis distance of the bound centre, farthest first
// (FUN_1414e8fc0, stable), merged with the ordered nodes' groups sorted by their node's distance (qsort, 0x1414f55e0) - a group drawn
// whole, its passes in their registration order (FUN_1414f19d0: the node's child order), before a list pass nearer than it and after one
// at its distance or farther (FUN_1414f5330). So: farther first; at one depth a plain draw before a group's; then by group, by ordinal,
// and last by the object index and the first index, so the order is the same every frame whatever order the atomic append gave.
//
// The rank is O(n^2) in the visible forward draws, with no cap: the range holds every draw the forward slots can produce (IndirectDraws:
// ReserveMainSequences). The keys are read through groupshared memory a tile of 64 at a time, so each thread group reads every key once
// per tile rather than each thread on its own. A much larger class would take a radix or bitonic sort here; the contract (staging half
// in, sorted half out, the count word) stays.
//
// The forward range is split by state class (GpuLayouts.h, kClassRangeForward): each class's staging and sorted halves are their own,
// sorted by one dispatch of this program each, its count the class's word (kSortCountOffset): the colour segment's classCounts word,
// the first-person view's forward range's class entry's bucket count word (its keys staged by the first-person BuildDraws with the
// first-person camera's view depth). A class's blended draws keep the engine's order among themselves; across classes the order is the
// classes' (each class's call draws after the one before).

// Must match ForwardSortConstants (Draws/IndirectDraws/Main/FrameLayout.h).
cbuffer ForwardSortConstants : register(b0)
{
	uint SequencesIndex;  // RWByteAddressBuffer: DrawSequence[], the main sequences
	uint CountIndex;      // ByteAddressBuffer: the count words (with kSortCountOffset, the class's: its draws appended to the staging half)
	uint StagingBase;     // the staging half's first slot; the sorted half follows it, Capacity slots on
	uint Capacity;        // each half's slots (the class's forward sub-range's capacity)
	uint SequenceStride;  // bytes (sizeof(DrawSequence))
	uint CountOffset;     // with kSortCountOffset, the byte offset of the count word in CountIndex (the first-person forward range's bucket count)
	uint Options;         // kSortCountOffset, kSortRankInstance
	uint Padding2;
}

static const uint kSortCountOffset = 1;   // ForwardSortConstants::kCountOffset
static const uint kSortRankInstance = 2;  // ForwardSortConstants::kRankInstance: firstInstance is the rank (a forward bucket's plain draw)

static const uint kCountForward = 128;     // GpuLayouts.h, kCountForwardWord
static const uint kObjectWordOffset = 20;  // DrawSequence::objectIndex
static const uint kInstanceCountOffset = 76;
static const uint kFirstIndexOffset = 80;  // the DrawIndexed arguments' firstIndex
static const uint kVertexOffsetOffset = 84;
static const uint kFirstInstanceOffset = 88;
static const uint kObjectIndexMask = (1u << 26) - 1;  // Records.h, kObjectIndexMask
static const uint kTile = 64;                         // the thread group (IndirectDraws: kForwardSortGroup)

struct SortKey
{
	float depth;
	uint group;
	uint ordinal;  // 0: a plain draw
	uint object;
	uint first;
};

SortKey LoadKey(RWByteAddressBuffer a_sequences, uint a_slot)
{
	const uint at = (StagingBase + a_slot) * SequenceStride;
	SortKey key;
	key.depth = asfloat(a_sequences.Load(at + kFirstInstanceOffset));
	key.group = a_sequences.Load(at + kInstanceCountOffset);
	key.ordinal = a_sequences.Load(at + kVertexOffsetOffset);
	key.object = a_sequences.Load(at + kObjectWordOffset) & kObjectIndexMask;
	key.first = a_sequences.Load(at + kFirstIndexOffset);
	return key;
}

// Whether draw a comes before draw b in the blended order.
bool Before(SortKey a, SortKey b)
{
	if (a.depth != b.depth)
		return a.depth > b.depth;
	if ((a.ordinal == 0) != (b.ordinal == 0))
		return a.ordinal == 0;
	if (a.group != b.group)
		return a.group < b.group;
	if (a.ordinal != b.ordinal)
		return a.ordinal < b.ordinal;
	if (a.object != b.object)
		return a.object < b.object;
	return a.first < b.first;
}

groupshared float sharedDepth[kTile];
groupshared uint sharedGroup[kTile];
groupshared uint sharedOrdinal[kTile];
groupshared uint sharedObject[kTile];
groupshared uint sharedFirst[kTile];

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID)
{
	ByteAddressBuffer counts = ResourceDescriptorHeap[CountIndex];
	const uint count = min(counts.Load((Options & kSortCountOffset) != 0 ? CountOffset : kCountForward), Capacity);
	if (groupID.x * kTile >= count)
		return;  // the whole group past the count (uniform in the group, so before any barrier)
	RWByteAddressBuffer sequences = ResourceDescriptorHeap[SequencesIndex];
	const uint index = dispatchID.x;
	const bool staged = index < count;
	const SortKey mine = LoadKey(sequences, staged ? index : 0);

	uint rank = 0;
	[loop] for (uint tile = 0; tile < count; tile += kTile) {
		const uint other = tile + threadID.x;
		if (other < count) {
			const SortKey key = LoadKey(sequences, other);
			sharedDepth[threadID.x] = key.depth;
			sharedGroup[threadID.x] = key.group;
			sharedOrdinal[threadID.x] = key.ordinal;
			sharedObject[threadID.x] = key.object;
			sharedFirst[threadID.x] = key.first;
		}
		GroupMemoryBarrierWithGroupSync();
		const uint keys = min(kTile, count - tile);
		if (staged) {
			[loop] for (uint k = 0; k < keys; ++k) {
				if (tile + k == index)
					continue;
				SortKey key;
				key.depth = sharedDepth[k];
				key.group = sharedGroup[k];
				key.ordinal = sharedOrdinal[k];
				key.object = sharedObject[k];
				key.first = sharedFirst[k];
				rank += Before(key, mine) ? 1u : 0u;
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (!staged)
		return;

	const uint source = (StagingBase + index) * SequenceStride;
	const uint target = (StagingBase + Capacity + rank) * SequenceStride;
	const uint firstInstance = (Options & kSortRankInstance) != 0 ? rank : 0u;
	[loop] for (uint word = 0; word + 4 <= SequenceStride; word += 4) {
		const uint value = word == kInstanceCountOffset ? 1u : word == kVertexOffsetOffset ? 0u : word == kFirstInstanceOffset ? firstInstance : sequences.Load(source + word);
		sequences.Store(target + word, value);
	}
}
