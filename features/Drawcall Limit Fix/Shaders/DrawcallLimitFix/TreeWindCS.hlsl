// Drawcall Limit Fix: tree wind on the GPU (drawcall-limit-fix.md, "Tree wind on the GPU"; Records.h, TreeStatic).
//
// The engine's tree manager (FUN_1404381e0, from Main::Update) advances each BSTreeNode's clock and takes its gust from it;
// BSLightingShader::SetupGeometry (case 0xc) turns the node into TreeParams and WindTimers. DCLF's members draw with what
// this pass makes of the same inputs, so nothing reads the tree nodes per frame.
//
// One thread per entry of the frame's wind buffer, on the compute queue in the Z-prepass epoch: entry 0 the nodeless trees',
// entry s + 1 tree slot s, whose clock row it first brings to this frame (a row whose generation is not its static row's is new,
// a listed node, and starts from the node's values). The entry is what SetupGeometry's case 0xc makes of the tree, which is the
// same for every member drawing under it (TreeParams, WindTimers with the distance and amplitude in zw), then the listing's
// generation. The buffers alternate by frame: this frame's
// is drawn by the next one (Common/DCLFObjects.hlsli, DCLFTreeWind), so nothing waits for this pass.

cbuffer TreeWindConstants : register(b0)
{
	uint TreesIndex;    // StructuredBuffer<TreeStatic>
	uint ClocksIndex;   // RWStructuredBuffer<TreeClock>
	uint FrameIndex;    // StructuredBuffer<TreeWindFrameRow>, one row (every commit's)
	uint Padding0;
	uint2 WindIndices;  // RWStructuredBuffer<float4>: the two wind buffers, written by the frame's parity
	uint2 Padding1;
}

// The frame's values (GpuLayouts.h, TreeWindFrameRow), a buffer rather than constants: the pass is prepared ahead of the
// commit that knows them. The dispatch covers every slot the buffers hold; the counts end it.
struct TreeWindFrameRow
{
	uint TreeCount;    // tree slots
	uint ObjectCount;  // members (unused)
	uint Frame;
	uint TreeSeedsIndex;  // T6b1a: StructuredBuffer<TreeStatic>, FrameValues' tree seeds (two rows a slot)
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
	uint RowPadding1;
	uint RowPadding2;
};

static TreeWindFrameRow W;

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
	uint SeedOdd;  // T6b1a: which of the slot's two seed rows holds its generation's
	uint Padding;
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
	precise float3 d = a_position - float3(W.CameraX, W.CameraY, W.CameraZ);
	return d.x * d.x + d.y * d.y + d.z * d.z;
}

[numthreads(64, 1, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	const uint entry = dispatchID.x;
	StructuredBuffer<TreeWindFrameRow> frames = ResourceDescriptorHeap[FrameIndex];
	W = frames[0];
	if (entry > W.TreeCount)
		return;
	// SetupGeometry's case 0xc (DeriveTreeAnim): with no node, distance 0, maximum amplitude 1, leaf frequency 1, no clock.
	float distance2 = 0.0f, amplitude = 1.0f, leafFrequency = 1.0f, timer = 0.0f, previousTimer = 0.0f;
	uint generation = kNodelessTree;
	if (entry != 0) {
		const uint slot = entry - 1;
		StructuredBuffer<TreeStatic> trees = ResourceDescriptorHeap[TreesIndex];
		RWStructuredBuffer<TreeClock> clocks = ResourceDescriptorHeap[ClocksIndex];
		TreeStatic tree = trees[slot];
		// T6b1a: the node's values are its seed's (Records.h, MergeTreeSeed: keep the two the same); none of the listing's generation yet:
		// no clock step and no entry this frame (a member draws with its shading row's until there is one).
		{
			if (W.TreeSeedsIndex == 0)
				return;
			StructuredBuffer<TreeStatic> seeds = ResourceDescriptorHeap[W.TreeSeedsIndex];
			uint count, stride;
			seeds.GetDimensions(count, stride);
			const uint at = 2u * slot + (tree.SeedOdd & 1u);
			if (at >= count || seeds[at].Generation != tree.Generation)
				return;
			const uint odd = tree.SeedOdd;
			tree = seeds[at];
			tree.SeedOdd = odd;
		}
		TreeClock clock = clocks[slot];
		if (clock.Generation != tree.Generation) {
			clock.Timer = tree.Timer;
			clock.PreviousTimer = tree.PreviousTimer;
			clock.Amplitude = tree.Amplitude;
			clock.Generation = tree.Generation;
			clock.Frame = W.Frame;
		} else if (clock.Frame != W.Frame) {
			// SetupGeometry kept last frame's timer as the previous one; the manager advances a node with a model, and takes
			// its gust only within its range.
			clock.PreviousTimer = clock.Timer;
			if (tree.Animated != 0) {
				clock.Timer = clock.Timer + W.DeltaTime;
				if (Distance2(tree.Position) < W.MaxDistance2)
					clock.Amplitude = Gust(W.WindSpeed * clock.Timer) * tree.ModelAmplitude;
			}
			clock.Frame = W.Frame;
		}
		clocks[slot] = clock;
		distance2 = Distance2(tree.Position);
		amplitude = clock.Amplitude;
		leafFrequency = tree.LeafFrequency;
		timer = clock.Timer;
		previousTimer = clock.PreviousTimer;
		generation = tree.Generation;
	}
	const float distance = entry != 0 ? FastSqrt(distance2) : 0.0f;
	precise float faded = (1.0f - (distance - W.FadeStart) / (W.FadeEnd - W.FadeStart)) * amplitude;
	faded = min(max(faded, 0.0f), amplitude);
	RWStructuredBuffer<float4> wind = ResourceDescriptorHeap[(W.Frame & 1u) != 0 ? WindIndices.y : WindIndices.x];
	wind[entry * 3] = float4(0.0f, W.WindMagnitude, faded, leafFrequency);
	wind[entry * 3 + 1] = float4(timer * W.TimerScale, previousTimer * W.TimerScale, distance2, amplitude);
	wind[entry * 3 + 2] = float4(asfloat(generation), 0.0f, 0.0f, 0.0f);
}
