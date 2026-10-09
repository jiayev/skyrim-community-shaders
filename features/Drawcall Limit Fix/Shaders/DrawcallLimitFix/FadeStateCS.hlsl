#include "DrawcallLimitFix/PortalPrograms.hlsli"
// Drawcall Limit Fix: the fade roots' state on the GPU (drawcall-limit-fix.md, "Fades on the GPU"; Records.h, FadeRootStatic).
//
// The engine updates a BSFadeNode's fade and LOD state in its OnVisible, for the main camera's cull, whenever the node's bound
// passes the cull's frustum test. This pass does the same for every fade root a DCLF member draws under, from the depth
// segment's view-projection: one thread per root slot. A state row whose generation is not its static row's is new (a listed
// node) and starts from the node's values. The update is Scene/FadeState.cpp's port of the engine's functions, instruction
// for instruction; keep the two the same.
//
// A frame ahead, on the compute queue in the Z-prepass epoch: the state rows are this pass's alone, and every root's state
// after the update is published into the frame's output of two (by the scene frame's parity). The builds read the other one,
// the frame before's (BuildDrawsLatch::fadeStatesIndex), so nothing waits for this pass.

cbuffer FadeStateConstants : register(b0)
{
	uint RootsIndex;    // StructuredBuffer<FadeRootStatic>
	uint StatesIndex;   // RWStructuredBuffer<FadeNodeState>
	uint FrameIndex;    // StructuredBuffer<FadeFrame>, one row
	uint LatchIndex;    // ByteAddressBuffer: the depth segment's BuildDrawsLatch (its view-projection)
	uint LatchOffset;
	uint LogIndex;      // RWStructuredBuffer<FadeLogEntry> (CS_DCLF_FADE_PARITY)
	uint OutIndex0;     // RWStructuredBuffer<FadeNodeState>: the published states of the even frames
	uint OutIndex1;     // and of the odd
	uint ProgramsIndex;      // ByteAddressBuffer: the main camera's portal programs (Records.h, kPortal*; PortalPrograms.hlsli)
	uint RootProgramsIndex;  // StructuredBuffer<uint>: each root's program (its room's; kPortalNoProgram: the frustum alone)
	uint AnimatedIndex;    // StructuredBuffer<uint>: per root, the scene frame whose animation batch updated it
	uint EventsIndex;      // RWStructuredBuffer<uint>: the write-back's events, after a count word (Records.h, FadeEvent)
	uint ReportedIndex;    // RWStructuredBuffer<uint2>: per root, the generation and the milestone last reported
	uint EventCapacity;    // the events the list holds (and every readback of it)
	uint Padding3;
}

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
	// The pass's per-frame values (the pass is prepared ahead of the commit that writes this row): the root slots, the scene
	// frame and the parity log's first root (~0u: none).
	uint RootCount;
	uint SceneFrame;
	uint LogBase;
	uint Reserved;
	// The animation job's update's inputs (Records.h, FadeFrame anim*).
	float3 AnimEye;
	float AnimLodAdjust;
	int AnimCounter;
	float AnimDeltaTime;
	uint2 AnimPadding;
};

struct FadeLogEntry
{
	FadeNodeState Before;
	FadeNodeState After;
	float3 Centre;
	uint Root;
};

// The placement rows (LightingConstants.h, BindlessPlacement).
struct PlacementRows
{
	float4 Rows[9];
};

static const uint kFadeFlagFadedIn = 1u << 14;
static const uint kFadeFlagSettled = 1u << 15;
static const uint kFadeFlagLodInUpdate = 1u << 27;
static const uint kFadeVerdictInView = 1u << 0;
static const uint kFadeVerdictAnimated = 1u << 4;
static const uint kFadeVerdictAboveLimit = 1u << 1;
static const uint kFadeVerdictServiced = 1u << 2;
static const uint kFadeVerdictDrawn = 1u << 3;
static const uint kFadeRootPlanMask = 0x3u;
static const uint kFadeRootFade = 0;
static const uint kFadeRootLeaf = 1;
static const uint kFadeRootTree = 2;
static const uint kFadeRootOther = 3;
static const uint kFadeRootBitsShift = 8;
static const uint kFadeRootTreeLod = 1u << 16;
static const uint kFadeRootTreeThresholds = 1u << 17;
static const uint kObjectSunEntryRow = 7;
static const uint kObjectFadeNodeRow = 8;
static const uint kNoObject = 0xFFFFFFFFu;
static const uint kFadeRootStoodIn = 1u << 19;
static const uint kFadeEventHeaderWords = 4;

static FadeFrame F;

// MINSS a, b: a when a < b, else b. MAXSS: a when a > b, else b. UCOMISS's equality: equal, or unordered.
float MinSS(float a, float b) { return a < b ? a : b; }
float MaxSS(float a, float b) { return a > b ? a : b; }
bool UnorderedEqual(float a, float b) { return !(a < b) && !(a > b); }

uint TypeOf(FadeNodeState s) { return (s.Levels >> 8) & 0xFu; }
int SinceVisible(FadeNodeState s) { return int(uint(F.Counter) - uint(s.LastVisible)); }
bool LongUnseen(FadeNodeState s) { return s.LastVisible < int(uint(F.Counter) - 20u); }
float Divisor(uint a_type) { return F.Divisors[a_type >> 2][a_type & 3]; }

// FUN_14147b110: the fade value; writes the LOD metric and the unscaled distance.
float FadeValue(inout FadeNodeState s, FadeRootStatic r, float3 c, out float distance)
{
	const uint type = TypeOf(s);
	const float divisor = Divisor(type);
	precise float scale;
	if (divisor > 0.0f) {
		scale = F.LodAdjust / divisor;
	} else {
		scale = F.DefaultScale;
		if (type != 6) {
			s.Flags |= kFadeFlagFadedIn;
			s.CurrentFade = 1.0f;
		}
	}
	precise float dy = c.y - F.Eye.y;
	precise float dx = c.x - F.Eye.x;
	precise float dz = c.z - F.Eye.z;
	precise float sum = (dy * dy + dx * dx) + dz * dz;
	precise float d = sqrt(sum);
	distance = d;
	precise float x = d * scale;
	precise float nearDistance = F.DistanceMult * r.NearDistance;
	precise float value = F.One;
	if (x > nearDistance && F.LodUpdates != 0)
		value = F.One - (x - nearDistance) / (F.DistanceMult * r.FarDistance - nearDistance);
	s.PreviousMetric = s.Metric;
	s.Metric = x * F.MetricScale;
	if (F.Overridden != 0)
		s.Metric = F.MetricOverride;
	return value;
}

// FUN_14147a430: the LOD level and its cross-fade.
void LodStep(inout FadeNodeState s, FadeRootStatic r, float distance)
{
	uint l152 = s.Levels & 0xFFu;
	uint l153 = (s.Levels >> 8) & 0xFFu;
	uint level = l152 & 0xFu;
	l152 = ((l152 & 0xFu) | (l152 << 4)) & 0xFFu;
	l153 &= 0x7Fu;
	if (((l153 & 0xF0u) == 0x20u && UnorderedEqual(F.One, s.CurrentFade)) || F.LodUpdates == 0) {
		const bool tree = (l153 & 0xFu) == 4u && (r.Bits & kFadeRootTreeThresholds) != 0;
		const float farThreshold = tree ? F.TreeLodFar : F.LodFar;
		const float nearThreshold = tree ? F.TreeLodNear : F.LodNear;
		precise float farDistance = farThreshold * r.LodScale;
		if (farThreshold > 0.0f)
			farDistance = MaxSS(F.LodMinimum, farDistance);
		precise float nearDistance = nearThreshold * r.LodScale;
		if (nearThreshold > 0.0f)
			nearDistance = MaxSS(F.LodMinimum, nearDistance);
		level = distance > farDistance ? 1u : (distance <= nearDistance ? 3u : 2u);
	}
	if (LongUnseen(s) || F.LodUpdates == 0) {
		l152 = (l152 & 0xF0u) | level;
		l153 = (l153 & 0xAFu) | 0x20u;
	} else {
		uint state = l153;
		const uint transition = state & 0x70u;
		if (transition > 0x20u) {
			state = (state & 0xBFu) | 0x30u;
			precise float blend = F.DeltaTime / F.BlendTime + s.Blend;
			s.Blend = MinSS(blend, F.One);
			if (UnorderedEqual(s.Blend, F.One)) {
				l152 = (l152 & 0xF0u) | ((l152 + 1u) & 0xFu);
				state = (state & 0x2Fu) | 0x80u;
			}
		} else if (transition < 0x20u) {
			state = (state & 0x9Fu) | 0x10u;
			precise float blend = s.Blend - F.DeltaTime / F.BlendTime;
			s.Blend = MaxSS(blend, 0.0f);
			if (UnorderedEqual(s.Blend, 0.0f))
				state = (state & 0x0Fu) | 0xA0u;
		} else {
			const uint previous = l152 >> 4;
			if (previous < level) {
				s.Blend = 0.0f;
				state = (state & 0x0Fu) | 0xC0u;
			} else if (previous > level) {
				s.Blend = 1.0f;
				state = (state & 0x0Fu) | 0x80u;
				l152 = (l152 & 0xF0u) | level;
			}
		}
		l153 = state;
	}
	s.Levels = (s.Levels & 0xFFFF0000u) | l152 | (l153 << 8);
}

// FUN_14147a160: the fade state machine.
void FadeUpdate(inout FadeNodeState s, FadeRootStatic r, float3 c, float amount)
{
	float distance;
	const float value = FadeValue(s, r, c, distance);
	if ((s.Flags & kFadeFlagLodInUpdate) != 0)
		LodStep(s, r, distance);
	const uint type = TypeOf(s);
	if (type != 8 && LongUnseen(s) && F.SnapRadiusLimit > s.SnapRadius) {
		if (value > F.FadeInAbove || ((s.Flags & kFadeFlagFadedIn) != 0 && value > F.FadeOutBelow)) {
			s.Flags |= kFadeFlagFadedIn;
			s.CurrentFade = 1.0f;
		} else {
			s.CurrentFade = 0.0f;
			s.Flags &= ~kFadeFlagFadedIn;
		}
	}
	const bool alwaysOut = ((r.Bits >> kFadeRootBitsShift) & 1u) != 0;
	const float one = F.One;
	if (!alwaysOut) {
		const bool fadedIn = (s.Flags & kFadeFlagFadedIn) != 0;
		if (fadedIn && (!UnorderedEqual(amount, one) || !UnorderedEqual(one, s.AmountFade))) {
			precise float step = MinSS(F.DeltaTime / F.AmountTime, F.StepMax);
			precise float fade;
			if (amount > F.AmountSnapAbove) {
				fade = amount - F.AmountSnapOffset;
			} else {
				const float last = s.AmountFade;
				fade = amount >= last ? MinSS(last + step, amount) : MaxSS(last - step, amount);
			}
			s.CurrentFade = fade;
			s.AmountFade = fade;
			return;
		}
		const float current = s.CurrentFade;
		const bool fadeIn = (!fadedIn && value > F.FadeInAbove) || (current > 0.0f && current < one && value > F.FadeOutBelow);
		if (fadeIn) {
			if (SinceVisible(s) > 1 && type != 8) {
				s.CurrentFade = 1.0f;
				s.Flags |= kFadeFlagFadedIn;
				return;
			}
			precise float fade = MinSS(MinSS(F.DeltaTime / F.FadeInTime, F.StepMax) + s.CurrentFade, one);
			s.CurrentFade = fade;
			if (fade < one)
				return;
			s.Flags |= kFadeFlagFadedIn;
			return;
		}
	}
	// 0x14147a36a: fading out, or nothing.
	if (!((s.Flags & kFadeFlagFadedIn) != 0 && (alwaysOut || value < F.FadeOutBelow))) {
		const float current = s.CurrentFade;
		if (!(current > 0.0f) || !(current < one))
			return;
		if (!alwaysOut && !(value < F.FadeInAbove))
			return;
	}
	if (SinceVisible(s) > 1 && type != 8) {
		s.CurrentFade = 0.0f;
	} else {
		precise float fade = MaxSS(s.CurrentFade - MinSS(F.DeltaTime / F.FadeOutTime, F.StepMax), 0.0f);
		s.CurrentFade = fade;
		if (fade > 0.0f)
			return;
	}
	s.Flags &= ~kFadeFlagFadedIn;
}

// BSFadeNode::OnVisible for the main camera: whether it goes on into the children.
bool FadeNodeOnVisible(inout FadeNodeState s, FadeRootStatic r, float3 c)
{
	const float amount = r.FadeAmount;
	if (F.FadesOn == 0)
		return true;
	if ((s.Flags & kFadeFlagSettled) != 0 && UnorderedEqual(F.One, amount) && UnorderedEqual(F.One, s.CurrentFade))
		return true;
	if (TypeOf(s) == 6 && F.Overridden == 0) {
		float distance;
		FadeValue(s, r, c, distance);
		if (F.LodUpdates == 0) {
			s.Flags |= kFadeFlagFadedIn;
			s.CurrentFade = 1.0f;
		}
		if ((s.Flags & kFadeFlagFadedIn) == 0) {
			if (SinceVisible(s) > 1) {
				s.CurrentFade = 1.0f;
				s.Flags |= kFadeFlagFadedIn;
			} else {
				precise float fade = MinSS(MinSS(F.DeltaTime / F.FadeInTime, F.StepMax) + s.CurrentFade, F.One);
				s.CurrentFade = fade;
				if (!(fade < F.One))
					s.Flags |= kFadeFlagFadedIn;
			}
		}
		s.LastVisible = F.Counter;
		return true;
	}
	FadeUpdate(s, r, c, amount);
	s.LastVisible = F.Counter;
	return s.CurrentFade > 0.0f && !UnorderedEqual(amount, 0.0f);
}

// The root's update for a frame its bound is in view: the tree height test, then the OnVisible of its class.
uint OnVisible(inout FadeNodeState s, FadeRootStatic r, float3 c)
{
	uint verdict = kFadeVerdictInView;
	const uint plan = r.Bits & kFadeRootPlanMask;
	if (plan == kFadeRootOther)
		return verdict | kFadeVerdictDrawn;
	if (plan == kFadeRootTree && c.z - F.TreeHeightBase > F.TreeHeightLimit)
		return verdict | kFadeVerdictAboveLimit;
	verdict |= kFadeVerdictServiced;
	if ((plan == kFadeRootLeaf || plan == kFadeRootTree) && F.LodUpdates != 0) {
		float distance;
		FadeValue(s, r, c, distance);
		LodStep(s, r, distance);
		s.LastVisible = F.Counter;
	}
	const bool drawn = FadeNodeOnVisible(s, r, c);
	if (plan == kFadeRootTree && (r.Bits & kFadeRootTreeLod) != 0) {
		s.Levels = (s.Levels & ~0xFF00u) | (((((s.Levels >> 8) & 0xAFu) | 0x20u)) << 8);
		s.Blend = 0.0f;
	}
	return verdict | (drawn ? kFadeVerdictDrawn : 0u);
}

// The cull's sphere test against the main camera's frustum: the depth segment's view-projection (camera translation folded
// in, so a world position projects directly), its planes normalised. NoNearPlane (the latch's cull flags, bit 9) drops the
// near plane, as BuildDraws does.
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

// The main camera's cull test for the root, as the list processes' Process1 (AE 0x140e28390) makes it before the node's
// OnVisible, through DCLF's portal programs (PortalPrograms.hlsli): the node's always-draw and preprocessed flags, its room's
// cull mode, the view planes, then its room's (or unbound space's) compound frustum.
static const uint kFadeRootAlwaysDraw = 1u << 20;
static const uint kFadeRootPreprocessed = 1u << 21;
static const uint kFadeRootPreprocessHidden = 1u << 22;

// The cull test of the root's program (kPortalNoProgram, or no programs: the frustum alone).
bool EngineInView(float3 a_centre, float a_radius, uint a_rootBits, uint a_program)
{
	if (ProgramsIndex == 0)
		return InView(a_centre, a_radius);
	ByteAddressBuffer programs = ResourceDescriptorHeap[ProgramsIndex];
	const int test = PortalTest(programs, a_program, a_centre, a_radius, (a_rootBits & kFadeRootAlwaysDraw) != 0u, (a_rootBits & kFadeRootPreprocessed) != 0u,
		(a_rootBits & kFadeRootPreprocessHidden) != 0u);
	return test < 0 ? InView(a_centre, a_radius) : test != 0;
}

// The fade write-back (drawcall-limit-fix.md, "The fade write-back"): the engine reads a stood-in root's node (the tree LOD's
// crossfade, the occlusion maps' cull, the next listing), which only this pass updates. Its milestones: 0 faded out, 1 fading,
// 2 faded in.
uint FadeMilestone(FadeNodeState a_state)
{
	return a_state.CurrentFade <= 0.0 ? 0u : (a_state.CurrentFade >= 1.0 ? 2u : 1u);
}

// A stood-in root whose milestone is not the one last reported appends an event, and counts it reported once it is in the
// list; one that does not fit tries again next frame (the count tells the host to grow the list). Any other root's node is
// the engine's own, so its milestone is what the node holds.
void ReportMilestone(uint a_index, FadeRootStatic a_root, FadeNodeState a_state)
{
	if (EventsIndex == 0 || ReportedIndex == 0)
		return;
	RWStructuredBuffer<uint2> reported = ResourceDescriptorHeap[ReportedIndex];
	uint2 last = reported[a_index];
	// A new listing seeds the state from the node (FadeState::ReadNode): the node holds that milestone.
	if (last.x != a_root.Generation)
		last = uint2(a_root.Generation, FadeMilestone(a_root.Initial));
	const uint milestone = FadeMilestone(a_state);
	if ((a_root.Bits & kFadeRootStoodIn) == 0 || last.y == milestone) {
		reported[a_index] = uint2(a_root.Generation, milestone);
		return;
	}
	RWStructuredBuffer<uint> events = ResourceDescriptorHeap[EventsIndex];
	events[1] = F.SceneFrame;  // the list's frame: the host applies the lists in frame order
	uint slot;
	InterlockedAdd(events[0], 1u, slot);
	if (slot >= EventCapacity) {
		reported[a_index] = last;
		return;
	}
	const uint at = kFadeEventHeaderWords + slot * 4u;
	events[at] = a_index;
	events[at + 1u] = a_root.Generation;
	events[at + 2u] = a_state.Flags;
	events[at + 3u] = asuint(a_state.CurrentFade);
	reported[a_index] = uint2(a_root.Generation, milestone);
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint index = dispatchID.x;
	StructuredBuffer<FadeFrame> frames = ResourceDescriptorHeap[FrameIndex];
	F = frames[0];
	// The dispatch covers every slot the buffers hold; the frame row's count ends it.
	if (index >= F.RootCount)
		return;
	StructuredBuffer<FadeRootStatic> roots = ResourceDescriptorHeap[RootsIndex];
	RWStructuredBuffer<FadeNodeState> states = ResourceDescriptorHeap[StatesIndex];
	const FadeRootStatic root = roots[index];
	RWStructuredBuffer<FadeNodeState> published = ResourceDescriptorHeap[(F.SceneFrame & 1u) != 0 ? OutIndex1 : OutIndex0];
	FadeNodeState state = states[index];
	if (root.Object == kNoObject || root.Generation == 0) {
		published[index] = state;
		return;
	}
	if (state.Generation != root.Generation) {
		state = root.Initial;
		state.Generation = root.Generation;
		state.Verdict = 0;
		state.Frame = 0;
	}
	// Once a frame, and only with a camera.
	if (state.Frame == F.SceneFrame || F.LodAdjust == 0.0f) {
		published[index] = state;
		return;
	}
	const FadeNodeState before = state;
	// The frame's placement rows (the latch's BuildDrawsLatch::placementsIndex): a root's centre is its member's fade node row.
	ByteAddressBuffer frameLatch = ResourceDescriptorHeap[LatchIndex];
	StructuredBuffer<PlacementRows> placements = ResourceDescriptorHeap[frameLatch.Load(LatchOffset + 252)];
	const float3 centre = placements[root.Object].Rows[kObjectFadeNodeRow].xyz;
	// The bound's radius as the engine reads it now: the member's sun entry node is its reference root, which is the fade
	// root (the placement row's sun entry, kept by the placements; a negative or unbounded radius: none, the listed radius instead).
	const float entryRadius = placements[root.Object].Rows[kObjectSunEntryRow].w;
	const float radius = entryRadius >= 0.0f && entryRadius < 1e30f ? entryRadius : root.Radius;
	state.Verdict = 0;
	// The animation job's update since the last cull (FUN_1402cff60 -> FUN_14147a160): an animated reference its cull did not
	// reach, from the camera and globals of that update; before this cull's test, as the engine's frame has it.
	// The word: the batch's scene frame (low 28 bits) and its count of updates (Records.h, FadeAnimatedWord).
	uint updates = 0;
	if (AnimatedIndex != 0) {
		StructuredBuffer<uint> stamps = ResourceDescriptorHeap[AnimatedIndex];
		const uint word = stamps[index];
		if ((word >> 4) == (F.SceneFrame & 0x0FFFFFFFu))
			updates = word & 0xFu;
	}
	const bool animated = updates != 0;
	if (animated) {
		const FadeFrame frame = F;
		F.Eye = F.AnimEye;
		F.LodAdjust = F.AnimLodAdjust;
		F.Counter = F.AnimCounter;
		F.DeltaTime = F.AnimDeltaTime;
		for (uint u = 0; u < updates; ++u)
			FadeUpdate(state, root, centre, root.FadeAmount);
		F = frame;
	}
	uint program = kPortalNoProgram;
	if (RootProgramsIndex != 0) {
		StructuredBuffer<uint> rootPrograms = ResourceDescriptorHeap[RootProgramsIndex];
		program = rootPrograms[index];
	}
	if (EngineInView(centre, radius, root.Bits, program))
		state.Verdict = OnVisible(state, root, centre);
	if (animated)
		state.Verdict |= kFadeVerdictAnimated | (updates << 5);
	state.Frame = F.SceneFrame;
	states[index] = state;
	published[index] = state;
	ReportMilestone(index, root, state);
	if (F.LogBase != 0xFFFFFFFFu && index >= F.LogBase && index - F.LogBase < 64u) {
		RWStructuredBuffer<FadeLogEntry> log = ResourceDescriptorHeap[LogIndex];
		FadeLogEntry entry;
		entry.Before = before;
		entry.After = state;
		entry.Centre = centre;
		entry.Root = index;
		log[index - F.LogBase] = entry;
	}
}
