#include "Common/EnvironmentProbeLighting.hlsli"
#include "LightLimitFix/Common.hlsli"

StructuredBuffer<Light> sourceLights : register(t0);
RWStructuredBuffer<uint> lightMasks : register(u0);

groupshared float3 coneAxis;
groupshared float2 coneAngle;  // cos, sin
groupshared float2 radialRange;
groupshared uint activeWords;

// Exact distance to a circular cone sector with inner/outer radii. The cone
// conservatively encloses the octahedral tile, including tiles crossing a fold.
bool Intersects(float3 p, float radius)
{
	float along = dot(p, coneAxis);
	float perpendicular = length(p - along * coneAxis);
	float distanceToSector;
	if (along >= 0 && perpendicular * coneAngle.x <= along * coneAngle.y) {
		float distance = length(p);
		distanceToSector = max(max(radialRange.x - distance, distance - radialRange.y), 0.0);
	} else {
		float nearestRadius = clamp(dot(float2(along, perpendicular), coneAngle), radialRange.x, radialRange.y);
		distanceToSector = length(float2(along, perpendicular) - nearestRadius * coneAngle);
	}
	// Conservative tolerance for world-position and cone construction rounding.
	return distanceToSector <= radius + 1e-5 * max(radialRange.y, length(p));
}

[numthreads(32, 1, 1)] void main(uint3 cluster : SV_GroupID, uint lane : SV_GroupIndex) {
	if (lane == 0) {
		float2 lo = float2(cluster.xy) / EnvironmentProbe::AngularBins;
		float2 hi = float2(cluster.xy + 1) / EnvironmentProbe::AngularBins;
		float3 a = EnvironmentProbe::Decode(lo);
		float3 b = EnvironmentProbe::Decode(float2(hi.x, lo.y));
		float3 c = EnvironmentProbe::Decode(hi);
		float3 d = EnvironmentProbe::Decode(float2(lo.x, hi.y));
		coneAxis = normalize(a + b + c + d);
		float cosine = min(min(dot(coneAxis, a), dot(coneAxis, b)), min(dot(coneAxis, c), dot(coneAxis, d)));
		coneAngle = float2(cosine, sqrt(saturate(1 - cosine * cosine)));
		// Even angular resolution aligns axes and fold intersections with tile corners.
		// On each side of a fold, rays are positive combinations of those corner rays.
		radialRange = EnvironmentProbe::NearDistance * exp2(
														   float2(cluster.z, cluster.z + 1) * log2(EnvironmentProbe::FarDistance / EnvironmentProbe::NearDistance) / EnvironmentProbe::RadialBins);
		activeWords = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	uint mask = 0;
	[loop] for (uint bit = 0; bit < 32 && lane * 32 + bit < EnvironmentProbe::LightCount; ++bit)
	{
		Light light = sourceLights[lane * 32 + bit];
		if (!(light.lightFlags & LightFlags::Disabled) &&
			Intersects(light.positionWS.xyz - EnvironmentProbe::LightOriginDelta, light.radius))
			mask |= 1u << bit;
	}
	uint address = (cluster.x + EnvironmentProbe::AngularBins * (cluster.y + EnvironmentProbe::AngularBins * cluster.z)) * EnvironmentProbe::ClusterStride;
	lightMasks[address + 1 + lane] = mask;
	if (mask != 0)
		InterlockedOr(activeWords, 1u << lane);
	GroupMemoryBarrierWithGroupSync();
	if (lane == 0)
		lightMasks[address] = activeWords;
}
