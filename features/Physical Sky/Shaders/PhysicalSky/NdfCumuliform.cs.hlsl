#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

RWTexture2DArray<unorm float> RWTexOutput : register(u0);

cbuffer CB : register(b1)
{
	uint2 scale0;
	float2 offset0;
	uint2 scale1;
	float2 offset1;
	uint2 scale2;
	float2 offset2;
	float2 clip_range;
	float power;
	float wispiness;
    float rot0;
    float rot1;
    float rot2;
    float _pad;
};

float2x2 rotationMatrix(float angle)
{
	float2 sin_cos;
	sincos(angle, sin_cos.y, sin_cos.x);
	return float2x2(sin_cos.x, sin_cos.y, -sin_cos.y, sin_cos.x);
}

float2 hash22(int2 seed)
{
	return Random::pcg2d(asuint(seed)) / 4294967295.f;
}

float Worley(float2 uv, uint2 freq)
{
	int2 id = floor(uv);
	float2 p = frac(uv);

	float minDist = 10000.;
	[unroll] for (int x = -1; x <= 1; ++x)
	{
		[unroll] for (int y = -1; y <= 1; ++y)
		{
			int2 offset = int2(x, y);
			float2 h = hash22((id + offset) % freq) * .5 + .5;
			h += offset;
			float2 d = p - h;
			minDist = min(minDist, dot(d, d));
		}
	}

	return minDist / 1.125;
}

[numthreads(8, 8, 1)] void main(uint2 tid
								: SV_DispatchThreadID) {
	uint3 dims;
	RWTexOutput.GetDimensions(dims.x, dims.y, dims.z);
	float2 uv = (tid + 0.5) / dims.xy;

    float2x2 rotmat0 = rotationMatrix(rot0);
	float2 uv0 = mul(rotmat0, uv * scale0) + offset0;
	float noise0 =
		Worley(uv0 + Random::R2Modified(0) * 100, scale0) * .625 +
		Worley(uv0 * 2 + Random::R2Modified(1) * 100, scale0 * 2) * .25 +
		Worley(uv0 * 4 + Random::R2Modified(2) * 100, scale0 * 4) * .125;
	noise0 = 1 - noise0;

    float2x2 rotmat1 = rotationMatrix(rot1);
	float2 uv1 = mul(rotmat1, uv * scale1) + offset1;
	float noise1 =
		Worley(uv1 + Random::R2Modified(3) * 100, scale1) * .625 +
		Worley(uv1 * 2 + Random::R2Modified(4) * 100, scale1 * 2) * .25 +
		Worley(uv1 * 4 + Random::R2Modified(5) * 100, scale1 * 4) * .125;
	noise1 = 1 - noise1;

    float2x2 rotmat2 = rotationMatrix(rot2);
	float2 uv2 = mul(rotmat2, uv * scale2) + offset2;
	float noise2 =
		Worley(uv2 + Random::R2Modified(6) * 100, scale2) * .625 +
		Worley(uv2 * 2 + Random::R2Modified(7) * 100, scale2 * 2) * .25 +
		Worley(uv2 * 4 + Random::R2Modified(8) * 100, scale2 * 4) * .125;
	noise2 = 1 - noise2;
	float noise = noise0 * noise1 * noise2;
	noise = saturate((noise - clip_range.x) / (clip_range.y - clip_range.x));

	RWTexOutput[uint3(tid, 0)] = 0.;                 // min_h
	RWTexOutput[uint3(tid, 1)] = 1.;                 // max_h
	RWTexOutput[uint3(tid, 2)] = pow(noise, power);  // coverage
	RWTexOutput[uint3(tid, 3)] = noise;              // cloud_type
	RWTexOutput[uint3(tid, 4)] = wispiness;          // bottom_type
}