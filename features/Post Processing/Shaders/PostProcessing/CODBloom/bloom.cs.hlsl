/// By ProfJack/五脚猫, 2024-2-28 UTC
/// ref:
/// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare

#include "../../Common/Color.hlsli"

Texture2D<float4> TexColor : register(t0);
Texture2D<float4> TexBloomIn : register(t1);

RWTexture2D<float4> RWTexBloomOut : register(u0);

cbuffer BloomCB : register(b1)
{
	// threshold
	float Threshold : packoffset(c0.x);
	// upsample
	float UpsampleRadius : packoffset(c0.y);
	float UpsampleMult : packoffset(c0.z);  // in composite: bloom mult
	float CurrentMipMult : packoffset(c0.w);
};

SamplerState SampColor : register(s0);

bool3 IsNaN(float3 x)
{
	return !(x < 0.f || x > 0.f || x == 0.f);
}

float3 Sanitise(float3 v)
{
	bool3 err = IsNaN(v) || (v < 0);
	v.x = err.x ? 0 : v.x;
	v.y = err.y ? 0 : v.y;
	v.z = err.z ? 0 : v.z;
	return v;
}

float3 ThresholdColor(float3 col, float threshold)
{
	float luma = RGBToLuminance(col);
	if (luma < 1e-3)
		return 0;
	return col * (max(0, luma - threshold) / luma);
}

float4 KarisAverage(float4 a, float4 b, float4 c, float4 d)
{
	float wa = rcp(1 + RGBToLuminance(a.rgb));
	float wb = rcp(1 + RGBToLuminance(b.rgb));
	float wc = rcp(1 + RGBToLuminance(c.rgb));
	float wd = rcp(1 + RGBToLuminance(d.rgb));
	float wsum = wa + wb + wc + wd;
	return (a * wa + b * wb + c * wc + d * wd) / wsum;
}

// Maybe rewrite as fetch
float4 DownsampleCOD(Texture2D tex, float2 uv, float2 out_px_size)
{
	int x, y;

	float4 retval = 0;
#ifdef FIRST_MIP
	float4 fetches2x2[4];
	float4 fetches3x3[9];

	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			fetches2x2[x * 2 + y] = tex.SampleLevel(SampColor, uv + (int2(x, y) * 2 - 1) * out_px_size, 0);
	[unroll] for (x = 0; x < 3; ++x)
		[unroll] for (y = 0; y < 3; ++y)
			fetches3x3[x * 3 + y] = tex.SampleLevel(SampColor, uv + (int2(x, y) - 1) * 2 * out_px_size, 0);

	retval += 0.5 * KarisAverage(fetches2x2[0], fetches2x2[1], fetches2x2[2], fetches2x2[3]);

	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			retval += 0.125 * KarisAverage(fetches3x3[x * 3 + y], fetches3x3[(x + 1) * 3 + y], fetches3x3[x * 3 + y + 1], fetches3x3[(x + 1) * 3 + y + 1]);
#else
	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			retval += 0.125 * tex.SampleLevel(SampColor, uv + (int2(x, y) - .5) * out_px_size, 0);

	// const static float weights[9] = { 0.03125, 0.625, 0.03125, 0.625, 0.125, 0.625, 0.03125, 0.625, 0.03125 };
	// corresponds to (1 << (!x + !y)) * 0.03125 when $x,y \in [-1, 1] \cap \mathbb N$
	[unroll] for (x = -1; x <= 1; ++x)
		[unroll] for (y = -1; y <= 1; ++y)
			retval += (1u << (!x + !y)) * 0.03125 * tex.SampleLevel(SampColor, uv + int2(x, y) * out_px_size, 0);
#endif

	return retval;
}

float4 UpsampleCOD(Texture2D tex, float2 uv, float2 radius)
{
	float4 retval = 0;
	for (int x = -1; x <= 1; ++x)
		for (int y = -1; y <= 1; ++y)
			retval += (1 << (!x + !y)) * 0.0625 * tex.SampleLevel(SampColor, uv + float2(x, y) * radius, 0);
	return retval;
}

float3 SampleChromatic(Texture2D tex, float2 uv, uint mip, float chromatic)
{
	float3 col;
	col.r = tex.SampleLevel(SampColor, lerp(.5, uv, 1 - chromatic), mip).r;
	col.g = tex.SampleLevel(SampColor, uv, mip).g;
	col.b = tex.SampleLevel(SampColor, lerp(.5, uv, 1 + chromatic), mip).b;
	return col;
}

[numthreads(32, 32, 1)] void CS_Threshold(uint2 tid
										  : SV_DispatchThreadID) {
	float3 col_input = TexColor[tid].rgb;

	float3 col = col_input;
	col = Sanitise(col);
	col = ThresholdColor(col, Threshold.x);
	RWTexBloomOut[tid] = float4(col, 1);
};

[numthreads(32, 32, 1)] void CS_Downsample(uint2 tid
										   : SV_DispatchThreadID) {
	uint2 dims;
	RWTexBloomOut.GetDimensions(dims.x, dims.y);

	float2 px_size = rcp(dims);
	float2 uv = (tid + .5) * px_size;

	float3 col = DownsampleCOD(TexBloomIn, uv, px_size).rgb;
	RWTexBloomOut[tid] = float4(col, 1);
};

[numthreads(32, 32, 1)] void CS_Upsample(uint2 tid
										 : SV_DispatchThreadID) {
	uint2 dims;
	RWTexBloomOut.GetDimensions(dims.x, dims.y);

	float2 px_size = rcp(dims);
	float2 uv = (tid + .5) * px_size;

	float3 col = RWTexBloomOut[tid].rgb * CurrentMipMult + UpsampleCOD(TexBloomIn, uv, px_size * UpsampleRadius).rgb * UpsampleMult;
	RWTexBloomOut[tid] = float4(col, 1);
};

[numthreads(32, 32, 1)] void CS_Composite(uint2 tid
										  : SV_DispatchThreadID) {
	uint2 dims;
	RWTexBloomOut.GetDimensions(dims.x, dims.y);

	float2 px_size = rcp(dims);
	float2 uv = (tid + .5) * px_size;

	float3 col = TexColor[tid].rgb + UpsampleCOD(TexBloomIn, uv, px_size * UpsampleRadius).rgb * UpsampleMult;

	RWTexBloomOut[tid] = float4(col, 1);
};