#ifndef PHYSICAL_SKY_CLOUD_SHAPE_HLSLI
#define PHYSICAL_SKY_CLOUD_SHAPE_HLSLI

#include "PhysicalSky/CloudNoise.hlsli"

Texture2D<float4> TexCloudWeather : register(t6);
Texture2D<float4> TexFiniteCloudWeather : register(t117);
Texture3D<float4> NoiseVoxel : register(t76);
Texture3D<float4> TexWeatherCurl : register(t104);
Texture3D<float> TexWeatherWorley : register(t107);
Texture2D<float4> TexWeatherProfiles : register(t108);

// This lattice uses a local tangent plane for Skyrim world positions.
static const float WeatherNoiseSphereRadius = 600000.0;
static const float WeatherNoiseBaseHeight = 1750.0;
static const float WeatherErosionDepth = 0.65;
static const float WeatherMinimumCoverageMargin = 0.01;
static const float WeatherBaseRiseFraction = 0.09;
static const float WeatherRoundedCrownMaxDepth = 5600.0;

float WeatherRoundedCrownCoverage(float height)
{
	// A rounded crown narrows slowly above the shoulder and faster near the tip.
	const float t = saturate((height - 0.085) / 0.915);
	return 0.98 * (1.0 - t * (0.35 + 0.65 * t));
}

float2 WeatherPeakExtinction(float2 bakedPeaks, float baseType, float3 peaks)
{
	// The first two low-cloud types are stratus and cumulus. The thicker
	// low-cloud types retain their density ratios relative to cumulus.
	const float base = baseType < 0.0  ? bakedPeaks.x * (peaks.x / 0.0015) :
	                   baseType <= 0.2 ? lerp(peaks.x, peaks.y, baseType * 5.0) :
	                                     bakedPeaks.x * (peaks.y / 0.012);
	return float2(base, bakedPeaks.y * (peaks.z / 0.002));
}

float4 ClampCloudWeather(float4 weather)
{
	return float4(saturate(weather.x), clamp(weather.y, -1.0, 1.0), saturate(weather.zw));
}

float WeatherBaseExtinctionScale(float coverage, float proceduralWeight)
{
	// Weak parts of the generated moisture field carry less condensed water.
	// Preserve peak extinction in the mature cores.
	const float dilute = 1.0 - smoothstep(0.26, 0.83, coverage);
	return 1.0 - (0.92 * proceduralWeight) * dilute;
}

float3 WeatherNoiseDirection(float2 positionMeters, float worldScale)
{
	const float3 up = float3(0.6315598931, 0.4203667638, -0.6514782309);
	const float3 right = float3(0.7396875121, -0.5785074392, 0.3437899463);
	const float3 forward = float3(0.2323671359, 0.6990142536, 0.6763021421);
	return normalize(up * WeatherNoiseSphereRadius + (positionMeters.x * right + positionMeters.y * forward) / worldScale);
}

float2 CloudWeatherUV(float2 uv, float proceduralWeight)
{
	[branch] if (proceduralWeight > 0.0 && any(abs(uv) > 0.5))
	{
		// Spread the phase change over a map width. A narrow ramp pulls
		// the coverage and cloud-type contours into long bands at the edge.
		const float amplitude = 0.24 * proceduralWeight;
		uv.x += amplitude * smoothstep(0.5, 1.5, abs(uv.y)) * sin(2.11 * uv.y - 0.26);
		uv.y += amplitude * smoothstep(0.5, 1.5, abs(uv.x)) * sin(0.90 * uv.x + 0.26);
	}
	return uv + 0.5;
}

float4 SampleCloudWeatherMap(float2 mapPosition, float proceduralWeight, uint finiteWeather)
{
	// mapPosition is in unwrapped, wind-relative world coordinates. The storm
	// map contains the full composition within its single world footprint.
	[branch] if (finiteWeather != 0u && all(abs(mapPosition) < 0.5)) return ClampCloudWeather(TexFiniteCloudWeather.SampleLevel(TileableSampler, mapPosition + 0.5, 0));
	return ClampCloudWeather(TexCloudWeather.SampleLevel(TileableSampler, CloudWeatherUV(mapPosition, proceduralWeight), 0));
}

float3 WeatherNoiseTileOffset(int2 cell)
{
	uint3 seed = asuint(int3(cell.y, cell.x, cell.y));
	seed = (seed ^ (seed.zxy >> 8u)) * 1103515245u;
	seed = (seed ^ (seed.zxy >> 8u)) * 1103515245u;
	return float3(seed) * (1.0 / 4294967296.0);
}

struct WeatherNoiseTriangle
{
	float3 a, b, c;
	float3 weight;
};

WeatherNoiseTriangle WeatherNoiseTiles(float3 radial, float inverseTile)
{
	const float2 octahedral = radial.xy / dot(abs(radial), float3(1, 1, 1));
	const float2 grid = (0.5 * octahedral + 0.5) * floor((WeatherNoiseSphereRadius + WeatherNoiseBaseHeight) * inverseTile * 2.93);
	const float2 f = frac(grid);
	const bool upper = f.x + f.y >= 1.0;
	const int direction = upper ? -1 : 1;
	const int2 cell = int2(floor(grid)) + (upper ? 1 : 0);
	const float2 edge = upper ? 1.0 - f : f;
	float3 weight = max(float3(1.0 - edge.x - edge.y, edge.y, edge.x) - 0.15, 0.0);
	weight /= dot(weight, float3(1, 1, 1));
	WeatherNoiseTriangle tiles;
	tiles.a = WeatherNoiseTileOffset(cell);
	tiles.b = WeatherNoiseTileOffset(cell + int2(0, direction));
	tiles.c = WeatherNoiseTileOffset(cell + int2(direction, 0));
	tiles.weight = weight;
	return tiles;
}

float WeatherWorley(float3 radial, float sourceAltitude, float inverseTile, float mip, float3 displacement)
{
	const WeatherNoiseTriangle tiles = WeatherNoiseTiles(radial, inverseTile);
	const float3 uvw = (radial * (WeatherNoiseSphereRadius + sourceAltitude) + displacement) * inverseTile;
	float3 noise = 0.0;
	[branch] if (tiles.weight.x > 0.0)
		noise.x = TexWeatherWorley.SampleLevel(TileableSampler, uvw + tiles.a, mip);
	[branch] if (tiles.weight.y > 0.0)
		noise.y = TexWeatherWorley.SampleLevel(TileableSampler, uvw + tiles.b, mip);
	[branch] if (tiles.weight.z > 0.0)
		noise.z = TexWeatherWorley.SampleLevel(TileableSampler, uvw + tiles.c, mip);
	return dot(noise, tiles.weight);
}

float WeatherProfileColumn(float type)
{
	return 128.0 + type * 127.0;
}

float4 WeatherTypeProperties(float type, uint layer)
{
	// The last row of each layer stores density, height support, edge width and noise period in 100 m units.
	return TexWeatherProfiles.SampleLevel(TransmittanceSampler,
		float2((WeatherProfileColumn(type) + 0.5) / 256.0, (128.5 + layer * 129.0) / 258.0), 0);
}

float WeatherStormWorley(float3 radial, float sourceAltitude, float type, float mip, float3 displacement)
{
	const float index = type * 2.0;
	const uint first = (uint)index;
	const uint second = min(first + 1u, 2u);
	const float blend = saturate(frac(index) * 2.0 - 0.5);
	// Blend two noise samples in a transition. Interpolating their tiling
	// would also interpolate the phase of the entire planetary lattice.
	float noise = 0.0;
	[branch] if (blend < 1.0)
	{
		const float period = TexWeatherProfiles.Load(int3(128u + first, 129, 0)).b;
		noise = (1.0 - blend) * WeatherWorley(radial, sourceAltitude, 0.01 / period, mip, displacement);
	}
	[branch] if (blend > 0.0)
	{
		const float period = TexWeatherProfiles.Load(int3(128u + second, 129, 0)).b;
		noise += blend * WeatherWorley(radial, sourceAltitude, 0.01 / period, mip, displacement);
	}
	return noise;
}

float WeatherBaseRiseWidth(float authoredRise, float width)
{
	return clamp(width, 0.25 * authoredRise, 3.0 * authoredRise);
}

float WeatherBaseProfileHeight(float height, float authoredRise, float width, float lift)
{
	// The end of the coverage rise and its return to the authored curve
	// have continuous slopes.
	const float crest = lift + width;
	const float end = max(crest, authoredRise) + authoredRise;
	if (height >= end)
		return height;

	const float tail = end - crest;
	const float riseSlope = authoredRise / width;
	const float tailSlope = (end - authoredRise) / tail;
	const float crestSlope = min(min(riseSlope, tailSlope), 1.0);
	if (height < crest) {
		const float t = saturate((height - lift) / width);
		return t * (authoredRise + t * (1.0 - t) * (authoredRise - width * crestSlope));
	}
	const float t = (height - crest) / tail;
	const float blend = t * (1.0 - t);
	return lerp(authoredRise, end, t) + tail * blend * ((1.0 - t) * (crestSlope - tailSlope) - t * (1.0 - tailSlope));
}

float3 SampleWeatherLayer(float2 weather, float layerDepth, uint layer, float3 radial, float3 detailPosition, float4 shaping, float mip, float detailMip, bool skipNoise, out float2 erosion)
{
	erosion = float2(-1.0, 1.0);
	const float height = detailPosition.z / layerDepth;
	if (height < 0.0 || height > 1.0 || weather.x <= 0.0)
		return 0.0;
	const float4 properties = WeatherTypeProperties(weather.y, layer);
	const float authoredRise = max(WeatherBaseRiseFraction * properties.y, 1e-4);
	const float shallow = layer == 0u ?
	                          smoothstep(0.03, 0.09, weather.y) * (1.0 - smoothstep(0.26, 0.48, weather.y)) *
	                              (1.0 - smoothstep(3300.0, 5700.0, layerDepth)) :
	                          0.0;
	float shapeHeight = height;
	float baseFade = 1.0;
	// In the storm layer only the rising core uses the cloud base profile.
	const float baseWeight = layer == 0u ? 1.0 : smoothstep(0.5, 0.85, weather.y);
	const float requestedFade = shaping.z / layerDepth;
	const float maxLift = min(shaping.y / layerDepth, 2.0 * authoredRise);
	const float coverageWidth = lerp(WeatherBaseRiseWidth(authoredRise, requestedFade), authoredRise, shallow);
	const float fadeWidth = min(requestedFade, 3.0 * authoredRise);
	const float coverageEnd = max(coverageWidth + maxLift, authoredRise) + authoredRise;
	const float baseEnd = shallow > 0.0 ? max(coverageEnd, maxLift + fadeWidth) : coverageEnd;
	[branch] if (baseWeight > 0.0 && height < baseEnd)
	{
		float lift = 0.0;
		[branch] if (maxLift > 0.0)
		{
			const float variation = skipNoise ? 0.5 : TexWeatherWorley.SampleLevel(TileableSampler, float3(detailPosition.xy / 3500.0, 0.37), 0.0);
			lift = maxLift * (1.0 - variation);
		}
		[branch] if (lift > 0.0 || coverageWidth != authoredRise)
			shapeHeight = lerp(height, WeatherBaseProfileHeight(height, authoredRise, coverageWidth, lift), baseWeight);
		// In shallow clouds, fade the shaped extinction through the rise.
		// The coverage curve provides the support for this thin density.
		if (shallow > 0.0 && fadeWidth > 0.0)
			baseFade = lerp(1.0, smoothstep(0.0, fadeWidth, height - lift), shallow);
	}
	const float altitude = WeatherNoiseBaseHeight + detailPosition.z;
	const float layerRow = 0.5 + layer * 129.0;
	const float2 uv = float2((WeatherProfileColumn(weather.y) + 0.5) / 256.0, (shapeHeight * 127.0 + layerRow) / 258.0);
	const float2 curve = TexWeatherProfiles.SampleLevel(TransmittanceSampler, uv, 0).rg;
	const float rounded = layer == 0u ? smoothstep(0.025, 0.105, weather.y) * (1.0 - smoothstep(0.44, 0.62, weather.y)) *
	                                        (1.0 - smoothstep(3400.0, WeatherRoundedCrownMaxDepth, layerDepth)) :
	                                    0.0;
	const float heightInCloud = shapeHeight / max(properties.y, 1e-4);
	const float crownWeight = rounded * smoothstep(0.08, 0.20, heightInCloud);
	const float coverage = lerp(curve.x, WeatherRoundedCrownCoverage(heightInCloud), crownWeight);
	const float margin = weather.x + coverage - 1.0;
	// Optically thin scraps below the rain deck retain the local billows.
	const float rainDeck = saturate(-weather.y) * smoothstep(0.13, 0.28, height);
	const float erosionDepth = lerp(WeatherErosionDepth, 0.28, rainDeck);

	const float edgeWidth = max(properties.z, 1e-4);
	float shape = saturate(margin / edgeWidth);
	// The signed Worley residual has an
	// interior gradient after its native narrow density ramp has saturated.
	// The shallow forms need support behind the visible contour for both
	// scales of billows to carve lobes into the Worley shape.
	const float profileWidth = lerp(edgeWidth, max(edgeWidth, (0.3 + 0.24 * shallow) * erosionDepth), shaping.x);
	const float activeWidth = max(profileWidth, 0.5 * (1.0 - erosionDepth));
	const float ramp = shaping.x > 0.0 ? profileWidth : edgeWidth;
	erosion.x = (margin - (skipNoise ? 0.0 : erosionDepth * 0.5)) / ramp;
	[branch] if (margin < WeatherMinimumCoverageMargin) return 0.0;
	erosion.x = (margin - (skipNoise ? 0.0 : erosionDepth)) / ramp;
	float profile = saturate(margin / profileWidth);
	[branch] if (!skipNoise && margin < erosionDepth + activeWidth)
	{
		float noise;
		if (layer == 0u) {
			// Fractus needs finer displaced contours than the undisturbed
			// cumulus profile. Keep the broad Worley scale inside the body.
			float3 displacement = 0.0;
			const float fractus = smoothstep(0.02, 0.08, weather.y) * (1.0 - smoothstep(0.13, 0.23, weather.y));
			const float fray = fractus * smoothstep(0.2, 1.0, shaping.w) * saturate(1.0 - margin);
			[branch] if (fray > 0.0)
			{
				const float3 curl = TexWeatherCurl.SampleLevel(TileableSampler, detailPosition / 2400.0, mip).rgb * 2.0 - 1.0;
				displacement = curl * (3200.0 * fray);
			}
			// Shallow cumulus has a compressed vertical envelope. Crossfade
			// stationary Worley lattices to keep the lobes round as it flattens.
			const float cumulus = smoothstep(0.10, 0.20, weather.y) * (1.0 - smoothstep(0.4, 0.6, weather.y));
			const float fine = cumulus * saturate(2.0 * (1.0 - layerDepth / 5700.0));
			noise = 0.0;
			[branch] if (fine < 1.0)
				noise = (1.0 - fine) * WeatherWorley(radial, altitude, 0.01 / properties.w, mip, displacement);
			[branch] if (fine > 0.0)
				noise += fine * WeatherWorley(radial, altitude, 0.02 / properties.w, mip, displacement);
		} else {
			float3 displacement = 0.0;
			// Interpolate the trail's curl strength through the edge type.
			const float curlStrength = saturate(1.0 - 2.0 * weather.y) * shaping.w;
			[branch] if (curlStrength > 0.0)
			{
				const float3 noisePosition = radial * (WeatherNoiseSphereRadius + altitude);
				const float3 curl = TexWeatherCurl.SampleLevel(TileableSampler, noisePosition / 20000.0, mip).rgb * 2.0 - 1.0;
				displacement = curl * (10000.0 * curlStrength);
			}
			noise = WeatherStormWorley(radial, altitude, weather.y, mip, displacement);
		}
		// Keep the condensate under mature crowns level and leave edges irregular.
		const float base = (1.0 - smoothstep(0.06, 0.30, heightInCloud)) * smoothstep(0.43, 0.76, weather.x);
		noise = lerp(noise, 0.548, rounded * base);
		const float residual = margin - erosionDepth * (1.0 - noise);
		erosion.x = residual / ramp;
		shape = saturate(residual / edgeWidth);
		profile = saturate(residual / profileWidth);
	}
	float densityScale = curve.y;
	[branch] if (shape > 0.0 && shapeHeight != height && shallow < 1.0)
	{
		const float densityHeight = lerp(height, shapeHeight, shallow);
		densityScale = TexWeatherProfiles.SampleLevel(TransmittanceSampler, float2(uv.x, (densityHeight * 127.0 + layerRow) / 258.0), 0).g;
	}
	float density = shape * densityScale;
	[branch] if (shaping.x > 0.0 && !skipNoise && profile > 0.0)
	{
		float eroded = profile;
		// Keep NVDF billows on the convective core. The spreading canopy and
		// ice trail take their shape from the larger Worley noise and curl fields.
		float detail = layer == 1u ? shaping.x * smoothstep(0.5, 0.85, weather.y) : shaping.x * (1.0 - 0.9 * rainDeck);
		[branch] if (profile < 1.0 && detail > 0.0)
		{
			const float3 noiseCoordinate = detailPosition / 575.0;  // Source-space detail period.
			const float4 noise = NoiseVoxel.SampleLevel(TileableSampler, noiseCoordinate, detailMip);
			const float2 thresholds = NubisErosionThresholds(noise, profile);
			float threshold = thresholds.y;
			float edgeGain = 0.0;
			if (layer == 0u) {
				// Use the same lifted and softened coordinates as the base profile.
				// Restore full detail above the rise into the body of shallow clouds.
				const float baseDetail = smoothstep(0.0, 2.0 * authoredRise, shapeHeight);
				detail *= lerp(1.0, baseDetail, shallow);
				const float fractus = smoothstep(0.02, 0.08, weather.y) * (1.0 - smoothstep(0.13, 0.23, weather.y));
				// Weak columns disperse first. Keep the billowy response
				// deeper in the support even where the weather map is sparse.
				const float dilute = 1.0 - smoothstep(0.43, 0.84, weather.x * (0.7 + 0.3 * saturate(densityScale)));
				const float fringe = 1.0 - smoothstep(0.30, 0.72, profile);
				// Dry air mixes into the lower exposed skirt. Let its low support
				// break into wisps while keeping the crown and opaque body intact.
				const float skirt = smoothstep(0.08, 0.23, heightInCloud) * (1.0 - smoothstep(0.50, 0.82, heightInCloud));
				const float frayedSkirt = rounded * skirt * dilute * fringe;
				const float wispy = max(frayedSkirt, max(fractus * (1.0 - shallow), shallow * dilute * fringe));
				const float billows = lerp(noise.b, noise.a, 0.30 + 0.40 * profile);
				threshold = lerp(threshold, 0.91 * billows, shallow);
				const float wispThreshold = lerp(thresholds.x, sqrt(thresholds.x), max(shallow * dilute, frayedSkirt));
				threshold = lerp(threshold, wispThreshold, wispy);
				edgeGain = shallow * detail * (1.0 - wispy);
			}
			threshold *= detail;
			erosion.x = (erosion.x - threshold) / max(1.0 - threshold, 1e-6);
			// Tighten the billowy edge without moving its signed zero or
			// changing where the density reaches the fully supported core.
			erosion.x *= (1.0 + edgeGain) / (1.0 + edgeGain * saturate(erosion.x));
			eroded = saturate(erosion.x);
		}
		// Nubis Cubed, p. 118. Density scale is dimensionless here; the
		// Cloud species extinction in inverse metres is applied by the caller.
		// Keep this response continuous through the cap and trail, including
		// where the fine erosion has faded out.
		const float scale = saturate(densityScale);
		const float exponent = lerp(0.3, 0.6, pow(scale, 4.0));
		erosion.y = lerp(1.0, exponent, shaping.x);
		density = pow(eroded * scale, erosion.y);
	}
	return float3(density * baseFade, margin, properties.x);
}

float3 SampleWeatherLayer(float2 weather, float layerDepth, uint layer, float3 radial, float3 detailPosition, float4 shaping, float mip, float detailMip, bool skipNoise)
{
	float2 erosion;
	return SampleWeatherLayer(weather, layerDepth, layer, radial, detailPosition, shaping, mip, detailMip, skipNoise, erosion);
}

#endif
