#ifndef __LOD_LANDSCAPE_DEPENDENCY_HLSL__
#define __LOD_LANDSCAPE_DEPENDENCY_HLSL__

// The game's depth pass (Utility) and colour pass (Lighting) must give a vertex the same depth, to the bit, since the colour pass tests EQUAL
// against it. FXC's output happens to match. DXC's matches only when it is precise, because the driver may contract each module differently.
#if defined(__hlsl_dx_compiler) && !defined(DCLF_ORG)
#	define DXC_PRECISE precise
#else
#	define DXC_PRECISE
#endif

namespace LodLandscape
{
	float4 AdjustLodLandscapeVertexPositionMS(float4 positionMS, row_major float4x4 world, float4 cellParams)
	{
		float4 positionWS = mul(world, positionMS);
		float worldXShift = positionWS.x - cellParams.x;
		float worldYShift = positionWS.y - cellParams.y;
		if ((abs(worldXShift) < cellParams.z) && (abs(worldYShift) < cellParams.w)) {
			positionMS.z -= (230 + positionWS.z / 1e9);
		}
		return positionMS;
	}

	float4 AdjustLodLandscapeVertexPositionCS(float4 positionCS)
	{
#if defined(__hlsl_dx_compiler) && !defined(DCLF_ORG)
		DXC_PRECISE float depthOffset = min(1, 1e-4 * max(0, positionCS.z - 70000)) * 0.5;
		DXC_PRECISE float depth = positionCS.z + depthOffset;
		positionCS.z = depth;
#else
		positionCS.z += min(1, 1e-4 * max(0, positionCS.z - 70000)) * 0.5;
#endif
		return positionCS;
	}
}

#endif  // __LOD_LANDSCAPE_DEPENDENCY_HLSL__