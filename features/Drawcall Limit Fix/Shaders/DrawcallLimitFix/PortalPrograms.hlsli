#ifndef DCLF_PORTAL_PROGRAMS_HLSLI
#define DCLF_PORTAL_PROGRAMS_HLSLI

// The main camera's portal and occluder programs as DCLF's walk built them (Engine/PortalViews; Records.h, kPortal*), and the
// test the list processes' Process1 (AE 0x140e28390) makes with them before a node's OnVisible: the node's always-draw and
// preprocessed flags, the cull mode its room gives the process, the view planes, then BSCompoundFrustum::Process (0x140e320b0).
// PortalViews::Visible is the same test on the CPU, on the same words.

// AMD (CS_GPU_AMD, set by ComputeProgram on an AMD device) runs the uniform scan. On an RX 6600 (driver 2.0.353) the short-circuit
// interpreter below gave wrong verdicts: about 900 visible objects a frame were culled from the main view, with returns or with breaks
// in its loop. The uniform scan visits every record and agreed with the CPU's verdicts (set parity: 0 wrong frustum rejections).
#if defined(CS_GPU_AMD) && !defined(DCLF_PORTAL_UNIFORM_SCAN)
#	define DCLF_PORTAL_UNIFORM_SCAN 1
#endif

static const uint kPortalSetBytes = 112;
static const uint kPortalHeaderBytes = kPortalSetBytes + 16;
static const uint kPortalDirectoryBytes = 32;
static const uint kPortalOpBytes = 16;
static const uint kPortalNoProgram = 0xFFFFFFFFu;
static const uint kPortalReached = 1u << 0;
static const uint kPortalSkipView = 1u << 1;
// BuildDrawsLatch::portalProgramsIndex (GpuLayouts.h, kLatchPortalProgramsOffset): the depth segment's published programs.
static const uint kLatchPortalPrograms = 320;

// DCLF_PORTAL_TRACE (CS_DCLF_PORTAL_TRACE): PortalTestAt records how it decided, for PortalCapture's check against the CPU.
// x: the verdict (low byte), the step that decided it (bits 8-11, kPortalTrace*), the cull mode (12-15), the view test's result
// (16), the plane it rejected at (20-23, 15: none), the batch's frame stamp (24-31, PortalBatchCS); y: a hash of the view planes and mask as loaded; z: a hash of the plane count
// and the program's directory entry as loaded; w: the rejecting plane's distance (or the smallest active distance).
#ifdef DCLF_PORTAL_TRACE
static uint4 PortalTrace;
uint PortalTraceHash(uint a_hash, uint a_word)
{
	return (a_hash ^ a_word) * 16777619u;
}
#	define PORTAL_TRACE_STEP(a_step) PortalTrace.x = (PortalTrace.x & ~(15u << 8)) | ((a_step) << 8)
#else
#	define PORTAL_TRACE_STEP(a_step)
#endif

// Every plane test's distance, in the engine's order: y, then x, then z, less the constant.
float PortalDistance(float4 a_plane, float3 a_point)
{
	return ((a_plane.y * a_point.y + a_plane.x * a_point.x) + a_point.z * a_plane.z) - a_plane.w;
}

// The process's sphere test on the view planes (FUN_140d3ff10): outside when the bound is wholly behind an active plane.
bool PortalViewTest(ByteAddressBuffer a_programs, uint a_base, float3 a_centre, float a_radius)
{
	const uint mask = a_programs.Load(a_base + 96u);
#ifdef DCLF_PORTAL_TRACE
	// The same test, every plane loaded and hashed, the first rejecting plane kept.
	uint hash = 2166136261u, rejected = 15u;
	float nearest = 3.4e38f;
	[unroll] for (uint p = 0; p < 6u; ++p)
	{
		const uint4 words = a_programs.Load4(a_base + p * 16u);
		hash = PortalTraceHash(PortalTraceHash(PortalTraceHash(PortalTraceHash(hash, words.x), words.y), words.z), words.w);
		const float d = PortalDistance(asfloat(words), a_centre);
		if ((mask & (1u << p)) != 0u && rejected == 15u) {
			if (d <= -a_radius) {
				rejected = p;
				nearest = d;
			} else
				nearest = min(nearest, d);
		}
	}
	PortalTrace.y = PortalTraceHash(hash, mask);
	PortalTrace.x |= (rejected == 15u ? 1u << 16 : 0u) | (rejected << 20);
	PortalTrace.w = asuint(nearest);
	return rejected == 15u;
#else
	[unroll] for (uint p = 0; p < 6u; ++p)
	{
		if ((mask & (1u << p)) != 0u && PortalDistance(asfloat(a_programs.Load4(a_base + p * 16u)), a_centre) <= -a_radius)
			return false;
	}
	return true;
#endif
}

// Both bytecode operations share one fixed six-plane calculation. No per-plane early exits.
bool PortalPlaneSetTest(ByteAddressBuffer a_programs, uint a_base, uint a_kind, float3 a_centre, float a_radius)
{
	const uint mask = a_programs.Load(a_base + 96u);
	bool outside = false;
	bool notContained = false;
	[unroll] for (uint p = 0; p < 6u; ++p)
	{
		const bool active = (mask & (1u << p)) != 0u;
		const precise float d = PortalDistance(asfloat(a_programs.Load4(a_base + p * 16u)), a_centre);
		outside = outside || (active && d <= -a_radius);
		notContained = notContained || (active && (d <= -a_radius || d < a_radius));
	}
	return a_kind == 7u ? !outside : notContained;
}

// CPU-validated, forward-only compact instructions: {kind, set, true target, false target}.
// The diagnostic scan visits every record in uniform order; the default follows each lane's short-circuit path.
bool PortalProgramRun(ByteAddressBuffer a_programs, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, uint a_setsBase, float3 a_centre, float a_radius)
{
	const uint records = a_recordsBase + a_entry2.x * kPortalOpBytes;
	const uint sets = a_setsBase + a_entry2.y * kPortalSetBytes;
	uint op = a_entry.w;
#ifdef DCLF_PORTAL_UNIFORM_SCAN
	bool accepted = false;
	[loop] for (uint i = 0; i < a_entry.y; ++i)
	{
		const uint4 record = a_programs.Load4(records + i * kPortalOpBytes);
		if (record.x == 2u || record.x == 3u) {
			accepted = accepted || (op == i && record.x == 2u);
		} else {
			const bool result = PortalPlaneSetTest(a_programs, sets + record.y * kPortalSetBytes, record.x, a_centre, a_radius);
			op = op == i ? (result ? record.z : record.w) : op;
		}
	}
	return accepted;
#else
	[loop] for (uint step = 0; step < a_entry2.w; ++step)
	{
		// A producer invariant failure must never silently accept a bound.
		if (op >= a_entry.y)
			return false;
		const uint4 record = a_programs.Load4(records + op * kPortalOpBytes);
		if (record.x == 2u || record.x == 3u)
			return record.x == 2u;
		if ((record.x != 7u && record.x != 8u) || record.y >= a_entry.z)
			return false;
		const bool result = PortalPlaneSetTest(a_programs, sets + record.y * kPortalSetBytes, record.x, a_centre, a_radius);
		op = result ? record.z : record.w;
	}
	return false;
#endif
}

// The program's evaluation for PortalTestAt: the interpreter, or (DCLF_PORTAL_EXPERIMENTAL: the portal bench's variants,
// tests/DclfPortalBench) an evaluator of PortalEvaluators.hlsli over the same words and its auxiliary tables.
#ifdef DCLF_PORTAL_EXPERIMENTAL
#	include "DrawcallLimitFix/PortalEvaluators.hlsli"
#else
bool PortalProgramEvaluate(ByteAddressBuffer a_programs, uint a_base, uint a_program, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, uint a_setsBase, float3 a_centre, float a_radius)
{
	return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
}
#endif

// 1 in view, 0 culled, -1 no program (the caller's frustum test). a_alwaysDraw, a_preprocessed, a_preprocessHidden: the node's
// flags 11, 12 and 20.
// The trace's steps (PortalTrace.x bits 8-11).
static const uint kPortalTraceNoProgram = 1;
static const uint kPortalTraceNoRadius = 2;
static const uint kPortalTraceUnreached = 3;
static const uint kPortalTraceCullMode = 4;      // cull mode 1 or 2, or always drawn
static const uint kPortalTracePreprocessed = 5;
static const uint kPortalTraceViewOnly = 6;      // cull mode 3, or no operators
static const uint kPortalTraceViewRejected = 7;  // ahead of the operators
static const uint kPortalTraceOperators = 8;

int PortalTestAtSteps(ByteAddressBuffer a_programs, uint a_base, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
	const uint count = a_programs.Load(a_base + kPortalSetBytes);
	PORTAL_TRACE_STEP(kPortalTraceNoProgram);
	// One unsigned test covers kPortalNoProgram too (a count is far below it). AMD's compiler (RX 6600, driver 26.10.1) miscompiled
	// "a_program == kPortalNoProgram || a_program >= count": past this return it took a_program to be kPortalNoProgram, so every
	// lane read the directory entry at 128 + 0xFFFFFFFF * 32 = byte 96 (the mask and the counts) and skipped the room test below. The
	// SPIR-V was right; the ISA loaded the entry from constant offsets 0x60, 0x64 and 0x78 (CS_DCLF_PORTAL_TRACE found it).
	if (a_program >= count)
		return -1;
	const uint directory = a_base + kPortalHeaderBytes + a_program * kPortalDirectoryBytes;
	const uint4 entry = a_programs.Load4(directory);         // flags, operators, sets, first operator
	const uint4 entry2 = a_programs.Load4(directory + 16u);  // first record, first set, cull mode, maximum path length
#ifdef DCLF_PORTAL_TRACE
	uint hash = PortalTraceHash(2166136261u, count);
	hash = PortalTraceHash(PortalTraceHash(PortalTraceHash(PortalTraceHash(hash, entry.x), entry.y), entry.z), entry.w);
	PortalTrace.z = PortalTraceHash(PortalTraceHash(PortalTraceHash(PortalTraceHash(hash, entry2.x), entry2.y), entry2.z), entry2.w);
	PortalTrace.x |= min(entry2.z, 15u) << 12;
#endif
	PORTAL_TRACE_STEP(kPortalTraceNoRadius);
	if (a_radius == 0.0f && !a_alwaysDraw)
		return 0;
	// A room the walk did not reach is not culled into.
	PORTAL_TRACE_STEP(kPortalTraceUnreached);
	if (a_program != 0u && (entry.x & kPortalReached) == 0u)
		return 0;
	const uint cullMode = entry2.z;
	PORTAL_TRACE_STEP(kPortalTraceCullMode);
	if (cullMode == 2u)
		return 0;
	if (cullMode == 1u || a_alwaysDraw)
		return 1;
	PORTAL_TRACE_STEP(kPortalTracePreprocessed);
	if (a_preprocessed && cullMode != 4u)
		return a_preprocessHidden ? 0 : 1;
	const bool view = PortalViewTest(a_programs, a_base, a_centre, a_radius);
	PORTAL_TRACE_STEP(kPortalTraceViewOnly);
	if (cullMode == 3u || entry.y == 0u)
		return view ? 1 : 0;
	PORTAL_TRACE_STEP(kPortalTraceViewRejected);
	if ((entry.x & kPortalSkipView) == 0u && !view)
		return 0;
	PORTAL_TRACE_STEP(kPortalTraceOperators);
	const uint recordsBase = a_base + kPortalHeaderBytes + count * kPortalDirectoryBytes;
	const uint setsBase = recordsBase + a_programs.Load(a_base + kPortalSetBytes + 4u) * kPortalOpBytes;
	return PortalProgramEvaluate(a_programs, a_base, a_program, entry, entry2, recordsBase, setsBase, a_centre, a_radius) ? 1 : 0;
}
#undef PORTAL_TRACE_STEP

int PortalTestAt(ByteAddressBuffer a_programs, uint a_base, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
#ifdef DCLF_PORTAL_TRACE
	PortalTrace = uint4(0u, 0u, 0u, 0u);
	const int verdict = PortalTestAtSteps(a_programs, a_base, a_program, a_centre, a_radius, a_alwaysDraw, a_preprocessed, a_preprocessHidden);
	PortalTrace.x |= asuint(verdict) & 0xFFu;
	return verdict;
#else
	return PortalTestAtSteps(a_programs, a_base, a_program, a_centre, a_radius, a_alwaysDraw, a_preprocessed, a_preprocessHidden);
#endif
}

// The standalone main-camera buffer is the same encoding at byte offset zero.
int PortalTest(ByteAddressBuffer a_programs, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
	return PortalTestAt(a_programs, 0u, a_program, a_centre, a_radius, a_alwaysDraw, a_preprocessed, a_preprocessHidden);
}

// A portal-strict point light's per-object gate (Portals/StrictGates.h): the object's gate (the ObjectGates column, by object slot), that
// gate's verdict for the light's context in the frame's atlas (PortalContextAtlas.h), and the bound in the verdict's room frustum. The
// verdicts' words: header {gates, contexts, atlas identity low, high, 0, 0, 0, 0}, then per gate per context a verdict: kStrictGateClosed,
// kPortalNoProgram (the sphere alone, which the view's own culling is) or the program. A publication of another atlas, or a context it
// does not hold, gates nothing.
static const uint kStrictGateClosed = 0xFFFFFFFEu;
static const uint kStrictGateHeaderBytes = 32;
static const uint kContextEntryBytes = 64;

// Process1's compound test on a strict light's room program: no view planes (a point process has none), operators or none.
bool StrictProgramPass(ByteAddressBuffer a_atlas, uint a_base, uint a_program, float3 a_centre, float a_radius)
{
	const uint count = a_atlas.Load(a_base + kPortalSetBytes);
	if (a_program >= count)
		return true;
	const uint directory = a_base + kPortalHeaderBytes + a_program * kPortalDirectoryBytes;
	const uint4 entry = a_atlas.Load4(directory);
	const uint4 entry2 = a_atlas.Load4(directory + 16u);
	if (entry.y == 0u)
		return true;
	const uint recordsBase = a_base + kPortalHeaderBytes + count * kPortalDirectoryBytes;
	const uint setsBase = recordsBase + a_atlas.Load(a_base + kPortalSetBytes + 4u) * kPortalOpBytes;
	return PortalProgramRun(a_atlas, entry, entry2, recordsBase, setsBase, a_centre, a_radius);
}

// Whether a strict light (a_context: its atlas entry) culls object a_object of bound (a_centre, a_radius).
bool StrictGateRejects(uint a_gatesIndex, uint a_atlasIndex, uint a_objectGatesIndex, uint a_context, uint a_object, float3 a_centre, float a_radius)
{
	if (a_gatesIndex == 0u || a_atlasIndex == 0u || a_objectGatesIndex == 0u)
		return false;
	ByteAddressBuffer gates = ResourceDescriptorHeap[a_gatesIndex];
	ByteAddressBuffer atlas = ResourceDescriptorHeap[a_atlasIndex];
	const uint4 head = gates.Load4(0);  // gates, contexts, atlas identity low, high
	const uint4 atlasHead = atlas.Load4(0);  // contexts, buckets, identity low, high
	if (head.z != atlasHead.z || head.w != atlasHead.w || a_context >= head.y || a_context >= atlasHead.x || head.x == 0u)
		return false;
	StructuredBuffer<uint> column = ResourceDescriptorHeap[a_objectGatesIndex];
	uint objects, stride;
	column.GetDimensions(objects, stride);
	uint gate = a_object < objects ? column[a_object] : 0u;
	gate = gate < head.x ? gate : 0u;
	const uint verdict = gates.Load(kStrictGateHeaderBytes + (gate * head.y + a_context) * 4u);
	if (verdict == kStrictGateClosed)
		return true;
	if (verdict == kPortalNoProgram)
		return false;
	// ContextEntry::programsBase (absolute bytes into the atlas).
	const uint base = atlas.Load(16u + a_context * kContextEntryBytes + 24u);
	return !StrictProgramPass(atlas, base, verdict, a_centre, a_radius);
}

#endif
