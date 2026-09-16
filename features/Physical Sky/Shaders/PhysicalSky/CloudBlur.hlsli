#ifndef PHYSICAL_SKY_CLOUD_BLUR_HLSLI
#define PHYSICAL_SKY_CLOUD_BLUR_HLSLI

Texture2D<float> TexCloudResultTr : register(t38);
Texture2D<float4> TexCloudResultLum : register(t39);
Texture2D<float4> TexCloudResultAux : register(t40);
RWTexture2D<float> RWTexFilteredTr : register(u3);
RWTexture2D<float4> RWTexFilteredLum : register(u4);
RWTexture2D<float4> RWTexFilteredAux : register(u5);

[numthreads(8, 8, 1)] void filterCloud(uint2 pixel : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(pixel >= uint2(info.activeFrameDim)))
		return;
	const float4 metadata = TexCloudResultAux[pixel];
	const float falloff = 1.0 - saturate(abs(metadata.x - 1.0) / 6.0);
	const float radius = pow(falloff, 4.0) * 0.67 + 0.33;
	const int2 maximum = int2(info.activeFrameDim) - 1;
	float4 color = 0.0;
	float transmittance = 0.0;
	float2 transmittanceRange = float2(1, 0);
	float weightSum = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const int2 samplePixel = clamp(int2(pixel) + int2(x, y), 0, maximum);
			const float sceneDepth = TexCloudResultAux[samplePixel].y;
			if (abs(sceneDepth - metadata.y) > 0.064)
				continue;
			const float weight = (x == 0 ? 1.0 - radius : radius * 0.5) * (y == 0 ? 1.0 - radius : radius * 0.5);
			const float sampleTr = TexCloudResultTr[samplePixel];
			transmittanceRange = float2(min(transmittanceRange.x, sampleTr), max(transmittanceRange.y, sampleTr));
			transmittance += sampleTr * weight;
			color += TexCloudResultLum[samplePixel] * weight;
			weightSum += weight;
		}
	}
	color = weightSum > 0.0 ? color / weightSum : TexCloudResultLum[pixel];
	RWTexFilteredTr[pixel] = weightSum > 0.0 ? clamp(transmittance / weightSum, transmittanceRange.x, transmittanceRange.y) : TexCloudResultTr[pixel];
	RWTexFilteredLum[pixel] = color;
	RWTexFilteredAux[pixel] = metadata;
}

#endif
