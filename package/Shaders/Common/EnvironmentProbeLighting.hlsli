#ifndef ENVIRONMENT_PROBE_LIGHTING_HLSLI
#define ENVIRONMENT_PROBE_LIGHTING_HLSLI

#include "Common/EnvironmentProbe.hlsli"

namespace EnvironmentProbe
{
	static const uint AngularBins = 16;
	static const uint RadialBins = 24;
	static const uint LightWords = 32;  // All 1024 LLF lights, without per-cluster overflow.
	static const uint ClusterStride = LightWords + 1;

	uint LightClusterAddress(float2 pixel, float radius)
	{
		uint2 xy = min(uint2(pixel * (float(AngularBins) / TextureSize)), AngularBins - 1);
		uint z = min(uint(max(0.0, log2(max(radius, NearDistance) / NearDistance)) *
						  (RadialBins / log2(FarDistance / NearDistance))),
			RadialBins - 1);
		return (xy.x + AngularBins * (xy.y + AngularBins * z)) * ClusterStride;
	}
}
#endif
