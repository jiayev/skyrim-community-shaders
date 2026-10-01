// Drawcall Limit Fix: tree wind on the GPU (drawcall-limit-fix.md, "Tree wind on the GPU"; Records.h, TreeStatic).
//
// The engine's tree manager (FUN_1404381e0, from Main::Update) advances each BSTreeNode's clock and takes its gust from it;
// BSLightingShader::SetupGeometry (case 0xc) turns the node into TreeParams and WindTimers. DCLF's members draw with what
// this pass makes of the same inputs, so nothing reads the tree nodes per frame.
//
// Mode 0, one thread per tree slot: the clock row brought to this frame, once per frame however many epochs run it. A
// row whose generation is not its static row's is new (a listed node) and starts from the node's values.
// Mode 1, one thread per member drawing under a tree: its record's TreeParams and WindTimers, written after the epoch's
// uploads, which carry the record as it was when the member joined.

cbuffer TreeWindConstants : register(b0)
{
	uint Mode;
	uint TreesIndex;    // StructuredBuffer<TreeStatic>
	uint ClocksIndex;   // RWStructuredBuffer<TreeClock> (mode 0), StructuredBuffer (mode 1)
	uint ListIndex;     // StructuredBuffer<uint2>: object slot, tree slot (mode 1)
	uint RecordsIndex;  // RWStructuredBuffer of the object records (mode 1)
	uint Count;         // tree slots (mode 0) or members (mode 1)
	uint Frame;
	uint TreeWord;      // the record's TreeParams, in 16-byte words (BindlessObject::tree)
	float DeltaTime;
	float CameraX;
	float CameraY;
	float CameraZ;
	float WindSpeed;
	float MaxDistance2;
	float WindMagnitude;
	float FadeStart;
	float FadeEnd;
	float TimerScale;
	uint Padding0;
	uint Padding1;
}

struct TreeStatic
{
	float3 Position;
	float LeafFrequency;
	float ModelAmplitude;
	float Timer;
	float PreviousTimer;
	float Amplitude;
	uint Generation;
	uint Animated;
	uint2 Padding;
};

struct TreeClock
{
	float Timer;
	float PreviousTimer;
	float Amplitude;
	uint Generation;
	uint Frame;
	uint3 Padding;
};

struct ObjectRecordWords
{
	uint4 Words[16];
};

static const uint kNodelessTree = 0xFFFFFFFEu;

// FUN_140438950(x, 3): a quarter of the sum of sin(k pi x) over k = 1, 3, 5, 7, each with the engine's argument reduction
// to [-pi, pi] and its odd polynomial (constants at 0x141846e64), summed in its order.
float Gust(float a_x)
{
	precise float4 v = a_x * float4(3.14159274f, 9.42477798f, 15.7079639f, 21.9911499f);
	precise float4 turns = v * 0.159154937f;
	v = v - round(turns) * 6.28318548f;
	precise float4 v2 = v * v;
	precise float4 v3 = v2 * v;
	precise float4 s = (-0.166521862f * v3 + v) + 0.00819991343f * (v3 * v2) + -0.000161475939f * (v3 * v2 * v2);
	s = s * 0.25f;
	return (s.w + s.z) + (s.y + s.x);
}

// The engine's square root of the squared distance (0x5f3759df and one Newton step; LightingDescriptors.cpp FastSqrt).
float FastSqrt(float a_value)
{
	const int bits = asint(a_value);
	const float estimate = asfloat(uint(0x5f3759df - (bits >> 1)));
	precise float result = (1.5f - a_value * 0.5f * estimate * estimate) * estimate * a_value;
	return result;
}

float Distance2(float3 a_position)
{
	precise float3 d = a_position - float3(CameraX, CameraY, CameraZ);
	return d.x * d.x + d.y * d.y + d.z * d.z;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint index = dispatchID.x;
	if (index >= Count)
		return;
	StructuredBuffer<TreeStatic> trees = ResourceDescriptorHeap[TreesIndex];
	if (Mode == 0) {
		RWStructuredBuffer<TreeClock> clocks = ResourceDescriptorHeap[ClocksIndex];
		const TreeStatic tree = trees[index];
		TreeClock clock = clocks[index];
		if (clock.Generation != tree.Generation) {
			clock.Timer = tree.Timer;
			clock.PreviousTimer = tree.PreviousTimer;
			clock.Amplitude = tree.Amplitude;
			clock.Generation = tree.Generation;
			clock.Frame = Frame;
		} else if (clock.Frame != Frame) {
			// SetupGeometry kept last frame's timer as the previous one; the manager advances a node with a model, and takes
			// its gust only within its range.
			clock.PreviousTimer = clock.Timer;
			if (tree.Animated != 0) {
				clock.Timer = clock.Timer + DeltaTime;
				if (Distance2(tree.Position) < MaxDistance2)
					clock.Amplitude = Gust(WindSpeed * clock.Timer) * tree.ModelAmplitude;
			}
			clock.Frame = Frame;
		}
		clocks[index] = clock;
		return;
	}
	StructuredBuffer<TreeClock> clocks = ResourceDescriptorHeap[ClocksIndex];
	StructuredBuffer<uint2> list = ResourceDescriptorHeap[ListIndex];
	RWStructuredBuffer<ObjectRecordWords> records = ResourceDescriptorHeap[RecordsIndex];
	const uint2 member = list[index];
	// SetupGeometry's case 0xc (DeriveTreeAnim): with no node, distance 0, maximum amplitude 1, leaf frequency 1, no clock.
	float distance2 = 0.0f, amplitude = 1.0f, leafFrequency = 1.0f, timer = 0.0f, previousTimer = 0.0f;
	if (member.y != kNodelessTree) {
		const TreeStatic tree = trees[member.y];
		const TreeClock clock = clocks[member.y];
		distance2 = Distance2(tree.Position);
		amplitude = clock.Amplitude;
		leafFrequency = tree.LeafFrequency;
		timer = clock.Timer;
		previousTimer = clock.PreviousTimer;
	}
	const float distance = member.y != kNodelessTree ? FastSqrt(distance2) : 0.0f;
	precise float faded = (1.0f - (distance - FadeStart) / (FadeEnd - FadeStart)) * amplitude;
	faded = min(max(faded, 0.0f), amplitude);
	records[member.x].Words[TreeWord] = asuint(float4(0.0f, WindMagnitude, faded, leafFrequency));
	records[member.x].Words[TreeWord + 1] = asuint(float4(timer * TimerScale, previousTimer * TimerScale, distance2, amplitude));
}
