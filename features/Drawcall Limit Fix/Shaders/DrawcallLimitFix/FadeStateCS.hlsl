// Drawcall Limit Fix: the fade roots' state on the GPU (drawcall-limit-fix.md, "Fades on the GPU"; Records.h, FadeRootStatic).
//
// The engine updates a BSFadeNode's fade and LOD state in its OnVisible, for the main camera's cull, whenever the node's bound
// passes the cull's frustum test. This pass does the same for every fade root a DCLF member draws under, from the depth
// segment's view-projection, before that segment's culling: one thread per root slot. A state row whose generation is not its
// static row's is new (a listed node) and starts from the node's values. The update is Scene/FadeState.cpp's port of the
// engine's functions, instruction for instruction; keep the two the same.

cbuffer FadeStateConstants : register(b0)
{
	uint RootsIndex;    // StructuredBuffer<FadeRootStatic>
	uint StatesIndex;   // RWStructuredBuffer<FadeNodeState>
	uint FrameIndex;    // StructuredBuffer<FadeFrame>, one row
	uint ObjectsIndex;  // StructuredBuffer of the object records (a root's centre: its member's fade node row)
	uint LatchIndex;    // ByteAddressBuffer: the depth segment's BuildDrawsLatch (its view-projection)
	uint LatchOffset;
	uint Count;         // root slots
	uint Frame;         // the scene frame
	uint LogIndex;      // RWStructuredBuffer<FadeLogEntry> (CS_DCLF_FADE_PARITY)
	uint LogBase;       // the first root logged this frame; ~0u: none
	// RWByteAddressBuffer: the write-back roots' changes (Records.h, FadeChange) after a 16-byte header whose first word counts
	// the appends; the capacity in changes; 1: every write-back root appends this frame (after an overflow).
	uint ChangesIndex;
	uint ChangeCapacity;
	uint WriteAll;
	uint Padding0;
	uint Padding1;
	uint Padding2;
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
};

struct FadeLogEntry
{
	FadeNodeState Before;
	FadeNodeState After;
	float3 Centre;
	uint Root;
};

struct ObjectRecordRows
{
	float4 Rows[16];
};

static const uint kFadeFlagFadedIn = 1u << 14;
static const uint kFadeFlagSettled = 1u << 15;
static const uint kFadeFlagLodInUpdate = 1u << 27;
static const uint kFadeVerdictInView = 1u << 0;
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
static const uint kFadeRootWriteBack = 1u << 19;
static const uint kFadeChangeHeaderBytes = 16;
static const uint kFadeChangeBytes = 64;
static const uint kObjectFadeNodeRow = 13;
static const uint kNoObject = 0xFFFFFFFFu;

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

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint index = dispatchID.x;
	if (index >= Count)
		return;
	StructuredBuffer<FadeRootStatic> roots = ResourceDescriptorHeap[RootsIndex];
	RWStructuredBuffer<FadeNodeState> states = ResourceDescriptorHeap[StatesIndex];
	StructuredBuffer<FadeFrame> frames = ResourceDescriptorHeap[FrameIndex];
	const FadeRootStatic root = roots[index];
	if (root.Object == kNoObject || root.Generation == 0)
		return;
	F = frames[0];
	FadeNodeState state = states[index];
	if (state.Generation != root.Generation) {
		state = root.Initial;
		state.Generation = root.Generation;
		state.Verdict = 0;
		state.Frame = 0;
	}
	// Once a frame, and only with a camera.
	if (state.Frame == Frame || F.LodAdjust == 0.0f)
		return;
	const FadeNodeState before = state;
	StructuredBuffer<ObjectRecordRows> objects = ResourceDescriptorHeap[ObjectsIndex];
	const float3 centre = objects[root.Object].Rows[kObjectFadeNodeRow].xyz;
	state.Verdict = 0;
	if (InView(centre, root.Radius))
		state.Verdict = OnVisible(state, root, centre);
	state.Frame = Frame;
	states[index] = state;
	// A write-back root's node follows (SceneStore::ApplyFadeChanges): what the engine's other readers see changed.
	if ((root.Bits & kFadeRootWriteBack) != 0 && ChangesIndex != 0) {
		const bool changed = before.CurrentFade != state.CurrentFade || ((before.Flags ^ state.Flags) & kFadeFlagFadedIn) != 0 ||
		                     ((before.Levels ^ state.Levels) & 0xFFFFu) != 0 || before.Blend != state.Blend || before.AmountFade != state.AmountFade;
		if (changed || WriteAll != 0) {
			RWByteAddressBuffer changes = ResourceDescriptorHeap[ChangesIndex];
			uint slot;
			changes.InterlockedAdd(0, 1, slot);
			if (slot < ChangeCapacity) {
				const uint offset = kFadeChangeHeaderBytes + slot * kFadeChangeBytes;
				changes.Store4(offset, uint4(index, 0, 0, 0));
				changes.Store4(offset + 16, uint4(state.Flags, asuint(state.CurrentFade), asuint(state.SnapRadius), asuint(state.LastVisible)));
				changes.Store4(offset + 32, uint4(asuint(state.AmountFade), asuint(state.Metric), asuint(state.PreviousMetric), asuint(state.Blend)));
				changes.Store4(offset + 48, uint4(state.Levels, state.Generation, state.Verdict, state.Frame));
			}
		}
	}
	if (LogBase != 0xFFFFFFFFu && index >= LogBase && index - LogBase < 64u) {
		RWStructuredBuffer<FadeLogEntry> log = ResourceDescriptorHeap[LogIndex];
		FadeLogEntry entry;
		entry.Before = before;
		entry.After = state;
		entry.Centre = centre;
		entry.Root = index;
		log[index - LogBase] = entry;
	}
}
