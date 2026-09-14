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
	const float2 center = float2(pixel) + 0.5;
	const float2 lower = max(center - radius, 0.5) * info.rcpFrameDim;
	const float2 upper = min(center + radius, info.activeFrameDim - 0.5) * info.rcpFrameDim;
	const float2 corners[4] = { float2(lower.x, upper.y), upper, lower, float2(upper.x, lower.y) };
	float4 color = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		color += TexCloudResultLum.SampleLevel(TransmittanceSampler, corners[i], 0);
	}
	color *= 0.25;
	RWTexFilteredTr[pixel] = 1.0 - color.a;
	RWTexFilteredLum[pixel] = color;
	RWTexFilteredAux[pixel] = metadata;
}

#endif
