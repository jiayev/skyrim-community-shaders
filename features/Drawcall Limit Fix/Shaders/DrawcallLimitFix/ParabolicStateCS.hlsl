// One native descriptor traversal per stream, in native preorder. Visited must already include ancestor and node-specific
// traversal rejection. A rejected Process1 call can still write state; Decision.x must not be used as the event predicate.
// Replay/integration stage: not scheduled until the persistent traversal and descriptor initial state are published.
cbuffer Constants : register(b0)
{
	uint DecisionsIndex;
	uint VisitedIndex;
	uint ValuesIndex;
	uint PrefixIndex;
	uint ResultsIndex;
	uint QueryCount;
	uint InitialMask;
	uint Base;
	uint Elements;
	uint SumsBase;
	uint ParentBase;
}

[numthreads(64, 1, 1)] void Seed(uint3 id : SV_DispatchThreadID) {
	const uint i = id.x + id.y * (64u * 65535u);
	if (i >= QueryCount)
		return;
	StructuredBuffer<uint4> decisions = ResourceDescriptorHeap[DecisionsIndex];
	StructuredBuffer<uint> visited = ResourceDescriptorHeap[VisitedIndex];
	RWStructuredBuffer<uint2> values = ResourceDescriptorHeap[ValuesIndex];
	const uint4 decision = decisions[i];
	const bool active = visited[i] != 0u;
	// Index+1 distinguishes no write from a write of mask zero. The second component propagates invalid state.
	values[i] = uint2(active && decision.z != 0u ? i + 1u : 0u, active && decision.w != 0u ? 1u : 0u);
}

// Same 256-wide scratch layout as PortalBatchLayout::ScanFor. Scan upward, then AddOffsets downward, with UAV barriers.
groupshared uint2 ScanValues[256];
[numthreads(256, 1, 1)] void Scan(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
	const uint block = group.x + group.y * 65535u;
	if (block >= Elements / 256u + (Elements % 256u != 0u ? 1u : 0u))
		return;
	const uint i = block * 256u + lane;
	RWStructuredBuffer<uint2> values = ResourceDescriptorHeap[ValuesIndex];
	RWStructuredBuffer<uint2> prefix = ResourceDescriptorHeap[PrefixIndex];
	ScanValues[lane] = i < Elements ? values[Base + i] : uint2(0, 0);
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 1; stride < 256u; stride <<= 1u)
	{
		const uint2 previous = lane >= stride ? ScanValues[lane - stride] : uint2(0, 0);
		GroupMemoryBarrierWithGroupSync();
		ScanValues[lane] = max(ScanValues[lane], previous);
		GroupMemoryBarrierWithGroupSync();
	}
	// Exclusive maximum cannot be obtained by subtraction as in the additive batch scan.
	if (i < Elements)
		prefix[Base + i] = lane != 0u ? ScanValues[lane - 1u] : uint2(0, 0);
	if (lane == 255u && SumsBase != 0u)
		values[SumsBase + block] = ScanValues[255];
}

	[numthreads(64, 1, 1)] void AddOffsets(uint3 id : SV_DispatchThreadID)
{
	const uint i = id.x + id.y * (64u * 65535u);
	if (i >= Elements)
		return;
	RWStructuredBuffer<uint2> prefix = ResourceDescriptorHeap[PrefixIndex];
	prefix[Base + i] = max(prefix[Base + i], prefix[ParentBase + i / 256u]);
}

[numthreads(64, 1, 1)] void Resolve(uint3 id : SV_DispatchThreadID) {
	const uint i = id.x + id.y * (64u * 65535u);
	if (i >= QueryCount)
		return;
	StructuredBuffer<uint4> decisions = ResourceDescriptorHeap[DecisionsIndex];
	StructuredBuffer<uint> visited = ResourceDescriptorHeap[VisitedIndex];
	StructuredBuffer<uint2> values = ResourceDescriptorHeap[ValuesIndex];
	StructuredBuffer<uint2> prefix = ResourceDescriptorHeap[PrefixIndex];
	RWStructuredBuffer<uint4> results = ResourceDescriptorHeap[ResultsIndex];
	const uint2 state = max(prefix[i], values[i]);
	const uint mask = state.x != 0u ? decisions[state.x - 1u].y : InitialMask;
	const bool active = visited[i] != 0u;
	// Invalid upstream state cannot produce a usable later verdict, including for an untested node.
	results[i] = uint4(active && state.y == 0u ? decisions[i].x : 0u, mask, active ? decisions[i].z : 0u, state.y);
}
