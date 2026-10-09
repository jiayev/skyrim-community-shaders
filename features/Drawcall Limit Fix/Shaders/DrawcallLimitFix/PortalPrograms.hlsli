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
bool PortalViewTest(ByteAddressBuffer a_programs, float3 a_centre, float a_radius)
{
	const uint mask = a_programs.Load(96u);
	[unroll] for (uint p = 0; p < 6u; ++p) {
		if ((mask & (1u << p)) != 0u && PortalDistance(asfloat(a_programs.Load4(p * 16u)), a_centre) <= -a_radius)
			return false;
	}
	return true;
}

// BSCompoundFrustum::Process: type 2 accepts, 3 rejects; type 7 passes unless the bound is wholly outside an active plane of its
// set, type 8 unless it is wholly inside every one; either goes on to its record's next-if-true or next-if-false.
bool PortalProgramRun(ByteAddressBuffer a_programs, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, uint a_setsBase, float3 a_centre, float a_radius)
{
	const uint records = a_recordsBase + a_entry2.x * kPortalOpBytes;
	const uint sets = a_setsBase + a_entry2.y * kPortalSetBytes;
	uint op = a_entry.w;
	[loop] for (uint step = 0; step < 512u; ++step) {
		if (op >= a_entry.y)
			return true;
		const uint3 record = a_programs.Load3(records + op * kPortalOpBytes);
		if (record.x == 2u)
			return true;
		if (record.x == 3u)
			return false;
		bool result = false;
		if (record.x == 7u || record.x == 8u) {
			const uint set = op + 1u < a_entry.y ? a_programs.Load(records + (op + 1u) * kPortalOpBytes) : 0xFFFFFFFFu;
			if (set >= a_entry.z)
				return true;
			const uint base = sets + set * kPortalSetBytes;
			const uint mask = a_programs.Load(base + 96u);
			if (mask == 0u) {
				result = record.x == 7u;
			} else {
				uint p = 0;
				[loop] for (; p < 6u; ++p) {
					if ((mask & (1u << p)) == 0u)
						continue;
					const float d = PortalDistance(asfloat(a_programs.Load4(base + p * 16u)), a_centre);
					if (d <= -a_radius)
						break;
					if (record.x == 8u && d < a_radius)
						break;
				}
				result = record.x == 7u ? p == 6u : p != 6u;
			}
		}
		op = result ? record.y : record.z;
	}
	return true;
}

// 1 in view, 0 culled, -1 no program (the caller's frustum test). a_alwaysDraw, a_preprocessed, a_preprocessHidden: the node's
// flags 11, 12 and 20.
int PortalTest(ByteAddressBuffer a_programs, uint a_program, float3 a_centre, float a_radius, bool a_alwaysDraw, bool a_preprocessed, bool a_preprocessHidden)
{
	const uint count = a_programs.Load(kPortalSetBytes);
	if (a_program == kPortalNoProgram || a_program >= count)
		return -1;
	const uint directory = kPortalHeaderBytes + a_program * kPortalDirectoryBytes;
	const uint4 entry = a_programs.Load4(directory);         // flags, operators, sets, first operator
	const uint4 entry2 = a_programs.Load4(directory + 16u);  // first record, first set, cull mode, 0
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
	const bool view = PortalViewTest(a_programs, a_centre, a_radius);
	if (cullMode == 3u || entry.y == 0u)
		return view ? 1 : 0;
	if ((entry.x & kPortalSkipView) == 0u && !view)
		return 0;
	const uint recordsBase = kPortalHeaderBytes + count * kPortalDirectoryBytes;
	const uint setsBase = recordsBase + a_programs.Load(kPortalSetBytes + 4u) * kPortalOpBytes;
	return PortalProgramRun(a_programs, entry, entry2, recordsBase, setsBase, a_centre, a_radius) ? 1 : 0;
}

#endif
