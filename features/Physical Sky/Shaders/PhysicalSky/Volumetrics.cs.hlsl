#define PHYS_VOLS
#define SKY_SAMPLERS
#include "PhysicalSky/PhysicalSky.hlsli"

#include "Common/VR.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float> TexDepth : register(t4);

Texture3D<unorm float4> TexNubisNoise : register(t5);
Texture2DArray<unorm float> TexCloudNDF : register(t6);
Texture2D<unorm float> TexCloudTopLUT : register(t7);
Texture2D<unorm float> TexCloudBottomLUT : register(t8);

Texture2DArray<float4> TexDirectShadows : register(t20);
struct PerShadow
{
	float4 VPOSOffset;
	float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam;
	float4 DebugColor;
	float4 PropertyColor;
	float4 AlphaTestRef;
	float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
	// Since PerGeometry is passed between c++ and hlsl, can't have different defines due to strong typing
	float4x3 ShadowMapProj[2][3];
	float4x4 CameraViewProjInverse[2];
};
StructuredBuffer<PerShadow> SharedPerShadow : register(t21);
#define TERRAIN_SHADOW_REGISTER t22
#include "TerrainShadows/TerrainShadows.hlsli"
Texture2D<float3> TexBeerShadowMap : register(t23);

RWTexture2D<float3> RWTexTr : register(u0);
RWTexture2D<float3> RWTexLum : register(u1);

RWTexture2D<float3> RWBeerShadowMap : register(u0);

// TODO: horizon transmittance

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
	ndf.dimension_profile = ndf.coverage = ndf.height_fraction = ndf.cloud_type = ndf.bottom_type = ndf.top_value = ndf.bottom_value = ndf.lut_value = 0;
}

NDFInfo sampleNDF(
	float3 pos, CloudLayer params,
	Texture2DArray<unorm float> tex_ndf, Texture2D<unorm float> tex_top, Texture2D<unorm float> tex_bottom)
{
	NDFInfo ndf;
	initNDFInfo(ndf);

	const float2 uv = pos.xy * params.ndf_freq;

	ndf.coverage = tex_ndf.SampleLevel(TileableSampler, float3(uv, 2), 0);
	if (ndf.coverage < 1e-8)
		return ndf;

	const float min_h = lerp(params.bottom, params.bottom + params.thickness, tex_ndf.SampleLevel(TileableSampler, float3(uv, 0), 0));
	const float max_h = lerp(params.bottom, params.bottom + params.thickness, tex_ndf.SampleLevel(TileableSampler, float3(uv, 1), 0));

	ndf.height_fraction = (pos.z - min_h) / (max_h - min_h);

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
	float3 pos, CloudLayer params, float mip_level, bool is_expensive,
	out NDFInfo ndf)
{
	if (pos.z > params.bottom && pos.z < params.bottom + params.thickness) {
		ndf = sampleNDF(pos, params, TexCloudNDF, TexCloudTopLUT, TexCloudBottomLUT);
		if (ndf.dimension_profile < 1e-8)
			return 0;

		float4 noise = TexNubisNoise.SampleLevel(TileableSampler, (pos + params.noise_offset_or_speed) * params.noise_scale_or_freq, mip_level);
		// Define wispy noise
		float wispy_noise = lerp(noise.r, noise.g, ndf.dimension_profile);
		// Define billowy noise
		float billowy_type_gradient = pow(ndf.dimension_profile, 0.25);
		float billowy_noise = lerp(noise.b * 0.3, noise.a * 0.3, billowy_type_gradient);
		// Define Noise composite - blend to wispy as the density scale decreases.
		float noise_composite = lerp(wispy_noise, billowy_noise, ndf.bottom_value);

		float density = saturate((ndf.dimension_profile - noise_composite) / (1 - noise_composite));

		density = saturate((clamp(density, params.remap_in.x, params.remap_in.y) - params.remap_in.x) / (params.remap_in.y - params.remap_in.x));
		density = pow(density, params.power);
		density = lerp(params.remap_out.x, params.remap_out.y, density);

		return saturate(density);
	} else {
		initNDFInfo(ndf);
		return 0;
	}
}

// sample sun ray.transmittance / shadowing
float3 sampleSunTransmittance(float3 pos, float3 sun_dir, uint eye_index, uint3 seed, out float3 cloud_transmittance)
{
	const PhySkyBufferContent info = PhysSkyBuffer[0];

	cloud_transmittance = 1.0;

	float3 shadow = 1.0;

	float3 pos_world = pos + float3(0, 0, info.bottom_z);
	float3 pos_world_relative = pos_world - FrameBuffer::CameraPosAdjust[eye_index].xyz;
	float3 pos_planet = pos_world_relative + float3(0, 0, info.planet_radius - info.bottom_z);

	// earth shadowing
	[branch] if (rayIntersectSphere(pos_planet, sun_dir, info.planet_radius) > 0.0) return 0;

	// dir shadow map
	{
		PerShadow sD = SharedPerShadow[0];
		float4 pos_camera_shifted = mul(FrameBuffer::CameraViewProj[eye_index], float4(pos_world_relative, 1));
		float shadow_depth = pos_camera_shifted.z / pos_camera_shifted.w;
		[branch] if (sD.EndSplitDistances.z >= shadow_depth)
		{
			uint cascade_index = sD.EndSplitDistances.x < shadow_depth;
			float3 positionLS = mul(transpose(sD.ShadowMapProj[eye_index][cascade_index]), float4(pos_world_relative, 1));
			float4 depths = TexDirectShadows.GatherRed(TransmittanceSampler, float3(saturate(positionLS.xy), cascade_index), 0);
			shadow *= dot(depths > positionLS.z, 0.25);
		}
	}
	[branch] if (all(shadow < 1e-8)) return 0;

	// terrain shadow
	shadow *= TerrainShadows::GetTerrainShadow(pos_world, TransmittanceSampler);
	[branch] if (all(shadow < 1e-8)) return 0;

	// analytic fog
	{
		float3 sun_fog_ceil_pos = pos + sun_dir * clamp((info.fog_h_max - pos.z) / sun_dir.z, 0, 10);
		shadow *= analyticFogTransmittance(pos, sun_fog_ceil_pos);
	}
	[branch] if (all(shadow < 1e-8)) return 0;

	// atmosphere
	{
		float2 lut_uv = getHeightZenithLutUv(pos.z + info.planet_radius, sun_dir);
		shadow *= TexTransmittance.SampleLevel(TransmittanceSampler, lut_uv, 0).rgb;
	}
	[branch] if (all(shadow < 1e-8)) return 0;

	// cloud
	{
		const static uint visibility_step = 8;
		const static float visibility_stride = 0.05 / 1.428e-5f;
		const float3 jitter = Random::R3Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f) * 2 - 1;

		float cloud_density = 0;

		for (uint i = 0; i < visibility_step; i++) {
			float3 vis_pos = pos + sun_dir * visibility_stride * (i + 1) + jitter * visibility_stride * (i + 1) / visibility_step;
			NDFInfo _;
			cloud_density += sampleCloudDensity(vis_pos, info.cloud_layer, i * 0.5, true, _) * visibility_stride;
		}
		cloud_transmittance = exp(-(info.cloud_layer.scatter + info.cloud_layer.absorption) * cloud_density);

		// long range (buggeed for now)
		// float2 uv;
		// float depth;
		// float3 vis_pos = pos + sun_dir * visibility_stride * (visibility_step + 1);
		// getOrthographicUV(vis_pos, uv, depth);
		// if (all(uv > 0) && all(uv < 1)) {
		// 	float3 cloud_shadow_sample = TexBeerShadowMap.SampleLevel(TransmittanceSampler, uv, 1 + length(vis_pos - FrameBuffer::CameraPosAdjust[eye_index].xyz) / 1.428e-5f);
		// 	cloud_density += min(cloud_shadow_sample.b, cloud_shadow_sample.g * max(0, depth - cloud_shadow_sample.r));
		// }

		float3 scaled_density = (info.cloud_layer.scatter + info.cloud_layer.absorption) * cloud_density;

		// cloud_transmittance = exp(-scaled_density);
		shadow *= exp(-scaled_density);
		// shadow *= exp(-scaled_density) - exp(-scaled_density * 3);  // beers-powder, 2015, horizon zero dawn
		// shadow *= max(exp(-scaled_density), exp(-scaled_density * 0.25) * 0.7);  // Wrenninge, 2013, multiscatter approx
	}

	return shadow;
}

[numthreads(8, 8, 1)] void main(uint2 tid
								: SV_DispatchThreadID) {
	const PhySkyBufferContent info = PhysSkyBuffer[0];
	const static float start_stride = 0.003 / 1.428e-5f;
	const static float far_stride = 1.0 / 1.428e-5f;
	const static float far_stride_dist = 5 / 1.428e-5f;
	const static uint max_step = 96;

	const uint2 px_coords = tid;

	const uint3 seed = Random::pcg3d(uint3(px_coords.xy, px_coords.x ^ 0xf874));
	const float3 rnd = Random::R3Modified(SharedData::FrameCountAlwaysActive, seed / 4294967295.f);

	///////////// get start and end
	const float depth = TexDepth[px_coords.xy];
	const bool is_sky = depth > 1 - 1e-6;

	const float2 stereo_uv = (px_coords + rnd.xy) * info.rcp_frame_dim;
	const uint eye_index = Stereo::GetEyeIndexFromTexCoord(stereo_uv);
	const float2 uv = Stereo::ConvertFromStereoUV(stereo_uv, eye_index);

	float4 pos_world = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	pos_world = mul(FrameBuffer::CameraViewProjInverse[eye_index], pos_world);
	pos_world.xyz = pos_world.xyz / pos_world.w;

	float ceil = info.fog_h_max;
	ceil = max(ceil, info.cloud_layer.bottom + info.cloud_layer.thickness);
	const float bottom = 0;

	RayMarchInfo ray;
	initRayMarchInfo(ray);

	const float solid_dist = length(pos_world.xyz);
	ray.eye_pos = FrameBuffer::CameraPosAdjust[eye_index].xyz - float3(0, 0, info.bottom_z);
	ray.ray_dir = pos_world.xyz / solid_dist;
	snapMarch(ray, bottom, ceil, is_sky ? 16 / 1.428e-5f : min(16 / 1.428e-5f, solid_dist));

	///////////// precalc
	const float cos_theta = dot(ray.ray_dir, info.dirlight_dir);
	const float3 fog_phase = miePhaseCloudFit(cos_theta);
	const float cloud_phase = lerp(miePhaseThomasSchander(cos_theta), miePhaseHenyeyGreenstein(cos_theta, -0.3), 0.3);
	const float cloud_secondary_phase = miePhaseHenyeyGreensteinDualLobe(cos_theta, 0.21, -0.15, 0.3);

	///////////// ray march
	float ap_dist = 0.0;
	float3 mean_shadowing = 0.0;
	float sum_shadowing_weights = 0.0;

	advanceRay(ray, lerp(start_stride, far_stride, saturate((ray.segment_dist + ray.start_dist) / far_stride_dist)), rnd.z);
	[loop] for (ray.step = 0; ray.step < max_step && ray.ray_dist < ray.march_dist;
				advanceRay(ray, lerp(start_stride, far_stride, saturate((ray.segment_dist + ray.start_dist) / far_stride_dist)), rnd.z))
	{
		const float dt = ray.ray_dist - ray.last_ray_dist;

		// sample scatter & extinction coeffs
		float3 fog_scatter, fog_extinction;
		sampleExponentialFog(ray.pos.z, fog_scatter, fog_extinction);

		NDFInfo ndf;
		float cloud_density = sampleCloudDensity(ray.pos, info.cloud_layer, (ray.start_dist + ray.ray_dist) * 1.428e-5f, true, ndf);
		float3 cloud_scatter = cloud_density * info.cloud_layer.scatter;

		const float3 extinction = fog_extinction + cloud_density * (info.cloud_layer.scatter + info.cloud_layer.absorption);

		// scattering
		[branch] if (max(extinction.x, max(extinction.y, extinction.z)) > 1e-8)
		{
			// dir light
			float3 scatter_phaseless = fog_scatter + cloud_scatter;
			float3 scatter = fog_scatter * fog_phase +
			                 cloud_scatter * cloud_phase;
			float3 cloud_transmittance;
			float3 sun_transmittance = sampleSunTransmittance(ray.pos, info.dirlight_dir, eye_index, seed + ray.step, cloud_transmittance);
			float3 in_scatter = scatter * sun_transmittance * info.dirlight_color;

			sum_shadowing_weights += dt;
			mean_shadowing += sun_transmittance * dt;

			// multiscatter
			float3 ms_volume = saturate((ndf.dimension_profile - 0.1) / (1.0 - 0.1)) * pow(ndf.coverage * ndf.cloud_type, 0.25);
			ms_volume *= pow(cloud_transmittance, info.cloud_ms_transmittance_power);
			ms_volume *= pow(ndf.height_fraction, info.cloud_ms_height_power);
			ms_volume *= info.cloud_ms_mult;
			in_scatter += (sun_transmittance / max(1e-8, cloud_transmittance)) * cloud_scatter * cloud_secondary_phase * ms_volume * info.dirlight_color;

			// ambient
			float3 ambient = Color::GammaToLinear(SharedData::DirectionalAmbient._14_24_34) / Color::LightPreMult;
			in_scatter += scatter_phaseless * sqrt(1.0 - ndf.dimension_profile) * ambient * RCP_PI;

			const float3 sample_transmittance = exp(-dt * extinction);
			const float3 scatter_factor = (1 - sample_transmittance) / max(extinction, 1e-8);
			const float3 scatter_integeral = in_scatter * scatter_factor;

			// update
			ray.lum += scatter_integeral * ray.transmittance;
			ray.transmittance *= sample_transmittance;
		}

		const float tr = max(ray.transmittance.x, max(ray.transmittance.y, ray.transmittance.z));
		ap_dist += tr * dt;
		[branch] if (tr < 1e-3) break;
	}

	// ap
	mean_shadowing = sum_shadowing_weights > 1e-8 ? mean_shadowing / sum_shadowing_weights : 1.0;

	uint3 ap_dims;
	TexAerialPerspective.GetDimensions(ap_dims.x, ap_dims.y, ap_dims.z);
	float2 ap_uv = cylinderMapAdjusted(ray.ray_dir);
	const float depth_slice = lerp(.5 / ap_dims.z, 1 - .5 / ap_dims.z, saturate(solid_dist * info.ap_enhancement / info.aerial_perspective_max_dist));
	const float4 ap_sample = TexAerialPerspective.SampleLevel(SkyViewSampler, float3(ap_uv, depth_slice), 0);
	const float vol_depth_slice = lerp(.5 / ap_dims.z, 1 - .5 / ap_dims.z, saturate(ap_dist * info.ap_enhancement / info.aerial_perspective_max_dist));
	const float4 vol_ap_sample = TexAerialPerspective.SampleLevel(SkyViewSampler, float3(ap_uv, vol_depth_slice), 0);

	if (!is_sky) {
		ray.lum = ray.lum + (ap_sample.rgb - vol_ap_sample.rgb) * ray.transmittance * info.dirlight_color;
		ray.transmittance *= ap_sample.a;
	} else
		ray.transmittance *= vol_ap_sample.a;
	ray.lum = ray.lum * vol_ap_sample.a + vol_ap_sample.rgb * mean_shadowing * info.dirlight_color;

	RWTexTr[px_coords] = ray.transmittance;
	RWTexLum[px_coords] = ray.lum;
};

[numthreads(8, 8, 1)] void renderBeerShadowMap(uint2 tid
											   : SV_DispatchThreadID) {
	const PhySkyBufferContent info = PhysSkyBuffer[0];
	const static uint steps = 50;

	const uint2 px_coords = tid;

	uint2 dims;
	RWBeerShadowMap.GetDimensions(dims.x, dims.y);
	const float2 rcp_dims = rcp(dims);
	const float2 uv = (px_coords + 0.5) * rcp_dims;

	const float3 target = FrameBuffer::CameraPosAdjust[0].xyz - float3(0, 0, info.bottom_z);
	const float3 eye = target + info.dirlight_dir * 1000 / 1.428e-5f;
	const float3 up = abs(info.dirlight_dir.z) == 1 ? float3(1, 0, 0) : float3(0, 0, 1);

	float3 eye_pos, ray_dir;
	orthographicRay(uv, eye, target, up, eye_pos, ray_dir);

	const float bottom = info.cloud_layer.bottom;
	const float ceil = info.cloud_layer.bottom + info.cloud_layer.thickness;
	float3 start_pos, end_pos;
	float start_dist, end_dist, march_dist;
	snapMarch(bottom, ceil, eye_pos, ray_dir, 2000 / 1.428e-5f,
		start_pos, end_pos, march_dist, start_dist, end_dist);

	uint n_samples = 0;
	float front_depth = -1;
	float mean_density = 0;
	float max_optical_depth = 0;
	float sum_density = 0;

	float stride = march_dist / (steps + 1);
	for (uint i = 0; i < steps; i++) {
		float3 pos = start_pos + (i + 1) * stride * ray_dir;

		NDFInfo _;
		float density = sampleCloudDensity(pos, info.cloud_layer, 0, true, _);

		if (density > 0.01) {
			front_depth = front_depth < 0 ? length(pos - eye_pos) : front_depth;

			sum_density += density;
			max_optical_depth += density * stride;  // here, weight optical depth by sample

			n_samples++;

			// early break
			float3 sum_extinction = sum_density * (info.cloud_layer.scatter + info.cloud_layer.absorption);
			if (max(sum_extinction.r, max(sum_extinction.g, sum_extinction.b)) >= 7.0f) {
				break;
			}
		}
	}
	if (n_samples > 0) {
		mean_density = sum_density / n_samples;
	} else {
		mean_density = 0.0f;
		front_depth = length(end_pos - eye_pos);
		max_optical_depth = 0;
	}

	RWBeerShadowMap[px_coords.xy] = float3(front_depth, mean_density, max_optical_depth);
}