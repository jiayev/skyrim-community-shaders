// Drawcall Limit Fix: grass's draws' arguments (dclf-architecture.md, "Grass: the draws"; Features/Grass/GrassLayout.h): one thread per
// draw slot (a bucket's LOD tier, slot = bucket * 3 + tier) packs the slot's non-indexed DrawInstanced into one of the draws buffer's
// arrays, counted in its header, which the family's ExecuteIndirect reads as its draw count. A slot past the draw row's buckets, of a
// bucket with no instances, or of a tier without a mesh packs nothing.
//
// Mode 1 (after phase 1's cull): phase 1's depth draws - the slot's count, kept as phase 1's.
// Mode 2 (after phase 2's cull): phase 2's - the instances phase 2 appended after phase 1's.
// Mode 3 (once both phases are done): the colour draws, into the array of the bucket's colour program (TruePbr: the second); then the
// slot's counts zeroed for the next frame's cull.
//
// A draw's firstInstance is the start of its range in the visible list: the vertex stage reads its instance's entry by SV_InstanceID,
// which counts from it.

// Must match GrassArgsConstants (Features/Grass/GrassTables.h).
cbuffer GrassArgsConstants : register(b0)
{
	uint DrawIndex;     // ByteAddressBuffer: GrassInstances::DrawRow (its bucketSlots bounds the slots)
	uint BucketsIndex;  // ByteAddressBuffer: GrassInstances::BucketRow per bucket slot
	uint MeshesIndex;   // ByteAddressBuffer: GrassInstances::MeshRow per mesh slot
	uint DrawsIndex;    // RWByteAddressBuffer: the draws buffer
	uint Mode;          // 1, 2 or 3
	uint DrawSlots;     // the slots the dispatch covers (the buckets the version holds, times 3)
	uint Padding0;
	uint Padding1;
}

// GrassLayout.h.
static const uint kTiers = 3;
static const uint kMaxDrawSlots = 3 * 1024;
static const uint kBucketRowBytes = 160;
static const uint kMeshRowBytes = 32;
static const uint kNoMesh = 0xFFFFFFFFu;
static const uint kBucketTruePbr = 1;
static const uint kRowBucketSlots = 152;
// The draws buffer.
static const uint kPhaseOneDraws = 0;
static const uint kPhaseTwoDraws = 4;
static const uint kColourDraws = 8;
static const uint kCountsOffset = 64;
static const uint kPhaseOneCountsOffset = kCountsOffset + kMaxDrawSlots * 4;
static const uint kPhaseOneArgs = kPhaseOneCountsOffset + kMaxDrawSlots * 4;
static const uint kPhaseTwoArgs = kPhaseOneArgs + kMaxDrawSlots * 16;
static const uint kColourArgs = kPhaseTwoArgs + kMaxDrawSlots * 16;  // the second colour program's follow, kMaxDrawSlots * 16 on

void Pack(RWByteAddressBuffer a_draws, uint a_counter, uint a_array, uint a_vertices, uint a_instances, uint a_first)
{
	uint at;
	a_draws.InterlockedAdd(a_counter, 1u, at);
	a_draws.Store4(a_array + at * 16, uint4(a_vertices, a_instances, 0, a_first));
}

[numthreads(64, 1, 1)] void main(uint a_id : SV_DispatchThreadID)
{
	ByteAddressBuffer draw = ResourceDescriptorHeap[DrawIndex];
	RWByteAddressBuffer draws = ResourceDescriptorHeap[DrawsIndex];
	if (a_id >= DrawSlots || a_id >= kMaxDrawSlots)
		return;
	const uint bucket = a_id / kTiers;
	const uint tier = a_id % kTiers;
	const uint count = draws.Load(kCountsOffset + a_id * 4);
	if (Mode == 3) {
		// Zeroed whatever the slot draws: a slot the next frame's row no longer names keeps no stale count.
		draws.Store(kCountsOffset + a_id * 4, 0u);
		draws.Store(kPhaseOneCountsOffset + a_id * 4, 0u);
	}
	if (bucket >= draw.Load(kRowBucketSlots))
		return;
	ByteAddressBuffer buckets = ResourceDescriptorHeap[BucketsIndex];
	const uint bucketBase = bucket * kBucketRowBytes;
	const uint mesh = buckets.Load(bucketBase + tier * 4);
	if (buckets.Load(bucketBase + 28) == 0 || mesh == kNoMesh)
		return;
	ByteAddressBuffer meshes = ResourceDescriptorHeap[MeshesIndex];
	const uint vertices = meshes.Load(mesh * kMeshRowBytes + 24);
	const uint first = buckets.Load(bucketBase + 16 + tier * 4);
	if (Mode == 1) {
		draws.Store(kPhaseOneCountsOffset + a_id * 4, count);
		if (count)
			Pack(draws, kPhaseOneDraws, kPhaseOneArgs, vertices, count, first);
	} else if (Mode == 2) {
		const uint phaseOne = draws.Load(kPhaseOneCountsOffset + a_id * 4);
		if (count > phaseOne)
			Pack(draws, kPhaseTwoDraws, kPhaseTwoArgs, vertices, count - phaseOne, first + phaseOne);
	} else if (count) {
		const uint program = (buckets.Load(bucketBase + 12) & kBucketTruePbr) != 0 ? 1u : 0u;
		Pack(draws, kColourDraws + program * 4, kColourArgs + program * kMaxDrawSlots * 16, vertices, count, first);
	}
}
