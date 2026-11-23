#ifndef PHYSICAL_SKY_WEATHER_BOUNDS_HLSLI
#define PHYSICAL_SKY_WEATHER_BOUNDS_HLSLI

static const uint CloudWeatherTileSize = 8u;
static const uint CloudWeatherTiles = 64u;
static const uint CloudWeatherSlices = 32u;

#ifdef BUILD_WEATHER_BOUNDS
Texture2D<float2> TexWeatherProfileBounds : register(t5);
RWTexture2D<float2> RWWeatherProfileBounds : register(u0);
RWTexture2D<uint> RWWeatherCloudBounds : register(u0);

static const uint WeatherProfileTypesPerBlock = 8u;
static const uint WeatherProfileHeightsPerBlock = 4u;
static const uint WeatherProfileHeightBlocks = 32u;

[numthreads(8, 8, 1)] void buildWeatherProfileBounds(uint2 block : SV_DispatchThreadID) {
	const uint layer = block.y / WeatherProfileHeightBlocks;
	const uint layerRow = layer * 129u;
	const uint2 origin = uint2(block.x * WeatherProfileTypesPerBlock,
		(block.y % WeatherProfileHeightBlocks) * WeatherProfileHeightsPerBlock + layerRow);
	float2 upper = 0.0;
	[unroll] for (uint x = 0u; x < WeatherProfileTypesPerBlock; ++x)
	{
		// Support metadata also limits the low layer's coverage extension.
		const float support = TexWeatherProfiles.Load(int3(origin.x + x, layerRow + 128u, 0)).y;
		upper.y = max(upper.y, isfinite(support) ? support : 65504.0);
		[unroll] for (uint y = 0u; y < WeatherProfileHeightsPerBlock; ++y)
		{
			const float coverage = TexWeatherProfiles.Load(int3(origin + uint2(x, y), 0)).r;
			upper.x = max(upper.x, isfinite(coverage) ? coverage : 65504.0);
		}
	}
	RWWeatherProfileBounds[block] = upper;
}

bool CloudWeatherLayerMayBeOccupied(float coverage, float2 type, float2 height, float2 baseShaping, uint layer, bool roundedCrown)
{
	if (coverage <= 0.0 || height.x > 1.0)
		return false;
	// Leave room for the arithmetic and subtexel rounding of the sampled path.
	const float threshold = 1.0 + WeatherMinimumCoverageMargin - 0.002;
	const uint2 typeBlocks = uint2(floor(WeatherProfileColumn(type.x)), ceil(WeatherProfileColumn(type.y))) / WeatherProfileTypesPerBlock;
	if (layer == 0u || type.y > 0.5) {
		float maxSupport = 0.0;
		[loop] for (uint column = typeBlocks.x; column <= typeBlocks.y; ++column)
			maxSupport = max(maxSupport, TexWeatherProfileBounds.Load(int3(column, layer * WeatherProfileHeightBlocks, 0)).y);
		if (maxSupport > 1.0)
			return true;
		const float rise = max(WeatherBaseRiseFraction * maxSupport, 1e-4);
		// Include both the original and shallow coverage rises.
		const float maxWidth = max(WeatherBaseRiseWidth(rise, baseShaping.y), rise);
		const float maxCrest = min(baseShaping.x, 2.0 * rise) + maxWidth;
		const float end = max(maxCrest, rise) + rise;
		if (height.x < end) {
			// The monotone warp rejoins the original curve at its end.
			// Include every height the warped part of this slice can reach.
			height.x = max(0.0, height.x - max(maxCrest, rise));
			height.y = max(height.y, min(height.y + rise, end));
		}
		// Crown coverage decreases with normalized height. The largest support
		// and lowest warped height bound every rounded profile in this tile.
		if (roundedCrown && type.y > 0.025 && type.x < 0.62 &&
			coverage + WeatherRoundedCrownCoverage(height.x / max(maxSupport, 1e-4)) >= threshold)
			return true;
	}
	const uint2 heightBlocks = uint2(floor(saturate(height.x) * 127.0), ceil(saturate(height.y) * 127.0)) / WeatherProfileHeightsPerBlock;
	[loop] for (uint row = heightBlocks.x; row <= heightBlocks.y; ++row)
	{
		[loop] for (uint column = typeBlocks.x; column <= typeBlocks.y; ++column)
		{
			if (coverage + TexWeatherProfileBounds.Load(int3(column, row + layer * WeatherProfileHeightBlocks, 0)).x >= threshold)
				return true;
		}
	}
	return false;
}

groupshared float4 CloudWeatherMax[64];
groupshared float2 CloudWeatherMinType[64];
groupshared uint CloudWeatherEmptySlices[CloudWeatherSlices];

[numthreads(8, 8, 1)] void buildWeatherCloudBounds(uint lane : SV_GroupIndex, uint2 tile : SV_GroupID) {
	const uint footprint = CloudWeatherTileSize + 2u;
	const int wrap = int(CloudWeatherTiles * CloudWeatherTileSize - 1u);
	float4 hi = float4(0.0, -1.0, 0.0, 0.0);
	float2 lo = 1.0;
	// Include the neighboring texels touched by bilinear filtering at tile edges.
	[unroll] for (uint offset = 0u; offset < footprint * footprint; offset += 64u)
	{
		const uint i = lane + offset;
		if (i < footprint * footprint) {
			const int2 texel = int2(tile * CloudWeatherTileSize + uint2(i % footprint, i / footprint)) - 1;
			const float4 weather = ClampCloudWeather(TexCloudWeather.Load(int3(texel & wrap, 0)));
			hi = max(hi, weather);
			lo = min(lo, weather.yw);
			// One conservative bound covers the local composition and the background beyond it.
			if (FiniteCloudWeather != 0u) {
				const float4 finite = ClampCloudWeather(TexFiniteCloudWeather.Load(int3(texel & wrap, 0)));
				hi = max(hi, finite);
				lo = min(lo, finite.yw);
			}
		}
	}
	CloudWeatherMax[lane] = hi;
	CloudWeatherMinType[lane] = lo;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32u; stride > 0u; stride >>= 1)
	{
		if (lane < stride) {
			CloudWeatherMax[lane] = max(CloudWeatherMax[lane], CloudWeatherMax[lane + stride]);
			CloudWeatherMinType[lane] = min(CloudWeatherMinType[lane], CloudWeatherMinType[lane + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane < CloudWeatherSlices) {
		const VolumetricCloudData info = VolumetricCloudBuffer[0];
		const float bottom = info.lowCloudBaseAltitude;
		const float2 h = (float(lane) + float2(0.0, 1.0)) * ((info.lowCloudTopAltitude - bottom) / CloudWeatherSlices);
		const float2 padding = float2(-0.002, 0.002);
		const float2 depth = float2(info.weatherBaseTopAltitude, info.weatherStormTopAltitude) - bottom;
		const float2 base = h / depth.x + padding;
		const float2 anvil = h / depth.y + padding;
		const float2 baseShaping = float2(CloudBaseHeightVariation, info.weatherDetail.z) / GAME_UNIT_TO_M;
		const float4 coverage = CloudWeatherMax[0];
		const float2 type = CloudWeatherMinType[0];
		const bool roundedCrown = depth.x * GAME_UNIT_TO_M / info.weatherWorldScale < WeatherRoundedCrownMaxDepth;
		const bool occupied = CloudWeatherLayerMayBeOccupied(coverage.x, float2(type.x, coverage.y), base, baseShaping / depth.x, 0u, roundedCrown) ||
		                      CloudWeatherLayerMayBeOccupied(coverage.z, float2(type.y, coverage.w), anvil, baseShaping / depth.y, 1u, false);
		CloudWeatherEmptySlices[lane] = occupied ? 0u : 1u << lane;
	}
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = CloudWeatherSlices / 2u; stride > 0u; stride >>= 1)
	{
		if (lane < stride)
			CloudWeatherEmptySlices[lane] |= CloudWeatherEmptySlices[lane + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane == 0u)
		RWWeatherCloudBounds[tile] = CloudWeatherEmptySlices[0];
}
#endif

#ifdef WEATHER_EMPTY_BOUNDS
Texture2D<uint> TexWeatherCloudBounds : register(t5);

bool CloudWeatherEmptyAt(float3 position, CloudLayer cloud, VolumetricCloudData info, WeatherDensityParameters parameters)
{
	const float altitude = WeatherCloudAltitude(position, parameters.curvatureRadius);
	if (altitude <= cloud.lowestAltitude || altitude >= cloud.highestAltitude)
		return true;
	const float height = (altitude - parameters.baseAltitude) / (info.lowCloudTopAltitude - parameters.baseAltitude);
	const uint slice = min(uint(saturate(height) * CloudWeatherSlices), CloudWeatherSlices - 1u);
	const float2 field = (position.xy - parameters.windOffset) * GAME_UNIT_TO_M;
	const float2 uv = CloudWeatherUV(field * parameters.weatherFrequency, WeatherProceduralWeight);
	const int2 tile = int2(floor(frac(uv) * CloudWeatherTiles));
	return (TexWeatherCloudBounds.Load(int3(tile, 0)) & (1u << slice)) != 0u;
}
#endif

#endif
