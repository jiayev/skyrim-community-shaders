#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#ifndef HIGHCLOUDMAPGEN
#	define HIGHCLOUDMAPGEN 0
#endif

#include "Common/Random.hlsli"

cbuffer HighCloudMapGenCB : register(b1)
{
	uint2 weatherDim;
	uint seed;
	uint solveRound;

	float coverage;
	float coverageEdgeWidth;
	float frontStrength;
	float asShare;

	float2 frontNormal;
	float2 frontTangent;
	float4 padding;
};

#define HISTOGRAM_BINS 256
static const uint kHistogramBins = HISTOGRAM_BINS;
static const uint kHistSupport = 0u;
static const uint kHistSpecies = 1u;
static const uint kThresholdSupport = 0u;
static const uint kThresholdSpecies = 1u;

uint2 WrapCell(int2 cell, uint2 period)
{
	const uint2 p = max(period, 1u);
	return uint2(cell + int2(p) * 64) % p;
}

float HashScalar(uint2 cell, uint stream)
{
	return Random::pcg2d(cell + uint2(stream, stream * 747796405u)).x * (1.0 / 4294967296.0);
}

float2 HashVector(uint2 cell, uint stream)
{
	return Random::pcg2d(cell + uint2(stream, stream * 747796405u)) * (1.0 / 4294967296.0);
}

float LatticeValue(float2 position, uint2 period, uint stream)
{
	const int2 base = int2(floor(position));
	const float2 f = frac(position);
	const float2 blend = f * f * (3.0 - 2.0 * f);
	const float v00 = HashScalar(WrapCell(base, period), stream);
	const float v10 = HashScalar(WrapCell(base + int2(1, 0), period), stream);
	const float v01 = HashScalar(WrapCell(base + int2(0, 1), period), stream);
	const float v11 = HashScalar(WrapCell(base + int2(1, 1), period), stream);
	return lerp(lerp(v00, v10, blend.x), lerp(v01, v11, blend.x), blend.y);
}

float FractalValue(float2 uv, uint initialPeriod, uint octaveCount, uint stream)
{
	uint period = max(initialPeriod, 1u);
	float amplitude = 0.5;
	float total = 0.0;
	float normalization = 0.0;
	[loop] for (uint octave = 0u; octave < octaveCount; ++octave)
	{
		total += LatticeValue(uv * period, uint2(period, period), stream + octave * 19u) * amplitude;
		normalization += amplitude;
		period *= 2u;
		amplitude *= 0.5;
	}
	return saturate(total / max(normalization, 1e-4));
}

float FractalValueOriented(float2 coordinates, uint2 initialPeriod, uint octaveCount, uint stream)
{
	uint2 period = max(initialPeriod, 1u);
	float amplitude = 0.5;
	float total = 0.0;
	float normalization = 0.0;
	[loop] for (uint octave = 0u; octave < octaveCount; ++octave)
	{
		total += LatticeValue(coordinates * period, period, stream + octave * 23u) * amplitude;
		normalization += amplitude;
		period *= 2u;
		amplitude *= 0.5;
	}
	return saturate(total / max(normalization, 1e-4));
}

float CellularMass(float2 uv, uint featureCount, uint stream)
{
	const uint count = max(featureCount, 1u);
	const float2 position = uv * count;
	const int2 base = int2(floor(position));
	float nearestDistanceSquared = 8.0;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const int2 cell = base + int2(x, y);
			const float2 featurePosition = float2(cell) + HashVector(WrapCell(cell, uint2(count, count)), stream);
			const float2 delta = featurePosition - position;
			nearestDistanceSquared = min(nearestDistanceSquared, dot(delta, delta));
		}
	}
	return saturate(1.0 - sqrt(nearestDistanceSquared) * 1.18);
}

float FrontalField(float2 uv)
{
	const float2 oriented = float2(dot(uv, frontNormal), dot(uv, frontTangent));
	const float driver = FractalValueOriented(oriented, uint2(1u, 4u), 3u, seed + 401u);
	const float band = 1.0 - smoothstep(0.07, 0.31, abs(driver - 0.5) * 2.0);
	const float continuity = smoothstep(0.22, 0.78, FractalValue(uv, 7u, 3u, seed + 463u));
	return band * lerp(0.35, 1.0, continuity);
}

float4 AuthorHighField(float2 uv)
{
	const float frontal = FrontalField(uv) * saturate(frontStrength);
	const float broad = FractalValue(uv, 2u, 4u, seed + 701u);
	const float meso = FractalValue(uv, 5u, 3u, seed + 809u);
	const float support = saturate(broad * 0.58 + meso * 0.22 + frontal * 0.36);
	const float species = saturate(0.12 + FractalValue(uv, 7u, 3u, seed + 907u) * 0.68 - frontal * 0.24);
	const float scatterWeight = saturate(broad * 0.58 + frontal * 0.28 + meso * 0.14);
	return float4(support, species, scatterWeight, 0.0);
}

float CoverageFromThreshold(float rank, float threshold, float edgeWidth)
{
	if (rank <= threshold)
		return 0.0;
	const float availableRange = max(1.0 - threshold, 1e-3);
	return saturate((rank - threshold) / (max(edgeWidth, 0.05) * availableRange));
}

uint HistogramBin(float value)
{
	return min(uint(saturate(value) * float(kHistogramBins)), kHistogramBins - 1u);
}

#if HIGHCLOUDMAPGEN == 0

RWTexture2D<float4> RWFieldHigh : register(u0);

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= weatherDim))
		return;
	const float2 uv = (float2(tid) + 0.5) / float2(weatherDim);
	RWFieldHigh[tid] = AuthorHighField(uv);
}

#elif HIGHCLOUDMAPGEN == 1

Texture2D<float4> TexFieldHigh : register(t0);
StructuredBuffer<float> Thresholds : register(t1);
RWByteAddressBuffer RWHistogram : register(u0);

void AddHistogramSample(uint group, float value)
{
	uint ignored;
	RWHistogram.InterlockedAdd(4u * (group * kHistogramBins + HistogramBin(value)), 1u, ignored);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= weatherDim))
		return;
	const float4 high = TexFieldHigh[tid];
	if (solveRound == 0u) {
		AddHistogramSample(kHistSupport, high.r);
		return;
	}
	const float covered = CoverageFromThreshold(high.r, Thresholds[kThresholdSupport], coverageEdgeWidth);
	if (covered > 0.0)
		AddHistogramSample(kHistSpecies, high.g);
}

#elif HIGHCLOUDMAPGEN == 2

RWByteAddressBuffer RWHistogram : register(u0);
RWStructuredBuffer<float> RWThresholds : register(u1);
groupshared uint gScan[HISTOGRAM_BINS];

void ScanHistogram(uint group, uint lane)
{
	gScan[lane] = RWHistogram.Load(4u * (group * kHistogramBins + lane));
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint offset = 1u; offset < kHistogramBins; offset <<= 1u)
	{
		const uint addend = lane >= offset ? gScan[lane - offset] : 0u;
		GroupMemoryBarrierWithGroupSync();
		gScan[lane] += addend;
		GroupMemoryBarrierWithGroupSync();
	}
}

void StoreQuantile(float fractionBelow, uint slot, uint lane)
{
	const uint total = gScan[kHistogramBins - 1u];
	if (lane == 0u) {
		if (total == 0u)
			RWThresholds[slot] = 0.5;
		else if (fractionBelow <= 0.0)
			RWThresholds[slot] = -1.0;
		else if (fractionBelow >= 1.0)
			RWThresholds[slot] = 2.0;
	}
	if (total > 0u && fractionBelow > 0.0 && fractionBelow < 1.0) {
		const float target = fractionBelow * float(total);
		const float current = float(gScan[lane]);
		const float previous = lane == 0u ? 0.0 : float(gScan[lane - 1u]);
		if (current > target && previous <= target) {
			const float binPopulation = current - previous;
			const float fractionInBin = binPopulation > 0.0 ? (target - previous) / binPopulation : 0.5;
			RWThresholds[slot] = (float(lane) + fractionInBin) / float(kHistogramBins);
		}
	}
}

[numthreads(HISTOGRAM_BINS, 1, 1)] void main(uint lane : SV_GroupThreadID) {
	if (solveRound == 0u) {
		ScanHistogram(kHistSupport, lane);
		StoreQuantile(1.0 - saturate(coverage), kThresholdSupport, lane);
	} else {
		ScanHistogram(kHistSpecies, lane);
		StoreQuantile(saturate(asShare), kThresholdSpecies, lane);
	}
}

#elif HIGHCLOUDMAPGEN == 3

Texture2D<float4> TexFieldHigh : register(t0);
StructuredBuffer<float> Thresholds : register(t1);
RWTexture2D<unorm float4> RWHighWeather : register(u0);
RWTexture2D<unorm float4> RWHighCell : register(u1);
RWTexture2D<unorm float4> RWHighWarp : register(u2);
RWTexture2D<unorm float4> RWHighWisp : register(u3);

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	if (any(tid >= weatherDim))
		return;
	const float2 uv = (float2(tid) + 0.5) / float2(weatherDim);
	const float4 high = TexFieldHigh[tid];
	const float highCoverage = CoverageFromThreshold(high.r, Thresholds[kThresholdSupport], coverageEdgeWidth);
	const float speciesFeather = 2.0 / float(kHistogramBins);
	const float highType = smoothstep(Thresholds[kThresholdSpecies] - speciesFeather, Thresholds[kThresholdSpecies] + speciesFeather, high.g);
	RWHighWeather[tid] = float4(highCoverage, highType, 0.0, highCoverage * saturate(0.34 + high.b * 0.66));

	const float2 structuralWarp = (float2(
									   FractalValue(uv, 2u, 4u, seed + 83u),
									   FractalValue(uv + float2(0.31, 0.57), 2u, 4u, seed + 89u)) -
									  0.5) *
	                              0.08;
	const float highCell = CellularMass(uv + structuralWarp, 3u, seed + 101u) * 0.81 +
	                       CellularMass(uv + structuralWarp + float2(0.29, 0.11), 7u, seed + 107u) * 0.19;
	RWHighCell[tid] = float4(highCell, highCell, highCell, 1.0);
	RWHighWarp[tid] = float4(
		FractalValue(uv, 2u, 5u, seed + 113u),
		FractalValue(uv + float2(0.37, 0.61), 2u, 5u, seed + 127u),
		0.5,
		1.0);
	const float wispDriver = FractalValue(uv, 7u, 5u, seed + 139u);
	const float wispCross = FractalValueOriented(
		float2(dot(uv, float2(1.0, 1.0)), dot(uv, float2(1.0, -1.0))),
		uint2(6u, 6u),
		5u,
		seed + 149u);
	const float wisp = pow(saturate(1.0 - abs(wispDriver * 2.0 - 1.0)), 4.0) * saturate(0.55 + wispCross * 0.45);
	RWHighWisp[tid] = float4(wisp, wisp, wisp, 1.0);
}

#endif
