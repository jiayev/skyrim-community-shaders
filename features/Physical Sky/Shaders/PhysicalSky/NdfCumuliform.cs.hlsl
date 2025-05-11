#ifndef COMPUTESHADER
#   define COMPUTESHADER
#endif

#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

RWTexture2DArray<unorm float> RWTexOutput : register(u0);

float2 hash22(int2 seed)
{
	return Random::pcg2d(asuint(seed)) / 4294967295.f;
}

float Worley(float2 uv, uint freq)
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

[numthreads(8, 8, 1)] 
void main(uint2 tid : SV_DispatchThreadID) {
    uint3 dims;
    RWTexOutput.GetDimensions(dims.x, dims.y, dims.z);
    float2 uv = (tid + 0.5) / dims.xy;

    float2 uv0 = uv * 10 + SharedData::Timer * 0.005;
	float noise0 = 
        Worley(uv0 + Random::R2Modified(0) * 100, 10) * .625 +
        Worley(uv0 * 2 + Random::R2Modified(1) * 100, 10 * 2) * .25 +
        Worley(uv0 * 4 + Random::R2Modified(2) * 100, 10 * 4) * .125;
	noise0 = 1 - noise0;

	float2 uv1 = uv * 20 + SharedData::Timer * 0.01;
	float noise1 = 
        Worley(uv1 + Random::R2Modified(3) * 100, 20) * .625 + 
        Worley(uv1 * 2 + Random::R2Modified(4) * 100, 20 * 2) * .25 + 
        Worley(uv1 * 4 + Random::R2Modified(5) * 100, 20 * 4) * .125;
	noise1 = 1 - noise1;

	float2 uv2 = uv * 40 + SharedData::Timer * 0.04;
	float noise2 = 
        Worley(uv2 + Random::R2Modified(6) * 100, 40) * .625 + 
        Worley(uv2 * 2 + Random::R2Modified(7) * 100, 40 * 2) * .25 + 
        Worley(uv2 * 4 + Random::R2Modified(8) * 100, 40 * 4) * .125;
	noise2 = 1 - noise2;
	float noise = noise0 * noise1 * noise2;

    float min_h = 0;
	float max_h = 1;
    float coverage = pow(saturate((noise - 0.4) / 0.6), 0.7);
    float cloud_type = saturate((noise - 0.4) / 0.6);
	float bottom_type = 0;

    RWTexOutput[uint3(tid, 0)] = min_h;
    RWTexOutput[uint3(tid, 1)] = max_h;
    RWTexOutput[uint3(tid, 2)] = coverage;
    RWTexOutput[uint3(tid, 3)] = cloud_type;
    RWTexOutput[uint3(tid, 4)] = bottom_type;
}