#ifndef ENVIRONMENT_PROBE_SAMPLING_HLSLI
#define ENVIRONMENT_PROBE_SAMPLING_HLSLI

namespace EnvironmentProbe
{
	int2 WrapPixel(int2 pixel, int2 dimensions)
	{
		// Crossing a square edge continues along its reflected octahedral edge.
		if (pixel.x < 0 || pixel.x >= dimensions.x) {
			pixel.x = pixel.x < 0 ? -pixel.x - 1 : 2 * dimensions.x - pixel.x - 1;
			pixel.y = dimensions.y - pixel.y - 1;
		}
		if (pixel.y < 0 || pixel.y >= dimensions.y) {
			pixel.y = pixel.y < 0 ? -pixel.y - 1 : 2 * dimensions.y - pixel.y - 1;
			pixel.x = dimensions.x - pixel.x - 1;
		}
		return pixel;
	}

	float3 SampleRadiance(Texture2D<float4> radianceTexture, SamplerState linearSampler, float2 uv, uint2 dimensions)
	{
		float2 texel = saturate(uv) * dimensions - 0.5;
		float3 color = 0;
		[branch] if (all(texel >= 0) && all(texel <= float2(dimensions - 1)))
		{
			color = radianceTexture.SampleLevel(linearSampler, uv, 0).rgb;
		}
		else
		{
			// Clamp/wrap addressing cannot represent the octahedron's reflected seams.
			// Only the outer half-texel needs explicit taps; the interior uses hardware filtering.
			int2 base = int2(floor(texel));
			float2 weight = frac(texel);
			float3 c00 = radianceTexture.Load(int3(WrapPixel(base, dimensions), 0)).rgb;
			float3 c10 = radianceTexture.Load(int3(WrapPixel(base + int2(1, 0), dimensions), 0)).rgb;
			float3 c01 = radianceTexture.Load(int3(WrapPixel(base + int2(0, 1), dimensions), 0)).rgb;
			float3 c11 = radianceTexture.Load(int3(WrapPixel(base + 1, dimensions), 0)).rgb;
			color = lerp(lerp(c00, c10, weight.x), lerp(c01, c11, weight.x), weight.y);
		}
		return color;
	}
}
#endif
