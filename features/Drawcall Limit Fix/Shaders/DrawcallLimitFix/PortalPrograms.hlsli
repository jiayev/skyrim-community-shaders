#ifndef DCLF_PORTAL_PROGRAMS_HLSLI
#define DCLF_PORTAL_PROGRAMS_HLSLI

// The main camera's portal and occluder programs as DCLF's walk built them (Engine/PortalViews; Records.h, kPortal*), and the
// test the list processes' Process1 (AE 0x140e28390) makes with them before a node's OnVisible: the node's always-draw and
// preprocessed flags, the cull mode its room gives the process, the view planes, then BSCompoundFrustum::Process (0x140e320b0).
// PortalViews::Visible is the same test on the CPU, on the same words.

static const uint kPortalSetBytes = 112;
static const uint kPortalHeaderBytes = kPortalSetBytes + 16;
static const uint kPortalDirectoryBytes = 32;
static const uint kPortalOpBytes = 16;
static const uint kPortalNoProgram = 0xFFFFFFFFu;
static const uint kPortalReached = 1u << 0;
static const uint kPortalSkipView = 1u << 1;

// Every plane test's distance, in the engine's order: y, then x, then z, less the constant.
float PortalDistance(float4 a_plane, float3 a_point)
{
	return ((a_plane.y * a_point.y + a_plane.x * a_point.x) + a_point.z * a_plane.z) - a_plane.w;
}

// The process's sphere test on the view planes (FUN_140d3ff10): outside when the bound is wholly behind an active plane.
bool PortalViewTest(ByteAddressBuffer a_programs, uint a_base, float3 a_centre, float a_radius)
{
	const uint mask = a_programs.Load(a_base + 96u);
	[unroll] for (uint p = 0; p < 6u; ++p)
	{
		if ((mask & (1u << p)) != 0u && PortalDistance(asfloat(a_programs.Load4(a_base + p * 16u)), a_centre) <= -a_radius)
			return false;
	}
	return true;
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

// 1 in view, 0 culled, -1 no program (the caller's frustum test). a_alwaysDraw, a_preprocessed, a_preprocessHidden: the node's
// flags 11, 12 and 20.
int PortalTestAt(ByteAddressBuffer a_programs, uint a_base, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
	const uint count = a_programs.Load(a_base + kPortalSetBytes);
	if (a_program == kPortalNoProgram || a_program >= count)
		return -1;
	const uint directory = a_base + kPortalHeaderBytes + a_program * kPortalDirectoryBytes;
	const uint4 entry = a_programs.Load4(directory);         // flags, operators, sets, first operator
	const uint4 entry2 = a_programs.Load4(directory + 16u);  // first record, first set, cull mode, maximum path length
	if (a_radius == 0.0f && !a_alwaysDraw)
		return 0;
	// A room the walk did not reach is not culled into.
	if (a_program != 0u && (entry.x & kPortalReached) == 0u)
		return 0;
	const uint cullMode = entry2.z;
	if (cullMode == 2u)
		return 0;
	if (cullMode == 1u || a_alwaysDraw)
		return 1;
	if (a_preprocessed && cullMode != 4u)
		return a_preprocessHidden ? 0 : 1;
	const bool view = PortalViewTest(a_programs, a_base, a_centre, a_radius);
	if (cullMode == 3u || entry.y == 0u)
		return view ? 1 : 0;
	if ((entry.x & kPortalSkipView) == 0u && !view)
		return 0;
	const uint recordsBase = a_base + kPortalHeaderBytes + count * kPortalDirectoryBytes;
	const uint setsBase = recordsBase + a_programs.Load(a_base + kPortalSetBytes + 4u) * kPortalOpBytes;
	return PortalProgramRun(a_programs, entry, entry2, recordsBase, setsBase, a_centre, a_radius) ? 1 : 0;
}

// The standalone main-camera buffer is the same encoding at byte offset zero.
int PortalTest(ByteAddressBuffer a_programs, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
	return PortalTestAt(a_programs, 0u, a_program, a_centre, a_radius, a_alwaysDraw, a_preprocessed, a_preprocessHidden);
}

#endif
