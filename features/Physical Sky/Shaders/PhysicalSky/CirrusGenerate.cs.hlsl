#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

#include "Common/Random.hlsli"

struct NoiseLayer
{
	uint noise;
	float frequency;
	float exponent;
	float padding0;
	float2 offset;
	float2 padding1;
	float4 range;
};

cbuffer CB : register(b1)
{
	NoiseLayer weather[2];
	uint seed;
	float warp;
	float detail;
	float padding;
	float2 windOffset;
	float2 padding1;
};
Texture2D<float> Noise0 : register(t0);
Texture2D<float> Noise1 : register(t1);
Texture2D<float> Noise2 : register(t2);
Texture2D<float> Noise3 : register(t3);
SamplerState NoiseSampler : register(s0);
RWTexture2D<float2> OutputWeather : register(u0);
RWTexture2D<float4> OutputPatterns : register(u1);

float SampleNoise(Texture2D<float> source, NoiseLayer layer, float2 uv, uint2 size)
{
	uint2 sourceSize;
	source.GetDimensions(sourceSize.x, sourceSize.y);
	const float2 footprint = float2(sourceSize) * layer.frequency / size;
	const float mip = log2(max(max(footprint.x, footprint.y), 1.0));
	return source.SampleLevel(NoiseSampler, (uv + layer.offset - windOffset * 0.0001) * layer.frequency, mip);
}

float WeatherValue(NoiseLayer layer, float2 uv, uint2 size)
{
	float value = 0.0;
	switch (layer.noise) {
	case 0u:
		value = SampleNoise(Noise0, layer, uv, size);
		break;
	case 1u:
		value = SampleNoise(Noise1, layer, uv, size);
		break;
	case 2u:
		value = SampleNoise(Noise2, layer, uv, size);
		break;
	case 3u:
		value = SampleNoise(Noise3, layer, uv, size);
		break;
	}
	value = value * 2.0 - 1.0;
	return saturate((value - layer.range.x) / (layer.range.y - layer.range.x)) * (layer.range.w - layer.range.z) + layer.range.z;
}

[numthreads(8, 8, 1)] void generateWeather(uint2 tid : SV_DispatchThreadID) {
	uint2 size;
	OutputWeather.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	const float2 uv = (float2(tid) + 0.5) / size;
	OutputWeather[tid] = float2(WeatherValue(weather[0], uv, size), WeatherValue(weather[1], uv, size));
}

float2 PatternHash(int2 cell, int2 period, uint salt)
{
	cell = (cell % period + period) % period;
	return float2(Random::pcg3d(uint3(asuint(cell), seed ^ salt)).xy >> 8u) / 16777216.0;
}

float PatternGradient(int2 cell, float2 offset, int2 period, uint salt)
{
	const float angle = PatternHash(cell, period, salt).x * 6.28318530718;
	return dot(float2(cos(angle), sin(angle)), offset);
}

float PatternNoise(float2 uv, int2 period, uint salt)
{
	const float2 position = frac(uv) * period;
	const int2 cell = int2(floor(position));
	const float2 f = frac(position);
	const float2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	return lerp(lerp(PatternGradient(cell, f, period, salt), PatternGradient(cell + int2(1, 0), f - float2(1, 0), period, salt), w.x),
		lerp(PatternGradient(cell + int2(0, 1), f - float2(0, 1), period, salt), PatternGradient(cell + 1, f - 1.0, period, salt), w.x), w.y);
}

float PatternCells(float2 uv)
{
	const int2 period = int2(32, 32);
	const float2 p = frac(uv) * period;
	const int2 cell = int2(floor(p));
	float distance = 2.0;
	[unroll] for (int y = -1; y <= 1; ++y)
		[unroll] for (int x = -1; x <= 1; ++x)
	{
		const int2 offset = int2(x, y);
		distance = min(distance, length(offset + PatternHash(cell + offset, period, 71u) - frac(p)));
	}
	return 1.0 - smoothstep(0.1, 0.75, distance);
}

[numthreads(8, 8, 1)] void generatePatterns(uint2 tid : SV_DispatchThreadID) {
	uint2 size;
	OutputPatterns.GetDimensions(size.x, size.y);
	if (any(tid >= size))
		return;
	const float2 uv = (float2(tid) + 0.5) / size;
	const float2 displacement = float2(PatternNoise(uv, int2(4, 4), 11u), PatternNoise(uv, int2(4, 4), 23u)) * warp;
	const float2 p = uv + displacement;
	const float envelope = saturate(0.5 + PatternNoise(uv, int2(4, 4), 37u));
	const float wispy = saturate(0.5 + 1.6 * PatternNoise(p, int2(8, 48), 41u) + detail * PatternNoise(p, int2(16, 96), 43u)) * sqrt(envelope);
	const float rounded = saturate(PatternCells(p) + detail * PatternNoise(p, int2(96, 96), 47u)) * sqrt(envelope);
	const float streaky = saturate(0.5 + 1.6 * PatternNoise(p, int2(4, 96), 53u) + detail * PatternNoise(p, int2(8, 192), 59u));
	OutputPatterns[tid] = float4(wispy, rounded, streaky, 1.0);
}
