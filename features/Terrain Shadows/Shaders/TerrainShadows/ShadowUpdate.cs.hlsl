Texture2D<float> TexHeight : register(t0);
RWTexture2D<float2> RWTexShadowHeights : register(u0);

cbuffer ShadowUpdateCB : register(b0)
{
	float2 LightPxDir : packoffset(c0.x);   // direction on which light descends, from one pixel to next via dda
	float2 LightDeltaZ : packoffset(c0.z);  // per lightUVDir, normalised, [upper, lower] penumbra, should be negative
	uint StartPxCoord : packoffset(c1.x);
	float2 PxSize : packoffset(c1.y);
	float BlendWeight : packoffset(c1.w);
	float2 PosRange : packoffset(c2.x);
	float2 ZRange : packoffset(c2.z);
}

float2 GetInterpolatedHeightRW(float2 pxCoord, bool isVertical)
{
	uint2 dims;
	RWTexShadowHeights.GetDimensions(dims.x, dims.y);

	int2 lerpPxCoordA = int2(floor(pxCoord - .5 * float2(isVertical, !isVertical)));
	int2 lerpPxCoordB = lerpPxCoordA + int2(isVertical, !isVertical);
	lerpPxCoordA = clamp(lerpPxCoordA, int2(0, 0), int2(dims) - 1);
	lerpPxCoordB = clamp(lerpPxCoordB, int2(0, 0), int2(dims) - 1);
	float2 heightA = RWTexShadowHeights[lerpPxCoordA];
	float2 heightB = RWTexShadowHeights[lerpPxCoordB];

	return lerp(heightA, heightB, frac((isVertical ? pxCoord.x : pxCoord.y) - .5));
}

#define NTHREADS 128
groupshared float2 g_shadowHeight[NTHREADS];

// Offsets can span more than one dimension on small heightmaps, so wrap by modulo rather than a single step.
uint GetWrappedCoord(int coord, uint dimension)
{
	uint magnitude = uint(abs(coord)) % dimension;
	return coord < 0 ? (dimension - magnitude) % dimension : magnitude;
}

[numthreads(NTHREADS, 1, 1)] void main(const uint gtid : SV_GroupThreadID, const uint gid : SV_GroupID) {
	uint2 dims;
	TexHeight.GetDimensions(dims.x, dims.y);

	bool isVertical = abs(LightPxDir.y) > abs(LightPxDir.x);
	int majorStep = (isVertical ? LightPxDir.y : LightPxDir.x) > 0.0 ? 1 : -1;
	int majorPxCoord = int(StartPxCoord) + int(gtid) * majorStep;
	uint majorDimension = isVertical ? dims.y : dims.x;
	uint minorDimension = isVertical ? dims.x : dims.y;
	bool isValid = majorPxCoord >= 0 && majorPxCoord < int(majorDimension);
	float minorDirection = isVertical ? LightPxDir.x : LightPxDir.y;
	float minorOffset = 0.5 + gtid * minorDirection;
	int rawMinorPxCoord = int(gid) + int(floor(minorOffset));
	uint minorPxCoord = GetWrappedCoord(rawMinorPxCoord, minorDimension);
	uint2 outputPxCoord = isVertical ? uint2(minorPxCoord, majorPxCoord) : uint2(majorPxCoord, minorPxCoord);
	float rayWrap = floor(float(rawMinorPxCoord) / minorDimension);

	float2 pastHeights = 0.0;
	if (isValid) {
		if (BlendWeight < 1.0)
			pastHeights = RWTexShadowHeights[outputPxCoord];

		float terrainHeight = lerp(PosRange.x, PosRange.y, TexHeight[outputPxCoord]);
		float2 heights = ((terrainHeight - ZRange.x) / (ZRange.y - ZRange.x)).xx;

		// fetch last dispatch
		int previousMajorCoord = majorPxCoord - majorStep;
		float previousMinorCoord = gid + 0.5 - minorDirection;
		if (gtid == 0 && previousMajorCoord >= 0 && previousMajorCoord < int(majorDimension) && previousMinorCoord >= 0.0 && previousMinorCoord < minorDimension) {
			float2 previousPxCoord = isVertical ? float2(previousMinorCoord, previousMajorCoord + 0.5) : float2(previousMajorCoord + 0.5, previousMinorCoord);
			float2 sampleHeights = GetInterpolatedHeightRW(previousPxCoord, isVertical) + LightDeltaZ;
			heights = max(heights, sampleHeights);
		}

		g_shadowHeight[gtid] = heights;
	}

	GroupMemoryBarrierWithGroupSync();

	// simple parallel scan
	[unroll] for (uint offset = 1; offset < NTHREADS; offset <<= 1)
	{
		bool combineHeights = false;
		float2 currentHeights = 0.0;
		float2 sampleHeights = 0.0;
		if (isValid && gtid >= offset) {
			int previousMinorPxCoord = int(gid) + int(floor(0.5 + (gtid - offset) * minorDirection));
			if (floor(float(previousMinorPxCoord) / minorDimension) == rayWrap) {
				combineHeights = true;
				currentHeights = g_shadowHeight[gtid];
				sampleHeights = g_shadowHeight[gtid - offset] + LightDeltaZ * offset;
			}
		}
		GroupMemoryBarrierWithGroupSync();
		if (combineHeights) {
			g_shadowHeight[gtid] = max(currentHeights, sampleHeights);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// save
	if (isValid) {
		RWTexShadowHeights[outputPxCoord] = lerp(pastHeights, g_shadowHeight[gtid], BlendWeight);
	}
}
