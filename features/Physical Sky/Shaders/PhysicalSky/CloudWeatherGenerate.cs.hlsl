cbuffer CloudWeatherParameters : register(b1)
{
	float4 Weather;  // base coverage, base type, type variation, convection coverage
	float4 Shape;    // base edge, storm edge, core fraction, painted map weight
	uint4 Base;      // horizontal lattice dimensions and seed
	uint4 Storm;
	float Phase;
	float Weight;
	uint HasPrevious;
	uint Resolve;
	float4 Organization;  // upper flow, cellular organization, outflow reach
	float4 CoverageMix;   // system, clusters, fragments, secondary cover
	float UpdraftOrganization;
	float RegionalCoverageStrength;
	uint RegionalCells;
	uint WriteFiniteWeather;
};

Texture2D<float4> PaintedMap : register(t0);
Texture2D<float4> PreviousMap : register(t1);
Texture2D<float> Systems : register(t2);
Texture2D<float> Clusters : register(t3);
Texture2D<float> Fragments : register(t4);
Texture2D<float> ConvectiveSystems : register(t5);
Texture2D<float4> PreviousFiniteWeather : register(t6);
SamplerState WeatherSampler : register(s0);
RWTexture2D<float4> WeatherMap : register(u0);
RWTexture2D<float4> FiniteWeatherMap : register(u1);

float WeatherLattice(uint2 cell, uint epoch, uint seed)
{
	uint hash = cell.x * 0x9e3779b9u ^ cell.y * 0x85ebca6bu ^ (epoch & 4095u) * 0xc2b2ae35u ^ seed;
	hash = (hash ^ (hash >> 16u)) * 0x7feb352du;
	hash = (hash ^ (hash >> 15u)) * 0x846ca68bu;
	return float(hash >> 8u) * (1.0 / 16777216.0);
}

float WeatherSlice(float2 uv, uint2 cells, uint epoch, uint seed)
{
	const float2 p = uv * cells;
	const uint2 lo = uint2(p);
	const uint2 hi = (lo + 1u) % cells;
	const float2 t = smoothstep(0.0, 1.0, frac(p));
	const float a = lerp(WeatherLattice(lo, epoch, seed), WeatherLattice(uint2(hi.x, lo.y), epoch, seed), t.x);
	const float b = lerp(WeatherLattice(uint2(lo.x, hi.y), epoch, seed), WeatherLattice(hi, epoch, seed), t.x);
	return lerp(a, b, t.y);
}

float WeatherField(float2 uv, uint2 cells, uint seed, float phase)
{
	const uint epoch = (uint)phase;
	const float t = smoothstep(0.0, 1.0, frac(phase));
	const float a = WeatherSlice(uv, cells, epoch, seed);
	const float b = WeatherSlice(uv, cells, epoch + 1u, seed);
	// Keep the field's contrast through the time transition.
	return saturate(0.5 + (lerp(a, b, t) - 0.5) * rsqrt((1.0 - t) * (1.0 - t) + t * t));
}

// Overlap tapered thermals so moving between neighbouring centres leaves
// a continuous slope in both coverage and cloud type.
float ThermalCoverage(float2 uv, uint2 cells, uint seed)
{
	const float2 grid = frac(uv) * cells;
	const int2 cell = int2(grid);
	const float2 f = frac(grid);
	float clear = 1.0;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const int2 offset = int2(x, y);
			const uint2 id = uint2(cell + offset + int2(cells)) % cells;
			const float2 centre = 0.13 + 0.74 * float2(
													WeatherLattice(id, 0, seed), WeatherLattice(id, 0, seed ^ 0x9e3779b9u));
			const float2 delta = float2(offset) + centre - f;
			const float radius = lerp(0.62, 1.0, WeatherLattice(id, 0, seed ^ 0x90673u));
			const float footprint = saturate(1.0 - dot(delta, delta) / (radius * radius));
			clear *= 1.0 - footprint * footprint;
		}
	}
	return 1.0 - clear;
}

float2 WeatherWarp(Texture2D<float> source, float2 uv)
{
	return 0.09 * (float2(source.SampleLevel(WeatherSampler, uv + float2(0.31, 0.67), 0),
					   source.SampleLevel(WeatherSampler, uv + float2(0.73, 0.18), 0)) -
					  0.5);
}

float LocalDevelopment(float2 uv, uint2 cells, uint seed)
{
	return WeatherField(uv, cells, seed, Phase) - WeatherSlice(uv, cells, 0u, seed);
}

float CoverageResponse(float field, float amount, float width)
{
	const float center = 0.64 + 0.44 * (0.55 - amount);
	return smoothstep(center - 0.5 * width, center + 0.5 * width, field);
}

// Coverage and type in the V5 base map have coincident, graded maxima
// under neighbouring turrets. Their skirts join before their summits do.
float2 ThermalGroup(float2 delta, float radius, float angle, float variety)
{
	const float2 axis = float2(cos(angle), sin(angle));
	const float2 p = float2(dot(delta, axis), dot(delta, float2(-axis.y, axis.x))) / radius;
	const float2 flank = p - lerp(float2(0.70, -0.10), float2(1.02, 0.38), variety);
	const float2 rear = p - lerp(float2(-0.70, 0.56), float2(-0.36, 0.82), variety);
	const float2 bud = p - lerp(float2(-0.44, -0.73), float2(-0.67, -0.36), variety);
	const float4 distance2 = float4(dot(p, p), dot(flank, flank), dot(rear, rear), dot(bud, bud));
	const float4 heads = exp2(-distance2 * float4(3.1, 4.3, 5.2, 6.0)) *
	                     float4(1.0, 0.72 + 0.22 * variety, 0.91 - 0.19 * variety, 0.65);
	const float crown = max(max(heads.x, heads.y), max(heads.z, heads.w));
	const float skirt = exp2(-1.15 * dot(p, p));
	// A dry gap must reach zero coverage. A weak positive Gaussian tail
	// integrated over kilometres otherwise fills the gaps between groups.
	const float cover = saturate((0.47 * skirt + 0.53 * crown - 0.17) / 0.83);
	return float2(cover * smoothstep(0.0, 0.12, cover), crown);
}

float2 GenerateCongestus(float2 uv, float fragments)
{
	const uint2 cells = (Base.xy + 1u) / 2u;
	const float2 grid = frac(uv) * cells;
	const int2 cell = int2(grid);
	const float2 f = frac(grid);
	const float3 weights = CoverageMix.xyz / dot(CoverageMix.xyz, 1.0);
	float2 group = 0.0;
	[loop] for (int y = -1; y <= 1; ++y)
	{
		[loop] for (int x = -1; x <= 1; ++x)
		{
			const int2 offset = int2(x, y);
			const uint2 id = uint2(cell + offset + int2(cells)) % cells;
			const float2 center = 0.25 + 0.50 * float2(WeatherLattice(id, 0u, Base.z), WeatherLattice(id, 0u, Base.z ^ 0x37d91u));
			const float2 delta = f - (offset + center);
			const float2 anchor = (id + center) / cells;
			const float variety = WeatherLattice(id, 0u, Base.z ^ 0x90673u);
			const float3 humidity = float3(Systems.SampleLevel(WeatherSampler, anchor, 0),
				Clusters.SampleLevel(WeatherSampler, anchor, 0), Fragments.SampleLevel(WeatherSampler, anchor, 0));
			const float moisture = dot(weights, humidity) + 0.38 * (variety - 0.5) + 0.18 * LocalDevelopment(anchor, cells, Base.z);
			const float vigor = smoothstep(0.57 - 0.40 * Weather.x, 0.84 - 0.48 * Weather.x, moisture);
			const float radius = (lerp(0.27, 0.52, Weather.x) + 0.11 * (Shape.x - 0.18)) * (0.72 + 0.32 * vigor + 0.18 * variety);
			const float angle = 6.2831853 * WeatherLattice(id, 0u, Base.z ^ 0xacc17u);
			group = max(group, vigor * ThermalGroup(delta, radius, angle, variety));
		}
	}
	group.x *= 0.90 + 0.18 * fragments;
	return saturate(group);
}

float4 GenerateConvection(float2 uv, float breakup)
{
	const bool isolated = all(Storm.xy == 1u);
	const float2 grid = uv * Storm.xy;
	const int2 cell = isolated ? int2(0, 0) : int2(floor(grid));
	const float2 shear = Organization.xy * Organization.w;
	const float outflowMoisture = Clusters.SampleLevel(WeatherSampler, uv, 0);
	const float threshold = lerp(0.76, 0.26, Weather.w);
	float cover = 0.0, tower = 0.0;
	float2 feeder = 0.0;
	// Each anvil belongs to an active parent updraft. Sample moisture at the
	// parent centre so a displaced canopy cannot become a second tower.
	const int neighbors = isolated ? 0 : 2;
	[loop] for (int y = -neighbors; y <= neighbors; ++y)
	{
		[loop] for (int x = -neighbors; x <= neighbors; ++x)
		{
			const int2 candidate = cell + int2(x, y);
			if (any(candidate < 0) || any(candidate >= int2(Storm.xy)))
				continue;
			const uint2 id = uint2(candidate);
			const float2 jitter = float2(WeatherLattice(id, 0, Storm.z), WeatherLattice(id, 0, Storm.z ^ 0x37d91u));
			// Leave space around the isolated updraft for the anvil and younger cells.
			const float2 centerInCell = isolated ? lerp(0.36, 0.64, jitter) : lerp(0.15, 0.85, jitter);
			const float2 delta = (grid - (id + centerInCell)) * (isolated ? 4.0 : 1.0);
			const float distance2 = dot(delta, delta);
			if (distance2 >= 4.0)
				continue;
			const float footprint = 1.0 - smoothstep(2.72, 4.0, distance2);
			const float2 anchor = (id + centerInCell) / Storm.xy;
			const float humidity = ConvectiveSystems.SampleLevel(WeatherSampler, anchor, 3);
			const uint epoch = (uint)Phase;
			const uint seed = Storm.z ^ 0x7f4a7c15u;
			const float change = 0.20 * (lerp(WeatherLattice(id, epoch, seed), WeatherLattice(id, epoch + 1u, seed),
											 smoothstep(0.0, 1.0, frac(Phase))) -
											WeatherLattice(id, 0u, seed));
			const float age = frac(Phase * 0.125 + lerp(0.42, 0.56, centerInCell.x));
			const float life = smoothstep(0.05, 0.40, age) * (1.0 - smoothstep(0.61, 0.92, age));
			const float vigor = isolated ? smoothstep(0.0, 0.34, Weather.w) * life :
			                               smoothstep(threshold - 0.08, threshold + 0.17, humidity + change);
			const float maturity = isolated ? smoothstep(0.33, 0.50, age) * (1.0 - smoothstep(0.83, 0.98, age)) :
			                                  smoothstep(0.70, 0.95, vigor);
			if (max(vigor, maturity) <= 0.0)
				continue;
			const float radius = lerp(0.13, 0.40, Shape.z) * (0.72 + 0.5 * vigor);
			const float roughEdge = 0.08 * (breakup - 0.5);
			// Younger thermals grow alongside the mature column at unequal
			// heights. They feed the same canopy without duplicating its peak.
			const float angle = 6.2831853 * WeatherLattice(id, 0, Storm.z ^ 0x90673u);
			const float2 axis = float2(cos(angle), sin(angle));
			const float2 centre = delta + 0.15 * radius * axis;
			const float coreDistance2 = dot(centre, centre) / (radius * radius);
			const float2 foothills = ThermalGroup(delta, (isolated ? 1.12 : 1.45) * radius, angle, centerInCell.x);
			// Preserve a broad, upright liquid core under the spreading ice.
			const float envelope = (1.0 - smoothstep(0.08, 1.35, coreDistance2)) * (0.76 + 0.24 * foothills.y);
			const float core = vigor * (isolated ? envelope : exp2(-2.0 * coreDistance2));
			feeder = max(feeder, footprint * smoothstep(0.32, 0.92, vigor) * vigor * foothills);
			if (isolated) {
				// Adjacent towers reach different heights and peak at different ages.
				const float2 side = float2(-axis.y, axis.x);
				const float earlier = smoothstep(0.17, 0.44, age) * (1.0 - smoothstep(0.58, 0.81, age));
				const float later = smoothstep(0.40, 0.68, age) * (1.0 - smoothstep(0.83, 1.0, age));
				const float strength = smoothstep(0.0, 0.34, Weather.w);
				feeder = max(feeder, footprint * 0.77 * earlier * strength * ThermalGroup(delta - radius * (2.2 * axis + 0.45 * side), 0.82 * radius, angle + 0.75, centerInCell.y));
				feeder = max(feeder, footprint * 0.72 * later * strength * ThermalGroup(delta + radius * (0.85 * axis + 2.9 * side), 0.62 * radius, angle - 1.6, centerInCell.x));
			}
			const float2 plume = delta - (isolated ? 0.26 : 0.39) * shear;
			const float alongShear = dot(plume, shear);
			const float2 tapered = plume - (isolated ? 0.63 * max(alongShear, 0.0) : 0.30 * alongShear) * shear;
			const float reach = radius + lerp(0.09, 0.39, Organization.w) * max(vigor, maturity);
			// Lower thermals can grow before the core reaches the V5 edge/trail
			// profiles. A young cell has no detached ice canopy overhead.
			const float2 capPosition = tapered / reach;
			const float rim = length(capPosition);
			const float mixing = smoothstep(0.22, 0.95, rim);
			const float2 direction = capPosition / max(rim, 0.5);
			const float along = dot(direction, axis);
			const float across = dot(direction, float2(-axis.y, axis.x));
			const float alongSquared = along * along;
			// Broad lobes on the spreading shoulder, oriented per parent updraft.
			const float lobes = 0.10 * along * (4.0 * alongSquared - 3.0) +
			                    0.05 * across * (16.0 * alongSquared * alongSquared - 12.0 * alongSquared + 1.0);
			const float outflowEdge = 0.36 * (0.5 - outflowMoisture) + (isolated ? 0.24 * (0.5 - breakup) : 0.0);
			const float cap = maturity * (isolated ? smoothstep(0.0, 0.34, Weather.w) : vigor) * (1.0 - smoothstep(0.30, 1.0 + 0.18 * Shape.y, rim + mixing * (roughEdge / reach + outflowEdge - lobes)));
			cover = max(cover, footprint * max((isolated ? 1.0 : maturity) * core, cap));
			// Feed the rounded edge profile with fresh outflow. The outer,
			// depleted part of the same canopy blends into the low trail type.
			const float shoulder = 0.5 * smoothstep(0.16, 0.57, cap);
			tower = max(tower, footprint * max(core, shoulder));
		}
	}
	const float onset = smoothstep(0.0, 0.18, Weather.w);
	return float4(cover * onset, tower, feeder * onset);
}

float2 GenerateBackgroundWeather(float2 uv, out float fragments)
{
	const float2 field = uv + WeatherWarp(Systems, uv) * min(1.0, 6.0 / float2(Base.xy));
	const float system = Systems.SampleLevel(WeatherSampler, field, 0);
	const float cluster = Clusters.SampleLevel(WeatherSampler, field, 0);
	fragments = Fragments.SampleLevel(WeatherSampler, field, 0);
	const float evolution = 0.115 * LocalDevelopment(uv, Base.xy, Base.z ^ 0x2e5be93u);
	const float3 weights = CoverageMix.xyz / dot(CoverageMix.xyz, 1.0);
	float moisture = dot(weights, float3(system, cluster, fragments));
	float typeVariation = 1.6 * (cluster - 0.5) + 0.8 * (system - 0.5);
	// Cloud coverage retains small structure through
	// clustered and closed cells. Organize the multiscale moisture field
	// without replacing it with nearest-cell borders or binary updrafts.
	[branch] if (UpdraftOrganization > 0.0 || Organization.z > 0.0)
	{
		const float thermal = ThermalCoverage(field + (cluster - 0.5) * float2(0.42, -0.27) / Base.xy, Base.xy, Base.z);
		const float anomaly = thermal - 0.5;
		moisture += UpdraftOrganization * 0.30 * anomaly;
		typeVariation += UpdraftOrganization * 0.65 * anomaly;
		const float deck = 0.48 + 0.47 * (system - 0.5) + 0.36 * (cluster - 0.5) + 0.58 * anomaly + 0.08 * (fragments - 0.5);
		moisture = lerp(moisture, deck, Organization.z);
		typeVariation += Organization.z * 0.55 * anomaly;
	}
	const float width = 0.36 + (Shape.x - 0.18) * 0.34;
	const float primary = CoverageResponse(moisture + evolution, Weather.x, width);
	float coverage = primary;
	[branch] if (CoverageMix.w > 0.0)
	{
		const float patch = 0.65 * Clusters.SampleLevel(WeatherSampler, field + float2(0.39, 0.72), 0) + 0.35 * Fragments.SampleLevel(WeatherSampler, field + float2(0.84, 0.27), 0);
		const float gain = lerp(0.46, 1.0, smoothstep(0.27, 0.61, system));
		const float secondary = gain * CoverageResponse(patch + 0.7 * evolution, Weather.x, width * 0.83);
		coverage = max(primary, CoverageMix.w * secondary);
	}
	coverage *= 0.97 * smoothstep(0.0, 0.20, Weather.x);
	// Join the moist cells into a deck, leaving dry gaps between groups.
	const float deckMoisture = smoothstep(0.27, 0.69, system + evolution);
	const float deckCover = Organization.z * smoothstep(0.46, 0.88, Weather.x) * (0.10 + 0.65 * deckMoisture);
	coverage += (1.0 - coverage) * deckCover;
	const float growth = clamp(typeVariation + evolution, -1.0, 1.0);
	const float typeFloor = min(0.0, floor(Weather.y));
	float baseType = Weather.y + growth * min(Weather.z, growth > 0.0 ? 1.0 - Weather.y : Weather.y - typeFloor);
	const float deep = UpdraftOrganization * (1.0 - Organization.z) * smoothstep(0.32, 0.53, Weather.y);
	[branch] if (deep > 0.0)
	{
		const float2 thermals = GenerateCongestus(field, fragments);
		const float amount = smoothstep(0.0, 0.20, Weather.x);
		const float summit = 1.8 * thermals.y - 0.8;
		const float thermalType = Weather.y + summit * min(Weather.z, summit > 0.0 ? 1.0 - Weather.y : Weather.y);
		coverage = lerp(coverage, max(0.30 * coverage, amount * thermals.x), deep);
		baseType = lerp(baseType, thermalType, deep);
	}
	coverage = lerp(coverage, 0.83 + 0.17 * saturate(moisture + evolution), smoothstep(0.90, 1.0, Weather.x));
	[branch] if (RegionalCoverageStrength > 0.0)
	{
		const float regionalMoisture = WeatherField(uv, RegionalCells.xx, Base.z ^ 0x976e21b7u, Phase);
		const float regionalGain = lerp(0.16, 1.35, smoothstep(0.19, 0.83, regionalMoisture));
		coverage = saturate(coverage * lerp(1.0, regionalGain, RegionalCoverageStrength));
	}

	return float2(coverage, baseType);
}

float4 GenerateFiniteWeather(float2 uv, float2 background, float fragments)
{
	const float2 border = min(uv, 1.0 - uv);
	// Both maps agree throughout the bilinear filter footprint at the boundary.
	const float fade = smoothstep(0.005, 0.12, min(border.x, border.y));
	if (fade <= 0.0)
		return float4(background, 0.0, 0.0);
	const float warp = all(Storm.xy == 1u) ? 0.32 : 1.0;
	const float2 stormField = uv + warp * WeatherWarp(ConvectiveSystems, uv) * min(1.0, 4.0 / float2(Storm.xy));
	const float4 convection = GenerateConvection(stormField, fragments);
	const float2 feeder = convection.zw * (UpdraftOrganization * smoothstep(0.0, 0.10, Weather.x));
	const float coverage = lerp(background.x, max(background.x, feeder.x), fade);
	const float baseType = lerp(background.y, Weather.y + min(Weather.z, 1.0 - Weather.y), fade * feeder.y);
	const float stormCoverage = fade * convection.x;
	return float4(coverage, baseType, stormCoverage, stormCoverage > 0.0 ? convection.y : 0.0);
}

float4 PremultiplyType(float4 weather)
{
	weather.yw *= weather.xz;
	return weather;
}

float4 ClampWeatherField(float4 weather)
{
	return float4(saturate(weather.x), clamp(weather.y, -1.0, 1.0), saturate(weather.zw));
}

float4 ResolveType(float4 weather)
{
	weather.yw /= max(weather.xz, 1e-6);
	return ClampWeatherField(weather);
}

float4 AccumulateWeather(float4 field, Texture2D<float4> previous, uint2 pixel)
{
	if (Resolve != 0u && HasPrevious == 0u && Weight == 1.0)
		return field;
	// Keep each map's type normalized by the coverage in that map.
	float4 sum = PremultiplyType(field) * Weight;
	if (HasPrevious != 0u)
		sum += previous.Load(int3(pixel, 0));
	return Resolve != 0u ? ResolveType(sum) : sum;
}

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 size;
	WeatherMap.GetDimensions(size.x, size.y);
	if (any(pixel >= size))
		return;

	const float2 uv = (float2(pixel) + 0.5) / size;
	float4 field = 0.0;
	float4 finite = 0.0;
	[branch] if (Shape.w < 1.0)
	{
		float fragments;
		field.xy = GenerateBackgroundWeather(uv, fragments);
		if (WriteFiniteWeather != 0u)
			finite = GenerateFiniteWeather(uv, field.xy, fragments);
	}
	[branch] if (Shape.w > 0.0)
	{
		const float4 painted = ClampWeatherField(PaintedMap.SampleLevel(WeatherSampler, uv, 0));
		const float4 premultipliedPaint = PremultiplyType(painted);
		field = Shape.w == 1.0 ? painted : ResolveType(lerp(PremultiplyType(field), premultipliedPaint, Shape.w));
		if (WriteFiniteWeather != 0u)
			finite = Shape.w == 1.0 ? painted : ResolveType(lerp(PremultiplyType(finite), premultipliedPaint, Shape.w));
	}
	WeatherMap[pixel] = AccumulateWeather(field, PreviousMap, pixel);
	[branch] if (WriteFiniteWeather != 0u)
		FiniteWeatherMap[pixel] = AccumulateWeather(finite, PreviousFiniteWeather, pixel);
}
