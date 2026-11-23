#ifndef PHYSICAL_SKY_SHADOW_VOLUME_HLSLI
#define PHYSICAL_SKY_SHADOW_VOLUME_HLSLI

#include "PhysicalSky/CloudGeometry.hlsli"

namespace CloudShadowVolume
{
	float2 GridCenter(float2 camera, float range, uint2 dimensions)
	{
		const float2 cellSize = range / float2(dimensions);
		return floor(camera / cellSize) * cellSize;
	}

	// Column 0 stores the height profile. Columns 1 onwards store the
	// complete sun columns in a plane perpendicular to the light.
	Texture2D<float4> MeanProfile : register(t69);

	void LightPlane(float3 lightDir, float range, out float3 across, out float3 up, out float2 size)
	{
		const float horizontal = length(lightDir.xy);
		across = horizontal > 1e-6 ? float3(-lightDir.y, lightDir.x, 0.0) / horizontal : float3(1, 0, 0);
		up = cross(lightDir, across);
		// At grazing angles the second axis is nearly vertical. Keep the
		// texels concentrated at receiver heights instead of covering sky.
		size = range * float2(2.0, 2.0 * abs(lightDir.z) + 0.25 * horizontal);
	}

	float2 ProjectedGridCenter(float3 camera, float3 across, float3 up, float2 size, uint2 dimensions)
	{
		const float2 cell = size / float2(dimensions);
		return floor(float2(dot(camera, across), dot(camera, up)) / cell) * cell;
	}

	float2 ProjectedTransmittance(float3 pos, float4 profile, float4 caster, float4 box, float2 projection,
		uint2 resolution, float fallback, out float tailWeight)
	{
		tailWeight = 0.0;
		if (caster.z <= 0.0)
			return float2(fallback, 1.0);
		float3 across, up;
		float2 size;
		LightPlane(caster.xyz, box.z, across, up, size);
		const float2 uv = (float2(dot(pos, across), dot(pos, up)) - projection) / size + 0.5;
		const float2 edge = min(uv, 1.0 - uv) * resolution;
		const float edgeWeight = smoothstep(0.5, 2.0, min(edge.x, edge.y));
		// The atlas stores the complete outgoing column from below the
		// clouds. The 3D volume still handles receivers inside the layer.
		const float altitude = max(WeatherCloudAltitude(pos, profile.z), CloudProfileAltitude(pos, profile.z));
		const float belowLayer = smoothstep(0.0, 40.0 / GAME_UNIT_TO_M, caster.w - altitude);
		// The local prefix can use the atlas tail if it starts inside the
		// box or directly below it. The two segments then meet at the exit.
		const bool startsUnderBox = all(abs(pos.xy - box.xy) < 0.5 * box.z);
		tailWeight = startsUnderBox ? edgeWeight : 0.0;
		[branch] if (edgeWeight == 0.0) return float2(fallback, 1.0);

		const float2 texel = clamp(uv * resolution - 0.5, 0.0, resolution - 1.0);
		const int2 p = int2(texel) + 1;
		const float2 w = frac(texel);
		const float3 a = MeanProfile.Load(int3(p, 0)).xyz;
		const float3 b = MeanProfile.Load(int3(p + int2(1, 0), 0)).xyz;
		const float3 c = MeanProfile.Load(int3(p + int2(0, 1), 0)).xyz;
		const float3 d = MeanProfile.Load(int3(p + 1, 0)).xyz;
		// A receiver can be above the global cloud base yet still be below
		// every bit of matter along its light ray, for example on a ridge.
		// Gate each texel before interpolation so the support remains smooth.
		const float4 distanceToCloud = float4(a.z, b.z, c.z, d.z) - dot(pos, caster.xyz);
		const float4 fullColumn = max(belowLayer, smoothstep(-20.0 / GAME_UNIT_TO_M, 20.0 / GAME_UNIT_TO_M, distanceToCloud));
		const float4 volume = lerp(fallback, float4(a.x, b.x, c.x, d.x), fullColumn);
		const float2 projected = lerp(lerp(float2(volume.x, a.y), float2(volume.y, b.y), w.x),
			lerp(float2(volume.z, c.y), float2(volume.w, d.y), w.x), w.y);
		return lerp(float2(fallback, 1.0), projected, float2(edgeWeight, tailWeight));
	}

	float2 FarTransmittance(float3 pos, float4 shadowSample, float4 profile, float4 caster, float4 box, out float localWeight)
	{
		localWeight = 0.0;
		if (caster.z > 0.0 && shadowSample.w > 0.0) {
			// The prefix is complete if the ray reaches the top of the box.
			// Use the actual box, which also encloses the world-space storms.
			const float exitDistance = max((profile.y - pos.z) / caster.z, 0.0);
			const float2 exitUv = (pos.xy + exitDistance * caster.xy - box.xy) / box.z + 0.5;
			const float2 edge = min(min(shadowSample.xy, 1.0 - shadowSample.xy), min(exitUv, 1.0 - exitUv));
			localWeight = smoothstep(0.0, 0.1, min(edge.x, edge.y));
		}
		if (localWeight * shadowSample.w >= 1.0)
			return 1.0;
		uint width, height;
		MeanProfile.GetDimensions(width, height);
		float mean = 1.0;
		if (pos.z < profile.y) {
			const float index = saturate((pos.z - profile.x) / max(profile.y - profile.x, 1.0)) * (height - 2u);
			const uint lo = (uint)index + 1u;
			mean = lerp(MeanProfile.Load(int3(0, lo, 0)).x,
				MeanProfile.Load(int3(0, min(lo + 1u, height - 1u), 0)).x, frac(index));
		}
		float tailWeight;
		const float2 transmission = ProjectedTransmittance(pos, profile, caster, box, MeanProfile.Load(int3(3, 0, 0)).xy,
			uint2(width, height) - 1u, mean, tailWeight);
		localWeight = max(localWeight, tailWeight);
		return transmission;
	}

	float EdgeWeight(float3 uvw, uint2 dimensions)
	{
		const float2 edge = min(uvw.xy, 1.0 - uvw.xy) * float2(dimensions);
		return smoothstep(0.0, 2.0, min(edge.x, edge.y));
	}

	float2 RayBoxInterval(float3 pos, float3 lightDir, float3 boundsMin, float3 boundsMax)
	{
		float tNear = 0.0;
		float tFar = 3.402823466e+38;
		[unroll] for (uint axis = 0; axis < 3; ++axis)
		{
			if (abs(lightDir[axis]) < 1e-8) {
				if (pos[axis] < boundsMin[axis] || pos[axis] > boundsMax[axis])
					return 0.0;
			} else {
				const float t0 = (boundsMin[axis] - pos[axis]) / lightDir[axis];
				const float t1 = (boundsMax[axis] - pos[axis]) / lightDir[axis];
				tNear = max(tNear, min(t0, t1));
				tFar = min(tFar, max(t0, t1));
			}
		}
		return float2(tNear, tFar);
	}

	// The volume stores transmittance toward the light. Outside
	// receivers must sample the ray's entry into the box, never its exit.
	float4 GetSample(float3 pos, float3 lightDir, float3 boundsMin, float3 boundsMax, uint2 dimensions)
	{
		const float2 interval = RayBoxInterval(pos, lightDir, boundsMin, boundsMax);
		if (interval.y <= interval.x)
			return -1.0;
		const float3 entry = saturate((pos + interval.x * lightDir - boundsMin) / (boundsMax - boundsMin));
		const float3 middle = saturate((pos + (0.5 * interval.x + 0.5 * interval.y) * lightDir - boundsMin) / (boundsMax - boundsMin));
		// Fade grazing paths at the projected footprint, including side-face entries.
		return float4(entry, max(EdgeWeight(entry, dimensions), EdgeWeight(middle, dimensions)));
	}

	// Only use this overload with a known linear-clamp sampler. Compute passes
	// own their samplers, unlike the material/depth/shadow-mask pixel shaders.
	float SampleTransmittance(Texture3D<float> volume, SamplerState linearClamp, float4 shadowSample, float outside, float tail)
	{
		const float3 uvw = shadowSample.xyz;
		if (any(uvw < 0.0) || any(uvw > 1.0))
			return outside;
		return lerp(outside, volume.SampleLevel(linearClamp, uvw, 0) * tail, shadowSample.w);
	}

	float SampleTransmittance(Texture3D<float> volume, float4 shadowSample, float outside, float tail)
	{
		const float3 uvw = shadowSample.xyz;
		if (any(uvw < 0.0) || any(uvw > 1.0))
			return outside;
		uint3 dims;
		volume.GetDimensions(dims.x, dims.y, dims.z);
		if (any(dims == 0))
			return outside;

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
		return lerp(outside, lerp(z0, z1, w.z) * tail, shadowSample.w);
	}
}
#endif
