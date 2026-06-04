#ifndef PHYSICAL_LIGHTING_COLOR_TEMPERATURE_HLSLI
#define PHYSICAL_LIGHTING_COLOR_TEMPERATURE_HLSLI

namespace ColorTemperature
{
	float3 KelvinToRGB(float kelvin, float tint)
	{
		float t = clamp(kelvin, 1000.0f, 40000.0f);
		float t2 = t * t;

		float x;
		if (t < 4000.0f)
			x = -0.2661239e9f / (t2 * t) - 0.2343589e6f / t2 + 0.8776956e3f / t + 0.179910f;
		else
			x = -3.0258469e9f / (t2 * t) + 2.1070379e6f / t2 + 0.2226347e3f / t + 0.240390f;

		float x2 = x * x;
		float y;
		if (t < 2222.0f)
			y = -1.1063814f * x2 * x - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f;
		else if (t < 4000.0f)
			y = -0.9549476f * x2 * x - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f;
		else
			y = 3.0817580f * x2 * x - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;

		y = max(y + clamp(tint, -1.0f, 1.0f) * 0.05f, 1e-5f);

		float Y = 1.0f;
		float X = Y / y * x;
		float Z = Y / y * (1.0f - x - y);

		float3 rgb;
		rgb.r = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
		rgb.g = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
		rgb.b = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;

		return max(rgb, 0.0f);
	}
}

#endif
