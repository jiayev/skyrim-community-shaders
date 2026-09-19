// Writes value + index into a bindless RWByteAddressBuffer (OrgDxvkInteropTest).
// The write is atomic on purpose: DXC emits Device-scope atomics, which are only valid on
// DXVK's device (vulkanMemoryModel on) when the interop request enabled vulkanMemoryModelDeviceScope.
cbuffer WriteConstants : register(b0)
{
	uint outputDescriptorIndex;
	uint value;
	uint count;
};

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= count) return;
	RWByteAddressBuffer output = ResourceDescriptorHeap[outputDescriptorIndex];
	uint previous;
	output.InterlockedExchange(id.x * 4, value + id.x, previous);
}
