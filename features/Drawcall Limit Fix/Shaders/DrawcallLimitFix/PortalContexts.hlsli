#ifndef DCLF_PORTAL_CONTEXTS_HLSLI
#define DCLF_PORTAL_CONTEXTS_HLSLI

#include "DrawcallLimitFix/PortalPrograms.hlsli"

// PortalContextAtlas.h: header {contexts, buckets, publicationLo, publicationHi}, followed by 64-byte context directories.
// A query's Program is a dense atlas bucket. Every task has one such bucket, so this lookup has uniform control flow.
bool PortalContextProgram(ByteAddressBuffer atlas, uint bucket, out uint base, out uint program, out uint frame)
{
	base = program = frame = 0;
	const uint contexts = atlas.Load(0);
	if (bucket >= atlas.Load(4) || contexts == 0)
		return false;
	// Upper bound handles contexts with no programs (and therefore repeated firstBucket values).
	uint first = 0, last = contexts;
	[loop] while (first < last)
	{
		const uint middle = first + (last - first) / 2;
		const uint firstBucket = atlas.Load(16u + middle * 64u + 28u);
		if (firstBucket <= bucket)
			first = middle + 1;
		else
			last = middle;
	}
	if (first == 0)
		return false;
	const uint directory = 16u + (first - 1u) * 64u;
	frame = atlas.Load(directory);
	base = atlas.Load(directory + 24u);
	program = bucket - atlas.Load(directory + 28u);
	return program < atlas.Load(base + kPortalSetBytes);
}

#endif
