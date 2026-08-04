// Community Shaders — Physical Sky volumetric cloud map generator
//
// Replaces the former CPU weather-map builder. Everything the volumetric cloud
// renderer reads as a 2D input is produced here so that a parameter change costs
// a handful of small dispatches instead of a synchronous 512^2 CPU rebuild on the
// render thread.
//
// Passes (selected by CLOUDMAPGEN):
//   0 generateFields   -> intermediate scalar fields (morphology, species score,
//                         stratiform score, cloud-body radial coordinate)
//   1 buildHistogram   -> 256-bin histograms of those fields
//   2 solveThresholds  -> quantile solve, turning "I want 38% coverage" into the
//                         exact field threshold that produces 38% of the map area
//   3 composeMaps      -> Low/High weather + Sc cell + High cell/warp/wisp
//   4 composeProfile   -> vertical profile LUT (Cu/Tcu/Cb)
//
// Every field tiles seamlessly over the unit square. The consumer samples the
// weather map with a wrapping sampler, so there is no map boundary and no edge
// fade; the finite map is a repeating synoptic pattern rather than a rectangle
// of cloud floating in an empty sky.

#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#ifndef CLOUDMAPGEN
#	define CLOUDMAPGEN 0
#endif

#include "Common/Random.hlsli"

cbuffer CloudMapGenCB : register(b1)
{
	uint2 weatherDim;
	uint2 profileDim;

	uint cellPeriod;    // convective cells across the map; derived from Cloud Size
	uint seed;          // pattern seed
	uint solveRound;    // 0 = coverage, 1 = Sc split, 2 = species splits
	float skyCoverage;  // target fraction of map area carrying low cloud

	float highCoverage;
	float instability;
	float character;  // 0 = convective, 1 = stratiform
	float breakup;    // convective cell separation

	float coverageEdgeWidth;
	float highCoverageEdgeWidth;
	float frontStrength;
	float domeStrength;  // profile LUT radial dome falloff

	float2 frontNormal;   // integer lattice vector, keeps fronts tileable
	float2 frontTangent;  // perpendicular integer vector

	float scShare;   // stratocumulus fraction of covered area
	float cuShare;   // cumulus fraction of the non-Sc covered area
	float tcuShare;  // towering-cumulus fraction, measured from the same base
	float asShare;   // altostratus fraction of high-covered area

	float cumulusDepth;
	float toweringCumulusDepth;
	float cumulonimbusDepth;
	float layerDepth;
};

// A literal is required for numthreads and the groupshared array bound.
#define HISTOGRAM_BINS 256
static const uint kHistogramBins = HISTOGRAM_BINS;

// Histogram group indices. Round 0 fills 0-1, round 1 fills 2-4.
static const uint kHistLowPotential = 0u;
static const uint kHistHighPotential = 1u;
static const uint kHistScScore = 2u;
static const uint kHistTypeScore = 3u;
static const uint kHistHighType = 4u;

// Threshold slots written by solveThresholds and consumed by composeMaps.
static const uint kThreshLowCoverage = 0u;
static const uint kThreshHighCoverage = 1u;
static const uint kThreshSc = 2u;
static const uint kThreshCu = 3u;
static const uint kThreshTcu = 4u;
static const uint kThreshAs = 5u;
static const uint kThresholdCount = 6u;

// ---------------------------------------------------------------------------
// Periodic noise primitives
//
// Seamless tiling is a hard requirement: the renderer wraps weather UV, so any
// lattice lookup must fold back onto itself at the period boundary. Every
// primitive below takes an explicit period and wraps the integer cell before
// hashing.
// ---------------------------------------------------------------------------

// Folds a lattice cell back into [0, period). Biasing by a whole multiple of the
// period first keeps the operand unsigned, since a signed modulus is both slower
// and sign-dependent for the negative cells produced by the -1 neighbour offset.
uint2 WrapCell(int2 cell, uint2 period)
{
	const uint2 p = max(period, 1u);
	return (uint2(cell + int2(p) * 64) % p);
}

float Hash11(uint2 cell, uint hashSeed)
{
	return Random::pcg2d(cell + uint2(hashSeed, hashSeed * 747796405u)).x * (1.0 / 4294967296.0);
}

float2 Hash21(uint2 cell, uint hashSeed)
{
	return Random::pcg2d(cell + uint2(hashSeed, hashSeed * 747796405u)) * (1.0 / 4294967296.0);
}

float3 Hash31(uint2 cell, uint hashSeed)
{
	return Random::pcg3d(uint3(cell, hashSeed)) * (1.0 / 4294967296.0);
}

float PeriodicValue(float2 p, uint2 period, uint hashSeed)
{
	const int2 i = int2(floor(p));
	const float2 f = frac(p);
	const float2 s = f * f * (3.0 - 2.0 * f);
	const float a = Hash11(WrapCell(i + int2(0, 0), period), hashSeed);
	const float b = Hash11(WrapCell(i + int2(1, 0), period), hashSeed);
	const float c = Hash11(WrapCell(i + int2(0, 1), period), hashSeed);
	const float d = Hash11(WrapCell(i + int2(1, 1), period), hashSeed);
	return lerp(lerp(a, b, s.x), lerp(c, d, s.x), s.y);
}

// uv is expected in [0,1); the lattice period doubles per octave so the result
// stays periodic on the unit square.
float PeriodicFbm(float2 uv, uint basePeriod, uint octaves, uint hashSeed)
{
	float amplitude = 0.5;
	float sum = 0.0;
	uint period = max(basePeriod, 1u);
	[loop] for (uint octave = 0u; octave < octaves; ++octave)
	{
		sum += amplitude * PeriodicValue(uv * period, uint2(period, period), hashSeed + octave * 17u);
		period *= 2u;
		amplitude *= 0.5;
	}
	return saturate(sum);
}

// Anisotropic variant. `coord` must be an integer linear combination of uv so
// that a unit step in u or v moves the lattice by a whole number of cells.
float PeriodicFbmAniso(float2 coord, uint2 basePeriod, uint octaves, uint hashSeed)
{
	float amplitude = 0.5;
	float sum = 0.0;
	uint2 period = max(basePeriod, 1u);
	[loop] for (uint octave = 0u; octave < octaves; ++octave)
	{
		sum += amplitude * PeriodicValue(coord * period, period, hashSeed + octave * 17u);
		period *= 2u;
		amplitude *= 0.5;
	}
	return saturate(sum);
}

// Bright cell centres, dark cell boundaries. Matches the thickness convention
// the renderer expects from the Sc and high-cloud cell maps.
float PeriodicCell(float2 uv, uint cellCount, uint hashSeed)
{
	const uint count = max(cellCount, 1u);
	const float2 p = uv * count;
	const int2 base = int2(floor(p));
	float nearestSq = 8.0;
	[unroll] for (int oy = -1; oy <= 1; ++oy)
	{
		[unroll] for (int ox = -1; ox <= 1; ++ox)
		{
			const int2 cell = base + int2(ox, oy);
			const float2 jitter = Hash21(WrapCell(cell, uint2(count, count)), hashSeed);
			const float2 featurePos = float2(cell) + jitter;
			const float2 delta = featurePos - p;
			nearestSq = min(nearestSq, dot(delta, delta));
		}
	}
	return saturate(1.0 - sqrt(nearestSq) * 1.25);
}

// ---------------------------------------------------------------------------
// Morphology fields
//
// Real low cloud is not one noise function. It is discrete convective cells,
// continuous stratiform sheets, and frontal bands, each with its own spatial
// signature. Summing octaves of FBM averages those signatures into a Gaussian
// haze with no gaps and no cloud-body structure, which is exactly what the
// previous generator produced.
// ---------------------------------------------------------------------------

struct ConvectiveSample
{
	float field;   // 0 in the gaps between cells, 1 at a vigorous cell core
	float radial;  // 0 at the cell centre, 1 at its outer edge
};

ConvectiveSample SampleConvective(float2 uv, uint period, uint hashSeed)
{
	ConvectiveSample result;
	result.field = 0.0;
	result.radial = 1.0;

	const uint cells = max(period, 2u);
	const float2 p = uv * cells;
	const int2 base = int2(floor(p));

	// Cells smaller than half a lattice step cannot touch their neighbours, so
	// the radius range is what actually opens gaps between cloud bodies. Coverage
	// is re-solved by quantile afterwards, so widening the gaps does not darken
	// the sky - it redistributes the same cloud area into fewer, denser bodies.
	const float radiusMin = lerp(0.60, 0.24, breakup);
	const float radiusMax = lerp(0.95, 0.46, breakup);

	[unroll] for (int oy = -1; oy <= 1; ++oy)
	{
		[unroll] for (int ox = -1; ox <= 1; ++ox)
		{
			const int2 cell = base + int2(ox, oy);
			const uint2 wrapped = WrapCell(cell, uint2(cells, cells));
			const float3 h = Hash31(wrapped, hashSeed);
			const float2 featurePos = float2(cell) + 0.5 + (h.xy - 0.5) * 0.85;
			const float radius = lerp(radiusMin, radiusMax, h.z);

			// Gate the whole cell on synoptic moisture sampled at its centre.
			// Gating per pixel instead would slice cloud bodies wherever the
			// moisture contour crosses them, destroying the discrete blobs.
			const float moisture = PeriodicFbm(featurePos / cells, 3u, 2u, hashSeed + 811u);
			const float activation = saturate((moisture - 0.30) / 0.32);

			const float d = length(p - featurePos) / max(radius, 1e-3);
			const float falloff = saturate(1.0 - d);
			const float field = activation * falloff * falloff * (3.0 - 2.0 * falloff);

			if (field > result.field) {
				result.field = field;
				result.radial = saturate(d);
			}
		}
	}
	return result;
}

float SampleStratiform(float2 uv, uint hashSeed)
{
	const float broad = PeriodicFbm(uv, 2u, 4u, hashSeed + 101u);
	const float meso = PeriodicFbm(uv, 6u, 3u, hashSeed + 211u);
	// Open cells (sinking centres, cloudy rims) form over relatively unstable
	// surfaces; closed cells (cloudy centres) form under stable subsidence.
	const float closedCell = PeriodicCell(uv, 7u, hashSeed + 307u);
	const float cellModulation = lerp(closedCell, 1.0 - closedCell, saturate(instability));
	const float sheet = saturate(broad * 0.72 + meso * 0.28);
	return saturate(sheet * lerp(0.55, 1.0, cellModulation) * 1.35);
}

float SampleFrontal(float2 uv, uint hashSeed)
{
	// Fronts are narrow contours of a large-scale field, elongated across the
	// flow. The band normal is quantised to an integer lattice vector on the CPU
	// so the anisotropic stretch keeps the pattern tileable.
	const float across = dot(uv, frontNormal);
	const float along = dot(uv, frontTangent);
	const float driver = PeriodicFbmAniso(float2(across, along), uint2(1u, 4u), 3u, hashSeed + 401u);
	const float distanceFromCore = abs(driver - 0.5) * 2.0;
	const float core = 1.0 - smoothstep(0.06, 0.26, distanceFromCore);
	const float breakupModulation = 0.35 + 0.65 * smoothstep(0.30, 0.70, PeriodicFbm(uv, 9u, 3u, hashSeed + 503u));
	return core * breakupModulation;
}

struct CloudFields
{
	float4 low;   // potential, species score, stratiform score, body radial
	float4 high;  // potential, species score, scatter weight, unused
};

CloudFields EvaluateFields(float2 uv)
{
	CloudFields fields;

	const ConvectiveSample convective = SampleConvective(uv, cellPeriod, seed);
	const float stratiform = SampleStratiform(uv, seed);
	const float frontal = SampleFrontal(uv, seed);

	// Morphology blend, then fronts. Fronts add cloud where there is none rather
	// than saturating regions that are already overcast.
	const float morphology = lerp(convective.field, stratiform, saturate(character));
	const float potential = saturate(morphology + frontal * frontStrength * (1.0 - morphology * 0.5));

	const float mesoConvection = PeriodicFbm(uv, 5u, 3u, seed + 601u);
	const float broadMoisture = PeriodicFbm(uv, 2u, 3u, seed + 617u);
	// Frontal lift produces layered cloud, not deep convection, so it lowers the
	// species score even though it raises coverage.
	const float typeScore = saturate(convective.field * 0.50 + mesoConvection * 0.35 + broadMoisture * 0.15 - frontal * 0.25);

	const float stableNoise = PeriodicFbm(uv, 4u, 3u, seed + 619u);
	const float scScore = saturate(0.15 + stratiform * 0.45 + frontal * 0.35 - convective.field * 0.50 + stableNoise * 0.30);

	// Stratiform sheets have no dome. Pulling their radial toward the cloud core
	// keeps the profile LUT near full depth so they read as flat slabs.
	const float radial = lerp(convective.radial, 0.30, saturate(character));

	fields.low = float4(potential, typeScore, scScore, radial);

	const float highBroad = PeriodicFbm(uv, 2u, 4u, seed + 701u);
	const float highMeso = PeriodicFbm(uv, 5u, 3u, seed + 809u);
	// Deep convection under a broad moist column spreads an anvil downwind.
	const float anvil = smoothstep(0.55, 0.85, typeScore) * smoothstep(0.30, 0.70, potential);
	const float highPotential = saturate(highBroad * 0.55 + highMeso * 0.20 + frontal * 0.35 + anvil * 0.35);
	const float highType = saturate(0.10 + PeriodicFbm(uv, 7u, 3u, seed + 907u) * 0.70 + anvil * 0.45 - frontal * 0.30);
	const float highScatter = saturate(highBroad * 0.45 + frontal * 0.25 + anvil * 0.30);

	fields.high = float4(highPotential, highType, highScatter, 0.0);
	return fields;
}

// Thinnest coverage written inside a selected region. The renderer rejects
// coverage below 0.1 and turns coverage into a density threshold of
// (1 - coverage), so a floor that is too low would select map area that never
// resolves into visible cloud and break the "coverage means coverage" contract.
static const float kCoverageFloor = 0.35;
static const float kCoverageCeiling = 0.85;

// Coverage remap. A pixel is cloudy exactly when its potential exceeds the
// solved threshold, so the covered area fraction equals the quantile the
// threshold was solved for. Above the threshold, coverage ramps across the
// remaining headroom: cloud cores go solid while their rims stay broken.
float ApplyCoverage(float potential, float threshold, float edgeWidth)
{
	if (potential <= threshold)
		return 0.0;
	const float headroom = max(1.0 - threshold, 1e-3);
	const float t = saturate((potential - threshold) / (max(edgeWidth, 0.05) * headroom));
	return lerp(kCoverageFloor, kCoverageCeiling, t);
}

uint ScoreBin(float value)
{
	return min(uint(saturate(value) * float(kHistogramBins)), kHistogramBins - 1u);
}

// ===========================================================================
#if CLOUDMAPGEN == 0  // generateFields
// ===========================================================================

RWTexture2D<float4> RWFieldLow : register(u0);
RWTexture2D<float4> RWFieldHigh : register(u1);

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= weatherDim))
		return;
	const float2 uv = (float2(tid) + 0.5) / float2(weatherDim);
	const CloudFields fields = EvaluateFields(uv);
	RWFieldLow[tid] = fields.low;
	RWFieldHigh[tid] = fields.high;
}

// ===========================================================================
#elif CLOUDMAPGEN == 1  // buildHistogram
// ===========================================================================

Texture2D<float4> TexFieldLow : register(t0);
Texture2D<float4> TexFieldHigh : register(t1);
StructuredBuffer<float> Thresholds : register(t2);
RWByteAddressBuffer RWHistogram : register(u0);

void Accumulate(uint group, float value)
{
	uint ignored;
	RWHistogram.InterlockedAdd(4u * (group * kHistogramBins + ScoreBin(value)), 1u, ignored);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= weatherDim))
		return;

	const float4 low = TexFieldLow[tid];
	const float4 high = TexFieldHigh[tid];

	if (solveRound == 0u) {
		Accumulate(kHistLowPotential, low.r);
		Accumulate(kHistHighPotential, high.r);
		return;
	}

	// Rounds 1 and 2 measure their scores over the area the coverage solve
	// selected, so the requested shares are shares of cloudy sky rather than of
	// the whole map.
	const float coverage = ApplyCoverage(low.r, Thresholds[kThreshLowCoverage], coverageEdgeWidth);
	const float highCover = ApplyCoverage(high.r, Thresholds[kThreshHighCoverage], highCoverageEdgeWidth);

	if (solveRound == 1u) {
		if (coverage > 0.0)
			Accumulate(kHistScScore, low.b);
		if (highCover > 0.0)
			Accumulate(kHistHighType, high.g);
		return;
	}

	// Round 2 needs the Sc threshold from round 1: species shares are measured
	// over the covered area that stratocumulus did not already claim.
	if (coverage > 0.0 && low.b < Thresholds[kThreshSc])
		Accumulate(kHistTypeScore, low.g);
}

// ===========================================================================
#elif CLOUDMAPGEN == 2  // solveThresholds
// ===========================================================================

RWByteAddressBuffer RWHistogram : register(u0);
RWStructuredBuffer<float> RWThresholds : register(u1);

groupshared uint gScan[HISTOGRAM_BINS];

// Inclusive Hillis-Steele scan of one histogram group. Every barrier below sits
// in flow that is uniform across the group, which is a hard requirement for
// GroupMemoryBarrierWithGroupSync.
void ScanGroup(uint group, uint tid)
{
	gScan[tid] = RWHistogram.Load(4u * (group * kHistogramBins + tid));
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint offset = 1u; offset < kHistogramBins; offset <<= 1u)
	{
		const uint addend = (tid >= offset) ? gScan[tid - offset] : 0u;
		GroupMemoryBarrierWithGroupSync();
		gScan[tid] += addend;
		GroupMemoryBarrierWithGroupSync();
	}
}

// Reads the already-scanned group and writes the value below which
// `fractionBelow` of the samples fall. The crossing bin is refined by linear
// interpolation so the solved coverage is not quantised to 1/256.
void EmitQuantile(float fractionBelow, uint slot, uint tid)
{
	const uint total = gScan[kHistogramBins - 1u];

	if (tid == 0u) {
		// Degenerate cases must still produce a usable threshold: -1 lets every
		// sample through, 2 rejects all of them.
		if (total == 0u)
			RWThresholds[slot] = 0.5;
		else if (fractionBelow <= 0.0)
			RWThresholds[slot] = -1.0;
		else if (fractionBelow >= 1.0)
			RWThresholds[slot] = 2.0;
	}

	if (total > 0u && fractionBelow > 0.0 && fractionBelow < 1.0) {
		const float target = fractionBelow * float(total);
		const float cumulative = float(gScan[tid]);
		const float previous = (tid == 0u) ? 0.0 : float(gScan[tid - 1u]);
		if (cumulative > target && previous <= target) {
			const float binCount = cumulative - previous;
			const float withinBin = binCount > 0.0 ? (target - previous) / binCount : 0.5;
			RWThresholds[slot] = (float(tid) + withinBin) / float(kHistogramBins);
		}
	}
}

[numthreads(HISTOGRAM_BINS, 1, 1)] void main(uint tid : SV_GroupThreadID) {
	if (solveRound == 0u) {
		ScanGroup(kHistLowPotential, tid);
		EmitQuantile(1.0 - saturate(skyCoverage), kThreshLowCoverage, tid);
		GroupMemoryBarrierWithGroupSync();

		ScanGroup(kHistHighPotential, tid);
		EmitQuantile(1.0 - saturate(highCoverage), kThreshHighCoverage, tid);
	} else if (solveRound == 1u) {
		ScanGroup(kHistScScore, tid);
		EmitQuantile(1.0 - saturate(scShare), kThreshSc, tid);
		GroupMemoryBarrierWithGroupSync();

		ScanGroup(kHistHighType, tid);
		EmitQuantile(saturate(asShare), kThreshAs, tid);
	} else {
		// Both species splits read the same scanned histogram.
		ScanGroup(kHistTypeScore, tid);
		EmitQuantile(saturate(cuShare), kThreshCu, tid);
		EmitQuantile(saturate(cuShare + tcuShare), kThreshTcu, tid);
	}
}

// ===========================================================================
#elif CLOUDMAPGEN == 3  // composeMaps
// ===========================================================================

Texture2D<float4> TexFieldLow : register(t0);
Texture2D<float4> TexFieldHigh : register(t1);
StructuredBuffer<float> Thresholds : register(t2);

RWTexture2D<unorm float4> RWLowWeather : register(u0);
RWTexture2D<unorm float4> RWHighWeather : register(u1);
RWTexture2D<unorm float4> RWScCell : register(u2);
RWTexture2D<unorm float4> RWHighCell : register(u3);
RWTexture2D<unorm float4> RWHighWarp : register(u4);
RWTexture2D<unorm float4> RWHighWisp : register(u5);

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= weatherDim))
		return;

	const float2 uv = (float2(tid) + 0.5) / float2(weatherDim);
	const float4 low = TexFieldLow[tid];
	const float4 high = TexFieldHigh[tid];

	// --- Low weather: R=coverage G=Cu/Tcu/Cb H=Sc mask A=cloud-body radial ---
	const float coverage = ApplyCoverage(low.r, Thresholds[kThreshLowCoverage], coverageEdgeWidth);
	const bool scRegion = coverage > 0.0 && low.b >= Thresholds[kThreshSc];
	const float cloudType = scRegion ? 0.0 : (low.g < Thresholds[kThreshCu] ? 0.0 : (low.g < Thresholds[kThreshTcu] ? 0.5 : 1.0));
	// Stratocumulus is a compressed slab rather than a dome. A near-core radial
	// keeps its profile at full depth so the vertical compression alone shapes it.
	const float radial = scRegion ? 0.12 : low.a;
	RWLowWeather[tid] = float4(coverage, cloudType, scRegion ? 1.0 : 0.0, radial);

	// --- High weather: R=coverage G=As/Ac B=0 A=multiple-scattering weight ---
	const float highCover = ApplyCoverage(high.r, Thresholds[kThreshHighCoverage], highCoverageEdgeWidth);
	const float highType = high.g < Thresholds[kThreshAs] ? 0.0 : 1.0;
	// A must vanish wherever R does: the renderer also uses it to soften low-cloud
	// density edges, so a non-zero floor would bleed into clear-sky columns.
	const float scatterWeight = highCover * saturate(0.35 + high.b * 0.65);
	RWHighWeather[tid] = float4(highCover, highType, 0.0, scatterWeight);

	// --- Tileable auxiliary maps, sampled at a multiple of the weather UV ---
	const float2 cellWarp = float2(
								PeriodicFbm(uv, 2u, 5u, seed + 83u),
								PeriodicFbm(uv + float2(0.31, 0.57), 2u, 5u, seed + 87u)) *
	                            0.08 -
	                        0.04;

	const float scCell = PeriodicCell(uv + cellWarp, 4u, seed + 89u) * 0.78 +
	                     PeriodicCell(uv + cellWarp + float2(0.17, -0.23), 9u, seed + 97u) * 0.22;
	RWScCell[tid] = float4(scCell, scCell, scCell, 1.0);

	const float highCell = PeriodicCell(uv + cellWarp.yx * float2(-1.0, 1.0), 3u, seed + 101u) * 0.82 +
	                       PeriodicCell(uv + cellWarp.yx * float2(-1.0, 1.0) + float2(0.29, 0.11), 7u, seed + 107u) * 0.18;
	RWHighCell[tid] = float4(highCell, highCell, highCell, 1.0);

	RWHighWarp[tid] = float4(
		PeriodicFbm(uv, 2u, 5u, seed + 113u),
		PeriodicFbm(uv + float2(0.37, 0.61), 2u, 5u, seed + 127u),
		0.5,
		1.0);

	const float wispField = PeriodicFbm(uv, 7u, 5u, seed + 139u);
	const float wispCross = PeriodicFbmAniso(float2(dot(uv, float2(1.0, 1.0)), dot(uv, float2(1.0, -1.0))), uint2(6u, 6u), 5u, seed + 149u);
	const float wisp = pow(saturate(1.0 - abs(wispField * 2.0 - 1.0)), 4.0) * saturate(0.55 + wispCross * 0.45);
	RWHighWisp[tid] = float4(wisp, wisp, wisp, 1.0);
}

// ===========================================================================
#elif CLOUDMAPGEN == 4  // composeProfile
// ===========================================================================

RWTexture2D<unorm float4> RWProfile : register(u0);

// Vertical density profile for one species. U is normalized height inside the
// shared cloud shell, V is the distance from the cloud body's own centre.
//
// A cumulus has a flat base at the lifting condensation level and a domed top,
// so the base stays anchored at height 0 for every radial while the usable depth
// collapses toward the cloud edge.
float ProfileForDepth(float height, float radial, float requestedDepth)
{
	const float dome = sqrt(saturate(1.0 - radial * radial));
	const float effectiveDepth = clamp(requestedDepth * lerp(1.0, dome, saturate(domeStrength)), 0.02, max(layerDepth, 0.05));
	const float localHeight = height * max(layerDepth, 0.05) / effectiveDepth;
	const float bottom = smoothstep(0.0, 0.10, localHeight);
	const float top = 1.0 - smoothstep(0.70, 1.0, localHeight);
	return saturate(bottom * top);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= profileDim))
		return;

	const float height = (float(tid.x) + 0.5) / float(profileDim.x);
	const float radial = (float(tid.y) + 0.5) / float(profileDim.y);

	RWProfile[tid] = float4(
		ProfileForDepth(height, radial, max(cumulusDepth, 0.05)),
		ProfileForDepth(height, radial, max(toweringCumulusDepth, 0.05)),
		ProfileForDepth(height, radial, max(cumulonimbusDepth, 0.05)),
		1.0);
}

#endif
