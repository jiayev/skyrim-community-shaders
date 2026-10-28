#ifndef ENVIRONMENT_PROBE_VISIBILITY_HLSLI
#define ENVIRONMENT_PROBE_VISIBILITY_HLSLI

#if defined(SKYLIGHTING)
#	define SKYLIGHTING_PROBE_REGISTER t6
#	include "Skylighting/Skylighting.hlsli"
#endif

// A traced ray is a direction, not a cosine lobe: do not convolve visibility
// again. Unknown single-layer visibility uses the independent world volume.
float ProbeUnknownVisibility(float3 origin, float3 direction)
{
#if defined(SKYLIGHTING)
	float3 position = Skylighting::ClampToArrayHeight(origin);
	sh2 visibility = Skylighting::Sample(position, direction);
	float directionalVisibility = saturate(SphericalHarmonics::Unproject(visibility, direction));
	return lerp(1.0, directionalVisibility, Skylighting::GetHorizontalFadeOutFactor(position));
#else
	// Without a visibility volume, unknown space is not evidence of open sky.
	return 0;
#endif
}
#endif
