#ifndef PHYSICAL_SKY_CLOUD_BLUR_HLSLI
#define PHYSICAL_SKY_CLOUD_BLUR_HLSLI

Texture2D<float> TexCloudResultTr : register(t38);
Texture2D<float3> TexCloudResultLum : register(t39);
Texture2D<float4> TexCloudResultAux : register(t40);

RWTexture2D<float> RWTexFilteredTr : register(u3);
RWTexture2D<float3> RWTexFilteredLum : register(u4);

struct CloudFilterResult
{
	float transmittance;
	float3 lum;
};

CloudFilterResult FilterCloud(uint2 tid, float2 dims, float2 bounds)
{
	CloudFilterResult result;
	const float4 aux = TexCloudResultAux[tid];
	const float falloff = 1.0 - saturate(abs(aux.x - 1.0) * (1.0 / 6.0));
	const float radius = pow(falloff, 4.0) * 0.67 + 0.33;
	const float2 center = float2(tid) + 0.5;
	const float2 lower = max(center - radius, 0.5);
	const float2 upper = min(center + radius, bounds - 0.5);
	result.transmittance = (TexCloudResultTr.SampleLevel(TransmittanceSampler, float2(lower.x, upper.y) / dims, 0) + TexCloudResultTr.SampleLevel(TransmittanceSampler, upper / dims, 0) + TexCloudResultTr.SampleLevel(TransmittanceSampler, lower / dims, 0) + TexCloudResultTr.SampleLevel(TransmittanceSampler, float2(upper.x, lower.y) / dims, 0)) * 0.25;
	result.lum = (TexCloudResultLum.SampleLevel(TransmittanceSampler, float2(lower.x, upper.y) / dims, 0) + TexCloudResultLum.SampleLevel(TransmittanceSampler, upper / dims, 0) + TexCloudResultLum.SampleLevel(TransmittanceSampler, lower / dims, 0) + TexCloudResultLum.SampleLevel(TransmittanceSampler, float2(upper.x, lower.y) / dims, 0)) * 0.25;
	return result;
}

[numthreads(8, 8, 1)] void filterCloud(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(tid >= uint2(info.activeFrameDim)))
		return;
	uint2 dims;
	TexCloudResultTr.GetDimensions(dims.x, dims.y);
	const CloudFilterResult result = FilterCloud(tid, float2(dims), info.activeFrameDim);
	RWTexFilteredTr[tid] = result.transmittance;
	RWTexFilteredLum[tid] = result.lum;
}

#endif
