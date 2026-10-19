#include "DrawcallLimitFix/PortalContexts.hlsli"
#include "DrawcallLimitFix/PortalPrograms.hlsli"
#include "DrawcallLimitFix/PortalQueries.hlsli"

cbuffer PortalBatchConstants : register(b0)
{
	uint QueriesIndex;
	uint ResultsIndex;
	uint ProgramsIndex;
	uint CountsIndex;
	uint OffsetsIndex;
	uint RanksIndex;
	uint IndicesIndex;
	uint TasksIndex;
	uint DispatchIndex;
	uint QueryCount;
	uint BucketCount;
	uint Base;
	uint Elements;
	uint SumsBase;
	uint ParentBase;
	// The depth latch (BuildDrawsLatch in this frame slot), whose portalProgramsIndex names the frame's programs when ProgramsIndex
	// is 0. A context atlas batch binds its publication directly.
	uint LatchIndex;
	uint LatchOffset;
	// The portal bench's auxiliary table for an experimental evaluator (PortalEvaluators.hlsli); 0 in production.
	uint AuxIndex;
	uint2 Padding;
}

ByteAddressBuffer Programs()
{
	uint index = ProgramsIndex;
	if (index == 0u && LatchIndex != 0u) {
		ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
		index = latch.Load(LatchOffset + kLatchPortalPrograms);
	}
	ByteAddressBuffer programs = ResourceDescriptorHeap[index];
	return programs;
}

// A query's verdict against its bucket's program: 1 in view, 0 culled, -1 no program, -2 an invalid atlas context.
int QueryVerdict(PortalQuery query, uint bucket)
{
#ifdef DCLF_PORTAL_EXPERIMENTAL
	PortalAuxIndex = AuxIndex;
#endif
	ByteAddressBuffer programs = Programs();
	uint programsBase = 0, program = bucket;
#ifdef DCLF_PORTAL_CONTEXT_ATLAS
	uint contextFrame;
	if (!PortalContextProgram(programs, bucket, programsBase, program, contextFrame) || contextFrame != query.Generation)
		return -2;
#endif
	return PortalTestAt(programs, programsBase, program, query.Bound.xyz, query.Bound.w,
		(query.Flags & (1u << 11)) != 0, (query.Flags & (1u << 12)) != 0, (query.Flags & (1u << 20)) != 0);
}

[numthreads(64, 1, 1)] void Clear(uint3 id : SV_DispatchThreadID) {
	RWStructuredBuffer<uint2> counts = ResourceDescriptorHeap[CountsIndex];
	if ((id.x + id.y * (64u * 65535u)) < BucketCount)
		counts[(id.x + id.y * (64u * 65535u))] = 0;
	if ((id.x + id.y * (64u * 65535u)) == 0) {
		RWStructuredBuffer<uint> dispatch = ResourceDescriptorHeap[DispatchIndex];
		dispatch[0] = dispatch[3] = 0;
		dispatch[1] = dispatch[2] = 1;
	}
}

	[numthreads(64, 1, 1)] void Count(uint3 id : SV_DispatchThreadID)
{
	if ((id.x + id.y * (64u * 65535u)) >= QueryCount)
		return;
	StructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[QueriesIndex];
	const PortalQuery query = queries[(id.x + id.y * (64u * 65535u))];
	RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[ResultsIndex];
	// Queries with an immediate verdict use Flags bit 31 and the signed verdict in Program.
	if ((query.Flags & 0x80000000u) != 0u) {
		results[query.Destination] = uint2(query.Generation, query.Program);
		return;
	}
	// Programs past the adopted bucket capacity (a growth not yet adopted) are evaluated here, unbatched, with the same verdict.
	if (query.Program >= BucketCount) {
		results[query.Destination] = uint2(query.Generation, asuint(QueryVerdict(query, query.Program)));
		return;
	}
	RWStructuredBuffer<uint2> counts = ResourceDescriptorHeap[CountsIndex];
	RWStructuredBuffer<uint> ranks = ResourceDescriptorHeap[RanksIndex];
	uint rank;
	InterlockedAdd(counts[query.Program].x, 1u, rank);
	ranks[(id.x + id.y * (64u * 65535u))] = rank;
}

[numthreads(64, 1, 1)] void TaskCounts(uint3 id : SV_DispatchThreadID) {
	if ((id.x + id.y * (64u * 65535u)) >= BucketCount)
		return;
	RWStructuredBuffer<uint2> counts = ResourceDescriptorHeap[CountsIndex];
	const uint count = counts[(id.x + id.y * (64u * 65535u))].x;
	counts[(id.x + id.y * (64u * 65535u))] = uint2(count, count / 64u + (count % 64u != 0u ? 1u : 0u));
}

// Hierarchical exclusive scan of {queries, tasks}. The host records levels and reverse offset propagation, without readback.
groupshared uint2 ScanValues[256];
[numthreads(256, 1, 1)] void Scan(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
	// A two-dimensional dispatch can contain surplus groups in its last row. Reject them uniformly before barriers.
	const uint block = group.x + group.y * 65535u;
	if (block >= Elements / 256u + (Elements % 256u != 0u ? 1u : 0u))
		return;
	RWStructuredBuffer<uint2> counts = ResourceDescriptorHeap[CountsIndex];
	RWStructuredBuffer<uint2> offsets = ResourceDescriptorHeap[OffsetsIndex];
	const uint i = (group.x + group.y * 65535u) * 256u + lane;
	const uint2 value = i < Elements ? counts[Base + i] : uint2(0, 0);
	ScanValues[lane] = value;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 1; stride < 256u; stride <<= 1u)
	{
		const uint2 add = lane >= stride ? ScanValues[lane - stride] : uint2(0, 0);
		GroupMemoryBarrierWithGroupSync();
		ScanValues[lane] += add;
		GroupMemoryBarrierWithGroupSync();
	}
	if (i < Elements)
		offsets[Base + i] = ScanValues[lane] - value;
	if (lane == 255u && SumsBase != 0u)
		counts[SumsBase + (group.x + group.y * 65535u)] = ScanValues[255];
}

	[numthreads(64, 1, 1)] void AddOffsets(uint3 id : SV_DispatchThreadID)
{
	if ((id.x + id.y * (64u * 65535u)) >= Elements)
		return;
	RWStructuredBuffer<uint2> offsets = ResourceDescriptorHeap[OffsetsIndex];
	offsets[Base + (id.x + id.y * (64u * 65535u))] += offsets[ParentBase + (id.x + id.y * (64u * 65535u)) / 256u];
}

[numthreads(64, 1, 1)] void Scatter(uint3 id : SV_DispatchThreadID) {
	if ((id.x + id.y * (64u * 65535u)) >= QueryCount)
		return;
	StructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[QueriesIndex];
	const PortalQuery query = queries[(id.x + id.y * (64u * 65535u))];
	if ((query.Flags & 0x80000000u) != 0u || query.Program >= BucketCount)
		return;
	RWStructuredBuffer<uint2> offsets = ResourceDescriptorHeap[OffsetsIndex];
	RWStructuredBuffer<uint> ranks = ResourceDescriptorHeap[RanksIndex];
	RWStructuredBuffer<uint> indices = ResourceDescriptorHeap[IndicesIndex];
	indices[offsets[query.Program].x + ranks[(id.x + id.y * (64u * 65535u))]] = (id.x + id.y * (64u * 65535u));
}

	[numthreads(64, 1, 1)] void MakeTasks(uint3 id : SV_DispatchThreadID)
{
	if ((id.x + id.y * (64u * 65535u)) >= BucketCount)
		return;
	RWStructuredBuffer<uint2> counts = ResourceDescriptorHeap[CountsIndex];
	RWStructuredBuffer<uint2> offsets = ResourceDescriptorHeap[OffsetsIndex];
	RWStructuredBuffer<PortalTask> tasks = ResourceDescriptorHeap[TasksIndex];
	const uint2 count = counts[(id.x + id.y * (64u * 65535u))], offset = offsets[(id.x + id.y * (64u * 65535u))];
	for (uint t = 0; t < count.y; ++t) {
		PortalTask task;
		task.Program = (id.x + id.y * (64u * 65535u));
		task.First = offset.x + t * 64u;
		task.Count = min(64u, count.x - t * 64u);
		task.Padding = 0;
		tasks[offset.y + t] = task;
	}
	if ((id.x + id.y * (64u * 65535u)) + 1u == BucketCount) {
		RWStructuredBuffer<uint> dispatch = ResourceDescriptorHeap[DispatchIndex];
		const uint total = offset.y + count.y;
		dispatch[0] = min(total, 65535u);
		dispatch[1] = max(1u, total / 65535u + (total % 65535u != 0u ? 1u : 0u));
		dispatch[3] = total;
	}
}

[numthreads(64, 1, 1)] void Evaluate(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
	StructuredBuffer<PortalTask> tasks = ResourceDescriptorHeap[TasksIndex];
	StructuredBuffer<uint> dispatch = ResourceDescriptorHeap[DispatchIndex];
	if ((group.x + group.y * 65535u) >= dispatch[3])
		return;
	const PortalTask task = tasks[(group.x + group.y * 65535u)];
	if (lane >= task.Count)
		return;
	StructuredBuffer<uint> indices = ResourceDescriptorHeap[IndicesIndex];
	StructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[QueriesIndex];
	const PortalQuery query = queries[indices[task.First + lane]];
	RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[ResultsIndex];
	results[query.Destination] = uint2(query.Generation, asuint(QueryVerdict(query, task.Program)));
}

// One thread per query, no batching: the verdict against the query's own program (the portal bench's inline variants; Count's
// past-capacity path is the same evaluation). DCLF_PORTAL_FLOOR writes the program word as the verdict without evaluating it: the
// dispatch's and the queries' memory cost alone.
[numthreads(64, 1, 1)] void Inline(uint3 id : SV_DispatchThreadID)
{
	if ((id.x + id.y * (64u * 65535u)) >= QueryCount)
		return;
	StructuredBuffer<PortalQuery> queries = ResourceDescriptorHeap[QueriesIndex];
	const PortalQuery query = queries[(id.x + id.y * (64u * 65535u))];
	RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[ResultsIndex];
#ifdef DCLF_PORTAL_FLOOR
	results[query.Destination] = uint2(query.Generation, query.Program);
#else
	if ((query.Flags & 0x80000000u) != 0u) {
		results[query.Destination] = uint2(query.Generation, query.Program);
		return;
	}
	results[query.Destination] = uint2(query.Generation, asuint(QueryVerdict(query, query.Program)));
#endif
}
