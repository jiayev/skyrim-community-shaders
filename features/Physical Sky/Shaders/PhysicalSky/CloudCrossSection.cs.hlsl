SamplerState TileableSampler : register(s2);
SamplerState TransmittanceSampler : register(s3);
RWTexture2D<float4> OutColor : register(u0);

#include "PhysicalSky/WeatherCloudShape.hlsli"

cbuffer CrossSectionCB : register(b0)
{
	float4 Field;
	float4 CameraSpan;
	float4 Shape;
	float4 Extinction;
	float4 Wind;
	float ProceduralWeight;
	uint FiniteCloudWeather;
};

[numthreads(8, 8, 1)] void main(uint2 pixel : SV_DispatchThreadID) {
	uint2 size;
	OutColor.GetDimensions(size.x, size.y);
	if (any(pixel >= size))
		return;

	const float2 uv = (float2(pixel) + 0.5) / size;
	const float altitude = lerp(Field.w, Field.z, uv.y);
	const float2 layerDepth = (Shape.xy - Field.z) / Shape.z;
	const float worldX = CameraSpan.x + (uv.x - 0.5) * CameraSpan.z;
	float depth = 0.0;
	[loop] for (uint slice = 0u; slice < 8u; ++slice)
	{
		const float worldY = CameraSpan.y + (((float(slice) + 0.5) / 8.0) - 0.5) * CameraSpan.w;
		const float2 field = float2(worldX, worldY) - Wind.xy;
		const float4 weather = SampleCloudWeatherMap(field * Field.xy, ProceduralWeight, FiniteCloudWeather);
		const float3 radial = WeatherNoiseDirection(field, Shape.z);
		const float3 detailPosition = float3(field, altitude - Field.z) / Shape.z;
		const float4 shaping = float4(Wind.z, Wind.w / Shape.z, Shape.w / Shape.z, Extinction.w);
		const float3 body = SampleWeatherLayer(weather.xy, layerDepth.x, 0u, radial, detailPosition, shaping, 0.0, 0.0, false);
		const float3 storm = SampleWeatherLayer(weather.zw, layerDepth.y, 1u, radial, detailPosition, shaping, 0.0, 0.0, false);
		const float2 peak = WeatherPeakExtinction(float2(body.z, storm.z), weather.y, Extinction.xyz);
		depth += dot(float2(body.x * WeatherBaseExtinctionScale(weather.x, ProceduralWeight), storm.x), peak) * 80.0;
	}
	OutColor[pixel] = float4(1.0, 1.0, 1.0, 1.0 - exp(-depth));
}
