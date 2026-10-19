#ifndef DCLF_PARABOLIC_PROGRAMS_HLSLI
#define DCLF_PARABOLIC_PROGRAMS_HLSLI

#include "DrawcallLimitFix/PortalPrograms.hlsli"

// {visible, hemisphere code, writes alphaGroupStopIndex, invalid producer input}.
// An untested visit does not invent a hemisphere code: native Process1 preserves the current process state.
uint ParabolicSphereMask(float4 bound, float4 sphere, float4 plane, bool bothSides)
{
	const precise float3 delta = bound.xyz - sphere.xyz;
	const precise float distance = sqrt((delta.x * delta.x + delta.y * delta.y) + delta.z * delta.z);
	const precise float outside = (distance - bound.w) - sphere.w;
	if (!(outside < 0.0f))
		return 0;
	const precise float side = ((bound.y * plane.y + bound.x * plane.x) + bound.z * plane.z) - plane.w;
	if (side - bound.w < 0.0f)
		return bothSides ? 3u : 1u;
	return side > 0.0f ? 1u : (bothSides ? 2u : 0u);
}

// Caller supplies the effective hierarchy mode and process parameters, not the room-walk camera's frustum.
// Room reachability and ancestor rejection must already be resolved. kPortalNoProgram explicitly means no compound frustum.
uint4 ParabolicTestAt(ByteAddressBuffer programs, uint base, uint program, uint mode, uint nodeFlags,
	bool ignorePreprocess, float4 bound, float4 sphere, float4 plane, bool bothSides)
{
	if (mode == 1u)
		return uint4(1, 0, 0, 0);
	if (mode == 2u)
		return 0;
	if ((nodeFlags & (1u << 12)) != 0u && !ignorePreprocess)
		return uint4((nodeFlags & (1u << 20)) == 0u ? 1u : 0u, 0, 0, 0);
	if ((nodeFlags & (1u << 11)) != 0u)
		return uint4(1, 0, 0, 0);
	const uint mask = ParabolicSphereMask(bound, sphere, plane, bothSides);
	if (mode != 3u && program != kPortalNoProgram) {
		const uint count = programs.Load(base + kPortalSetBytes);
		if (program >= count)
			return uint4(0, 0, 0, 1);
		const uint directory = base + kPortalHeaderBytes + program * kPortalDirectoryBytes;
		const uint4 entry = programs.Load4(directory), entry2 = programs.Load4(directory + 16u);
		if (entry.y != 0u) {
			const bool skipView = (entry.x & kPortalSkipView) != 0u;
			if (!skipView && mask == 0u)
				return 0;
			const uint records = base + kPortalHeaderBytes + count * kPortalDirectoryBytes;
			const uint sets = records + programs.Load(base + kPortalSetBytes + 4u) * kPortalOpBytes;
			if (!PortalProgramRun(programs, entry, entry2, records, sets, bound.xyz, bound.w))
				return 0;
		}
	}
	return uint4(mask != 0u ? 1u : 0u, mask, 1, 0);
}

#endif
