#ifndef PHYSICAL_SKY_CLOUD_NOISE_HLSLI
#define PHYSICAL_SKY_CLOUD_NOISE_HLSLI

float ReconstructCloudNoiseDensity(float4 noise, float profile, float topType, float height, float distanceMeters)
{
	const float erosion = pow(noise.a, 0.45) * -0.3;
	const float smallErosion = noise.a * 0.2;
	const float heightNoise = pow(noise.r, 0.2) - smallErosion *
	                                                  (1.0 - pow(saturate((saturate((topType - 0.2) * 9.999999) * 5.0 + 5.0) * height), 3.0));
	const float rounded = saturate((pow(noise.r, 0.45) - erosion) / (1.0 - erosion));
	const float billow = sqrt(saturate((saturate(lerp(heightNoise, rounded, saturate((profile - 0.1) * 4.9999995))) - 0.4) * 1.6666666));
	const float shape = lerp(noise.b, noise.g, saturate((distanceMeters - 1000.0) * 0.001));
	const float wisps = pow(lerp(noise.a, lerp(shape, noise.g, saturate((profile - 0.2) * 5.0)),
								saturate((profile + 0.6) * 1.111111)),
		0.1);
	const float difference = wisps - billow;
	const float typeBlend = difference * 0.5 * saturate((topType - 0.5) * 9.999998);
	const float composite = (billow + typeBlend + (difference * 0.7 - typeBlend) * saturate((height - 0.1) * 4.9999995)) * 0.975;
	return saturate(composite - (1.0 - min(0.7, profile)));
}

float ShapeCloudBaseDensity(float eroded, float height, float bottomWidth, float bottomPower)
{
	if (eroded <= 0.0)
		return 0.0;
	const float bottomBoost = pow(1.0 - eroded, 10.0) * 8.0;
	const float baseWidth = 10.0 - saturate((bottomWidth - 1.0) * 0.11111111) * 9.0;
	const float baseProfile = pow(height, 0.3) * pow(saturate(height * baseWidth), bottomPower);
	const float density = lerp(bottomBoost, 1.0, saturate(height * 5.0)) * eroded * baseProfile;
	return pow(density, saturate((height - 0.25) * 4.0) * 0.29999998 + 0.35);
}

#endif
