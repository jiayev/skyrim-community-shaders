#define PHYSICAL_SKY_VOLUMETRICS
#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#define PS_SKY_SAMPLERS
#define PS_PREPASS_RSRCS
#define PS_NO_RSRCS
#define OMIT_PS_NAMESPACE
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "PhysicalSky/Common.hlsli"

SamplerState TileableSampler : register(s2);
#define TransmittanceSampler SampTr
#define SkyViewSampler SampSv

struct CloudLayer
{
	float bottom;
	float thickness;
	float2 ndf_freq;
	float noise_scale_or_freq;
	float3 noise_offset_or_speed;
	float power;
	float3 scatter;
	float3 absorption;
	float average_density;
	float ms_mult;
	float ms_transmittance_power;
	float ms_height_power;
	float ambient_mult;
	float density_erosion_weak;
	float density_erosion_strong;
	float noise_mip_bias_weak;
	float noise_mip_bias_strong;
	float hhf_min_blend;
	float hhf_profile_threshold;
	float2 _pad0;

	float2 low_frame_dim;
	float2 rcp_low_frame_dim;
	uint history_valid;
	uint3 padding;
};

struct VolumetricCloudData
{
	float rayMarchRange;
	float shadowVolumeRange;
	uint cloudMaxStep;
	uint fullResolution;

	float2 frameDim;
	float2 rcpFrameDim;
	float3 dirlightDir;
	float _pad1;
	float3 dirlightColor;
	float _pad2;
	float bottomZ;
	float planetRadius;
	float atmosThickness;
	float aerialPerspectiveMaxDist;

	float cloudBottom;
	float cloudThickness;
	float2 ndfFreq;
	float noiseFreq;
	float3 noiseOffset;
	float power;
	float3 cloudScatter;
	float3 cloudAbsorption;
	float averageDensity;
	float msMult;
	float msTransmittancePower;
	float msHeightPower;
	float ambientMult;
	float densityErosionWeak;
	float densityErosionStrong;
	float noiseMipBiasWeak;
	float noiseMipBiasStrong;
	float hhfMinBlend;
	float hhfProfileThreshold;
	float2 _pad3;

	float2 lowFrameDim;
	float2 rcpLowFrameDim;
	uint historyValid;
	uint3 padding;
};

CloudLayer GetCloudLayer(VolumetricCloudData info)
{
	CloudLayer cloud;
	cloud.bottom = info.cloudBottom;
	cloud.thickness = info.cloudThickness;
	cloud.ndf_freq = info.ndfFreq;
	cloud.noise_scale_or_freq = info.noiseFreq;
	cloud.noise_offset_or_speed = info.noiseOffset;
	cloud.power = info.power;
	cloud.scatter = info.cloudScatter;
	cloud.absorption = info.cloudAbsorption;
	cloud.average_density = info.averageDensity;
	cloud.ms_mult = info.msMult;
	cloud.ms_transmittance_power = info.msTransmittancePower;
	cloud.ms_height_power = info.msHeightPower;
	cloud.ambient_mult = info.ambientMult;
	cloud.density_erosion_weak = info.densityErosionWeak;
	cloud.density_erosion_strong = info.densityErosionStrong;
	cloud.noise_mip_bias_weak = info.noiseMipBiasWeak;
	cloud.noise_mip_bias_strong = info.noiseMipBiasStrong;
	cloud.hhf_min_blend = info.hhfMinBlend;
	cloud.hhf_profile_threshold = info.hhfProfileThreshold;
	cloud._pad0 = 0;
	cloud.low_frame_dim = info.lowFrameDim;
	cloud.rcp_low_frame_dim = info.rcpLowFrameDim;
	cloud.history_valid = info.historyValid;
	cloud.padding = info.padding;
	return cloud;
}

StructuredBuffer<VolumetricCloudData> VolumetricCloudBuffer : register(t0);
Texture2D<float4> TexTransmittance : register(t1);
Texture2D<float4> TexMultiScatter : register(t2);
Texture3D<float4> TexAerialPerspective : register(t3);

Texture2D<float> TexDepth : register(t4);

Texture3D<unorm float4> TexNubisNoise : register(t5);
Texture2DArray<unorm float> TexCloudNDF : register(t6);
Texture2D<unorm float> TexCloudTopLUT : register(t7);
Texture2D<unorm float> TexCloudBottomLUT : register(t8);
Texture2D<unorm float> TexApShadow : register(t9);
Texture2D<float4> TexSkyView : register(t10);
Texture2D<sh2> TexCloudAmbientSH : register(t11);

Texture2DArray<float4> TexDirectShadows : register(t20);
struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
};
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t21);
#define TERRAIN_SHADOW_REGISTER t22
#include "TerrainShadows/TerrainShadows.hlsli"
Texture3D<float> TexShadowVolume : register(t23);
TextureCube<float3> TexVolCubeTrHistory : register(t24);
TextureCube<float3> TexVolCubeLumHistory : register(t25);
Texture2D<float4> TexVolHistoryTr : register(t26);
Texture2D<float3> TexVolHistoryLum : register(t27);
Texture2D<float4> TexVolHistoryAux : register(t28);
Texture2D<float4> TexVolLowTr : register(t29);
Texture2D<float3> TexVolLowLum : register(t30);
Texture2D<float4> TexVolLowAux : register(t31);
Texture2D<float4> TexVolUpscaleTr : register(t32);
Texture2D<float3> TexVolUpscaleLum : register(t33);
Texture2D<float4> TexVolUpscaleAux : register(t34);

cbuffer VolumetricCloudCubeHistoryCB : register(b1)
{
	float CubeHistoryWeight;
	float3 CubeHistoryPad0;
};

RWTexture2D<float4> RWTexTr : register(u0);
RWTexture2D<float3> RWTexLum : register(u1);
RWTexture2D<float4> RWTexAux : register(u2);

RWTexture3D<float> RWShadowVolume : register(u0);

RWTexture2DArray<float3> RWTexCubeTr : register(u0);
RWTexture2DArray<float3> RWTexCubeLum : register(u1);
RWTexture2D<sh2> RWCloudAmbientSH : register(u0);

#define ISNAN(x) (!(x < 0.f || x > 0.f || x == 0.f))

float CloudAlphaFromTransmittance(float3 transmittance)
{
	const float tr = max(transmittance.x, max(transmittance.y, transmittance.z));
	return 1.0 - saturate((tr - 0.1) * 1.1111111111);
}

float LinearDepthOrSky(float depth)
{
	return depth > 1.0 - 1e-6 ? 16384.0 : SharedData::GetScreenDepth(depth);
}

float RayIntersectSphereCentered(float3 orig, float3 dir, float r)
{
	return RayIntersectSphere(orig, dir, 0, r);
}

float4 ApplyAerialPerspectiveSettings(float4 apSample)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

	if (data.tonemapper == 2)
		apSample.rgb = apSample.rgb / (1 + apSample.rgb);

	apSample.rgb = lerp(0, apSample.rgb, data.apLumMix);
	apSample.a = lerp(1, apSample.a, data.apTrMix);

	return apSample;
}

float3 SampleApMultiScatter()
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

	float3 multi_scatter = TexMultiScatter.SampleLevel(SkyViewSampler, TrLutUv(data.zCameraPlanet, data.sunDir.z), 0).rgb * data.sunlightColor;
	multi_scatter += TexMultiScatter.SampleLevel(SkyViewSampler, TrLutUv(data.zCameraPlanet, data.masserDir.z), 0).rgb * data.masserColor;
	multi_scatter += TexMultiScatter.SampleLevel(SkyViewSampler, TrLutUv(data.zCameraPlanet, data.secundaDir.z), 0).rgb * data.secundaColor;
	return multi_scatter;
}

float3 SampleCloudAmbientSkyView(float3 viewDir)
{
	const float3 shViewDir = float3(viewDir.x, viewDir.z, viewDir.y);
	// Integrate sky radiance against a normalized cloud phase function; this is not diffuse irradiance.
	const sh2 phaseForward = SphericalHarmonics::EvaluatePhaseHG(shViewDir, 0.21);
	const sh2 phaseBackward = SphericalHarmonics::EvaluatePhaseHG(shViewDir, -0.15);
	const sh2 phase = SphericalHarmonics::Add(SphericalHarmonics::Scale(phaseForward, 0.7), SphericalHarmonics::Scale(phaseBackward, 0.3));

	const float r = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(0, 0)], phase);
	const float g = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(1, 0)], phase);
	const float b = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(2, 0)], phase);
	return max(0.0, float3(r, g, b));
}

float SampleFilteredApShadow(uint2 fullPxCoord)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

	uint2 apDims;
	TexApShadow.GetDimensions(apDims.x, apDims.y);

	const uint2 apPxCoord = data.halfResApShadow ? fullPxCoord / 2u : fullPxCoord;
	const float2 apCoord = min(float2(apPxCoord) + 0.5, float2(apDims) - 0.5);

	return TexApShadow.SampleLevel(TransmittanceSampler, apCoord / apDims, 0);
}

groupshared sh2 gCloudAmbientSHR[256];
groupshared sh2 gCloudAmbientSHG[256];
groupshared sh2 gCloudAmbientSHB[256];

[numthreads(16, 16, 1)] void buildCloudAmbientSH(uint3 tid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 sampleCoord = (float2(tid.xy) + 0.5) / 16.0;
	const float3 shSampleDir = SphericalHarmonics::GetUniformSphereSample(sampleCoord.x, sampleCoord.y);
	const float3 rayDir = float3(shSampleDir.x, shSampleDir.z, shSampleDir.y);
	const float3 cloudCenterPlanet = float3(0.0, 0.0, info.planetRadius + info.cloudBottom + 0.5 * info.cloudThickness);
	const float planetVisibility = RayIntersectSphereCentered(cloudCenterPlanet, rayDir, info.planetRadius) > 0.0 ? 0.0 : 1.0;
	const float3 skyColor = TexSkyView.SampleLevel(SkyViewSampler, SkyViewLutUv(rayDir), 0).rgb * planetVisibility;
	const float shFactor = 4.0 * Math::PI / 256.0;
	const sh2 sh = SphericalHarmonics::Evaluate(shSampleDir);

	gCloudAmbientSHR[groupIndex] = SphericalHarmonics::Scale(sh, skyColor.r * shFactor);
	gCloudAmbientSHG[groupIndex] = SphericalHarmonics::Scale(sh, skyColor.g * shFactor);
	gCloudAmbientSHB[groupIndex] = SphericalHarmonics::Scale(sh, skyColor.b * shFactor);

	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stride = 128; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride) {
			gCloudAmbientSHR[groupIndex] = SphericalHarmonics::Add(gCloudAmbientSHR[groupIndex], gCloudAmbientSHR[groupIndex + stride]);
			gCloudAmbientSHG[groupIndex] = SphericalHarmonics::Add(gCloudAmbientSHG[groupIndex], gCloudAmbientSHG[groupIndex + stride]);
			gCloudAmbientSHB[groupIndex] = SphericalHarmonics::Add(gCloudAmbientSHB[groupIndex], gCloudAmbientSHB[groupIndex + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex == 0) {
		RWCloudAmbientSH[int2(0, 0)] = gCloudAmbientSHR[0];
		RWCloudAmbientSH[int2(1, 0)] = gCloudAmbientSHG[0];
		RWCloudAmbientSH[int2(2, 0)] = gCloudAmbientSHB[0];
	}
}

float InBetweenSphereDistance(float3 orig, float3 dir, float rInner, float rOuter)
{
	float innerDist = max(RayIntersectSphereCentered(orig, dir, rInner), 0);
	float outerDist = max(RayIntersectSphereCentered(orig, dir, rOuter), 0);
	return abs(outerDist - innerDist);
}

float2 RayIntersectAABB(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax)
{
	float3 raySign = float3(rayDir.x < 0 ? -1 : 1, rayDir.y < 0 ? -1 : 1, rayDir.z < 0 ? -1 : 1);
	float3 safeRayDir = raySign * max(abs(rayDir), 1e-6);
	float3 tMin = (boxMin - rayOrigin) / safeRayDir;
	float3 tMax = (boxMax - rayOrigin) / safeRayDir;
	float3 t1 = min(tMin, tMax);
	float3 t2 = max(tMin, tMax);
	float tNear = max(max(t1.x, t1.y), t1.z);
	float tFar = min(min(t2.x, t2.y), t2.z);
	return float2(tNear, tFar);
}

float3 GetShadowVolumeSampleUvw(float3 pos, float3 rayDir, VolumetricCloudData info, CloudLayer cloud)
{
	float3 boundsMin = float3(FrameBuffer::CameraPosAdjust.xy - 0.5 * info.shadowVolumeRange, cloud.bottom);
	float3 boundsMax = float3(FrameBuffer::CameraPosAdjust.xy + 0.5 * info.shadowVolumeRange, cloud.bottom + cloud.thickness);

	float3 samplePos = pos;
	if (any(pos < boundsMin) || any(pos > boundsMax)) {
		float2 hitDists = RayIntersectAABB(pos, rayDir, boundsMin, boundsMax);
		if (hitDists.x > hitDists.y)
			return -1;
		samplePos += (hitDists.x + 128) * rayDir;
	}

	float3 uvw = samplePos - float3(FrameBuffer::CameraPosAdjust.xy, cloud.bottom);
	uvw /= float3(info.shadowVolumeRange.xx, cloud.thickness);
	uvw.xy += 0.5;
	return uvw;
}

struct RayMarchInfo
{
	// constant
	float3 ray_dir;
	float3 eye_pos;

	float3 start_pos;
	float3 end_pos;
	float start_dist;
	float end_dist;
	float march_dist;

	// updated
	uint step;

	float3 pos;
	float segment_dist;
	float last_segment_dist;
	float ray_dist;  // actual ray distance
	float last_ray_dist;

	float3 transmittance;
	float3 lum;
};

void initRayMarchInfo(out RayMarchInfo ray)
{
	ray.ray_dir = 0;
	ray.eye_pos = 0;

	ray.start_pos = 0;
	ray.end_pos = 0;
	ray.start_dist = 0;
	ray.end_dist = 0;
	ray.march_dist = 0;

	ray.step = 0;

	ray.pos = 0;
	ray.segment_dist = 0;
	ray.last_segment_dist = 0;
	ray.ray_dist = 0;
	ray.last_ray_dist = 0;

	ray.transmittance = 1;
	ray.lum = 0;
}

// assume inputs are correct
void snapMarch(
	float bottom, float ceil, float3 eye_pos, float3 ray_dir, float max_dist,
	out float3 start_pos, out float3 end_pos, out float march_dist, out float start_dist, out float end_dist)
{
	end_dist = clamp(max_dist, 0, ((ray_dir.z > 0 ? ceil : bottom) - eye_pos.z) / ray_dir.z);
	end_pos = eye_pos + ray_dir * end_dist;
	start_dist = clamp(((ray_dir.z > 0 ? bottom : ceil) - eye_pos.z) / ray_dir.z, 0, end_dist);
	start_pos = eye_pos + ray_dir * start_dist;
	march_dist = end_dist - start_dist;
}

void snapMarch(inout RayMarchInfo ray, float bottom, float ceil, float max_dist)
{
	snapMarch(bottom, ceil, ray.eye_pos, ray.ray_dir, max_dist,
		ray.start_pos, ray.end_pos, ray.march_dist, ray.start_dist, ray.end_dist);
}

void advanceRay(inout RayMarchInfo ray, float dist, float jitter)
{
	ray.step++;
	ray.last_segment_dist = ray.segment_dist;
	ray.segment_dist += dist;
	ray.last_ray_dist = ray.ray_dist;
	ray.ray_dist = lerp(ray.last_segment_dist, ray.segment_dist, jitter);
	ray.pos = ray.start_pos + ray.ray_dist * ray.ray_dir;
}

float2 NubisRayJitter(uint2 pixelCoord, uint frameIndex)
{
	const float jitter_x = frac(float(pixelCoord.x) * 0.1031000018);
	const float jitter_y = frac(float(pixelCoord.y) * 0.1031000018);
	const float jitter_frame = frac(float(frameIndex) * 0.1031000018);
	const float jitter_sum = jitter_x + jitter_y;

	const float near_dot = dot(float3(jitter_x, jitter_y, jitter_frame), float3(jitter_frame + 31.3199997, jitter_y + 31.3199997, jitter_x + 31.3199997));
	const float far_dot = dot(float3(jitter_x, jitter_y, jitter_x), float3(jitter_y + 19.1900005, jitter_x + 19.1900005, jitter_x + 19.1900005));

	return float2(
		frac((jitter_sum + near_dot * 2.0) * (near_dot + jitter_frame)),
		frac((jitter_sum + far_dot * 2.0) * (far_dot + jitter_x)));
}

float SelectNubisRayJitter(float rayDistance, float2 jitter)
{
	return rayDistance < 0.25 / 1.428e-5f ? jitter.x : jitter.y;
}

float NubisVerticalStep(float rayDistance)
{
	return rayDistance * 0.003662109375 + 0.003 / 1.428e-5f;
}

void advanceNubisRay(inout RayMarchInfo ray, float2 jitter)
{
	const float ray_distance = ray.start_dist + ray.segment_dist;
	advanceRay(ray, NubisVerticalStep(ray_distance), SelectNubisRayJitter(ray_distance, jitter));
}

float StabilizeVerticalProfileDensity(float dimensionProfile, float noiseComposite, CloudLayer cloud)
{
	float erosionWidth = max(1.0 - noiseComposite, lerp(cloud.density_erosion_weak, cloud.density_erosion_strong, saturate(dimensionProfile)));
	return saturate((dimensionProfile - noiseComposite) / erosionWidth);
}

struct NDFInfo
{
	bool in_layer;
	float dimension_profile;
	float coverage;
	float height_fraction;
	float cloud_type;
	float bottom_type;
	float top_value;
	float bottom_value;
	float lut_value;
};

void initNDFInfo(out NDFInfo ndf)
{
	ndf.in_layer = false;
	ndf.dimension_profile = ndf.coverage = ndf.height_fraction = ndf.cloud_type = ndf.bottom_type = ndf.top_value = ndf.bottom_value = ndf.lut_value = 0;
}

NDFInfo sampleNDF(
	float3 pos, CloudLayer cloud,
	Texture2DArray<unorm float> tex_ndf, Texture2D<unorm float> tex_top, Texture2D<unorm float> tex_bottom)
{
	NDFInfo ndf;
	initNDFInfo(ndf);

	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float planet_z = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planet_z < cloud.bottom || planet_z > cloud.bottom + cloud.thickness)
		return ndf;

	ndf.in_layer = true;

	const float2 uv = pos.xy * cloud.ndf_freq;

	ndf.coverage = tex_ndf.SampleLevel(TileableSampler, float3(uv, 2), 0);
	if (ndf.coverage < 1e-8)
		return ndf;

	const float min_h = lerp(cloud.bottom, cloud.bottom + cloud.thickness, tex_ndf.SampleLevel(TileableSampler, float3(uv, 0), 0));
	const float max_h = lerp(cloud.bottom, cloud.bottom + cloud.thickness, tex_ndf.SampleLevel(TileableSampler, float3(uv, 1), 0));

	ndf.height_fraction = (planet_z - min_h) / max(max_h - min_h, 1e-5);

	if (ndf.height_fraction < 0 || ndf.height_fraction > 1)
		return ndf;

	ndf.cloud_type = tex_ndf.SampleLevel(TileableSampler, float3(uv, 3), 0);
	ndf.bottom_type = tex_ndf.SampleLevel(TileableSampler, float3(uv, 4), 0);

	ndf.top_value = tex_top.SampleLevel(TransmittanceSampler, float2(ndf.cloud_type, 1 - ndf.height_fraction), 0);
	ndf.bottom_value = tex_bottom.SampleLevel(TransmittanceSampler, float2(ndf.bottom_type, 1 - ndf.height_fraction), 0);
	ndf.lut_value = ndf.top_value * ndf.bottom_value;

	ndf.dimension_profile = ndf.coverage * ndf.lut_value;

	return ndf;
}

float sampleCloudDensity(
	float3 pos, float eye_dist, CloudLayer cloud, float mip_level, bool is_expensive,
	out NDFInfo ndf)
{
	// sample NDF
	ndf = sampleNDF(pos, cloud, TexCloudNDF, TexCloudTopLUT, TexCloudBottomLUT);
	if (ndf.dimension_profile < 1e-8)
		return 0;

	// sample noise
	float noise_mip = mip_level + lerp(cloud.noise_mip_bias_weak, cloud.noise_mip_bias_strong, saturate(ndf.dimension_profile));
	float4 noise = TexNubisNoise.SampleLevel(TileableSampler, (pos + cloud.noise_offset_or_speed) * cloud.noise_scale_or_freq, noise_mip);
	// Define wispy noise
	float wispy_noise = lerp(noise.r, noise.g, ndf.dimension_profile);
	// Define billowy noise
	float billowy_type_gradient = pow(ndf.dimension_profile, 0.25);
	float billowy_noise = lerp(noise.b * 0.3, noise.a * 0.3, billowy_type_gradient);
	// Define Noise composite - blend to wispy as the density scale decreases.
	float noise_composite = lerp(wispy_noise, billowy_noise, ndf.bottom_value);

	// Upres
	float hhf_fraction;
	bool close_range = eye_dist < 0.15 / 1.428e-5f;
	if (close_range) {
		float hhf_noise = saturate(lerp(1.0 - pow(abs(abs(noise.g * 2.0 - 1.0) * 2.0 - 1.0), 4.0), pow(abs(abs(noise.a * 2.0 - 1.0) * 2.0 - 1.0), 2.0), ndf.bottom_value));

		hhf_fraction = (eye_dist - 0.05 / 1.428e-5f) / (0.15 / 1.428e-5f - 0.05 / 1.428e-5f);
		float profile_edge_suppression = saturate((ndf.dimension_profile - cloud.hhf_profile_threshold) / max(1.0 - cloud.hhf_profile_threshold, 1e-5));
		float hhf_noise_distance_range_blender = lerp(1.0, lerp(cloud.hhf_min_blend, 1.0, hhf_fraction), profile_edge_suppression);
		noise_composite = lerp(hhf_noise, noise_composite, hhf_noise_distance_range_blender);
	}

	float density = StabilizeVerticalProfileDensity(ndf.dimension_profile, noise_composite, cloud);

	// Sharpen result
	density = pow(density, cloud.power);
	if (close_range) {
		density = pow(density, lerp(0.5, 1.0, hhf_fraction)) * lerp(0.666, 1.0, hhf_fraction);
	}

	return saturate(density);
}

// sample sun transmittance / shadowing
float3 sampleSunTransmittance(float3 pos, float3 sun_dir, uint3 seed, out float3 cloud_transmittance)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	cloud_transmittance = 1.0;

	float3 shadow = 1.0;

	float3 pos_world = pos + float3(0, 0, info.bottomZ);
	float3 pos_world_relative = pos_world - FrameBuffer::CameraPosAdjust.xyz;
	float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);

	// earth shadowing
	[branch] if (RayIntersectSphereCentered(pos_planet, sun_dir, info.planetRadius) > 0.0) return 0;

	// dir shadow map
	{
		DirectionalShadowLightData directionalShadowLightData = DirectionalShadowLights[0];
		float shadow_depth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(pos_world_relative));
		[branch] if (directionalShadowLightData.EndSplitDistances.y > 0.0 &&
					 shadow_depth < directionalShadowLightData.EndSplitDistances.y)
		{
			float cascade_select = saturate(
				(shadow_depth - directionalShadowLightData.StartSplitDistances.y) /
				(directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));
			uint cascade_index = uint(cascade_select);
			float3 positionLS = mul(directionalShadowLightData.ShadowProj[cascade_index], float4(pos_world, 1)).xyz;
			float4 depths = TexDirectShadows.GatherRed(TransmittanceSampler, float3(saturate(positionLS.xy), cascade_index), 0);
			shadow *= dot(float4(depths > positionLS.z), 0.25);
		}
	}
	[branch] if (all(shadow < 1e-8)) return 0;

	// terrain shadow
	shadow *= TerrainShadows::GetTerrainShadow(pos_world, TransmittanceSampler);
	[branch] if (all(shadow < 1e-8)) return 0;

	// atmosphere
	{
		float2 lut_uv = TrLutUv(pos_planet.z, sun_dir.z);
		shadow *= TexTransmittance.SampleLevel(TransmittanceSampler, lut_uv, 0).rgb;
	}
	[branch] if (all(shadow < 1e-8)) return 0;

	// cloud self-shadowing
	{
		const static uint visibility_step = 6;
		const static float visibility_stride = 0.05 / 1.428e-5f;
		const float3 jitter = Random::R3Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f) * 2 - 1;

		float cloud_density = 0;

		for (uint i = 0; i < visibility_step; i++) {
			float3 vis_pos = pos + sun_dir * visibility_stride * (i + 1) + jitter * visibility_stride * (i + 1) / visibility_step;
			NDFInfo _;
			cloud_density += sampleCloudDensity(vis_pos, 1e8, cloud, i * 0.5, true, _) * visibility_stride;
		}

		// long range
		float3 vis_pos = pos + sun_dir * visibility_stride * visibility_step;
		float3 pos_sample_shadow_uvw = GetShadowVolumeSampleUvw(vis_pos, sun_dir, info, cloud);
		if (all(pos_sample_shadow_uvw.xyz > 0))
			cloud_density += TexShadowVolume.SampleLevel(TransmittanceSampler, pos_sample_shadow_uvw.xyz, 0);
		else
			cloud_density += InBetweenSphereDistance(
								 vis_pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius), sun_dir,
								 info.planetRadius + cloud.bottom, info.planetRadius + cloud.bottom + cloud.thickness) *
			                 cloud.average_density;

		float3 scaled_density = (cloud.scatter + cloud.absorption) * cloud_density;

		cloud_transmittance = exp(-scaled_density);
		shadow *= cloud_transmittance;
	}

	return shadow;
}

struct VolumetricCloudResult
{
	float3 transmittance;
	float3 lum;
	float cloud_depth;
	float reject_depth;
	float scatter_weight;
	float weighted_depth;
};

VolumetricCloudResult RenderVolumetricCloudRay(float3 ray_dir, float3 eye_pos, float solid_dist, bool is_sky, uint3 seed, float2 jitter, float ap_shadow, bool skip_below_cloud_bottom)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	const float ceil = cloud.bottom + cloud.thickness;
	const float bottom = skip_below_cloud_bottom ? cloud.bottom : 0.0;

	RayMarchInfo ray;
	initRayMarchInfo(ray);

	ray.eye_pos = eye_pos;
	ray.ray_dir = ray_dir;
	const float max_march_dist = is_sky ? info.rayMarchRange : min(info.rayMarchRange, solid_dist);
	snapMarch(ray, bottom, ceil, max_march_dist);

	float skipped_ap_dist = 0.0;
	if (skip_below_cloud_bottom) {
		float3 legacy_start_pos;
		float3 legacy_end_pos;
		float legacy_march_dist;
		float legacy_start_dist;
		float legacy_end_dist;
		snapMarch(0.0, ceil, ray.eye_pos, ray.ray_dir, max_march_dist,
			legacy_start_pos, legacy_end_pos, legacy_march_dist, legacy_start_dist, legacy_end_dist);
		skipped_ap_dist = max(0.0, ray.start_dist - legacy_start_dist);
	}

	///////////// precalc
	const float cos_theta = dot(ray.ray_dir, info.dirlightDir);
	const float cloud_phase = lerp(Phase::ThomasSchander(cos_theta), Phase::HG(cos_theta, -0.3), 0.3);
	const float cloud_secondary_phase = Phase::HGDualLobe(cos_theta, 0.21, -0.15, 0.3);

	///////////// ray march
	float ap_dist = skipped_ap_dist;
	float3 mean_shadowing = 0.0;
	float sum_shadowing_weights = 0.0;
	float scatter_weight = 0.0;
	float weighted_depth = 0.0;

	advanceNubisRay(ray, jitter);
	[loop] for (ray.step = 0; ray.step < info.cloudMaxStep && ray.ray_dist < ray.march_dist; advanceNubisRay(ray, jitter))
	{
		const float dt = ray.ray_dist - ray.last_ray_dist;

		NDFInfo ndf;
		float cloud_density = sampleCloudDensity(ray.pos, ray.start_dist + ray.ray_dist, cloud, (ray.start_dist + ray.ray_dist) * 1.428e-5f, true, ndf);
		float3 cloud_scatter = cloud_density * cloud.scatter;

		const float3 extinction = cloud_density * (cloud.scatter + cloud.absorption);

		// scattering
		[branch] if (max(extinction.x, max(extinction.y, extinction.z)) > 1e-7)
		{
			// dir light
			float3 scatter = cloud_scatter * cloud_phase;
			float3 cloud_transmittance;
			float3 sun_transmittance = sampleSunTransmittance(ray.pos, info.dirlightDir, seed + ray.step, cloud_transmittance);
			float3 in_scatter = scatter * sun_transmittance * info.dirlightColor;

			sum_shadowing_weights += dt;
			mean_shadowing += sun_transmittance * dt;

			// multiscatter
			float3 ms_volume = saturate((ndf.dimension_profile - 0.1) / (1.0 - 0.1)) * pow(ndf.coverage * ndf.cloud_type, 0.25);
			ms_volume *= pow(cloud_transmittance, cloud.ms_transmittance_power);
			ms_volume *= pow(saturate(ndf.height_fraction), cloud.ms_height_power);
			ms_volume *= cloud.ms_mult;
			in_scatter += (sun_transmittance / max(1e-8, cloud_transmittance)) * cloud_scatter * cloud_secondary_phase * ms_volume * info.dirlightColor;

			// ambient
			float3 ambient = SampleCloudAmbientSkyView(ray.ray_dir);
			float profile_indirect = sqrt(1.0 - saturate(ndf.dimension_profile));
			float vertical_transmittance = dot(TexTransmittance.SampleLevel(TransmittanceSampler, TrLutUvPlanet(ray.pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius), info.dirlightDir), 0).rgb, float3(0.2126, 0.7152, 0.0722));
			float vertical_indirect = exp(vertical_transmittance);
			in_scatter += cloud_scatter * profile_indirect * vertical_indirect * cloud.ambient_mult * ambient;

			const float3 sample_transmittance = exp(-dt * extinction);
			const float3 scatter_factor = (1 - sample_transmittance) / max(extinction, 1e-8);
			const float3 scatter_integeral = in_scatter * scatter_factor;

			// update
			ray.lum += scatter_integeral * ray.transmittance;
			ray.transmittance *= sample_transmittance;
		}

		const float tr = max(ray.transmittance.x, max(ray.transmittance.y, ray.transmittance.z));
		const float step_opacity = saturate(1.0 - tr);
		scatter_weight += step_opacity * dt;
		weighted_depth += step_opacity * dt * (ray.start_dist + ray.ray_dist);
		ap_dist += tr * dt;
		[branch] if (tr < 1e-3) break;
	}

	mean_shadowing = sum_shadowing_weights > 1e-8 ? mean_shadowing / sum_shadowing_weights : 1.0;

	uint3 ap_dims;
	TexAerialPerspective.GetDimensions(ap_dims.x, ap_dims.y, ap_dims.z);
	float2 ap_uv = SkyViewLutUv(ray.ray_dir);
	const float depth_slice = lerp(.5 / ap_dims.z, 1 - .5 / ap_dims.z, saturate(solid_dist / info.aerialPerspectiveMaxDist));
	float4 ap_sample = TexAerialPerspective.SampleLevel(SkyViewSampler, float3(ap_uv, depth_slice), 0);
	const float vol_depth_slice = lerp(.5 / ap_dims.z, 1 - .5 / ap_dims.z, saturate(ap_dist / info.aerialPerspectiveMaxDist));
	float4 vol_ap_sample = TexAerialPerspective.SampleLevel(SkyViewSampler, float3(ap_uv, vol_depth_slice), 0);

	const float ap_direct_visibility = 1.0 - saturate(ap_shadow);
	const float vol_ap_direct_visibility = saturate(dot(mean_shadowing, float3(0.2126, 0.7152, 0.0722))) * ap_direct_visibility;
	const float3 ap_multi_scatter = SampleApMultiScatter();
	ap_sample.rgb *= GetApShadowedMultiScatterVisibility(1.0 - ap_direct_visibility, ap_multi_scatter);
	vol_ap_sample.rgb *= GetApShadowedMultiScatterVisibility(1.0 - vol_ap_direct_visibility, ap_multi_scatter);

	ap_sample = ApplyAerialPerspectiveSettings(ap_sample);
	vol_ap_sample = ApplyAerialPerspectiveSettings(vol_ap_sample);

	if (!is_sky) {
		ray.lum += (ap_sample.rgb - vol_ap_sample.rgb) * ray.transmittance;
		ray.transmittance *= ap_sample.a;
	} else {
		ray.transmittance *= vol_ap_sample.a;
	}
	ray.lum = ray.lum * vol_ap_sample.a + vol_ap_sample.rgb;

	VolumetricCloudResult result;
	result.transmittance = ray.transmittance;
	result.lum = ray.lum;
	result.cloud_depth = scatter_weight > 1e-6 ? weighted_depth / scatter_weight : (is_sky ? 16384.0 : solid_dist);
	result.reject_depth = is_sky ? 16384.0 : solid_dist;
	result.scatter_weight = scatter_weight;
	result.weighted_depth = weighted_depth;
	return result;
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];

	if (any(tid >= uint2(info.lowFrameDim)))
		return;

	const uint2 px_coords = tid;
	const bool full_resolution = info.fullResolution != 0u;
	const uint frame_subpixel = full_resolution ? 0u : (SharedData::FrameCountAlwaysActive & 15u);
	const uint2 phase_offset = full_resolution ? 0u.xx : uint2(frame_subpixel & 3u, frame_subpixel >> 2u);
	const uint2 full_px_coords = full_resolution ? px_coords : min(px_coords * 4u + phase_offset, uint2(info.frameDim) - 1u);

	const uint3 seed = Random::pcg3d(uint3(px_coords.xy, px_coords.x ^ 0xf874));
	const float2 ray_jitter = NubisRayJitter(full_px_coords, SharedData::FrameCountAlwaysActive);

	///////////// get start and end
	const float depth = TexDepth[full_px_coords.xy];
	const bool is_sky = depth > 1 - 1e-6;

	const float2 texture_uv = (full_px_coords + 0.5) * info.rcpFrameDim;
	const float2 logic_uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(texture_uv);

	float4 pos_world = float4(2 * float2(logic_uv.x, -logic_uv.y + 1) - 1, depth, 1);
	pos_world = mul(FrameBuffer::CameraViewProjInverse, pos_world);
	pos_world.xyz = pos_world.xyz / pos_world.w;

	const float solid_dist = length(pos_world.xyz);
	const float3 eye_pos = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float3 ray_dir = pos_world.xyz / solid_dist;

	const float ap_shadow = SampleFilteredApShadow(full_px_coords);
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, solid_dist, is_sky, seed, ray_jitter, ap_shadow, true);

	RWTexTr[px_coords] = float4(result.transmittance, CloudAlphaFromTransmittance(result.transmittance));
	RWTexLum[px_coords] = result.lum;
	RWTexAux[px_coords] = float4(result.cloud_depth, result.reject_depth, result.scatter_weight, 1.0);
};

float3 GetCubemapSamplingVector(uint3 threadId, in RWTexture2DArray<float3> outputTexture)
{
	float width = 0.0f;
	float height = 0.0f;
	float depth = 0.0f;
	outputTexture.GetDimensions(width, height, depth);

	float2 st = threadId.xy / float2(width, height);
	float2 uv = 2.0 * float2(st.x, 1.0 - st.y) - 1.0;

	float3 result = 0.0f;
	switch (threadId.z) {
	case 0:
		result = float3(1.0, uv.y, -uv.x);
		break;
	case 1:
		result = float3(-1.0, uv.y, uv.x);
		break;
	case 2:
		result = float3(uv.x, 1.0, -uv.y);
		break;
	case 3:
		result = float3(uv.x, -1.0, uv.y);
		break;
	case 4:
		result = float3(uv.x, uv.y, 1.0);
		break;
	case 5:
		result = float3(-uv.x, uv.y, -1.0);
		break;
	}
	return normalize(result);
}

[numthreads(8, 8, 1)] void renderCubemap(uint3 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];

	uint3 dims;
	RWTexCubeTr.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims))
		return;

	const uint3 seed = Random::pcg3d(uint3(tid.xy, tid.z * 0x9e37u + tid.x ^ 0xf874u));
	const float2 ray_jitter = NubisRayJitter(tid.xy + tid.z * uint2(131u, 719u), SharedData::FrameCountAlwaysActive);

	const float3 eye_pos = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float3 ray_dir = GetCubemapSamplingVector(tid, RWTexCubeTr);
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, info.rayMarchRange, true, seed, ray_jitter, 0.0, false);

	RWTexCubeTr[tid] = result.transmittance;
	RWTexCubeLum[tid] = result.lum;
};

[numthreads(8, 8, 1)] void accumulateCubemap(uint3 tid : SV_DispatchThreadID) {
	uint3 dims;
	RWTexCubeTr.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims))
		return;

	const float3 ray_dir = GetCubemapSamplingVector(tid, RWTexCubeTr);
	const float3 tr = RWTexCubeTr[tid];
	const float3 lum = RWTexCubeLum[tid];
	const float3 history_tr = TexVolCubeTrHistory.SampleLevel(SkyViewSampler, ray_dir, 0);
	const float3 history_lum = TexVolCubeLumHistory.SampleLevel(SkyViewSampler, ray_dir, 0);

	RWTexCubeTr[tid] = lerp(tr, history_tr, CubeHistoryWeight);
	RWTexCubeLum[tid] = lerp(lum, history_lum, CubeHistoryWeight);
};

float2 GetPreviousCloudUv(float2 logic_uv, float depth, out bool valid)
{
	valid = false;
	const float reprojection_depth = min(depth, 16384.0);

	float4 pos_world = float4(2 * float2(logic_uv.x, -logic_uv.y + 1) - 1, 1.0, 1);
	pos_world = mul(FrameBuffer::CameraViewProjInverse, pos_world);
	pos_world.xyz = normalize(pos_world.xyz / pos_world.w) * reprojection_depth;
	pos_world.w = 1.0;
	pos_world.xyz += FrameBuffer::CameraPosAdjust.xyz - FrameBuffer::CameraPreviousPosAdjust.xyz;

	float4 prev_clip = mul(FrameBuffer::CameraPreviousViewProjUnjittered, pos_world);
	if (prev_clip.w <= 0.0)
		return logic_uv;

	float2 prev_uv = prev_clip.xy / prev_clip.w * float2(0.5, -0.5) + 0.5;
	valid = all(prev_uv >= 0.0) && all(prev_uv <= 1.0);
	return prev_uv;
}

float SafeCloudDepth(float4 aux, float minDepth)
{
	return aux.x < 19500.0 ? aux.x : minDepth;
}

float3 ResampleComparableColor(float3 lum)
{
	return lum / (1.0 + max(max(lum.x, lum.y), lum.z));
}

float4 BilinearWeights(float2 frac)
{
	return float4((1.0 - frac.x) * (1.0 - frac.y), frac.x * (1.0 - frac.y), (1.0 - frac.x) * frac.y, frac.x * frac.y);
}

float4 NormalizeCloudWeights(float4 weights)
{
	const float weight_sum = dot(weights, float4(1.0, 1.0, 1.0, 1.0));
	return weight_sum > 1e-5 ? weights / weight_sum : 0.0;
}

float CloudDepthWeight(float sampleRejectDepth, float referenceDepth)
{
	const float tolerance = max(64.0, min(referenceDepth, 16384.0) * 0.02);
	return saturate(1.0 - abs(sampleRejectDepth - referenceDepth) / tolerance);
}

float4 CloudDepthWeights(float4 sampleRejectDepths, float referenceDepth)
{
	const float tolerance = max(64.0, min(referenceDepth, 16384.0) * 0.02);
	return saturate(1.0 - abs(sampleRejectDepths - referenceDepth) / tolerance);
}

float4 DepthWeightedCloudWeights(float4 rawWeights, float4 sampleRejectDepths, float4 sampleValidities, float referenceDepth)
{
	return NormalizeCloudWeights(rawWeights * CloudDepthWeights(sampleRejectDepths, referenceDepth) * sampleValidities);
}

bool SampleCloudBilinear(float2 texelPos, uint2 dims, float referenceDepth, out float3 tr, out float3 lum, out float4 aux)
{
	const int2 basePx = int2(floor(texelPos));
	const float2 frac = saturate(texelPos - floor(texelPos));
	const uint2 px00 = min(uint2(max(basePx, 0)), dims - 1);
	const uint2 px10 = min(uint2(max(basePx + int2(1, 0), 0)), dims - 1);
	const uint2 px01 = min(uint2(max(basePx + int2(0, 1), 0)), dims - 1);
	const uint2 px11 = min(uint2(max(basePx + int2(1, 1), 0)), dims - 1);

	const float4 aux00 = TexVolLowAux[px00];
	const float4 aux10 = TexVolLowAux[px10];
	const float4 aux01 = TexVolLowAux[px01];
	const float4 aux11 = TexVolLowAux[px11];
	const float min_depth = min(min(aux00.x, aux10.x), min(aux01.x, aux11.x));

	const float4 weights = DepthWeightedCloudWeights(
		BilinearWeights(frac),
		float4(aux00.y, aux10.y, aux01.y, aux11.y),
		float4(aux00.w, aux10.w, aux01.w, aux11.w),
		referenceDepth);
	if (dot(weights, float4(1.0, 1.0, 1.0, 1.0)) <= 0.0) {
		tr = 1.0;
		lum = 0.0;
		aux = float4(referenceDepth, referenceDepth, 0.0, 0.0);
		return false;
	}

	tr = TexVolLowTr[px00].rgb * weights.x + TexVolLowTr[px10].rgb * weights.y + TexVolLowTr[px01].rgb * weights.z + TexVolLowTr[px11].rgb * weights.w;
	lum = TexVolLowLum[px00] * weights.x + TexVolLowLum[px10] * weights.y + TexVolLowLum[px01] * weights.z + TexVolLowLum[px11] * weights.w;
	aux = float4(
		SafeCloudDepth(aux00, min_depth) * weights.x + SafeCloudDepth(aux10, min_depth) * weights.y + SafeCloudDepth(aux01, min_depth) * weights.z + SafeCloudDepth(aux11, min_depth) * weights.w,
		aux00.y * weights.x + aux10.y * weights.y + aux01.y * weights.z + aux11.y * weights.w,
		aux00.z * weights.x + aux10.z * weights.y + aux01.z * weights.z + aux11.z * weights.w,
		aux00.w * weights.x + aux10.w * weights.y + aux01.w * weights.z + aux11.w * weights.w);
	return true;
}

bool SampleHistoryBilinear(float2 uv, float referenceDepth, out float3 tr, out float3 lum, out float4 aux)
{
	uint2 dims;
	TexVolHistoryTr.GetDimensions(dims.x, dims.y);
	const float2 texelPos = uv * dims - 0.5;
	const int2 basePx = int2(floor(texelPos));
	const float2 frac = saturate(texelPos - floor(texelPos));
	const uint2 px00 = min(uint2(max(basePx, 0)), dims - 1);
	const uint2 px10 = min(uint2(max(basePx + int2(1, 0), 0)), dims - 1);
	const uint2 px01 = min(uint2(max(basePx + int2(0, 1), 0)), dims - 1);
	const uint2 px11 = min(uint2(max(basePx + int2(1, 1), 0)), dims - 1);

	const float4 aux00 = TexVolHistoryAux[px00];
	const float4 aux10 = TexVolHistoryAux[px10];
	const float4 aux01 = TexVolHistoryAux[px01];
	const float4 aux11 = TexVolHistoryAux[px11];
	const float min_depth = min(min(aux00.x, aux10.x), min(aux01.x, aux11.x));

	const float4 weights = DepthWeightedCloudWeights(
		BilinearWeights(frac),
		float4(aux00.y, aux10.y, aux01.y, aux11.y),
		float4(aux00.w, aux10.w, aux01.w, aux11.w),
		referenceDepth);
	if (dot(weights, float4(1.0, 1.0, 1.0, 1.0)) <= 0.0) {
		tr = 1.0;
		lum = 0.0;
		aux = float4(referenceDepth, referenceDepth, 0.0, 0.0);
		return false;
	}

	tr = TexVolHistoryTr[px00].rgb * weights.x + TexVolHistoryTr[px10].rgb * weights.y + TexVolHistoryTr[px01].rgb * weights.z + TexVolHistoryTr[px11].rgb * weights.w;
	lum = TexVolHistoryLum[px00] * weights.x + TexVolHistoryLum[px10] * weights.y + TexVolHistoryLum[px01] * weights.z + TexVolHistoryLum[px11] * weights.w;
	aux = float4(
		SafeCloudDepth(aux00, min_depth) * weights.x + SafeCloudDepth(aux10, min_depth) * weights.y + SafeCloudDepth(aux01, min_depth) * weights.z + SafeCloudDepth(aux11, min_depth) * weights.w,
		aux00.y * weights.x + aux10.y * weights.y + aux01.y * weights.z + aux11.y * weights.w,
		aux00.z * weights.x + aux10.z * weights.y + aux01.z * weights.z + aux11.z * weights.w,
		aux00.w * weights.x + aux10.w * weights.y + aux01.w * weights.z + aux11.w * weights.w);
	return true;
}

bool SampleCloudFallback(float2 texelPos, uint2 dims, float referenceDepth, out float3 tr, out float3 lum, out float4 aux)
{
	const int2 basePx = int2(floor(texelPos));
	const float2 frac = saturate(texelPos - floor(texelPos));
	const uint2 px00 = min(uint2(max(basePx, 0)), dims - 1);
	const uint2 px10 = min(uint2(max(basePx + int2(1, 0), 0)), dims - 1);
	const uint2 px01 = min(uint2(max(basePx + int2(0, 1), 0)), dims - 1);
	const uint2 px11 = min(uint2(max(basePx + int2(1, 1), 0)), dims - 1);

	const float4 aux00 = TexVolLowAux[px00];
	const float4 aux10 = TexVolLowAux[px10];
	const float4 aux01 = TexVolLowAux[px01];
	const float4 aux11 = TexVolLowAux[px11];
	const float min_depth = min(min(aux00.x, aux10.x), min(aux01.x, aux11.x));
	const float4 rawWeights = BilinearWeights(frac);
	const float4 tr00 = TexVolLowTr[px00];
	const float4 tr10 = TexVolLowTr[px10];
	const float4 tr01 = TexVolLowTr[px01];
	const float4 tr11 = TexVolLowTr[px11];
	float4 weights = float4(
		tr00.w <= 1e-5 ? 0.01 : rawWeights.x,
		tr10.w <= 1e-5 ? 0.01 : rawWeights.y,
		tr01.w <= 1e-5 ? 0.01 : rawWeights.z,
		tr11.w <= 1e-5 ? 0.01 : rawWeights.w);
	weights = DepthWeightedCloudWeights(
		weights,
		float4(aux00.y, aux10.y, aux01.y, aux11.y),
		float4(aux00.w, aux10.w, aux01.w, aux11.w),
		referenceDepth);
	if (dot(weights, float4(1.0, 1.0, 1.0, 1.0)) <= 0.0) {
		tr = 1.0;
		lum = 0.0;
		aux = float4(referenceDepth, referenceDepth, 0.0, 0.0);
		return false;
	}

	tr = tr00.rgb * weights.x + tr10.rgb * weights.y + tr01.rgb * weights.z + tr11.rgb * weights.w;
	lum = TexVolLowLum[px00] * weights.x + TexVolLowLum[px10] * weights.y + TexVolLowLum[px01] * weights.z + TexVolLowLum[px11] * weights.w;
	aux = float4(
		SafeCloudDepth(aux00, min_depth) * weights.x + SafeCloudDepth(aux10, min_depth) * weights.y + SafeCloudDepth(aux01, min_depth) * weights.z + SafeCloudDepth(aux11, min_depth) * weights.w,
		aux00.y * weights.x + aux10.y * weights.y + aux01.y * weights.z + aux11.y * weights.w,
		aux00.z * weights.x + aux10.z * weights.y + aux01.z * weights.z + aux11.z * weights.w,
		aux00.w * weights.x + aux10.w * weights.y + aux01.w * weights.z + aux11.w * weights.w);
	return true;
}

[numthreads(8, 8, 1)] void resample(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(tid >= uint2(info.frameDim)))
		return;

	uint2 low_dims;
	TexVolLowTr.GetDimensions(low_dims.x, low_dims.y);
	const float2 texture_uv = (tid + 0.5) * info.rcpFrameDim;
	const float2 logic_uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(texture_uv);
	const float current_reject_depth = LinearDepthOrSky(TexDepth[tid]);
	const uint frame_subpixel = SharedData::FrameCountAlwaysActive & 15u;
	const uint pixel_subpixel = ((tid.y & 3u) << 2u) + (tid.x & 3u);
	const float2 pattern_offset = (float2(frame_subpixel & 3u, frame_subpixel >> 2u) * 0.25 - 0.375) * info.rcpLowFrameDim;
	const bool history_available = info.historyValid != 0;
	float3 current_tr;
	float3 current_lum;
	float4 current_aux;
	const bool current_valid = SampleCloudBilinear(texture_uv * low_dims - 0.5, low_dims, current_reject_depth, current_tr, current_lum, current_aux);

	bool projection_valid;
	float2 projected_uv = GetPreviousCloudUv(logic_uv, current_aux.x, projection_valid);
	projection_valid = projection_valid && history_available && current_valid;
	const float2 history_uv = FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(projected_uv);

	float3 tr;
	float3 lum;
	float4 aux;
	if (projection_valid) {
		if (!SampleHistoryBilinear(history_uv, current_reject_depth, tr, lum, aux))
			SampleCloudFallback((texture_uv - pattern_offset) * low_dims - 0.5, low_dims, current_reject_depth, tr, lum, aux);
	} else {
		SampleCloudFallback((texture_uv - pattern_offset) * low_dims - 0.5, low_dims, current_reject_depth, tr, lum, aux);
	}

	if (pixel_subpixel == frame_subpixel) {
		const uint2 low_px = min(uint2(texture_uv * low_dims), low_dims - 1);
		const float4 low_tr_sample = TexVolLowTr[low_px];
		const float3 low_tr = low_tr_sample.rgb;
		const float3 low_lum = TexVolLowLum[low_px];
		const float4 low_aux = TexVolLowAux[low_px];

		float color_confidence = projection_valid ? 1.0 - saturate(pow(length(ResampleComparableColor(low_lum) - ResampleComparableColor(lum)), 0.5)) * 0.8 : 1.0;
		float reprojection_confidence = projection_valid ? (saturate((length(projected_uv - logic_uv) - 0.0001) * 2500.0) * 0.5 + 0.5) : 1.0;
		float direct_weight = current_valid ? color_confidence * reprojection_confidence * CloudDepthWeight(low_aux.y, current_reject_depth) * low_aux.w : 0.0;

		tr = lerp(tr, low_tr, direct_weight);
		lum = lerp(lum, low_lum, direct_weight);
		aux = lerp(aux, low_aux, direct_weight);
	} else if (projection_valid) {
		if (abs(current_reject_depth - aux.y) > 64.0) {
			SampleCloudFallback((texture_uv - pattern_offset) * low_dims - 0.5, low_dims, current_reject_depth, tr, lum, aux);
		}
	}

	RWTexTr[tid] = float4(tr, CloudAlphaFromTransmittance(tr));
	RWTexLum[tid] = lum;
	RWTexAux[tid] = aux;
};

[numthreads(8, 8, 1)] void blur(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(tid >= uint2(info.frameDim)))
		return;

	const float2 texture_uv = (tid + 0.5) * info.rcpFrameDim;
	const float2 rcp_texture_dim = info.rcpFrameDim;
	const float4 aux_center = TexVolUpscaleAux[tid];
	const float reference_depth = LinearDepthOrSky(TexDepth[tid]);
	const float falloff = pow(1.0 - saturate(abs(aux_center.x - 1000.0) * 0.00016666667), 4.0) * 0.67 + 0.33;
	const float blur_footprint = 1.0;  // kVolCloudDownsample
	const float2 radius = falloff * blur_footprint * rcp_texture_dim;
	const float2 texture_min_uv = 0.5f * rcp_texture_dim;
	const float2 texture_max_uv = FrameBuffer::DynamicResolutionParams1.xy - texture_min_uv;
	const float2 uv0 = max(texture_uv - radius, texture_min_uv);
	const float2 uv1 = min(texture_uv + radius, texture_max_uv);

	const float2 uv00 = float2(uv0.x, uv1.y);
	const float2 uv10 = uv1;
	const float2 uv01 = uv0;
	const float2 uv11 = float2(uv1.x, uv0.y);
	const float4 tr00 = TexVolUpscaleTr.SampleLevel(SkyViewSampler, uv00, 0);
	const float4 tr10 = TexVolUpscaleTr.SampleLevel(SkyViewSampler, uv10, 0);
	const float4 tr01 = TexVolUpscaleTr.SampleLevel(SkyViewSampler, uv01, 0);
	const float4 tr11 = TexVolUpscaleTr.SampleLevel(SkyViewSampler, uv11, 0);
	const float3 lum00 = TexVolUpscaleLum.SampleLevel(SkyViewSampler, uv00, 0);
	const float3 lum10 = TexVolUpscaleLum.SampleLevel(SkyViewSampler, uv10, 0);
	const float3 lum01 = TexVolUpscaleLum.SampleLevel(SkyViewSampler, uv01, 0);
	const float3 lum11 = TexVolUpscaleLum.SampleLevel(SkyViewSampler, uv11, 0);
	const float4 aux00 = TexVolUpscaleAux.SampleLevel(SkyViewSampler, uv00, 0);
	const float4 aux10 = TexVolUpscaleAux.SampleLevel(SkyViewSampler, uv10, 0);
	const float4 aux01 = TexVolUpscaleAux.SampleLevel(SkyViewSampler, uv01, 0);
	const float4 aux11 = TexVolUpscaleAux.SampleLevel(SkyViewSampler, uv11, 0);
	const float4 weights = DepthWeightedCloudWeights(
		float4(0.25, 0.25, 0.25, 0.25),
		float4(aux00.y, aux10.y, aux01.y, aux11.y),
		float4(aux00.w, aux10.w, aux01.w, aux11.w),
		reference_depth);
	const bool has_weights = dot(weights, float4(1.0, 1.0, 1.0, 1.0)) > 0.0;

	float4 tr = has_weights ? (tr00 * weights.x + tr10 * weights.y + tr01 * weights.z + tr11 * weights.w) : TexVolUpscaleTr[tid];
	float3 lum = has_weights ? (lum00 * weights.x + lum10 * weights.y + lum01 * weights.z + lum11 * weights.w) : TexVolUpscaleLum[tid];

	float aux_z = aux_center.z;
	if (aux_center.z > 0.0 && has_weights)
		aux_z = aux00.z * weights.x + aux10.z * weights.y + aux01.z * weights.z + aux11.z * weights.w;

	RWTexTr[tid] = tr;
	RWTexLum[tid] = lum;
	RWTexAux[tid] = float4(aux_center.x, aux_center.y, aux_z, aux_center.w);
};

#define NTHREADS 256
groupshared float g_density[NTHREADS];

[numthreads(NTHREADS, 1, 1)] void renderShadowVolume(const uint gtid : SV_GroupThreadID, const uint2 gid : SV_GroupID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	uint3 dims;
	RWShadowVolume.GetDimensions(dims.x, dims.y, dims.z);
	const float3 rcp_dims = 1.0 / float3(dims);
	const float3 scale = float3(info.shadowVolumeRange.xx, cloud.thickness);
	const float3 rcp_scale = 1.0 / scale;

	const float3 ray_dir = -info.dirlightDir;  // from sun

	float3 ray_px_increment = ray_dir * rcp_scale * dims;
	const float dir_max_component = max(max(abs(ray_px_increment.x), abs(ray_px_increment.y)), abs(ray_px_increment.z));

	uint3 start_px;
	bool3 component_mask = false;
	if (abs(ray_px_increment.x) == dir_max_component) {
		start_px = uint3(ray_px_increment.x > 0 ? 0 : dims.x - 1, gid);
		component_mask.x = true;
	} else if (abs(ray_px_increment.y) == dir_max_component) {
		start_px = uint3(gid.x, ray_px_increment.y > 0 ? 0 : dims.y - 1, gid.y);
		component_mask.y = true;
	} else {
		start_px = uint3(gid, ray_px_increment.z > 0 ? 0 : dims.z - 1);
		component_mask.z = true;
	}
	ray_px_increment /= dir_max_component;
	const float3 ray_uv_increment = ray_px_increment * rcp_dims;
	const float3 start_uv = (start_px + 0.5) * rcp_dims;
	const float3 raw_thread_uv = start_uv + gtid * ray_uv_increment;

	const bool is_valid_x = component_mask.x && raw_thread_uv.x > 0 && raw_thread_uv.x < 1;
	const bool is_valid_y = component_mask.y && raw_thread_uv.y > 0 && raw_thread_uv.y < 1;
	const bool is_valid_z = component_mask.z && raw_thread_uv.z > 0 && raw_thread_uv.z < 1;
	const bool is_valid = is_valid_x || is_valid_y || is_valid_z;

	const float3 thread_uv = raw_thread_uv - floor(raw_thread_uv);  // wraparound
	const uint3 thread_px_coord = thread_uv * dims;

	float past_density = RWShadowVolume[thread_px_coord];
	if (ISNAN(past_density))
		past_density = 0;

	if (is_valid) {
		const float3 pos = float3(FrameBuffer::CameraPosAdjust.xy + (thread_uv.xy - 0.5) * info.shadowVolumeRange, cloud.bottom + cloud.thickness * thread_uv.z);

		// fetch density using only ndf
		NDFInfo _;
		float density = sampleCloudDensity(pos, 1e8, cloud, 2, false, _) * length(ray_uv_increment * scale);  // scaled by ray length

		// average visibility for boundary
		float3 prev_uv = thread_uv - ray_uv_increment;
		float3 prev_pos = float3(FrameBuffer::CameraPosAdjust.xy + (prev_uv.xy - 0.5) * info.shadowVolumeRange, cloud.bottom + cloud.thickness * prev_uv.z);
		if ((any(prev_uv < 0) || any(prev_uv > 1)) && prev_pos.z > cloud.bottom && prev_pos.z < cloud.bottom + cloud.thickness)
			density += InBetweenSphereDistance(
						   prev_pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius), info.dirlightDir,
						   info.planetRadius + cloud.bottom, info.planetRadius + cloud.bottom + cloud.thickness) *
			           cloud.average_density;

		g_density[gtid] = density;
	}
	GroupMemoryBarrierWithGroupSync();

	// parallel summation
	[unroll] for (uint offset = 1; offset < NTHREADS; offset <<= 1)
	{
		if (is_valid && gtid >= offset) {
			if (all(floor(raw_thread_uv - ray_uv_increment * offset) == floor(raw_thread_uv)))  // no wraparound happened
			{
				float current_density = g_density[gtid];
				float sample_density = g_density[gtid - offset];
				g_density[gtid] = current_density + sample_density;
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// save
	if (is_valid) {
		RWShadowVolume[thread_px_coord] = lerp(past_density, g_density[gtid], 0.1f);
	}
}
