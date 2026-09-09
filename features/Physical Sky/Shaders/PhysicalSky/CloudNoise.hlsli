#ifndef PHYSICAL_SKY_CLOUD_NOISE_HLSLI
#define PHYSICAL_SKY_CLOUD_NOISE_HLSLI

float ReconstructCloudNoiseDensity(float4 noise, float coverage, float roundness, float verticalProfile,
	float distanceMeters, bool includeDetail)
{
	coverage = saturate(coverage);
	roundness = saturate(roundness);
	verticalProfile = saturate(verticalProfile);
	if (coverage <= 0.0 || verticalProfile <= 0.0)
		return 0.0;

	noise = saturate(noise);
	const float base = lerp(noise.g, noise.r, coverage);
	const float cellular = lerp(noise.a * 0.2 + 0.1, noise.b * 0.3, pow(coverage, 0.0625));
	float threshold = lerp(base, cellular, roundness);
	if (includeDetail && distanceMeters < 150.0) {
		const float foldedA = abs(abs(noise.a * 2.0 - 1.0) * 2.0 - 1.0);
		const float foldedG = abs(abs(noise.g * 2.0 - 1.0) * 2.0 - 1.0);
		const float wispy = 1.0 - pow(foldedG, 4.0);
		const float nearThreshold = saturate(lerp(wispy, foldedA * foldedA, roundness));
		threshold = lerp(nearThreshold, threshold, 0.9 + 0.1 * saturate((distanceMeters - 50.0) * 0.01));
	}
	// An empty thresholded sample must not enter the density power response.
	if (coverage <= threshold)
		return 0.0;
	const float eroded = saturate((coverage - threshold) / (1.0 - threshold));
	const float profileDensity = pow(verticalProfile, 4.0);
	const float density = profileDensity * eroded;
	if (density <= 0.0)
		return 0.0;
	return pow(density, 0.3 + 0.3 * profileDensity);
}

#endif
