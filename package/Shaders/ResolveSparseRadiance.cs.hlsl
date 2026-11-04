#include "Common/SparseRadiance.hlsli"

Texture2D<float4> Input : register(t0);
Texture2D<float4> InputSH1 : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float3> Normals : register(t3);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> OutputSH1 : register(u1);

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	uint2 size = (uint2)(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
	if (any(tid.xy >= size))
		return;
#ifdef SPARSE_FIXED_GRID
	// RR consumes a complete image. Reconstruct a stationary lattice rather than
	// repeating individual samples from a checkerboard that changes each frame.
	uint2 step = uint2(2, SPARSE_MODE == 2 ? 2 : 1);
	uint2 base = tid.xy / step;
	float2 fraction = float2(tid.xy % step) / float2(step);
	uint2 lastSample = (size - 1u) / step;
	float centerZ = SharedData::CameraData.w / (SharedData::CameraData.x - Depth[tid.xy] * SharedData::CameraData.z);
	float3 centerNormal = GBuffer::DecodeNormal(Normals[tid.xy].xy);
	float4 value = 0;
#	ifdef SPARSE_SH
	float4 sh1 = 0;
#	endif
	float weightSum = 0;
	[unroll] for (uint y = 0; y < 2; ++y)
	{
		[unroll] for (uint x = 0; x < 2; ++x)
		{
			uint2 p = min(base + uint2(x, y), lastSample) * step;
			float sampleZ = SharedData::CameraData.w / (SharedData::CameraData.x - Depth[p] * SharedData::CameraData.z);
			float depthWeight = exp2(-64.0 * abs(sampleZ - centerZ) / max(centerZ, 1e-6));
			float normalWeight = pow(saturate(dot(centerNormal, GBuffer::DecodeNormal(Normals[p].xy))), 32.0);
			float2 spatialWeight = lerp(1.0 - fraction, fraction, float2(x, y));
			// Retain a normalized spatial fallback when all samples cross an edge.
			float weight = spatialWeight.x * spatialWeight.y * max(depthWeight * normalWeight, 1e-6);
			uint2 source = SPARSE_MODE == 1 ? uint2(p.x / 2u, p.y) : p;
			value += Input[source] * weight;
#	ifdef SPARSE_SH
			sh1 += InputSH1[source] * weight;
#	endif
			weightSum += weight;
		}
	}
	Output[tid.xy] = value / weightSum;
#	ifdef SPARSE_SH
	OutputSH1[tid.xy] = sh1 / weightSum;
#	endif
#else
	uint2 source = SparseRadianceCoord(tid.xy, SPARSE_MODE, SPARSE_PARITY, Depth, Normals);
	float4 value = Input[source];
#	if SPARSE_MODE == 2
	// Undo the tracer's probabilistic energy compensation when filling every pixel.
	value.xyz *= 0.25;
#	endif
	Output[tid.xy] = value;
#	ifdef SPARSE_SH
	float4 sh1 = InputSH1[source];
#		if SPARSE_MODE == 2
	sh1.xyz *= 0.25;
#		endif
	OutputSH1[tid.xy] = sh1;
#	endif
#endif
}
