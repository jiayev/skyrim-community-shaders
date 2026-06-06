#define PHYSICAL_SKY_VOLUMETRICS
#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#define PS_SKY_SAMPLERS
#define PS_PREPASS_RSRCS
#define PS_NO_RSRCS
#define OMIT_PS_NAMESPACE
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
};

struct VolumetricCloudData
{
	float rayMarchRange;
	float shadowVolumeRange;
	uint cloudMaxStep;
	float _pad0;

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

cbuffer VolumetricCloudCubeHistoryCB : register(b1)
{
	float CubeHistoryWeight;
	float3 CubeHistoryPad0;
};

RWTexture2D<float3> RWTexTr : register(u0);
RWTexture2D<float3> RWTexLum : register(u1);

RWTexture3D<float> RWShadowVolume : register(u0);

RWTexture2DArray<float3> RWTexCubeTr : register(u0);
RWTexture2DArray<float3> RWTexCubeLum : register(u1);

#define ISNAN(x) (!(x < 0.f || x > 0.f || x == 0.f))

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

float3 SampleCloudAmbientSkyView(float3 pos)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust[0].xy, info.planetRadius);
	const float altitude = length(pos_planet);
	const float3 up_dir = pos_planet / max(altitude, 1e-8);

	const float3 basis_ref = abs(up_dir.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
	const float3 tangent = normalize(cross(basis_ref, up_dir));
	const float3 bitangent = cross(up_dir, tangent);

	const float horizonLift = 0.35;
	float3 ambientRadiance = TexSkyView.SampleLevel(SkyViewSampler, SkyViewLutUv(up_dir), 0).rgb * 0.4;
	ambientRadiance += TexSkyView.SampleLevel(SkyViewSampler, SkyViewLutUv(normalize(tangent + up_dir * horizonLift)), 0).rgb * 0.15;
	ambientRadiance += TexSkyView.SampleLevel(SkyViewSampler, SkyViewLutUv(normalize(-tangent + up_dir * horizonLift)), 0).rgb * 0.15;
	ambientRadiance += TexSkyView.SampleLevel(SkyViewSampler, SkyViewLutUv(normalize(bitangent + up_dir * horizonLift)), 0).rgb * 0.15;
	ambientRadiance += TexSkyView.SampleLevel(SkyViewSampler, SkyViewLutUv(normalize(-bitangent + up_dir * horizonLift)), 0).rgb * 0.15;
	return ambientRadiance * Math::PI;
}

float SampleFilteredApShadow(uint2 pxCoord)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

	uint2 apDims;
	TexApShadow.GetDimensions(apDims.x, apDims.y);

	float2 apCoord = float2(pxCoord) + 0.5;
	if (data.halfResApShadow)
		apCoord *= 0.5;

	return TexApShadow.SampleLevel(TransmittanceSampler, apCoord / apDims, 0);
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
	float3 boundsMin = float3(FrameBuffer::CameraPosAdjust[0].xy - 0.5 * info.shadowVolumeRange, cloud.bottom);
	float3 boundsMax = float3(FrameBuffer::CameraPosAdjust[0].xy + 0.5 * info.shadowVolumeRange, cloud.bottom + cloud.thickness);

	float3 samplePos = pos;
	if (any(pos < boundsMin) || any(pos > boundsMax)) {
		float2 hitDists = RayIntersectAABB(pos, rayDir, boundsMin, boundsMax);
		if (hitDists.x > hitDists.y)
			return -1;
		samplePos += (hitDists.x + 128) * rayDir;
	}

	float3 uvw = samplePos - float3(FrameBuffer::CameraPosAdjust[0].xy, cloud.bottom);
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
	float planet_z = length(pos + float3(-FrameBuffer::CameraPosAdjust[0].xy, info.planetRadius)) - info.planetRadius;
	if (planet_z < cloud.bottom || planet_z > cloud.bottom + cloud.thickness)
		return ndf;

	ndf.in_layer = true;

	const float2 uv = pos.xy * cloud.ndf_freq;

	ndf.coverage = tex_ndf.SampleLevel(TileableSampler, float3(uv, 2), 0);
	if (ndf.coverage < 1e-8)
		return ndf;

	const float min_h = lerp(cloud.bottom, cloud.bottom + cloud.thickness, tex_ndf.SampleLevel(TileableSampler, float3(uv, 0), 0));
	const float max_h = lerp(cloud.bottom, cloud.bottom + cloud.thickness, tex_ndf.SampleLevel(TileableSampler, float3(uv, 1), 0));

	ndf.height_fraction = (planet_z - min_h) / (max_h - min_h);

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
	float4 noise = TexNubisNoise.SampleLevel(TileableSampler, (pos + cloud.noise_offset_or_speed) * cloud.noise_scale_or_freq, mip_level);
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
		float hhf_noise_distance_range_blender = lerp(0.9, 1.0, hhf_fraction);
		noise_composite = lerp(hhf_noise, noise_composite, hhf_noise_distance_range_blender);
	}

	float density = saturate((ndf.dimension_profile - noise_composite) / (1 - noise_composite));

	// Sharpen result
	density = pow(density, cloud.power);
	if (close_range) {
		density = pow(density, lerp(0.5, 1.0, hhf_fraction)) * lerp(0.666, 1.0, hhf_fraction);
	}

	return saturate(density);
}

// sample sun transmittance / shadowing
float3 sampleSunTransmittance(float3 pos, float3 sun_dir, uint eye_index, uint3 seed, out float3 cloud_transmittance)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	cloud_transmittance = 1.0;

	float3 shadow = 1.0;

	float3 pos_world = pos + float3(0, 0, info.bottomZ);
	float3 pos_world_relative = pos_world - FrameBuffer::CameraPosAdjust[eye_index].xyz;
	float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust[0].xy, info.planetRadius);

	// earth shadowing
	[branch] if (RayIntersectSphereCentered(pos_planet, sun_dir, info.planetRadius) > 0.0) return 0;

	// dir shadow map
	{
		DirectionalShadowLightData directionalShadowLightData = DirectionalShadowLights[0];
		float shadow_depth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(pos_world_relative, eye_index));
		[branch] if (directionalShadowLightData.EndSplitDistances.y > 0.0 &&
					 shadow_depth < directionalShadowLightData.EndSplitDistances.y)
		{
			float cascade_select = saturate(
				(shadow_depth - directionalShadowLightData.StartSplitDistances.y) /
				(directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));
			uint cascade_index = uint(cascade_select);
			float3 positionLS = mul(directionalShadowLightData.ShadowProj[cascade_index], float4(pos_world, 1)).xyz;
			float4 depths = TexDirectShadows.GatherRed(TransmittanceSampler, float3(saturate(positionLS.xy), cascade_index), 0);
			shadow *= dot(depths > positionLS.z, 0.25);
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
		const static uint visibility_step = 2;
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
								 vis_pos + float3(-FrameBuffer::CameraPosAdjust[0].xy, info.planetRadius), sun_dir,
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
};

VolumetricCloudResult RenderVolumetricCloudRay(float3 ray_dir, float3 eye_pos, float solid_dist, bool is_sky, uint eye_index, uint3 seed, float jitter, float ap_shadow)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);
	const static float zero_density_stride_mult = 1.5;

	const float ceil = cloud.bottom + cloud.thickness;
	const float bottom = 0;

	RayMarchInfo ray;
	initRayMarchInfo(ray);

	ray.eye_pos = eye_pos;
	ray.ray_dir = ray_dir;
	snapMarch(ray, bottom, ceil, is_sky ? info.rayMarchRange : min(info.rayMarchRange, solid_dist));

	///////////// precalc
	const float cos_theta = dot(ray.ray_dir, info.dirlightDir);
	const float cloud_phase = lerp(Phase::ThomasSchander(cos_theta), Phase::HG(cos_theta, -0.3), 0.3);
	const float cloud_secondary_phase = Phase::HGDualLobe(cos_theta, 0.21, -0.15, 0.3);

	///////////// ray march
	float ap_dist = 0.0;
	float3 mean_shadowing = 0.0;
	float sum_shadowing_weights = 0.0;

	float stride = 0.003 / 1.428e-5f;

	advanceRay(ray, stride, jitter);
	[loop] for (ray.step = 0; ray.step < 150 && ray.ray_dist < ray.march_dist; advanceRay(ray, stride, jitter))
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
			float3 sun_transmittance = sampleSunTransmittance(ray.pos, info.dirlightDir, eye_index, seed + ray.step, cloud_transmittance);
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
			float3 ambient = SampleCloudAmbientSkyView(ray.pos);
			in_scatter += cloud_scatter * sqrt(1.0 - ndf.dimension_profile) * cloud.ambient_mult * ambient * RCP_PI;

			const float3 sample_transmittance = exp(-dt * extinction);
			const float3 scatter_factor = (1 - sample_transmittance) / max(extinction, 1e-8);
			const float3 scatter_integeral = in_scatter * scatter_factor;

			// update
			ray.lum += scatter_integeral * ray.transmittance;
			ray.transmittance *= sample_transmittance;
		}

		// stride
		float rcp_step = ndf.in_layer ? rcp(info.cloudMaxStep) * (ndf.dimension_profile > 1e-8 ? 1 : zero_density_stride_mult) : rcp(info.cloudMaxStep);
		float march_prop = (ray.start_dist + ray.march_dist) / info.rayMarchRange;
		stride = (pow(sqrt(march_prop) + rcp_step, 2) - march_prop) * info.rayMarchRange;

		const float tr = max(ray.transmittance.x, max(ray.transmittance.y, ray.transmittance.z));
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
	return result;
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];

	const uint2 px_coords = tid;

	const uint3 seed = Random::pcg3d(uint3(px_coords.xy, px_coords.x ^ 0xf874));
	const float3 rnd = Random::R3Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f);

	///////////// get start and end
	const float depth = TexDepth[px_coords.xy];
	const bool is_sky = depth > 1 - 1e-6;

	const float2 stereo_uv = (px_coords + rnd.xy) * info.rcpFrameDim;
	const uint eye_index = Stereo::GetEyeIndexFromTexCoord(stereo_uv);
	const float2 uv = Stereo::ConvertFromStereoUV(stereo_uv, eye_index) * FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res

	float4 pos_world = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	pos_world = mul(FrameBuffer::CameraViewProjInverse[eye_index], pos_world);
	pos_world.xyz = pos_world.xyz / pos_world.w;

	const float solid_dist = length(pos_world.xyz);
	const float3 eye_pos = FrameBuffer::CameraPosAdjust[eye_index].xyz - float3(0, 0, info.bottomZ);
	const float3 ray_dir = pos_world.xyz / solid_dist;

	const float ap_shadow = SampleFilteredApShadow(px_coords);
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, solid_dist, is_sky, eye_index, seed, rnd.z, ap_shadow);

	RWTexTr[px_coords] = result.transmittance;
	RWTexLum[px_coords] = result.lum;
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
	const float3 rnd = Random::R3Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f);

	const float3 eye_pos = FrameBuffer::CameraPosAdjust[0].xyz - float3(0, 0, info.bottomZ);
	const float3 ray_dir = GetCubemapSamplingVector(tid, RWTexCubeTr);
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, info.rayMarchRange, true, 0, seed, rnd.z, 0.0);

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

#define NTHREADS 256
groupshared float g_density[NTHREADS];

[numthreads(NTHREADS, 1, 1)] void renderShadowVolume(const uint gtid : SV_GroupThreadID, const uint2 gid : SV_GroupID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	uint3 dims;
	RWShadowVolume.GetDimensions(dims.x, dims.y, dims.z);
	const float3 rcp_dims = rcp(dims);
	const float3 scale = float3(info.shadowVolumeRange.xx, cloud.thickness);
	const float3 rcp_scale = rcp(scale);

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

	const bool3 is_uv_in_range = (raw_thread_uv > 0) && (raw_thread_uv < 1);
	const bool is_valid = dot(is_uv_in_range, component_mask);

	const float3 thread_uv = raw_thread_uv - floor(raw_thread_uv);  // wraparound
	const uint3 thread_px_coord = thread_uv * dims;

	float past_density = RWShadowVolume[thread_px_coord];
	if (ISNAN(past_density))
		past_density = 0;

	if (is_valid) {
		const float3 pos = float3(FrameBuffer::CameraPosAdjust[0].xy + (thread_uv.xy - 0.5) * info.shadowVolumeRange, cloud.bottom + cloud.thickness * thread_uv.z);

		// fetch density using only ndf
		NDFInfo _;
		float density = sampleCloudDensity(pos, 1e8, cloud, 2, false, _) * length(ray_uv_increment * scale);  // scaled by ray length

		// average visibility for boundary
		float3 prev_uv = thread_uv - ray_uv_increment;
		float3 prev_pos = float3(FrameBuffer::CameraPosAdjust[0].xy + (prev_uv.xy - 0.5) * info.shadowVolumeRange, cloud.bottom + cloud.thickness * prev_uv.z);
		if ((any(prev_uv < 0) || any(prev_uv > 1)) && prev_pos.z > cloud.bottom && prev_pos.z < cloud.bottom + cloud.thickness)
			density += InBetweenSphereDistance(
						   prev_pos + float3(-FrameBuffer::CameraPosAdjust[0].xy, info.planetRadius), info.dirlightDir,
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
