#ifndef PHYSICAL_SKY_WEATHER_LIGHT_HLSLI
#define PHYSICAL_SKY_WEATHER_LIGHT_HLSLI

// Mip 0 R stores sqrt(solar optical depth) / 10. G stores base-layer extinction in inverse metres.
// A footprint that straddles clear and dense rays otherwise averages optical
// depth linearly and over-darkens the clear part after Beer-Lambert attenuation.
// The independent middle-cloud and terrain shadow maps are applied at shading.
Texture3D<float2> TexWeatherDirectLight : register(t97);
RWTexture3D<float2> RWWeatherDirectLight : register(u0);

float3 WeatherSunUVW(float3 pos, float3 origin)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 delta = pos - origin;
	const float2 projected = delta.xy / max(length(delta) + delta.z, 1e-5);
	const float altitude = WeatherCloudAltitude(pos, info.cloudCurvatureRadius);
	return float3(projected * 0.5 + 0.5, (altitude - info.lowCloudBaseAltitude) / max(info.lowCloudTopAltitude - info.lowCloudBaseAltitude, 1.0));
}

bool WeatherSunInsideGrid(float3 uvw)
{
	const float2 disk = 2.0 * uvw.xy - 1.0;
	return uvw.z >= 0.0 && uvw.z <= 1.0 && dot(disk, disk) < 1.0;
}

bool WeatherSunPosition(uint3 pixel, uint3 dims, out float3 position)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 uvw = (pixel + 0.5) / float3(dims);
	const float2 p = 2.0 * uvw.xy - 1.0;
	const float r2 = dot(p, p);
	position = 0.0;
	if (r2 >= 1.0)
		return false;
	const float3 dir = float3(2.0 * p, 1.0 - r2) / (1.0 + r2);
	const float altitude = lerp(info.lowCloudBaseAltitude, info.lowCloudTopAltitude, uvw.z);
	const float t = IntersectWeatherCloudBelow(WeatherLightGrid.xyz, dir, altitude, info.cloudCurvatureRadius).y;
	if (t <= 0.0)
		return false;
	position = WeatherLightGrid.xyz + t * dir;
	return true;
}

float2 WeatherLightShellSegment(float3 pos, float3 direction, uint layer, VolumetricCloudData info)
{
	const float top = layer == 0u ? info.weatherBaseTopAltitude : info.weatherStormTopAltitude;
	const float2 outer = IntersectWeatherCloudBelow(pos, direction, top, info.cloudCurvatureRadius);
	const float2 inner = IntersectWeatherCloudBelow(pos, direction, info.lowCloudBaseAltitude, info.cloudCurvatureRadius);
	const float start = max(outer.x, 0.0);
	// The light column ends at the first inner crossing, including when the
	// grid point lies inside the inner sphere after the camera origin moves.
	const float end = inner.y > start ? min(outer.y, inner.x) : outer.y;
	return float2(start, max(end, start));
}

float WeatherSunColumn(float3 pos, uint layer)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 segment = WeatherLightShellSegment(pos, info.dirlightDir, layer, info);
	const float distance = segment.y - segment.x;
	if (distance <= 0.0)
		return 0.0;
	// The provisional grid is replaced by full V5 columns over the next slice cycle.
	const uint maxSteps = WeatherLightSeedStride > 1u ? 16u : 50u;
	const float inverseSteps = WeatherLightSeedStride > 1u ? 1.0 / 16.0 : 1.0 / 50.0;
	const float step = max(distance * inverseSteps, (layer == 0u ? 75.0 : 300.0) * info.weatherWorldScale * GAME_UNITS_PER_METER);
	const uint count = min((uint)(distance / step), maxSteps);
	const float3 start = pos + segment.x * info.dirlightDir;
	float sum = 0.0;
	[loop] for (uint i = 1u; i <= count; ++i)
	{
		CloudDensityContext context;
		sum += sampleWeatherCloudDensity(start + info.dirlightDir * (i * step), GetCloudLayer(info), 3.0, 0.0, 0.0, context, 1u << layer);
	}
	return sum * step * GAME_UNIT_TO_M;
}

[numthreads(8, 8, 1)] void buildWeatherDirectLight(uint3 pixel : SV_DispatchThreadID) {
	uint3 dims;
	RWWeatherDirectLight.GetDimensions(dims.x, dims.y, dims.z);
	dims.xy /= max(WeatherLightSeedStride, 1u);
	pixel.z = (pixel.z + WeatherLightFirstSlice) % dims.z;
	float3 pos;
	float2 light = 0.0;
	if (WeatherSunPosition(pixel, dims, pos)) {
		const VolumetricCloudData info = VolumetricCloudBuffer[0];
		CloudDensityContext context;
		light.y = sampleWeatherCloudDensity(pos, GetCloudLayer(info), 0.0, 0.0, 0.0, context, 1u);
		float tau = WeatherSunColumn(pos, 0u);
		if (info.lowCloudTopAltitude >= info.weatherStormTopAltitude)
			tau += WeatherSunColumn(pos, 1u);
		light.x = sqrt(tau) * 0.1;
	}
	RWWeatherDirectLight[pixel] = light;
}

// Mip 0 carries sunlight and local extinction. Mip 1 stores the linear
// six-ray hemispherical depth divided by 100.
float2 ReprojectWeatherVoxel(uint3 pixel, uint3 dims, uint mip)
{
	if (WeatherLightSeedStride > 1u) {
		const uint stride = mip == 0u ? WeatherLightSeedStride : 4u;
		const uint2 seedSize = dims.xy / stride;
		const float2 texel = clamp((float2(pixel.xy) + 0.5) / stride, 0.5, float2(seedSize) - 0.5);
		return TexWeatherDirectLight.SampleLevel(TransmittanceSampler, float3(texel, pixel.z + 0.5) / float3(dims), mip);
	}
	float3 pos;
	if (WeatherSunPosition(pixel, dims, pos)) {
		const float3 uvw = WeatherSunUVW(pos, WeatherPreviousLightGrid.xyz);
		if (WeatherSunInsideGrid(uvw))
			return TexWeatherDirectLight.SampleLevel(TransmittanceSampler, uvw, mip);
	}
	return 0.0;
}

RWTexture3D<float2> RWWeatherAmbientMip : register(u1);

[numthreads(8, 8, 1)] void reprojectWeatherDirectLight(uint3 pixel : SV_DispatchThreadID) {
	uint3 dims;
	RWWeatherDirectLight.GetDimensions(dims.x, dims.y, dims.z);
	RWWeatherDirectLight[pixel] = ReprojectWeatherVoxel(pixel, dims, 0u);
	if (UpdateAmbientMip != 0u && all(pixel < dims / 2u))
		RWWeatherAmbientMip[pixel] = ReprojectWeatherVoxel(pixel, dims / 2u, 1u);
}

float WeatherAmbientExtinction(float3 position, uint layer)
{
	// The provisional grid itself is still being seeded on a cut.
	if (WeatherLightSeedStride == 1u) {
		const float3 uvw = WeatherSunUVW(position, WeatherLightGrid.xyz);
		[branch] if (WeatherSunInsideGrid(uvw)) return TexWeatherDirectLight.SampleLevel(TransmittanceSampler, uvw, layer).g;
	}
	CloudDensityContext context;
	return sampleWeatherCloudDensity(position, GetCloudLayer(VolumetricCloudBuffer[0]), 3.0, 0.0, 0.0, context, 1u << layer);
}

float WeatherAmbientColumn(float3 pos, float3 direction, uint layer)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 segment = WeatherLightShellSegment(pos, direction, layer, info);
	const float range = 4.0 * (info.lowCloudTopAltitude - info.lowCloudBaseAltitude);
	const float distance = min(range, segment.y) - segment.x;
	if (distance <= 0.0)
		return 0.0;
	// V5 uses floor(direct march budget / 3), and samples at the far end of each step.
	const uint budget = WeatherLightSeedStride > 1u ? 8u : 16u;
	const float step = max(distance / float(budget), (layer == 0u ? 75.0 : 300.0) * info.weatherWorldScale * GAME_UNITS_PER_METER);
	const uint count = min(budget, uint(distance / step));
	const float3 start = pos + segment.x * direction;
	float sum = 0.0;
	[loop] for (uint i = 1u; i <= count; ++i)
		sum += WeatherAmbientExtinction(start + (i * step) * direction, layer);
	return sum * step * GAME_UNIT_TO_M;
}

[numthreads(8, 8, 1)] void buildWeatherAmbientLight(uint3 pixel : SV_DispatchThreadID) {
	uint3 dims;
	RWWeatherAmbientMip.GetDimensions(dims.x, dims.y, dims.z);
	const bool seed = WeatherLightSeedStride > 1u;
	dims.xy /= seed ? 4u : 1u;
	if (any(pixel.xy >= dims.xy))
		return;
	pixel.z = seed ? pixel.z : WeatherAmbientUpdate % dims.z;
	float3 pos;
	float depth = 0.0;
	float anvilExtinction = 0.0;
	if (WeatherSunPosition(pixel, dims, pos)) {
		const VolumetricCloudData info = VolumetricCloudBuffer[0];
		const float3 up = normalize(pos + float3(0, 0, 0.5 * info.cloudCurvatureRadius));
		const float3 tangent = normalize(float3(up.z, 0.0, -up.x));
		const float3 bitangent = cross(up, tangent);
		if (info.lowCloudTopAltitude >= info.weatherStormTopAltitude) {
			CloudDensityContext context;
			anvilExtinction = sampleWeatherCloudDensity(pos, GetCloudLayer(info), 3.0, 0.0, 0.0, context, 2u);
		}
		const uint cycle = WeatherAmbientUpdate / dims.z;
		uint hash = (pixel.x * 1597334677u) ^ (pixel.y * 3812015801u) ^ (pixel.z * 2798796415u) ^ (cycle * 2246822519u);
		hash = (hash ^ (hash >> 16u)) * 2246822519u;
		const float2 rotation = float2(hash & 65535u, hash >> 16u) / 65536.0;
		// Six stratified directions rotate each update over the local upper hemisphere.
		[loop] for (uint ray = 0u; ray < 6u; ++ray)
		{
			const float z = (ray + rotation.x) / 6.0;
			float sn, cs;
			sincos(6.283185307 * frac(rotation.y + ray * 0.61803399), sn, cs);
			const float3 dir = z * up + sqrt(1.0 - z * z) * (cs * tangent + sn * bitangent);
			depth += WeatherAmbientColumn(pos, dir, 0u);
			if (info.lowCloudTopAltitude >= info.weatherStormTopAltitude)
				depth += WeatherAmbientColumn(pos, dir, 1u);
		}
		depth *= 1.0 / 600.0;
		if (!seed) {
			const float old = TexWeatherDirectLight.Load(int4(pixel, 1)).x;
			const float blend = clamp(rcp(max(float((WeatherAmbientUpdate + 1u) / dims.z), 1.0)), 0.01, 0.07);
			depth = lerp(old, depth, blend);
		}
	}
	RWWeatherAmbientMip[pixel] = float2(depth, anvilExtinction);
}

void WeatherCubicCoordinates(float3 uvw, uint3 dim, out float3 lo, out float3 hi, out float3 lowWeight)
{
	const float3 coord = uvw * dim - 0.5;
	const float3 cell = floor(coord);
	const float3 f = frac(coord), g = 1.0 - f;
	const float3 w0 = g * g * g / 6.0;
	const float3 w1 = (4.0 - 3.0 * f * f * (2.0 - f)) / 6.0;
	const float3 w3 = f * f * f / 6.0;
	lowWeight = w0 + w1;
	const float3 highWeight = 1.0 - lowWeight;
	lo = (cell - 0.5 + w1 / lowWeight) / dim;
	hi = (cell + 1.5 + w3 / highWeight) / dim;
}

float WeatherStoredLightDepth(float3 pos, uint mip)
{
	const float3 uvw = WeatherSunUVW(pos, WeatherLightGrid.xyz);
	if (!WeatherSunInsideGrid(uvw))
		return 0.0;
	uint3 dim;
	uint levels;
	TexWeatherDirectLight.GetDimensions(mip, dim.x, dim.y, dim.z, levels);
	// Cubic B-spline reconstruction using the eight trilinear fetches in V5.
	float3 lo, hi, a;
	WeatherCubicCoordinates(uvw, dim, lo, hi, a);
	const float3 b = 1.0 - a;
	float result = 0.0;
	[unroll] for (uint i = 0; i < 8u; ++i)
	{
		const bool3 high = (i & uint3(1, 2, 4)) != 0;
		const float3 weights = high ? b : a;
		result += weights.x * weights.y * weights.z * TexWeatherDirectLight.SampleLevel(TransmittanceSampler, high ? hi : lo, mip).r;
	}
	return 100.0 * (mip == 0u ? result * result : result);
}

float WeatherAmbientVisibility(float3 pos)
{
	const float3 uvw = WeatherSunUVW(pos, WeatherLightGrid.xyz);
	const float horizonFade = saturate((0.5 - length(uvw.xy - 0.5)) / 0.2);
	if (horizonFade <= 0.0)
		return 1.0;

	uint3 dim;
	uint levels;
	TexWeatherDirectLight.GetDimensions(1u, dim.x, dim.y, dim.z, levels);
	float3 lo, hi, lowWeight;
	WeatherCubicCoordinates(uvw, dim, lo, hi, lowWeight);
	// The ambient mip has fewer altitude slices. Reconstruct Z cubically
	// and let the sampler interpolate XY.
	const float low = TexWeatherDirectLight.SampleLevel(TransmittanceSampler, float3(uvw.xy, lo.z), 1u).r;
	const float high = TexWeatherDirectLight.SampleLevel(TransmittanceSampler, float3(uvw.xy, hi.z), 1u).r;
	const float tau = 10.0 * horizonFade * (lowWeight.z * low + (1.0 - lowWeight.z) * high);
	return 0.3 + 0.7 / (1.0 + tau * (0.35 + 0.065 * tau));
}

float CloudLightProbeJitter(float2 positionMeters)
{
	const float2 seed = frac(positionMeters * 0.1031);
	const float scramble = dot(seed.xyx, seed.yxx + 19.19);
	return frac((seed.y + seed.x + scramble * 2.0) * (scramble + seed.x));
}

struct WeatherSolarDisk
{
	float radius;
	float minimumMidpoint;
};

WeatherSolarDisk MakeWeatherSolarDisk(CloudLayer cloud)
{
	WeatherSolarDisk disk = (WeatherSolarDisk)0;
#ifndef CLOUD_REFLECTION
	disk.radius = tan(SharedData::physSkyData.diskAngularRadius[CloudDiskIndex()]);
	uint w, h, d, mipCount;
	TexWeatherDirectLight.GetDimensions(0u, w, h, d, mipCount);
	const float thickness = max(cloud.highestAltitude - cloud.lowestAltitude, 1.0);
	disk.minimumMidpoint = 0.25 * thickness / max(disk.radius * float(d), 1e-8);
#endif
	return disk;
}

float3 WeatherSunGridOffset(float3 offset, float2 plane, float projectionScale, float3 projectionGradient, float3 heightGradient)
{
	return float3((offset.xy - plane * dot(projectionGradient, offset)) * projectionScale, dot(heightGradient, offset));
}

float2 WeatherFilteredSunSample(float3 uvw, float3 center, float2 tailExtinction)
{
	const float tail = TexWeatherDirectLight.SampleLevel(TransmittanceSampler, WeatherSunInsideGrid(uvw) ? uvw : center, 0u).r;
	return exp(-tailExtinction * (tail * tail));
}

// Integrate each transmission response over the same solar disk.
float2 WeatherCloudLightTransmission(float3 pos, WeatherSolarDisk disk, float cosine, float viewMetres, float height, WeatherOpticalSample previous,
	VolumetricCloudData info, CloudLayer cloud, WeatherDensityParameters parameters)
{
#ifdef CLOUD_REFLECTION
	const float depth = WeatherStoredLightDepth(pos, 0u) * info.sunExtinction;
	return float2(exp(-depth), exp(-depth * info.msTransportExtinction));
#else
	const uint count = uint(10.0 - saturate((viewMetres - 512.0) / 2488.0) * 7.0);
	const float inverseCount = rcp(float(count));
	const float nearRange = 240.0 - 120.0 * saturate(height * 3.3333333);
	const float exit = WeatherCloudShellExit(pos, info.dirlightDir,
		float2(cloud.lowestAltitude, cloud.highestAltitude), parameters.curvatureRadius,
		nearRange * GAME_UNITS_PER_METER, disk.minimumMidpoint);
	if (exit <= 0.0)
		return 1.0;
	const float exitMetres = exit * GAME_UNIT_TO_M;
	const float extent = min(nearRange, exitMetres);
	const float angularOffset = -0.25 * cosine;
	const float jitter = CloudLightProbeJitter(pos.xy * GAME_UNIT_TO_M);
	const float3 sun = info.dirlightDir * GAME_UNITS_PER_METER;
	float distance = 0.0;
	float depth = 0.0;
	[loop] for (uint i = 0u; i < count; ++i)
	{
		const float fraction = (i + 0.5) * inverseCount;
		const float angularScale = 0.5 + angularOffset + fraction * fraction * fraction * (0.5 - angularOffset);
		// With three probes, cover the surface and interior before the fixed handoff to the far volume.
		const float fractionOffset = count == 3u ?
		                                 (i == 0u ? lerp(0.085, 0.155, jitter) : lerp(0.41, 0.49, jitter)) :
		                                 (i + 0.5 + jitter) * inverseCount * angularScale;
		const float offset = i + 1u == count ? extent : extent * fractionOffset;
		float4 weather;
		float3 body, anvil;
		float2 peak;
		float relativeAltitude;
		const WeatherOpticalSample probe = SampleWeatherOptics(pos + offset * sun, cloud, parameters, 2.0, 3u,
			weather, body, anvil, peak, relativeAltitude);
		float2 widths;
		depth += dot(WeatherIntervalColumn(previous, probe, widths), 1.0) * (offset - distance);
		previous = probe;
		distance = offset;
	}
	const float2 extinction = info.sunExtinction * float2(1.0, info.msTransportExtinction);
	// The shared grid starts at the last actual density probe.
	if (exitMetres > nearRange) {
		const float3 handoff = pos + distance * sun;
		const float3 center = WeatherSunUVW(handoff, WeatherLightGrid.xyz);
		if (WeatherSunInsideGrid(center)) {
			const float tailScale = 100.0 * saturate((exitMetres - nearRange) / nearRange);
			const float midpoint = 0.5 * (exit + distance * GAME_UNITS_PER_METER);
			[branch] if (exit < 1e30 && midpoint > disk.minimumMidpoint)
			{
				const float offset = disk.radius * midpoint * smoothstep(disk.minimumMidpoint, 3.0 * disk.minimumMidpoint, midpoint);
				// Linearize the volume projection once for the four disk samples.
				const float3 delta = handoff - WeatherLightGrid.xyz;
				const float radialDistance = length(delta);
				const float projectionScale = 0.5 / max(radialDistance + delta.z, 1e-5);
				const float3 projectionGradient = delta / max(radialDistance, 1e-5) + float3(0, 0, 1);
				const float thickness = max(cloud.highestAltitude - cloud.lowestAltitude, 1.0);
				const float3 heightGradient = normalize(handoff + float3(0, 0, 0.5 * parameters.curvatureRadius)) / thickness;
				const float2 plane = 2.0 * center.xy - 1.0;
				const float3 direction = info.dirlightDir;
				const float3 axis = abs(direction.z) < 0.9 ? float3(0, 0, 1) : float3(0, 1, 0);
				const float3 across = cross(axis, direction);
				const float3 tangent = across * (0.5 * offset * rsqrt(dot(across, across)));
				const float3 bitangent = cross(direction, tangent);
				const float3 u = WeatherSunGridOffset(tangent + bitangent, plane, projectionScale, projectionGradient, heightGradient);
				const float3 v = WeatherSunGridOffset(tangent - bitangent, plane, projectionScale, projectionGradient, heightGradient);
				const float2 tailExtinction = tailScale * extinction;
				const float2 farTransmission = 0.25 * (WeatherFilteredSunSample(center + u, center, tailExtinction) +
														  WeatherFilteredSunSample(center - u, center, tailExtinction) +
														  WeatherFilteredSunSample(center + v, center, tailExtinction) +
														  WeatherFilteredSunSample(center - v, center, tailExtinction));
				return exp(-depth * extinction) * farTransmission;
			}
			const float tail = TexWeatherDirectLight.SampleLevel(TransmittanceSampler, center, 0u).r;
			depth += tailScale * tail * tail;
		}
	}
	return exp(-depth * extinction);
#endif
}

#endif
