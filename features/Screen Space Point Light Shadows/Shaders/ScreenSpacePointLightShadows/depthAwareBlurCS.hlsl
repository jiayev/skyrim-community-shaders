#include "Common/SharedData.hlsli"

Texture2D<float> texDepth : register(t0);
Texture2D<float4> texShadow : register(t1);

RWTexture2D<float4> outBlurredShadow : register(u0);

SamplerState linearSampler : register(s0);

cbuffer blurBuffer : register(b1)
{
	uint MipLevel;
	float Scale;
	uint ResX;
	uint ResY;
};

[numthreads(8, 8, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
	float2 texCoord = (dtid + 0.5) / float2(ResX, ResY);
	if (MipLevel <= 1) {
		outBlurredShadow[dtid] = texShadow[dtid];
	}

	float tolerance_mult = 1.0f / float(pow(2, MipLevel));
	static const int radius = 2;

	float4 sum_data = 0;
	float sum_weight = 0;

	float center_view_depth = SharedData::GetScreenDepth(texDepth.SampleLevel(linearSampler, texCoord, 0).x);

	for (int x = -radius; x < (radius + 1); x++) {
		for (int y = -radius; y < (radius + 1); y++) {
			float2 sample_pixel_coord = clamp(texCoord.xy + float2(x, y) / float2(ResX, ResY), 0, 1);
			float4 data_sample = texShadow.SampleLevel(linearSampler, sample_pixel_coord, 0);

			float blur_sample_depth = texDepth.SampleLevel(linearSampler, sample_pixel_coord, 0).x;
			float blur_sample_view_depth = SharedData::GetScreenDepth(blur_sample_depth);

			//Depthaware weight
			float weight = exp(-abs(center_view_depth - blur_sample_view_depth) * 100.0f * tolerance_mult);

			sum_data += data_sample * weight;
			sum_weight += weight;
		}
	}

	outBlurredShadow[dtid] = sum_data / sum_weight;
}