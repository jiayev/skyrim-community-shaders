#include "Common/EnvironmentProbe.hlsli"

cbuffer BuildProbeDepth : register(b0)
{
	uint4 Faces[2];
	uint Size;
	float CaptureFar;
	uint SharedSeams;
	uint Padding;
};

#if defined(REDUCE_PROBE_DEPTH)
Texture2DArray<float2> SourceDepth : register(t0);
#else
Texture2D<float> SourceDepth : register(t0);
RWTexture2DArray<float2> Mip0 : register(u0);
#endif
RWTexture2DArray<float2> Mip1 : register(u1);
RWTexture2DArray<float2> Mip2 : register(u2);
RWTexture2DArray<float2> Mip3 : register(u3);
RWTexture2DArray<float2> Mip4 : register(u4);

groupshared float2 Bounds[256];

float2 MergeBounds(float2 a, float2 b)
{
	// The sign of the minimum also records unknown coverage (an unowned texel
	// or invalid depth). Such a tile can reject geometry, but cannot establish
	// that a ray has entered the front/free side of a surface.
	float minimum = min(abs(a.x), abs(b.x));
	return float2(a.x < 0 || b.x < 0 ? -minimum : minimum, max(a.y, b.y));
}

// Two dispatches build a 256^2 octant pyramid: mip 0..4, then 5..8.
// Every array slice has its own capture origin; no reduction crosses octants.
[numthreads(16, 16, 1)] void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex) {
	uint face = Faces[group.z >> 2][group.z & 3];
	uint2 pixel = group.xy * 16 + local.xy;
	float2 bounds = float2(-1e30, -1e30);
	if (all(pixel < Size)) {
#if defined(REDUCE_PROBE_DEPTH)
		bounds = SourceDepth.Load(int4(pixel, face, 0));
#else
		uint2 quadrant = uint2((face & 1u) == 0, (face & 2u) == 0);
		uint2 sourcePixel = pixel + quadrant * Size;
		float2 uv = (sourcePixel + 0.5) / (Size * 2);
		float2 p = uv * 2.0 - 1.0;
		float z = 1.0 - abs(p.x) - abs(p.y);
		float3 direction = EnvironmentProbe::Decode(uv);
		bool overlaps = ((face & 4u) ? -z : z) >= -rcp(float(Size));
		if (EnvironmentProbe::GetFace(direction) == face || ((SharedSeams & (1u << face)) && overlaps)) {
			bounds = float2(1e30, -1e30);
			float depth = SourceDepth.Load(int3(sourcePixel, 0));
			if (!isfinite(depth) || depth <= 0) {
				bounds = float2(-1e30, -1e30);
			} else if (depth < CaptureFar) {
				// Unnormalized octahedral direction has L1 length 1. Its L2
				// length varies by at most sqrt(6)/sourceSize within a texel.
				// Thus these bounds include the whole radial-depth texel patch,
				// not just its center, when expressed as w = dot(faceSign, p).
				float2 sign = float2((face & 1u) ? -1 : 1, (face & 2u) ? -1 : 1);
				float3 unfolded = float3((face & 4u) ? (1.0 - abs(p.yx)) * sign : p, z);
				float norm = length(unfolded);
				float delta = sqrt(6.0) / (Size * 2);
				bounds = float2(depth / min(1.0, norm + delta), depth / max(rsqrt(3.0), norm - delta));
				bounds *= float2(1.0 - 1e-5, 1.0 + 1e-5);
			}
		}
		Mip0[uint3(pixel, face)] = bounds;
#endif
	}
	Bounds[index] = bounds;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint level = 1; level <= 4; ++level)
	{
		uint stride = 1u << level;
		if (all((local.xy & (stride - 1)) == 0)) {
			uint halfStride = stride >> 1;
			float2 reduced = MergeBounds(Bounds[index], Bounds[index + halfStride]);
			reduced = MergeBounds(reduced, Bounds[index + halfStride * 16]);
			reduced = MergeBounds(reduced, Bounds[index + halfStride * 17]);
			Bounds[index] = reduced;
			uint2 target = pixel >> level;
			if (Size >= stride && all(target < (Size >> level))) {
				if (level == 1)
					Mip1[uint3(target, face)] = reduced;
				if (level == 2)
					Mip2[uint3(target, face)] = reduced;
				if (level == 3)
					Mip3[uint3(target, face)] = reduced;
				if (level == 4)
					Mip4[uint3(target, face)] = reduced;
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
}
