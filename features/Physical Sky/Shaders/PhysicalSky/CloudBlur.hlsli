#ifndef PHYSICAL_SKY_CLOUD_BLUR_HLSLI
#define PHYSICAL_SKY_CLOUD_BLUR_HLSLI

Texture2D<float> TexCloudResultTr : register(t29);
Texture2D<float4> TexCloudResultLum : register(t30);
Texture2D<float4> TexCloudResultAux : register(t31);
RWTexture2D<float> RWTexFilteredTr : register(u3);
RWTexture2D<float4> RWTexFilteredLum : register(u4);
RWTexture2D<float4> RWTexFilteredAux : register(u5);

void AccumulateCloudBlur(float sampleTr, float4 sampleColor, float centerTr, float4 centerColor,
	float spatialWeight, inout float transmittance, inout float4 color)
{
	// A small opening can carry far more energy than a cloud-covered pixel
	// when the background is the solar disc. Fade the blur with the optical
	// depth difference. Return rejected weight to the center for both outputs.
	const float low = min(centerTr, sampleTr) + 1e-5;
	const float high = max(centerTr, sampleTr) + 1e-5;
	const float similarity = saturate(2.0 - 0.5 * (high / low));
	const float sampleWeight = spatialWeight * similarity;
	const float centerWeight = spatialWeight - sampleWeight;
	transmittance += sampleTr * sampleWeight + centerTr * centerWeight;
	color += sampleColor * sampleWeight + centerColor * centerWeight;
}

[numthreads(8, 8, 1)] void filterCloudWideFast(uint2 pixel : SV_DispatchThreadID) {
	const int2 maximum = int2(VolumetricCloudBuffer[0].activeFrameDim) - 1;
	if (any(int2(pixel) > maximum))
		return;
	uint2 dimensions;
	TexCloudResultTr.GetDimensions(dimensions.x, dimensions.y);
	const float4 metadata = TexCloudResultAux[pixel];
	const float centerTr = TexCloudResultTr[pixel];
	const float4 centerColor = TexCloudResultLum[pixel];
	const float falloff = 1.0 - saturate(abs(metadata.x - 1.0) / 6.0);
	const float radius = (pow(falloff, 4.0) * 0.67 + 0.33) * 2.0;
	float4 color = 0.0;
	float transmittance = 0.0;
	float weight = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const float2 offset = (float2(i & 1u, i >> 1u) * 2.0 - 1.0) * radius;
		const float2 position = clamp(float2(pixel) + offset, 0.0, float2(maximum));
		const float2 uv = (position + 0.5) / dimensions;
		// Opposite depth discontinuities can average to the center depth.
		const float4 sceneDepths = TexCloudResultAux.GatherGreen(TransmittanceSampler, uv);
		[branch] if (all(abs(sceneDepths - metadata.y) <= 0.064))
		{
			AccumulateCloudBlur(TexCloudResultTr.SampleLevel(TransmittanceSampler, uv, 0),
				TexCloudResultLum.SampleLevel(TransmittanceSampler, uv, 0), centerTr, centerColor, 1.0, transmittance, color);
			weight += 1.0;
		}
		else
		{
			const int2 base = int2(position);
			const float2 fraction = position - base;
			[unroll] for (uint tap = 0u; tap < 4u; ++tap)
			{
				const int2 corner = int2(tap & 1u, tap >> 1u);
				const float2 axisWeight = lerp(1.0 - fraction, fraction, corner);
				const float tapWeight = axisWeight.x * axisWeight.y;
				const int2 samplePixel = min(base + corner, maximum);
				if (tapWeight > 0.0 && abs(TexCloudResultAux[samplePixel].y - metadata.y) <= 0.064) {
					AccumulateCloudBlur(TexCloudResultTr[samplePixel], TexCloudResultLum[samplePixel],
						centerTr, centerColor, tapWeight, transmittance, color);
					weight += tapWeight;
				}
			}
		}
	}
	RWTexFilteredTr[pixel] = weight > 0.0 ? saturate(transmittance / weight) : centerTr;
	RWTexFilteredLum[pixel] = weight > 0.0 ? color / weight : centerColor;
	RWTexFilteredAux[pixel] = metadata;
}

#ifndef CLOUD_BLUR_RADIUS
#	define CLOUD_BLUR_RADIUS 1
#endif
static const uint CloudFilterWidth = 8u + 2u * CLOUD_BLUR_RADIUS;
static const uint CloudFilterCount = CloudFilterWidth * CloudFilterWidth;
groupshared float CloudFilterTr[CloudFilterCount];
groupshared float4 CloudFilterLum[CloudFilterCount];
groupshared float4 CloudFilterAux[CloudFilterCount];

[numthreads(8, 8, 1)] void filterCloud(uint2 pixel : SV_DispatchThreadID, uint2 thread : SV_GroupThreadID, uint lane : SV_GroupIndex) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const int2 maximum = int2(info.activeFrameDim) - 1;
	const int2 origin = int2(pixel - thread) - CLOUD_BLUR_RADIUS;
	[unroll] for (uint load = 0u; load < (CloudFilterCount + 63u) / 64u; ++load)
	{
		const uint index = lane + load * 64u;
		if (index >= CloudFilterCount)
			continue;
		const int2 samplePixel = clamp(origin + int2(index % CloudFilterWidth, index / CloudFilterWidth), 0, maximum);
		CloudFilterTr[index] = TexCloudResultTr[samplePixel];
		CloudFilterLum[index] = TexCloudResultLum[samplePixel];
		CloudFilterAux[index] = TexCloudResultAux[samplePixel];
	}
	GroupMemoryBarrierWithGroupSync();
	if (any(pixel >= uint2(info.activeFrameDim)))
		return;
	const uint center = (thread.y + CLOUD_BLUR_RADIUS) * CloudFilterWidth + thread.x + CLOUD_BLUR_RADIUS;
	const float4 metadata = CloudFilterAux[center];
	const float centerTr = CloudFilterTr[center];
	const float4 centerColor = CloudFilterLum[center];
	const float falloff = 1.0 - saturate(abs(metadata.x - 1.0) / 6.0);
	const float radius = (pow(falloff, 4.0) * 0.67 + 0.33) * CLOUD_BLUR_RADIUS;
	float4 color = 0.0;
	float transmittance = 0.0;
	float weightSum = 0.0;
	[unroll] for (int y = -CLOUD_BLUR_RADIUS; y <= CLOUD_BLUR_RADIUS; ++y)
	{
		[unroll] for (int x = -CLOUD_BLUR_RADIUS; x <= CLOUD_BLUR_RADIUS; ++x)
		{
			const uint sampleIndex = uint(int(center) + y * int(CloudFilterWidth) + x);
			const float sceneDepth = CloudFilterAux[sampleIndex].y;
			if (abs(sceneDepth - metadata.y) > 0.064)
				continue;
#if CLOUD_BLUR_RADIUS == 1
			const float weight = (x == 0 ? 1.0 - radius : radius * 0.5) * (y == 0 ? 1.0 - radius : radius * 0.5);
#else
			// Expand the four bilinear samples at (+/-radius, +/-radius) into
			// texel weights, preserving the scene-depth rejection at each texel.
			const float2 axisWeight = (saturate(1.0 - abs(float2(x, y) - radius)) + saturate(1.0 - abs(float2(x, y) + radius))) * 0.5;
			const float weight = axisWeight.x * axisWeight.y;
#endif
			AccumulateCloudBlur(CloudFilterTr[sampleIndex], CloudFilterLum[sampleIndex],
				centerTr, centerColor, weight, transmittance, color);
			weightSum += weight;
		}
	}
	RWTexFilteredTr[pixel] = weightSum > 0.0 ? saturate(transmittance / weightSum) : centerTr;
	RWTexFilteredLum[pixel] = weightSum > 0.0 ? color / weightSum : centerColor;
	RWTexFilteredAux[pixel] = metadata;
}

#endif
