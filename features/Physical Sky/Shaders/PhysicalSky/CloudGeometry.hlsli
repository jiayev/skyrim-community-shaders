#ifndef PHYSICAL_SKY_CLOUD_GEOMETRY_HLSLI
#define PHYSICAL_SKY_CLOUD_GEOMETRY_HLSLI

float CloudCurvatureOffset(float2 relativeXY, float radius)
{
	return dot(relativeXY, relativeXY) / radius;
}

float CloudProfileAltitude(float3 position, float radius)
{
	return position.z + CloudCurvatureOffset(position.xy, radius);
}

float2 IntersectCloudBelow(float3 origin, float3 direction, float altitude, float radius)
{
	const float a = dot(direction.xy, direction.xy) / radius;
	const float b = direction.z + 2.0 * dot(origin.xy, direction.xy) / radius;
	const float c = origin.z + CloudCurvatureOffset(origin.xy, radius) - altitude;
	if (a == 0.0) {
		if (b == 0.0)
			return c <= 0.0 ? float2(-1e30, 1e30) : float2(0, 0);
		const float root = -c / b;
		return b > 0.0 ? float2(-1e30, root) : float2(root, 1e30);
	}
	const float discriminant = b * b - 4.0 * a * c;
	if (discriminant <= 0.0)
		return 0.0;
	const float q = -0.5 * (b + (b < 0.0 ? -sqrt(discriminant) : sqrt(discriminant)));
	const float2 roots = float2(q / a, c / q);
	return float2(min(roots.x, roots.y), max(roots.x, roots.y));
}

// The weather shell is tangent to the worldspace datum at world XY zero.
// Positions are absolute XY and altitude relative to that datum.
float WeatherCloudAltitude(float3 position, float curvatureRadius)
{
	const float horizontalSquared = dot(position.xy, position.xy);
	const float axis = 0.5 * curvatureRadius + position.z;
	const float radial = sqrt(axis * axis + horizontalSquared);
	return axis >= 0.0 ? position.z + horizontalSquared / max(radial + axis, 1e-5) : radial - 0.5 * curvatureRadius;
}

float WeatherCloudWorldHeight(float2 relativeXY, float altitude, float curvatureRadius)
{
	const float r = 0.5 * curvatureRadius + altitude;
	const float xy2 = dot(relativeXY, relativeXY);
	return altitude - xy2 / (r + sqrt(max(r * r - xy2, 0.0)));
}

float2 WeatherSphereRoots(float axis, float radialDelta)
{
	const float discriminant = axis * axis - radialDelta;
	if (discriminant <= 0.0)
		return 0.0;
	const float q = -axis - (axis < 0.0 ? -1.0 : 1.0) * sqrt(discriminant);
	const float other = radialDelta / q;
	return float2(min(other, q), max(other, q));
}

float2 IntersectWeatherCloudBelow(float3 origin, float3 direction, float altitude, float curvatureRadius)
{
	const float axis = dot(origin, direction) + 0.5 * curvatureRadius * direction.z;
	const float radialDelta = dot(origin.xy, origin.xy) + (origin.z - altitude) * (curvatureRadius + origin.z + altitude);
	return WeatherSphereRoots(axis, radialDelta);
}

// First exit for a ray starting inside the weather shell.
float WeatherCloudShellExit(float3 origin, float3 direction, float2 altitudes, float curvatureRadius, float nearRange, float minimumPenumbraMidpoint)
{
	const float axis = dot(origin, direction) + 0.5 * curvatureRadius * direction.z;
	const float2 delta = dot(origin.xy, origin.xy) + (origin.z - altitudes) * (curvatureRadius + origin.z + altitudes);
	if (delta.x < 0.0 || delta.y > 0.0)
		return 0.0;
	const float limit = 2.0 * nearRange;
	const float closest = clamp(-axis, 0.0, limit);
	if (closest > 0.0 && delta.x + closest * (2.0 * axis + closest) < 0.0)
		return WeatherSphereRoots(axis, delta.x).x;
	if (delta.y + limit * (2.0 * axis + limit) >= 0.0)
		return WeatherSphereRoots(axis, delta.y).y;
	// An outward ray exits no later than -delta.y / (2 * axis). Beyond
	// the near handoff only resolve the root if the disk footprint can matter.
	const float centerOnlyExit = 2.0 * minimumPenumbraMidpoint - nearRange;
	if (axis > 0.0 && -delta.y <= 2.0 * axis * centerOnlyExit)
		return 1e30;
	const float axisSquared = axis * axis;
	const bool hitsInner = axis < 0.0 && axisSquared > delta.x;
	const float boundary = hitsInner ? delta.x : delta.y;
	const float root = abs(axis) + sqrt(max(axisSquared - boundary, 0.0));
	return hitsInner || axis >= 0.0 ? abs(boundary) / max(root, 1e-5) : root;
}

#endif
