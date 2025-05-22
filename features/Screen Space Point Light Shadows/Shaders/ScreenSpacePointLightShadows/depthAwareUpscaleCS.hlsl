#include "Common/SharedData.hlsli"
#define OFFSET_COUNT 4
#define DEPTH_OFFSET_SCALE 0.5f

// Out Shadow should be one mip level lower than Blurred Shadow.
Texture2D<float> texDepth : register(t0);
Texture2D<float4> texBlurredShadow : register(t1);

RWTexture2D<float4> outShadow : register(u0);

SamplerState linearSampler : register(s0);

cbuffer blurBuffer : register(b1)
{
	uint MipLevel;
	float Scale;
	uint ResX;
	uint ResY;
};

float GetDepthWeight(float center_depth, float sample_depth, float threshold)
{
	return 1.0f - saturate((abs(center_depth - sample_depth) * DEPTH_OFFSET_SCALE - threshold) / threshold);
}

const float2 offsets[OFFSET_COUNT] = {
	float2(0, 0),
	float2(1, 0),
	float2(1, 1),
	float2(0, 1)
};

[numthreads(8, 8, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
	float2 texCoord = (dtid + 0.5) / float2(ResX, ResY);
	float center_depth = texDepth.SampleLevel(linearSampler, texCoord, 0).x;
	float4 shadow = 0;

	float sum_weight = 0;

	for (int i = 0; i < OFFSET_COUNT; i++) {
		float2 offset = offsets[i] / float2(ResX, ResY);
		float2 sample_pixel_coord = clamp(texCoord.xy + offset, 0, 1);
		float4 data_sample = texBlurredShadow.SampleLevel(linearSampler, sample_pixel_coord, 0);

		float depth = texDepth.SampleLevel(linearSampler, sample_pixel_coord, 0).x;
		float weight = GetDepthWeight(center_depth, depth, 0.01f) + 1e-6f;

		sum_weight += weight;
		shadow += data_sample * weight;
	}
	outShadow[dtid] = shadow / sum_weight;
}