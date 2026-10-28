#ifndef SSGI_PROBE_GI_HLSLI
#define SSGI_PROBE_GI_HLSLI

Texture2D<float4> ProbeRadianceTexture : register(t7);
Texture2D<float> ProbeDepthTexture : register(t9);
Texture2D<unorm float2> ProbeNormalTexture : register(t10);

#define PROBE_TRACE_NORMALS
#define PROBE_TRACE_VISIBILITY
#define PROBE_TRACE_SAMPLER samplerLinearClamp
#include "Common/EnvironmentProbeTrace.hlsli"
#include "Common/EnvironmentProbeVisibility.hlsli"
#undef PROBE_TRACE_SAMPLER
#undef PROBE_TRACE_NORMALS

float3 SampleProbeSkyRadiance(float3 direction)
{
	float3 radiance = 0;
#if defined(IBL)
	if (SkyIBLValid != 0 && SharedData::iblSettings.EnableIBL && !SharedData::InInterior) {
		// SkyIBL stores raw radiance SH. GetSkyIBL() is already cosine-convolved;
		// integrating it here would convolve twice. Projection samples -rayDir.
		sh2 basis = SphericalHarmonics::Evaluate(-direction);
		float3 sky = max(0, float3(dot(ImageBasedLighting::SkyIBLTexture.Load(int3(0, 0, 0)), basis),
								dot(ImageBasedLighting::SkyIBLTexture.Load(int3(1, 0, 0)), basis),
								dot(ImageBasedLighting::SkyIBLTexture.Load(int3(2, 0, 0)), basis)));
		sky = Color::Saturation(sky, SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
		radiance = ColorManagement::SceneToLinear(sky) * DiffuseCubemapMult;
	}
#endif
	return radiance;
}

float3 SampleProbeGI(float3 origin, float3 direction, float thickness)
{
	float3 radiance, sourceNormal;
	float hitDistance;
	bool unobstructed;
	if (TraceEnvironmentProbe(origin, direction, 0.5, thickness, radiance, hitDistance, unobstructed, sourceNormal)) {
		// A back-facing hit still occludes the sky. Radiance already includes the
		// source's material response, so apply neither source albedo nor cosine again.
		return dot(sourceNormal, -direction) > 0 ? radiance * GIStrength : 0;
	}
	if (UseDynamicCubemap == 0)
		return 0;
	return SampleProbeSkyRadiance(direction) * (unobstructed ? 1.0 : ProbeUnknownVisibility(origin, direction));
}
#endif
