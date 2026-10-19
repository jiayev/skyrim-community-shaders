#include "DrawcallLimitFix/ParabolicPrograms.hlsli"

// Reference/replay kernel. Production consumer migration also requires the persistent hierarchy and ordered hemisphere state.
struct ParabolicQuery
{
	float4 Bound;
	float4 Sphere;
	float4 Plane;
	uint4 Control;  // local program, effective mode, node flags, process flags (bit 0 ignorePreprocess, bit 1 bothSides)
};
cbuffer Constants : register(b0)
{
	uint QueriesIndex;
	uint ResultsIndex;
	uint ProgramsIndex;
	uint QueryCount;
	uint ProgramsBase;
}
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	const uint index = id.x + id.y * (64u * 65535u);
	if (index >= QueryCount)
		return;
	StructuredBuffer<ParabolicQuery> queries = ResourceDescriptorHeap[QueriesIndex];
	RWStructuredBuffer<uint4> results = ResourceDescriptorHeap[ResultsIndex];
	ByteAddressBuffer programs = ResourceDescriptorHeap[ProgramsIndex];
	const ParabolicQuery query = queries[index];
	results[index] = ParabolicTestAt(programs, ProgramsBase, query.Control.x, query.Control.y, query.Control.z,
		(query.Control.w & 1u) != 0u, query.Bound, query.Sphere, query.Plane, (query.Control.w & 2u) != 0u);
}
