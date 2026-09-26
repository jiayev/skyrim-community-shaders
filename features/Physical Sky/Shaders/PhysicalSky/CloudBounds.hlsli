#ifndef PHYSICAL_SKY_CLOUD_BOUNDS_HLSLI
#define PHYSICAL_SKY_CLOUD_BOUNDS_HLSLI

RWTexture2D<float2> RWCloudHeightBounds : register(u6);
groupshared float2 CloudHeightBoundsShared[256];

void ReduceCloudHeightBounds(uint index, uint count)
{
	GroupMemoryBarrierWithGroupSync();
	[loop] for (uint stride = count / 2u; stride > 0u; stride >>= 1u)
	{
		if (index < stride) {
			const float2 other = CloudHeightBoundsShared[index + stride];
			CloudHeightBoundsShared[index] = float2(min(CloudHeightBoundsShared[index].x, other.x),
				max(CloudHeightBoundsShared[index].y, other.y));
		}
		GroupMemoryBarrierWithGroupSync();
	}
}

[numthreads(8, 8, 1)] void buildCloudHeightBounds(uint2 group : SV_GroupID, uint2 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
	uint2 size, tiles;
	TexCloudHeight.GetDimensions(size.x, size.y);
	RWCloudHeightBounds.GetDimensions(tiles.x, tiles.y);
	const uint2 tileSize = (size + tiles - 1u) / tiles;
	const uint2 origin = group * tileSize;
	const uint2 end = min(origin + tileSize, size);
	float2 bounds = float2(1, 0);
	[loop] for (uint y = origin.y + thread.y; y < end.y; y += 8u)
	{
		[loop] for (uint x = origin.x + thread.x; x < end.x; x += 8u)
		{
			const float2 heights = saturate(TexCloudHeight.Load(int3(x, y, 0)));
			bounds = float2(min(bounds.x, heights.x), max(bounds.y, heights.y));
		}
	}
	CloudHeightBoundsShared[index] = bounds;
	ReduceCloudHeightBounds(index, 64u);
	if (index == 0u)
		RWCloudHeightBounds[group] = CloudHeightBoundsShared[0];
}

	[numthreads(16, 16, 1)] void reduceCloudHeightBounds(uint2 pixel : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
	CloudHeightBoundsShared[index] = TexCloudHeightBounds[pixel];
	ReduceCloudHeightBounds(index, 256u);
	if (index == 0u)
		RWCloudHeightBounds[uint2(0, 0)] = CloudHeightBoundsShared[0];
}

#endif
