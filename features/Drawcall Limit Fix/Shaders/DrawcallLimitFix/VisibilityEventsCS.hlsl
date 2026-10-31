// Drawcall Limit Fix: the main camera's visibility as change events (dclf-architecture.md, "The visibility events").
//
// The engine's main-camera cull leaves kAccumulated (NiAVObject flag bit 26) on what passed it: on a reference root when its
// bound passed the cull's test, on a geometry when the cull reached it in view. Its readers (the animated references' update,
// the dynamic lights' receivers, the tree clock: skyrim-engine-notes.md, "Engine side effects of the culls") need it
// once the engine's culls stop. DCLF already has both verdicts on the GPU: a fade root's is FadeStateCS's kFadeVerdictInView
// (the list processes' Process1 test through the root's portal program), a geometry's is depth phase 1's frustum stamp
// (cs.dclf.frustum: stamped this frame, and not dropped by its root's fade). Neither is DCLF's HZB occlusion, which the engine's cull
// does not make: kAccumulated means in the main camera's view (frustum, portals, occlusion planes), not app-culled, and for a geometry
// not under a root whose OnVisible stops (dclf-architecture.md, "What kAccumulated means"). Phase 1 stamps every main input,
// the decal, forward and refraction ones too, which the depth segment draws none of. This pass compares them, once a frame after both,
// with what it last reported per slot, and appends only the changes to the event list, which the host reads back
// asynchronously (VisibilityReadback) and decodes on DCLF's executor.
//
// One thread per root slot, then one per object slot. A root's reported word is keyed by its listing's generation: a new
// listing (or a zeroed word: a new backing) reports its verdict when it is in view, and the host takes a generation it has no
// event of as out of view. An object slot has no generation on the GPU: its word is the slot's (whatever occupies it), and the
// host resolves the occupant with the tables of the event's frame. A change that does not fit the list leaves its word as it
// was, so it is appended again next frame; the count past the list tells the host to grow it (no change is ever dropped).
//
// The normal path runs this pass: DCLF writes kAccumulated on the roots the cut stands in for from these events (dclf-architecture.md,
// "kAccumulated written by DCLF"); CS_DCLF_ACCUMULATED_PARITY adds the observer against the engine's bits. InMargin (a verdict against
// widened planes, for the readback's latency) stays reserved: this pass binds no camera planes (the host's rule keeps two decoded frames
// instead).

// Must match VisibilityEventsConstants (Draws/IndirectDraws/Core/GpuLayouts.h).
cbuffer VisibilityEventsConstants : register(b0)
{
	uint RootsIndex;           // StructuredBuffer<FadeRootStatic>: the listings' generations
	uint StatesIndex;          // StructuredBuffer<FadeNodeState>: FadeStateCS's state rows (this frame's after it)
	uint FrameIndex;           // StructuredBuffer<FadeFrame>, one row: the scene frame, the root count, the camera
	uint FrustumIndex;         // ByteAddressBuffer: per object, depth phase 1's frustum stamp (BuildDrawsCS)
	uint RootReportedIndex;    // RWStructuredBuffer<uint2>: per root, the generation and the bits last reported
	uint ObjectReportedIndex;  // RWStructuredBuffer<uint>: per object, the bits last reported
	uint EventsIndex;          // RWStructuredBuffer<uint>: the header, then the events (GpuLayouts.h, VisibilityEvent)
	uint EventCapacity;        // the events the list and every readback slot hold
	uint RootSlots;            // the root threads (the root buffers' capacity); the object threads follow
	uint ObjectSlots;          // the object threads (the scene's object capacity)
	uint Padding0;
	uint Padding1;
}

// FadeStateCS.hlsl's rows: keep the three the same as there and as Records.h's FadeNodeState, FadeRootStatic and FadeFrame.
struct FadeNodeState
{
	uint Flags;
	float CurrentFade;
	float SnapRadius;
	int LastVisible;
	float AmountFade;
	float Metric;
	float PreviousMetric;
	float Blend;
	uint Levels;
	uint Generation;
	uint Verdict;
	uint Frame;
};

struct FadeRootStatic
{
	FadeNodeState Initial;
	float Radius;
	float FadeAmount;
	float NearDistance;
	float FarDistance;
	uint Object;
	uint Bits;
	float LodScale;
	uint Generation;
};

struct FadeFrame
{
	float3 Eye;
	float LodAdjust;
	int Counter;
	float DeltaTime;
	uint FadesOn;
	uint LodUpdates;
	float FadeInTime;
	float FadeOutTime;
	float FadeInAbove;
	float FadeOutBelow;
	float BlendTime;
	float DistanceMult;
	float StepMax;
	float AmountTime;
	float MetricScale;
	float DefaultScale;
	float MetricOverride;
	uint Overridden;
	float LodFar;
	float LodNear;
	float TreeLodFar;
	float TreeLodNear;
	float LodMinimum;
	float One;
	float AmountSnapAbove;
	float AmountSnapOffset;
	float SnapRadiusLimit;
	float TreeHeightBase;
	float TreeHeightLimit;
	uint Padding;
	float4 Divisors[4];
	uint RootCount;
	uint SceneFrame;
	uint LogBase;
	uint Reserved;
	float3 AnimEye;
	float AnimLodAdjust;
	int AnimCounter;
	float AnimDeltaTime;
	uint2 AnimPadding;
};

static const uint kNoObject = 0xFFFFFFFFu;
static const uint kFadeVerdictInView = 1u << 0;
// BuildDrawsCS's frustum stamp word: the stamp in the low 28 bits, the fade test's drop in bit 31.
static const uint kFrustumStampMask = 0x0FFFFFFFu;
static const uint kFrustumFadeHidden = 0x80000000u;
// The events (GpuLayouts.h: kVisibilityEvent*): a header of four words (the count appended, which may pass the capacity; the
// scene frame), then four words an event (slot, generation, kind, bits).
static const uint kHeaderWords = 4;
static const uint kKindRoot = 0;
static const uint kKindObject = 1;
static const uint kBitVisible = 1u << 0;   // a root: InView (its kAccumulated); an object: stamped and not fade-hidden
static const uint kBitInMargin = 1u << 1;  // reserved (InView against widened planes: not yet produced)
static const uint kBitVerdict = 1u << 2;   // a root: FadeStateCS updated its listing this frame (else DCLF has no verdict for it)
static const uint kBitFadeHidden = 1u << 3;  // an object: stamped (in view) but dropped by its root's fade
// An object's reported word: these bits, and kReportedValid once a verdict has been reported for the slot.
static const uint kReportedValid = 1u << 31;

static uint SceneFrame;

// Appends an event; false when it does not fit (the caller leaves its reported word, so it is appended again next frame).
bool Append(uint a_slot, uint a_generation, uint a_kind, uint a_bits)
{
	RWStructuredBuffer<uint> events = ResourceDescriptorHeap[EventsIndex];
	uint at;
	InterlockedAdd(events[0], 1u, at);
	if (at >= EventCapacity)
		return false;
	const uint word = kHeaderWords + at * 4u;
	events[word] = a_slot;
	events[word + 1u] = a_generation;
	events[word + 2u] = a_kind;
	events[word + 3u] = a_bits;
	return true;
}

// A root slot: its InView this frame against what was reported for its listing.
void RootSlot(uint a_slot, FadeFrame a_frame)
{
	// No camera this frame: FadeStateCS updated nothing, so nothing is known (the words stand).
	if (a_slot >= a_frame.RootCount || a_frame.LodAdjust == 0.0f)
		return;
	StructuredBuffer<FadeRootStatic> roots = ResourceDescriptorHeap[RootsIndex];
	const FadeRootStatic root = roots[a_slot];
	if (root.Object == kNoObject || root.Generation == 0)
		return;
	StructuredBuffer<FadeNodeState> states = ResourceDescriptorHeap[StatesIndex];
	const FadeNodeState state = states[a_slot];
	// The state of this frame's update of this listing: one not updated this frame (no seed yet, no member drawn) has no verdict.
	const bool updated = state.Generation == root.Generation && state.Frame == SceneFrame;
	const uint bits = updated ? (kBitVerdict | ((state.Verdict & kFadeVerdictInView) != 0 ? kBitVisible : 0u)) : 0u;
	RWStructuredBuffer<uint2> reported = ResourceDescriptorHeap[RootReportedIndex];
	const uint2 last = reported[a_slot];
	// A listing with no report yet has no verdict as far as the host knows.
	const uint lastBits = last.x == root.Generation ? last.y : 0u;
	if (lastBits == bits) {
		reported[a_slot] = uint2(root.Generation, bits);
		return;
	}
	if (Append(a_slot, root.Generation, kKindRoot, bits))
		reported[a_slot] = uint2(root.Generation, bits);
}

// An object slot: stamped by this frame's depth phase 1 and not fade-hidden, against what was reported for the slot.
void ObjectSlot(uint a_slot)
{
	ByteAddressBuffer frustum = ResourceDescriptorHeap[FrustumIndex];
	uint frustumBytes;
	frustum.GetDimensions(frustumBytes);
	RWStructuredBuffer<uint> reported = ResourceDescriptorHeap[ObjectReportedIndex];
	uint reportedSlots, stride;
	reported.GetDimensions(reportedSlots, stride);
	if (a_slot * 4u >= frustumBytes || a_slot >= reportedSlots)
		return;
	const uint stamp = frustum.Load(a_slot * 4u);
	const bool stamped = (stamp & kFrustumStampMask) == (SceneFrame & kFrustumStampMask);
	const uint bits = stamped ? ((stamp & kFrustumFadeHidden) == 0 ? kBitVisible : kBitFadeHidden) : 0u;
	const uint last = reported[a_slot];
	// A slot with no report yet (a new backing) was out of view as far as the host knows (its decoder starts again with it).
	const uint lastBits = (last & kReportedValid) != 0 ? (last & ~kReportedValid) : 0u;
	if (lastBits == bits) {
		reported[a_slot] = kReportedValid | bits;
		return;
	}
	if (Append(a_slot, 0u, kKindObject, bits))
		reported[a_slot] = kReportedValid | bits;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	StructuredBuffer<FadeFrame> frames = ResourceDescriptorHeap[FrameIndex];
	const FadeFrame frame = frames[0];
	SceneFrame = frame.SceneFrame;
	const uint index = dispatchID.x;
	if (index == 0) {
		// The list's frame: the host checks it against the frame it armed the readback for.
		RWStructuredBuffer<uint> events = ResourceDescriptorHeap[EventsIndex];
		events[1] = SceneFrame;
	}
	if (index < RootSlots)
		RootSlot(index, frame);
	else if (index - RootSlots < ObjectSlots)
		ObjectSlot(index - RootSlots);
}
