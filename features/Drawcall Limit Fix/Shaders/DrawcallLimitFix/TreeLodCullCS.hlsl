// Drawcall Limit Fix: tree LOD's instances culled for the main camera (dclf-lod.md, "Tree LOD: the draws"; Scene/TreeLod.h).
//
// One thread per instance record: shape slot (id / 75), record (id % 75). A record past its slot's count, or hidden (scale 0,
// as the engine packs a hidden instance), draws nothing; the rest are tested against the depth segment's frustum and appended
// to the visible list, whose second word is the draw's instance count (zeroed by the depth commit). The Z-prepass's and the
// colour pass's tree LOD draws both draw the list. The dispatch covers every slot the tables hold; the draw row bounds them.

cbuffer TreeLodCullConstants : register(b0)
{
	uint ShapesIndex;     // ByteAddressBuffer: TreeLod::ShapeRow per shape slot
	uint InstancesIndex;  // ByteAddressBuffer: TreeLod::Instance, kMaxGroupInstances per shape slot
	uint VisibleIndex;    // RWByteAddressBuffer: the draw's arguments (4 words), then the visible records' indices
	uint LatchIndex;      // ByteAddressBuffer: the depth segment's BuildDrawsLatch (its view-projection)
	uint LatchOffset;
	uint DrawIndex;       // ByteAddressBuffer: TreeLod::DrawRow, whose shapeSlots (0: tree LOD does not draw this frame) bounds the slots
	uint Padding0;
	uint Padding1;
}

static const uint kMaxGroupInstances = 75;
static const uint kShapeRowBytes = 32;
static const uint kInstanceBytes = 32;
static const uint kVisibleHeaderBytes = 16;

// The cull's sphere test against the main camera's frustum, as FadeStateCS's InView: the depth segment's view-projection
// (camera translation folded in), its planes normalised; NoNearPlane (the latch's cull flags, bit 9) drops the near plane.
bool InView(float3 a_centre, float a_radius)
{
	ByteAddressBuffer latch = ResourceDescriptorHeap[LatchIndex];
	const uint cullFlags = latch.Load(LatchOffset + 16);
	const float4 r0 = asfloat(latch.Load4(LatchOffset + 32));
	const float4 r1 = asfloat(latch.Load4(LatchOffset + 48));
	const float4 r2 = asfloat(latch.Load4(LatchOffset + 64));
	const float4 r3 = asfloat(latch.Load4(LatchOffset + 80));
	float4 planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 };
	[unroll] for (uint p = 0; p < 6; ++p) {
		if (p == 4 && (cullFlags & 0x200u) != 0)
			continue;
		const float length = sqrt(dot(planes[p].xyz, planes[p].xyz));
		if (length <= 0.0f)
			continue;
		if ((dot(planes[p].xyz, a_centre) + planes[p].w) / length < -a_radius)
			return false;
	}
	return true;
}

[numthreads(64, 1, 1)] void main(uint a_id : SV_DispatchThreadID)
{
	const uint slot = a_id / kMaxGroupInstances;
	const uint record = a_id % kMaxGroupInstances;
	ByteAddressBuffer draw = ResourceDescriptorHeap[DrawIndex];
	if (slot >= draw.Load(44))
		return;
	ByteAddressBuffer shapes = ResourceDescriptorHeap[ShapesIndex];
	const uint shapeBase = slot * kShapeRowBytes;
	const uint count = shapes.Load(shapeBase + 16);
	if (record >= count)
		return;
	const float3 translate = asfloat(shapes.Load3(shapeBase));
	const float extent = asfloat(shapes.Load(shapeBase + 20));
	ByteAddressBuffer instances = ResourceDescriptorHeap[InstancesIndex];
	const uint index = slot * kMaxGroupInstances + record;
	const uint2 words = instances.Load2(index * kInstanceBytes);  // x, y | z, scale
	const float scale = f16tof32(words.y >> 16);
	if (scale <= 0.0f)
		return;
	const float3 position = translate + float3(f16tof32(words.x), f16tof32(words.x >> 16), f16tof32(words.y));
	// The instance's bound: its type's larger extent (width or height) times its scale, about its position, which holds the
	// billboard whichever way it turns (the engine pads its group bound by the same).
	if (!InView(position, extent * scale))
		return;
	RWByteAddressBuffer visible = ResourceDescriptorHeap[VisibleIndex];
	uint at;
	visible.InterlockedAdd(4, 1u, at);
	visible.Store(kVisibleHeaderBytes + at * 4, index);
}
