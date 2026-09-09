#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

#include "Common/Random.hlsli"

RWTexture2DArray<float4> RWTexOutput : register(u0);

cbuffer CB : register(b1)
{
	uint seed;
	uint form;
	float coverage;
	float weatherStrength;
	float cloudSize;
	float weatherScale;
	float elongation;
	float bearing;
	float sizeVariation;
	float clustering;
	float development;
	float heightVariation;
	float baseVariation;
	float edgeSoftness;
	float topType;
	float topTypeVariation;
	float bottomType;
	float shoulders;
	float2 shapePadding;
	float2 worldSize;
	float2 padding;
};

int2 WrapCell(int2 cell, uint2 period)
{
	const int2 size = int2(period);
	return (cell % size + size) % size;
}

float3 CellRandom(int2 cell, uint2 period, uint stream)
{
	const uint2 wrapped = uint2(WrapCell(cell, period));
	return float3(Random::pcg3d(uint3(wrapped, seed ^ stream)) >> 8u) * (1.0 / 16777216.0);
}

float ValueNoise(float2 uv, uint2 period, uint stream)
{
	const float2 p = frac(uv) * period;
	const int2 cell = int2(floor(p));
	const float2 f = frac(p);
	const float2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	return lerp(
		lerp(CellRandom(cell, period, stream).x, CellRandom(cell + int2(1, 0), period, stream).x, w.x),
		lerp(CellRandom(cell + int2(0, 1), period, stream).x, CellRandom(cell + int2(1, 1), period, stream).x, w.x), w.y);
}

float Weather(float2 uv, uint2 period)
{
	return ValueNoise(uv, period, 11u) * 0.75 + ValueNoise(uv, period * 2u, 17u) * 0.25;
}

float LocalAmount(float weather)
{
	return saturate(coverage + (weather - 0.5) * weatherStrength * 2.0 * min(coverage, 1.0 - coverage));
}

float Dome(float2 p, float2 radii, out float envelope)
{
	const float r = length(p / radii);
	envelope = smoothstep(0.0, edgeSoftness, 1.0 - r);
	return sqrt(saturate(1.0 - r * r));
}

float MergeHeight(float a, float b)
{
	return max(a, b) + 0.08 * shoulders * min(a, b);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	uint3 dims;
	RWTexOutput.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims.xy))
		return;
	const float2 uv = (float2(tid) + 0.5) / float2(dims.xy);
	const uint2 weatherPeriod = uint2(clamp(round(worldSize / weatherScale), 1.0, 32.0));
	const float base = 0.03 + 0.15 * baseVariation * ValueNoise(uv, weatherPeriod, 29u);
	float amount = 0.0;
	float height = 0.0;
	const float localAmount = LocalAmount(Weather(uv, weatherPeriod));
	if (coverage > 0.0 && form != 2u) {
		// Integer cell counts preserve the tile; the world metric preserves circular domes on rectangular maps.
		const uint2 period = uint2(clamp(round(worldSize / cloudSize), 1.0, float(min(dims.x, dims.y) / 8u)));
		const float2 cellSize = worldSize / period;
		const float cellWidth = min(cellSize.x, cellSize.y);
		const int2 cell = int2(floor(uv * period));
		const float2 position = uv * worldSize;
		// All lobes fit within 1.6 cell widths of their center, including rotated major axes.
		[loop] for (int y = -2; y <= 2; ++y)
			[loop] for (int x = -2; x <= 2; ++x)
		{
			const int2 id = cell + int2(x, y);
			const float3 random = CellRandom(id, period, 41u);
			const float3 shape = CellRandom(id, period, 53u);
			const float2 center = (float2(id) + 0.2 + random.xy * 0.6) * cellSize;
			const float centerAmount = LocalAmount(Weather(center / worldSize, weatherPeriod));
			const float occurrence = smoothstep(random.z - 0.12, random.z + 0.12,
				centerAmount * lerp(1.6, 1.1, clustering));
			if (occurrence <= 0.0)
				continue;
			const float radius = cellWidth * lerp(0.28, 0.65, centerAmount) *
			                     lerp(1.0, 0.65 + shape.x * 0.7, sizeVariation) * (form == 1u ? 1.2 : 1.0);
			const float major = min(radius * sqrt(elongation), cellWidth * 1.05);
			const float2 radii = float2(major, major / elongation);
			const float angle = bearing + (shape.y - 0.5) * 6.28318530718 * lerp(1.0, 0.12, clustering);
			float sn, cs;
			sincos(angle, sn, cs);
			const float2 delta = position - center;
			const float2 p = float2(dot(delta, float2(cs, sn)), dot(delta, float2(-sn, cs)));
			float envelope;
			float massHeight = Dome(p, radii, envelope);
			[unroll] for (uint lobe = 0u; lobe < 3u; ++lobe)
			{
				const float3 detail = CellRandom(id, period, 67u + lobe * 13u);
				const float theta = (float(lobe) + detail.x * 0.5) * 2.09439510239;
				const float2 offset = float2(cos(theta), sin(theta)) * radii * 0.75;
				const float2 lobeRadii = radii * lerp(0.35, 0.7, shoulders);
				float lobeEnvelope;
				const float lobeHeight = Dome(p - offset, lobeRadii, lobeEnvelope) * lerp(0.7, 1.1, detail.y);
				massHeight = MergeHeight(massHeight, lobeHeight * sqrt(shoulders));
				envelope = max(envelope, lobeEnvelope * shoulders);
			}
			const float rise = lerp(1.0, 0.35 + shape.z * 0.65, heightVariation);
			amount = max(amount, envelope * occurrence);
			height = MergeHeight(height, massHeight * rise * occurrence);
		}
	}
	if (coverage > 0.0 && form != 0u) {
		const float sheet = form == 2u ? smoothstep(0.3, 0.7, localAmount) : smoothstep(0.55, 0.85, localAmount) * clustering;
		amount = max(amount, sheet);
		const float sheetHeight = lerp(1.0, 0.65 + 0.35 * ValueNoise(uv, weatherPeriod * 2u, 83u), heightVariation);
		height = max(height, sheet * sheetHeight);
	}
	const float riseScale = form == 0u ? lerp(0.2, 0.92, development) :
	                                     (form == 1u ? lerp(0.12, 0.5, development) : lerp(0.08, 0.3, development));
	const float top = min(0.99, base + max(0.01, saturate(height) * riseScale));
	const float type = saturate(topType + (ValueNoise(uv, weatherPeriod * 2u, 97u) - 0.5) * topTypeVariation);
	RWTexOutput[uint3(tid, 0)] = float4(base, top, saturate(amount), type);
	RWTexOutput[uint3(tid, 1)] = float4(bottomType, 0, 0, 0);
}
