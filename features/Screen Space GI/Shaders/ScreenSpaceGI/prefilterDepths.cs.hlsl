///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>         (\_/)
// Version:         (see XeGTAO.h)                                                                            (='.'=)
// Details:         https://github.com/GameTechDev/XeGTAO                                                     (")_(")
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "common.hlsli"

Texture2D<float> srcNDCDepth : register(t0);

RWTexture2D<lpfloat> outDepth0 : register(u0);
RWTexture2D<lpfloat> outDepth1 : register(u1);
RWTexture2D<lpfloat> outDepth2 : register(u2);
RWTexture2D<lpfloat> outDepth3 : register(u3);
RWTexture2D<lpfloat> outDepth4 : register(u4);

// This is also a good place to do non-linear depth conversion for cases where one wants the 'radius' (effectively the threshold between near-field and far-field GI),
// is required to be non-linear (i.e. very large outdoors environments).
lpfloat ClampDepth(float depth)
{
#ifdef USE_HALF_FLOAT_PRECISION
	return (lpfloat)clamp(depth, 0.0h, 65504.0h);
#else
	return clamp(depth, 0.0, 3.402823466e+38);
#endif
}

// weighted average depth filter
lpfloat DepthMIPFilter(lpfloat depth0, lpfloat depth1, lpfloat depth2, lpfloat depth3)
{
	lpfloat maxDepth = max(max(depth0, depth1), max(depth2, depth3));

	const lpfloat depthRangeScaleFactor = 0.75;  // found empirically :)
	const lpfloat effectRadius = depthRangeScaleFactor * (lpfloat)EffectRadius;
	const lpfloat falloffRange = (lpfloat)EffectFalloffRange * effectRadius;
	const lpfloat rcpFalloffRange = rcp(falloffRange);
	const lpfloat falloffFrom = (lpfloat)EffectRadius * ((lpfloat)1 - (lpfloat)EffectFalloffRange);
	const lpfloat falloffMul = -rcpFalloffRange;
	const lpfloat falloffAdd = falloffFrom * rcpFalloffRange + (lpfloat)1.0;

	lpfloat weight0 = saturate((maxDepth - depth0) * falloffMul + falloffAdd);
	lpfloat weight1 = saturate((maxDepth - depth1) * falloffMul + falloffAdd);
	lpfloat weight2 = saturate((maxDepth - depth2) * falloffMul + falloffAdd);
	lpfloat weight3 = saturate((maxDepth - depth3) * falloffMul + falloffAdd);

	lpfloat weightSum = weight0 + weight1 + weight2 + weight3;
	return (weight0 * depth0 + weight1 * depth1 + weight2 * depth2 + weight3 * depth3) / weightSum;
}

groupshared lpfloat g_scratchDepths[8][8];
[numthreads(8, 8, 1)] void main(uint2 dispatchThreadID
								: SV_DispatchThreadID, uint2 groupThreadID
								: SV_GroupThreadID) {
	// MIP 0
	const uint2 baseCoord = dispatchThreadID;
	const uint2 pixCoord = baseCoord * 2;
	const float2 uv = (pixCoord + .5) * RcpFrameDim * res_scale;
	const uint eyeIndex = GET_EYE_IDX(uv);

	float4 depths4 = srcNDCDepth.GatherRed(samplerPointClamp, uv, int2(1, 1));
	lpfloat depth0 = ClampDepth(ScreenToViewDepth(depths4.w));
	lpfloat depth1 = ClampDepth(ScreenToViewDepth(depths4.z));
	lpfloat depth2 = ClampDepth(ScreenToViewDepth(depths4.x));
	lpfloat depth3 = ClampDepth(ScreenToViewDepth(depths4.y));
	outDepth0[pixCoord + uint2(0, 0)] = (lpfloat)depth0;
	outDepth0[pixCoord + uint2(1, 0)] = (lpfloat)depth1;
	outDepth0[pixCoord + uint2(0, 1)] = (lpfloat)depth2;
	outDepth0[pixCoord + uint2(1, 1)] = (lpfloat)depth3;

	// MIP 1
	lpfloat dm1 = DepthMIPFilter(depth0, depth1, depth2, depth3);
	outDepth1[baseCoord] = (lpfloat)dm1;
	g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm1;

	GroupMemoryBarrierWithGroupSync();

	// MIP 2
	[branch] if (all((groupThreadID.xy % 2) == 0))
	{
		lpfloat inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
		lpfloat inTR = g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 0];
		lpfloat inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 1];
		lpfloat inBR = g_scratchDepths[groupThreadID.x + 1][groupThreadID.y + 1];

		lpfloat dm2 = DepthMIPFilter(inTL, inTR, inBL, inBR);
		outDepth2[baseCoord / 2] = (lpfloat)dm2;
		g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm2;
	}

	GroupMemoryBarrierWithGroupSync();

	// MIP 3
	[branch] if (all((groupThreadID.xy % 4) == 0))
	{
		lpfloat inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
		lpfloat inTR = g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 0];
		lpfloat inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 2];
		lpfloat inBR = g_scratchDepths[groupThreadID.x + 2][groupThreadID.y + 2];

		lpfloat dm3 = DepthMIPFilter(inTL, inTR, inBL, inBR);
		outDepth3[baseCoord / 4] = (lpfloat)dm3;
		g_scratchDepths[groupThreadID.x][groupThreadID.y] = dm3;
	}

	GroupMemoryBarrierWithGroupSync();

	// MIP 4
	[branch] if (all((groupThreadID.xy % 8) == 0))
	{
		lpfloat inTL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 0];
		lpfloat inTR = g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 0];
		lpfloat inBL = g_scratchDepths[groupThreadID.x + 0][groupThreadID.y + 4];
		lpfloat inBR = g_scratchDepths[groupThreadID.x + 4][groupThreadID.y + 4];

		lpfloat dm4 = DepthMIPFilter(inTL, inTR, inBL, inBR);
		outDepth4[baseCoord / 8] = (lpfloat)dm4;
		//g_scratchDepths[ groupThreadID.x ][ groupThreadID.y ] = dm4;
	}
}