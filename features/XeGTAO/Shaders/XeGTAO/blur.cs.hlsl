#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

#include "XeGTAO/XeGTAO.hlsli"

Texture2D<uint> texNormal : register(t0);

RWTexture2D<uint> outBlurred : register(u0);

SamplerState linearSampler : register(s0);

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
    float invResX = FrameBuffer::DynamicResolutionParams2.x;
    float invResY = FrameBuffer::DynamicResolutionParams2.y;

	float2 texCoord = (dtid + 0.5) * float2(invResX, invResY);

	float tolerance_mult = 1.0f;
	static const int radius = 2;

	float3 sum_data = 0;
	float sum_weight = 0;

	float center_view_depth = SharedData::GetScreenDepth(texCoord);

	for (int x = -radius; x < (radius + 1); x++) {
		for (int y = -radius; y < (radius + 1); y++) {
			float2 sample_tex_coord = clamp(texCoord.xy + float2(x, y) * float2(invResX, invResY), 0, 1);
            uint2 sample_pixel_coord = dtid + uint2(x, y);
			uint data_sample_packed = texNormal.Load(sample_pixel_coord).x;
            float3 data_sample = XeGTAO_R11G11B10_UNORM_to_FLOAT3(data_sample_unpacked);

			float blur_sample_view_depth = SharedData::GetScreenDepth(sample_tex_coord);

			//Depthaware weight
			float weight = exp(-abs(center_view_depth - blur_sample_view_depth) * 100.0f * tolerance_mult);

			sum_data += data_sample * weight;
			sum_weight += weight;
		}
	}

    float3 output = sum_data / sum_weight;

	outBlurred[dtid] = XeGTAO_FLOAT3_to_R11G11B10_UNORM(output);
}