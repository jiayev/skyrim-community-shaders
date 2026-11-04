#ifndef SPARSE_RADIANCE_HLSLI
#define SPARSE_RADIANCE_HLSLI

#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

// Find a traced surface near this pixel. Half resolution is column-packed,
// quarter resolution keeps the rotating 2x2 phase at full-resolution coordinates.
uint2 SparseRadianceCoord(uint2 pixel, uint mode, uint parity, Texture2D<float> depth, Texture2D<float3> normals)
{
	if (mode == 0)
		return pixel;
	bool alreadyTraced = mode == 2 ? all((pixel & 1u) == uint2(SharedData::FrameCount & 1u, (SharedData::FrameCount >> 1u) & 1u)) :
	                                 ((pixel.x & 1u) == (((pixel.y + SharedData::FrameCount) & 1u) ^ parity));
	if (alreadyTraced)
		return mode == 1 ? uint2(pixel.x / 2u, pixel.y) : pixel;
	uint2 size = (uint2)(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
	float centerDepth = depth[pixel];
	float3 centerNormal = GBuffer::DecodeNormal(normals[pixel].xy);
	float bestScore = 1e30;
	uint2 best = pixel;
	[unroll] for (int y = -2; y <= 2; ++y)
	{
		[unroll] for (int x = -2; x <= 2; ++x)
		{
			int2 candidate = int2(pixel) + int2(x, y);
			if (any(candidate < 0) || any(candidate >= int2(size)))
				continue;
			uint2 p = uint2(candidate);
			bool traced = mode == 2 ? all((p & 1u) == uint2(SharedData::FrameCount & 1u, (SharedData::FrameCount >> 1u) & 1u)) :
			                          ((p.x & 1u) == (((p.y + SharedData::FrameCount) & 1u) ^ parity));
			if (!traced)
				continue;
			float score = abs(depth[p] - centerDepth) / max(1.0 - centerDepth, 1e-6) * 1000.0;
			score += (1.0 - saturate(dot(centerNormal, GBuffer::DecodeNormal(normals[p].xy)))) * 10.0;
			score += float(x * x + y * y) * 0.01;
			if (score < bestScore) {
				bestScore = score;
				best = p;
			}
		}
	}
	return mode == 1 ? uint2(best.x / 2u, best.y) : best;
}
#endif
