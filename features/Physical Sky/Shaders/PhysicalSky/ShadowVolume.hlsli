#ifndef PHYSICAL_SKY_SHADOW_VOLUME_HLSLI
#define PHYSICAL_SKY_SHADOW_VOLUME_HLSLI

namespace CloudShadowVolume
{
	// The volume stores the remaining density column toward the light. Outside
	// receivers must sample the ray's entry into the box, never its exit.
	float3 GetSampleUvw(float3 pos, float3 lightDir, float3 boundsMin, float3 boundsMax)
	{
		float tNear = 0.0;
		float tFar = 3.402823466e+38;
		[unroll] for (uint axis = 0; axis < 3; ++axis)
		{
			if (abs(lightDir[axis]) < 1e-8) {
				if (pos[axis] < boundsMin[axis] || pos[axis] > boundsMax[axis])
					return -1.0;
			} else {
				const float t0 = (boundsMin[axis] - pos[axis]) / lightDir[axis];
				const float t1 = (boundsMax[axis] - pos[axis]) / lightDir[axis];
				tNear = max(tNear, min(t0, t1));
				tFar = min(tFar, max(t0, t1));
			}
		}
		if (tFar <= tNear)
			return -1.0;
		return saturate((pos + tNear * lightDir - boundsMin) / (boundsMax - boundsMin));
	}

	float SampleDensity(Texture3D<float> volume, float3 uvw)
	{
		if (any(uvw < 0.0) || any(uvw > 1.0))
			return 0.0;
		uint3 dims;
		volume.GetDimensions(dims.x, dims.y, dims.z);
		if (any(dims == 0))
			return 0.0;

		// Callers include material, depth and shadow-mask paths with different
		// sampler states. Explicit trilinear filtering keeps all of them clamped
		// and continuous, including receivers projected exactly onto a box face.
		const float3 coord = uvw * dims - 0.5;
		const int3 base = int3(floor(coord));
		const int3 lo = clamp(base, 0, int3(dims) - 1);
		const int3 hi = clamp(base + 1, 0, int3(dims) - 1);
		const float3 w = frac(coord);
		const float z0 = lerp(
			lerp(volume.Load(int4(lo.x, lo.y, lo.z, 0)), volume.Load(int4(hi.x, lo.y, lo.z, 0)), w.x),
			lerp(volume.Load(int4(lo.x, hi.y, lo.z, 0)), volume.Load(int4(hi.x, hi.y, lo.z, 0)), w.x), w.y);
		const float z1 = lerp(
			lerp(volume.Load(int4(lo.x, lo.y, hi.z, 0)), volume.Load(int4(hi.x, lo.y, hi.z, 0)), w.x),
			lerp(volume.Load(int4(lo.x, hi.y, hi.z, 0)), volume.Load(int4(hi.x, hi.y, hi.z, 0)), w.x), w.y);
		return lerp(z0, z1, w.z);
	}
}
#endif
