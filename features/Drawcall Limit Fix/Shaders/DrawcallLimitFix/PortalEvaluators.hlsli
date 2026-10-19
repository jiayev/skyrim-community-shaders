#ifndef DCLF_PORTAL_EVALUATORS_HLSLI
#define DCLF_PORTAL_EVALUATORS_HLSLI

// The portal bench's program evaluators (tests/DclfPortalBench), included by PortalPrograms.hlsli under DCLF_PORTAL_EXPERIMENTAL in
// place of the interpreter. Each reads the same encoding plus an auxiliary table the bench builds from it on the CPU (PortalAuxIndex,
// set by the entry point; 0: none), and must give PortalProgramRun's verdict bit for bit: the same distance arithmetic (PortalDistance,
// precise) and the same comparisons, only evaluated in another order or shape. A program the table does not cover, or a program of a
// context atlas (a_base != 0), takes the interpreter.
//
//   DCLF_PORTAL_FLAT        the hand-written baseline: each program's accepting paths, flattened on the CPU into terms of literals
//                           {set, kind, expected}; a bound is accepted when every literal of some term holds.
//   DCLF_PORTAL_PLANE_BITS  each program's distinct planes once into two bit masks (outside, not contained), then the program's
//                           records on the bits: a set's verdict is its active planes' bits.
//   (neither)               the interpreter (PortalProgramRun).

static uint PortalAuxIndex;

#ifdef DCLF_PORTAL_FLAT
// Header {programs, terms base, literals base, 0} (words); per program {first term, terms} (terms 0xFFFFFFFF: the interpreter); per
// term {first literal, literals}; a literal: set (bits 0-15), occluder (bit 16), the verdict the path needs (bit 17).
static const uint kFlatFallback = 0xFFFFFFFFu;
bool PortalFlatRun(ByteAddressBuffer a_programs, uint a_program, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, uint a_setsBase, float3 a_centre, float a_radius)
{
	ByteAddressBuffer aux = ResourceDescriptorHeap[PortalAuxIndex];
	const uint4 header = aux.Load4(0);
	if (a_program >= header.x)
		return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
	const uint2 terms = aux.Load2((4u + 2u * a_program) * 4u);
	if (terms.y == kFlatFallback)
		return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
	const uint sets = a_setsBase + a_entry2.y * kPortalSetBytes;
	[loop] for (uint t = 0; t < terms.y; ++t)
	{
		const uint2 term = aux.Load2((header.y + 2u * (terms.x + t)) * 4u);
		bool holds = true;
		[loop] for (uint l = 0; l < term.y && holds; ++l)
		{
			const uint literal = aux.Load((header.z + term.x + l) * 4u);
			const uint set = literal & 0xFFFFu;
			if (set >= a_entry.z)
				return false;
			const bool result = PortalPlaneSetTest(a_programs, sets + set * kPortalSetBytes, (literal & 0x10000u) != 0u ? 8u : 7u, a_centre, a_radius);
			holds = result == ((literal & 0x20000u) != 0u);
		}
		if (holds)
			return true;
	}
	return false;
}
#endif

#ifdef DCLF_PORTAL_PLANE_BITS
// Header {programs, planes base, set masks base, 0} (words); per program {first plane, planes, first set mask, 0} (planes 0xFFFFFFFF:
// the interpreter; at most 64); the planes (float4 {n, d}); per set of the encoding a uint2 mask of its active planes' slots.
static const uint kPlaneBitsFallback = 0xFFFFFFFFu;
bool PortalPlaneBitsRun(ByteAddressBuffer a_programs, uint a_program, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, float3 a_centre, float a_radius,
	uint a_setsBase)
{
	ByteAddressBuffer aux = ResourceDescriptorHeap[PortalAuxIndex];
	const uint4 header = aux.Load4(0);
	if (a_program >= header.x)
		return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
	const uint4 info = aux.Load4((4u + 4u * a_program) * 4u);
	if (info.y == kPlaneBitsFallback)
		return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
	uint2 outside = uint2(0, 0), notContained = uint2(0, 0);
	[loop] for (uint i = 0; i < info.y; ++i)
	{
		const precise float d = PortalDistance(asfloat(aux.Load4((header.y + 4u * (info.x + i)) * 4u)), a_centre);
		const uint bit = 1u << (i & 31u);
		const uint o = d <= -a_radius ? bit : 0u;
		const uint n = (d <= -a_radius || d < a_radius) ? bit : 0u;
		if (i < 32u) {
			outside.x |= o;
			notContained.x |= n;
		} else {
			outside.y |= o;
			notContained.y |= n;
		}
	}
	const uint records = a_recordsBase + a_entry2.x * kPortalOpBytes;
	uint op = a_entry.w;
	[loop] for (uint step = 0; step < a_entry2.w; ++step)
	{
		if (op >= a_entry.y)
			return false;
		const uint4 record = a_programs.Load4(records + op * kPortalOpBytes);
		if (record.x == 2u || record.x == 3u)
			return record.x == 2u;
		if ((record.x != 7u && record.x != 8u) || record.y >= a_entry.z)
			return false;
		const uint2 mask = aux.Load2((header.z + 2u * (info.z + record.y)) * 4u);
		const bool result = record.x == 7u ? !any((outside & mask) != 0u) : any((notContained & mask) != 0u);
		op = result ? record.z : record.w;
	}
	return false;
}
#endif

bool PortalProgramEvaluate(ByteAddressBuffer a_programs, uint a_base, uint a_program, uint4 a_entry, uint4 a_entry2, uint a_recordsBase, uint a_setsBase, float3 a_centre, float a_radius)
{
	if (a_base != 0u || PortalAuxIndex == 0u)
		return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
#if defined(DCLF_PORTAL_FLAT)
	return PortalFlatRun(a_programs, a_program, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
#elif defined(DCLF_PORTAL_PLANE_BITS)
	return PortalPlaneBitsRun(a_programs, a_program, a_entry, a_entry2, a_recordsBase, a_centre, a_radius, a_setsBase);
#else
	return PortalProgramRun(a_programs, a_entry, a_entry2, a_recordsBase, a_setsBase, a_centre, a_radius);
#endif
}

#endif
