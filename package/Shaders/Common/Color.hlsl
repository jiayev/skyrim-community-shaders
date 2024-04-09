static float GammaCorrectionValue = 2.2;

float RGBToLuminance(float3 color)
{
	return dot(color, float3(0.2125, 0.7154, 0.0721));
}

float RGBToLuminanceAlternative(float3 color)
{
	return dot(color, float3(0.3, 0.59, 0.11));
}

float RGBToLuminance2(float3 color)
{
	return dot(color, float3(0.299, 0.587, 0.114));
}

float3 sRGB2Lin(float3 color)
{
	return color > 0.04045 ? pow(color / 1.055 + 0.055 / 1.055, 2.4) : color / 12.92;
}

float3 Lin2sRGB(float3 color)
{
	return color > 0.0031308 ? 1.055 * pow(color, 1.0 / 2.4) - 0.055 : 12.92 * color;
}