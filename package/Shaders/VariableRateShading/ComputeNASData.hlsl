#define GROUP_SIZE (8 * 8)

Texture2D<float3> InputTexture : register(t0);
Texture2D<float2> InputTextureMotionVectors : register(t1);
RWTexture2D<float4> OutputTexture : register(u0);

groupshared float4 sampleCache[GROUP_SIZE];
groupshared float2 sampleCacheMotionVectors[GROUP_SIZE];
groupshared float errXCache[GROUP_SIZE];
groupshared float errYCache[GROUP_SIZE];

float RgbToLuminance(float3 color)
{
	return dot(color, float3(0.299, 0.587, 0.114));
}

[numthreads(8, 8, 1)] void main(uint3 GroupID
								: SV_GroupID, uint3 GroupThreadID
								: SV_GroupThreadID) {
	const uint threadIndex = GroupThreadID.y * 8 + GroupThreadID.x;
	const uint2 sampleIndex = (GroupID.xy * 8 + GroupThreadID.xy) * 2.0;

	// Fetch color (final post-AA) data
	// l0.x  l0.y
	// l0.z  l0.w  l2.x
	// l1.x  l1.y
	// l1.z  l1.w  l2.y
	//		 l2.z
	float4 l0;
	l0.x = RgbToLuminance(InputTexture[sampleIndex + uint2(0, 0)]);
	l0.y = RgbToLuminance(InputTexture[sampleIndex + uint2(1, 0)]);
	l0.z = RgbToLuminance(InputTexture[sampleIndex + uint2(0, 1)]);
	l0.w = RgbToLuminance(InputTexture[sampleIndex + uint2(1, 1)]);

	float4 l1;
	l1.x = RgbToLuminance(InputTexture[sampleIndex + uint2(0, 2)]);
	l1.y = RgbToLuminance(InputTexture[sampleIndex + uint2(1, 2)]);
	l1.z = RgbToLuminance(InputTexture[sampleIndex + uint2(0, 3)]);
	l1.w = RgbToLuminance(InputTexture[sampleIndex + uint2(1, 3)]);

	float3 l2;
	l2.x = RgbToLuminance(InputTexture[sampleIndex + uint2(2, 1)]);
	l2.y = RgbToLuminance(InputTexture[sampleIndex + uint2(2, 3)]);
	l2.z = RgbToLuminance(InputTexture[sampleIndex + uint2(1, 4)]);

	sampleCache[threadIndex] = l0 + l1;

	float2 m = 0.0f;
	m += InputTextureMotionVectors[sampleIndex + uint2(0, 0)];
	m += InputTextureMotionVectors[sampleIndex + uint2(1, 0)];
	m += InputTextureMotionVectors[sampleIndex + uint2(0, 1)];
	m += InputTextureMotionVectors[sampleIndex + uint2(1, 1)];

	m += InputTextureMotionVectors[sampleIndex + uint2(0, 2)];
	m += InputTextureMotionVectors[sampleIndex + uint2(1, 2)];
	m += InputTextureMotionVectors[sampleIndex + uint2(0, 3)];
	m += InputTextureMotionVectors[sampleIndex + uint2(1, 3)];

	sampleCacheMotionVectors[threadIndex] = m;

	// Derivatives X
	float4 a = float4(l0.y, l2.x, l1.y, l2.y);
	float4 b = float4(l0.x, l0.w, l1.x, l1.w);
	float4 dx = abs(a - b);

	// Derivatives Y
	a = float4(l0.z, l1.y, l1.z, l2.z);
	b = float4(l0.x, l0.w, l1.x, l1.w);
	float4 dy = abs(a - b);

	// Compute maximum partial derivative of all 16x16 pixels (256 total)
	// this approach is more "sensitive" to individual outliers in a tile, since it takes the max instead of the average
	float maxDx = max(max(dx.x, dx.y), max(dx.z, dx.w));
	float maxDy = max(max(dy.x, dy.y), max(dy.z, dy.w));

	errXCache[threadIndex] = maxDx;
	errYCache[threadIndex] = maxDy;

	GroupMemoryBarrierWithGroupSync();

	// Parallel reduction
	[unroll] for (uint s = (64 >> 1); s > 0; s >>= 1)
	{
		if (threadIndex < s) {
			sampleCache[threadIndex] += sampleCache[threadIndex + s];
			sampleCacheMotionVectors[threadIndex] += sampleCacheMotionVectors[threadIndex + s];
			errXCache[threadIndex] = max(errXCache[threadIndex], errXCache[threadIndex + s]);
			errYCache[threadIndex] = max(errYCache[threadIndex], errYCache[threadIndex + s]);
		}

		GroupMemoryBarrierWithGroupSync();
	}

	// Average
	if (threadIndex == 0) {
		float avgLuma = dot(sampleCache[0], 1.0 / 8.0) / GROUP_SIZE + 0.1;
		float2 avgMotionVectors = dot(sampleCacheMotionVectors[0], 1.0 / 8.0) / GROUP_SIZE;
		float errX = errXCache[0];
		float errY = errYCache[0];
		OutputTexture[GroupID.xy] = float4(float2(errX, errY) / abs(avgLuma), avgMotionVectors);
	}
}