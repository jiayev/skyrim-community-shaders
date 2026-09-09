#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif

cbuffer CB : register(b1)
{
	uint packedNdf;
	uint3 padding;
};

Texture2DArray<float4> Ndf : register(t0);
Texture2D<float> Occupancy : register(t1);
RWTexture2D<float> Output : register(u0);

[numthreads(8, 8, 1)] void buildOccupancy(uint2 tid : SV_DispatchThreadID) {
	uint width, height, layers;
	Ndf.GetDimensions(width, height, layers);
	uint2 dims;
	Output.GetDimensions(dims.x, dims.y);
	if (any(tid >= dims))
		return;
	const int2 first = int2(floor(float2(tid) * uint2(width, height) / dims)) - 1;
	const int2 last = int2(ceil(float2(tid + 1u) * uint2(width, height) / dims));
	float coverage = 0.0;
	[loop] for (int y = first.y; y <= last.y; ++y)
		[loop] for (int x = first.x; x <= last.x; ++x)
	{
		const int2 size = int2(width, height);
		const int2 pixel = (int2(x, y) % size + size) % size;
		const float4 value = Ndf.Load(int4(pixel, packedNdf != 0u ? 0 : 2, 0));
		coverage = max(coverage, packedNdf != 0u ? value.b : value.r);
	}
	Output[tid] = coverage;
}

	[numthreads(8, 8, 1)] void buildDistance(uint2 tid : SV_DispatchThreadID)
{
	uint2 dims;
	Output.GetDimensions(dims.x, dims.y);
	if (any(tid >= dims))
		return;
	float distance = 8.0;
	[loop] for (int y = -8; y <= 8; ++y)
		[loop] for (int x = -8; x <= 8; ++x)
	{
		const int2 pixel = (int2(tid) + int2(x, y) + int2(dims)) % int2(dims);
		if (Occupancy[pixel] > 0.0)
			distance = min(distance, length(float2(x, y)));
	}
	// Cover both cells' extents before turning centre distances into empty space.
	Output[tid] = max(distance - 2.0, 0.0);
}
