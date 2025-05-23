#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

#include "XeGTAO/XeGTAO.hlsli"

Texture2D<uint> texAO : register(t0);

RWTexture2D<uint> outBlurred : register(u0);

SamplerState linearSampler : register(s0);

float GetAO(int2 coord, out float3 bentNormal)
{
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	uint packed = texAO.Load(int3(coord, 0));
	float unpackedAO = 1;
	XeGTAO_DecodeVisibilityBentNormal(packed, unpackedAO, bentNormal);
	return unpackedAO;
#else
	bentNormal = 0;
	uint packed = texAO.Load(int3(coord, 0)).x;
	return (float)packed / 255.0f;
#endif
}

[numthreads(XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1)] void main(const uint2 dtid
																	  : SV_DispatchThreadID) {
	float invResX = FrameBuffer::DynamicResolutionParams2.x;
	float invResY = FrameBuffer::DynamicResolutionParams2.y;

	float2 texCoord = (dtid + 0.5) * float2(invResX, invResY);

	float tolerance_mult = 1.f;
	static const int radius = 4;

	float sum_data = 0;
	float sum_weight = 0;

	float3 bentNormal = 0;
	float dataCenter = GetAO(dtid, bentNormal);

	float center_view_depth = SharedData::GetScreenDepth(texCoord);

	for (int x = -radius; x < (radius + 1); x++) {
		for (int y = -radius; y < (radius + 1); y++) {
			float2 sample_tex_coord = clamp(texCoord.xy + float2(x, y) * float2(invResX, invResY), 0, 1);
			int2 sample_pixel_coord = dtid + int2(x, y);
			float3 tempnormal = 0;
			float data_sample = GetAO(sample_pixel_coord, tempnormal);

			float blur_sample_view_depth = SharedData::GetScreenDepth(sample_tex_coord);

			//Depthaware weight
			float weight = exp(-abs(center_view_depth - blur_sample_view_depth) * 100.0f * tolerance_mult);

			sum_data += data_sample * weight;
			sum_weight += weight;
		}
	}

	float output = sum_data / sum_weight;

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	outBlurred[dtid] = XeGTAO_EncodeVisibilityBentNormal(output, bentNormal);
#else
	outBlurred[dtid] = uint(output * 255.0f + 0.5f);
#endif
}