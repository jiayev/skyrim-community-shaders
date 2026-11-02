#ifndef ENVIRONMENT_PROBE_HIERARCHY_HLSLI
#define ENVIRONMENT_PROBE_HIERARCHY_HLSLI

#include "Common/EnvironmentProbe.hlsli"
#include "Common/EnvironmentProbeSampling.hlsli"
#ifdef PROBE_TRACE_VISIBILITY
#	include "Common/EnvironmentProbeMiss.hlsli"
#endif

// Per-octant min/max projective depth. Original radial depth remains the leaf
// intersection representation; these conservative bounds only reject work.
Texture2DArray<float2> ProbeDepthHierarchy : register(t10);

float3 ProbeFaceSign(uint face)
{
	return float3((face & 1u) ? -1 : 1, (face & 2u) ? -1 : 1, (face & 4u) ? -1 : 1);
}

float2 ProbeFaceNumerator(float3 p, float3 sign, uint face)
{
	float2 q = sign.z > 0 ? p.xy : p.xy + sign.xy * sign.z * p.z;
	return q + float2((face & 1u) != 0, (face & 2u) != 0) * dot(sign, p);
}

bool ProbeClipOctant(float3 origin, float3 direction, float3 sign, inout float begin, inout float end)
{
	float3 p = origin * sign;
	float3 d = direction * sign;
	[unroll] for (uint axis = 0; axis < 3; ++axis)
	{
		if (abs(d[axis]) < 1e-8) {
			if (p[axis] < 0)
				return false;
		} else {
			float t = -p[axis] / d[axis];
			if (d[axis] > 0)
				begin = max(begin, t);
			else
				end = min(end, t);
		}
	}
	return begin < end;
}

// The sign of each projected derivative is constant inside the octant.
// Solve the boundary in world-ray distance, not by interpolating octahedral UV.
float ProbeCellExit(float2 numerator, float2 slope, float w, float dw, float2 velocity,
	int2 cell, uint size, float end, out float2 boundary, out uint crossed)
{
	boundary = (cell + float2(velocity.x > 0, velocity.y > 0)) / size;
	crossed = 0;
	float2 divisor = slope - boundary * dw;
	float2 t = (boundary * w - numerator) / divisor;
	// A boundary beyond the project's asymptote has a solution on the other
	// side of w=0. It is not a future cell boundary inside this octant.
	float2 boundaryW = w + dw * t;
	if (velocity.x != 0 && boundaryW.x > 0 && isfinite(t.x) && t.x <= end) {
		end = t.x;
		crossed = 1;
	}
	if (velocity.y != 0 && boundaryW.y > 0 && isfinite(t.y) && t.y <= end) {
		crossed = t.y == end ? crossed | 2u : 2u;
		end = t.y;
	}
	return end;
}

uint ProbeSelectFace(float3 position)
{
	uint face = EnvironmentProbe::GetFace(position);
	[loop] for (uint i = 0; i < 3; ++i)
	{
		uint nextFace = EnvironmentProbe::GetFace(position - ProbeOriginOffsets[face].xyz);
		if (nextFace == face)
			break;
		face = nextFace;
	}
	return face;
}

bool ProbeNextInterval(float3 origin, float3 direction, float begin, float end, uint visitedFaces,
	out uint face, out float first, out float last)
{
	face = 0;
	first = end;
	last = end;
	[loop] for (uint candidateFace = 0; candidateFace < 8; ++candidateFace)
	{
		if ((visitedFaces & (1u << candidateFace)) != 0)
			continue;
		float candidateFirst = begin, candidateLast = end;
		if (!ProbeClipOctant(origin - ProbeOriginOffsets[candidateFace].xyz, direction,
				ProbeFaceSign(candidateFace), candidateFirst, candidateLast))
			continue;
		if (candidateFirst < first) {
			face = candidateFace;
			first = candidateFirst;
			last = candidateLast;
		}
	}
	return first < end;
}

bool TraceEnvironmentProbe(float3 origin, float3 direction, float startDistance, float thickness,
	out float3 radiance, out float hitDistance
#ifdef PROBE_TRACE_VISIBILITY
	,
	out bool unobstructed
#endif
)
{
	radiance = 0;
	hitDistance = 0;
#ifdef PROBE_TRACE_VISIBILITY
	unobstructed = false;
#endif
	if (ProbeValidFaces == 0 || !all(isfinite(origin)) || !all(isfinite(direction)) || !isfinite(startDistance))
		return false;
	float b = dot(origin, direction);
	float discriminant = b * b - dot(origin, origin) + ProbeBoundsRadius * ProbeBoundsRadius;
	if (discriminant <= 0)
		return false;
	float root = sqrt(discriminant);
	float begin = max(max(startDistance, -b - root), 0.5);
	float closest = -b + root;
	if (begin >= closest)
		return false;
	uint width, height, slices, levels;
	ProbeDepthHierarchy.GetDimensions(0, width, height, slices, levels);
#ifdef PROBE_TRACE_VISIBILITY
	if (ProbeKnownEmptySegment(origin, direction, begin, closest, uint2(width, height) * 2)) {
		unobstructed = true;
		return false;
	}
#endif
	uint maxLevel = levels - 1;
	uint budget = clamp(ProbeMaxSteps, 8u, 128u);
	float2 hitUV = 0;
	bool found = false;
#ifdef PROBE_TRACE_VISIBILITY
	bool clear = true;
	float coveredUntil = begin;
	float traceEnd = closest;
#endif
	uint visits = 0;
	uint visitedFaces = 0;
	float rayDistance = begin;
	uint face = ProbeSelectFace(origin + direction * rayDistance);
	// Follow the ray through successive octants instead of sorting all possible
	// overlapping capture intervals before traversal.
	[loop] for (uint interval = 0; interval < 8; ++interval)
	{
		if (rayDistance >= closest)
			break;
		float first = rayDistance, last = closest;
		float3 localOrigin = origin - ProbeOriginOffsets[face].xyz;
		if (!ProbeClipOctant(localOrigin, direction, ProbeFaceSign(face), first, last)) {
			if (!ProbeNextInterval(origin, direction, rayDistance, closest, visitedFaces, face, first, last))
				break;
			localOrigin = origin - ProbeOriginOffsets[face].xyz;
		}
		visitedFaces |= 1u << face;
		bool validFace = (ProbeValidFaces & (1u << face)) != 0;
#ifdef PROBE_TRACE_VISIBILITY
		// Missing or ambiguous rotating octants must not be called open sky.
		clear = clear && validFace && first <= coveredUntil + max(0.001, first * 2e-6);
#endif
		float t = first;
		float end = last;
		bool traverse = validFace;
		if (validFace) {
			float2 rootBounds = ProbeDepthHierarchy.Load(int4(0, 0, face, maxLevel));
			traverse = abs(rootBounds.x) <= rootBounds.y;
#ifdef PROBE_TRACE_VISIBILITY
			if (!traverse)
				clear = clear && rootBounds.x > 0;
#endif
		}
		if (traverse) {
			float3 sign = ProbeFaceSign(face);
			float w = dot(sign, localOrigin);
			float dw = dot(sign, direction);
			float2 numerator = ProbeFaceNumerator(localOrigin, sign, face);
			float2 slope = ProbeFaceNumerator(direction, sign, face);
			float2 velocity = slope * w - numerator * dw;
			uint level = min(4u, maxLevel);
			bool front = false;
			float2 cursor = 0;
			uint crossed = 0;
			[loop] while (visits < budget && t < end)
			{
				++visits;
				uint size = max(1u, width >> level);
				float denominator = w + dw * t;
				if (denominator <= 1e-6) {
					if (dw <= 0)
						break;
					t = max(asfloat(asuint(t) + 1u), (1e-5 - w) / dw);
					continue;
				}
				float2 p = (numerator + slope * t) / denominator * size;
				// Carry exact dyadic boundaries through ascent/descent. Reprojecting a
				// one-ULP advance in world distance may round back into the old texel.
				if (crossed & 1u)
					p.x = cursor.x * size;
				if (crossed & 2u)
					p.y = cursor.y * size;
				int2 cell = int2(floor(p));
				if (velocity.x < 0 && p.x == cell.x)
					--cell.x;
				if (velocity.y < 0 && p.y == cell.y)
					--cell.y;
				cell = clamp(cell, 0, int(size) - 1);
				float2 boundary;
				uint nextCrossed;
				float exit = max(t, ProbeCellExit(numerator, slope, w, dw, velocity, cell, size, end, boundary, nextCrossed));
				float2 bounds = ProbeDepthHierarchy.Load(int4(cell, face, level));
				bool known = bounds.x > 0;
				bounds.x = abs(bounds.x);
				float endW = w + dw * exit;
				float lo = min(denominator, endW);
				float hi = max(denominator, endW);
				// Radial thickness projects to at most sqrt(3) times that distance.
				// Decode's unnormalized direction is sqrt(3)-conditioned at worst.
				// sqrt(18)/sourceWidth bounds the angular radius of a whole texel.
				float angularFootprint = sqrt(4.5) / width;
				float shell = (max(thickness, 0.5) + min(ProbeFarDistance, max(0, bounds.y)) * angularFootprint) / (1.0 - angularFootprint);
				bool empty = bounds.x > bounds.y;
				bool before = hi < bounds.x;
				bool after = lo > bounds.y + sqrt(3.0) * shell;
				if (!empty && !before && !after && level > 0) {
					--level;
					continue;
				}
#ifdef PROBE_TRACE_VISIBILITY
				clear = clear && known && (empty || before);
#endif
				if (empty || before)
					front = known;
				else if (after)
					front = false;
				else {
					uint2 quadrant = uint2((face & 1u) == 0, (face & 2u) == 0);
					uint2 pixel = uint2(cell) + quadrant * width;
					float depth = ProbeDepthTexture.Load(int3(pixel, 0));
					float3 entryPosition = localOrigin + direction * t;
					float radius = length(entryPosition);
					if (isfinite(depth) && depth > 0 && depth < ProbeFarDistance) {
						float sphereB = dot(localOrigin, direction);
						float disc = sphereB * sphereB - dot(localOrigin, localOrigin) + depth * depth;
						float candidate = disc >= 0 ? -sphereB + sqrt(max(0, disc)) : -1;
						float tolerance = max(0.001, t * 2e-6);
						float footprint = radius * angularFootprint;
						// An angular depth discontinuity can enter the finite thickness
						// shell, but a ray starting behind the surface is not a new hit.
						if (front && radius >= depth && radius <= depth + max(thickness, 0.5) + footprint)
							candidate = t;
						if (candidate >= max(begin, t - tolerance) && candidate <= exit + tolerance && candidate < closest) {
							closest = clamp(candidate, t, exit);
							hitUV = EnvironmentProbe::Encode(localOrigin + direction * closest);
							found = true;
							break;
						}
						front = length(localOrigin + direction * exit) < depth;
					} else
						front = depth >= ProbeFarDistance;
				}
				// A chart boundary is also an octant boundary. Its projective solution
				// can differ by a few ULPs from the half-space clip; do not repeatedly
				// clamp an already-exited cursor into the chart's last cell.
				if (((nextCrossed & 1u) && (velocity.x > 0 ? cell.x == int(size) - 1 : cell.x == 0)) ||
					((nextCrossed & 2u) && (velocity.y > 0 ? cell.y == int(size) - 1 : cell.y == 0))) {
					t = end;
					break;
				}
				// Move to the next representable ray distance to resolve boundary ties.
				t = asfloat(asuint(max(t, exit)) + 1u);
				cursor = boundary;
				crossed = nextCrossed;
				// Ascend only across an actual parent boundary. Unconditionally going
				// up after every child repeats the same parent test on long grazing rays.
				uint nextLevel = level;
				if (crossed & 1u)
					nextLevel = max(nextLevel, (uint)firstbitlow(uint(cell.x + (velocity.x > 0)) << level));
				if (crossed & 2u)
					nextLevel = max(nextLevel, (uint)firstbitlow(uint(cell.y + (velocity.y > 0)) << level));
				level = min(nextLevel, maxLevel);
			}
			if (found)
				break;
			// Exhaustion is unresolved visibility, not permission to return a farther hit.
			if (t < end)
				return false;
		} else {
			t = end;
		}
#ifdef PROBE_TRACE_VISIBILITY
		coveredUntil = max(coveredUntil, min(t, end));
#endif
		rayDistance = end + max(0.001, end * 2e-6);
		if (rayDistance >= closest)
			break;
		uint nextFace = ProbeSelectFace(origin + direction * rayDistance);
		if (nextFace == face || (visitedFaces & (1u << nextFace)) != 0) {
			float nextFirst, nextLast;
			if (!ProbeNextInterval(origin, direction, rayDistance, closest, visitedFaces, nextFace, nextFirst, nextLast))
				break;
			rayDistance = nextFirst;
		}
		face = nextFace;
	}
	if (found) {
		float3 color = EnvironmentProbe::SampleRadiance(ProbeRadianceTexture, PROBE_TRACE_SAMPLER, hitUV, uint2(width, height) * 2);
		if (all(isfinite(color))) {
			radiance = max(color, 0);
			hitDistance = closest;
			return true;
		}
	}
#ifdef PROBE_TRACE_VISIBILITY
	unobstructed = !found && clear && coveredUntil >= traceEnd;
#endif
	return false;
}
#endif
