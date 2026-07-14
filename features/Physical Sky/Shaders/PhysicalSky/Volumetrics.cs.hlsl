// Community Shaders
// Authors: Profjack, Jiaye
//
// References:
// HanPi Volume Cloud © AshenOneArt
// https://github.com/AshenOneArt/HPVolumeCloud

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
	float2 weatherCenter;
	float weatherWorldSize;
	float highCloudEnabled;
	float2 noiseWindOffset;
	float3 noiseScale;
	float detailNoiseScale;
	float3 noiseOffset;
	float baseNoiseWindSpeed;
	float detailNoiseWindSpeed;
	float detailNoiseVerticalWindSpeed;
	float billowyLow;
	float billowyHigh;
	float wispyLow;
	float wispyHigh;
	float detailStrengthCu;
	float detailStrengthTcu;
	float detailStrengthCb;
	float densityThreshold;
	float densityMultiplier;
	float densityMultiplierCu;
	float densityMultiplierTcu;
	float densityMultiplierCb;
	float bottomSmoothHeight;
	float bottomSmoothPow;
	float wispyEdgeWidth;
	float wispyReach;
	float wispyTopHeight;
	float wispyTopHardness;
	float coverageCoverIntensity;
	float coverageCoverContrast;
	float coverageHeightIntensity;
	float coverageHeightContrast;
	float coverTopStrength;
	float coverTopMax;
	float coverTopCurvePow;
	float2 scCellScale;
	float scWorleyStrength;
	float scHeightScale;
	float scDetailStrength;
	float scCellThickPow;
	float scCellThickStrength;
	float scCellNoiseStrength;
	float scCoverageIntensity;
	float scCoverageContrast;
	float2 highCellScale;
	float highCellWindSpeed;
	float2 highCellWarpScale;
	float highCellWarpStrength;
	float highCellThickStrength;
	float highAsCellThickStrength;
	float highCellThickPow;
	float highCloudBottom;
	float highCloudTop;
	float highBottomCoverageScale;
	float highHeightCurvePow;
	float highDensityThreshold;
	float highDensitySoftness;
	float highCloudSoftness;
	float2 highWispScale;
	float highWispStrength;
	float highHorizonDistanceStart;
	float highHorizonDistanceEnd;
	float highDensityMultiplier;
	float highDensitySoftAIntensity;
	float highDensitySoftAContrast;
	float highDensityModAIntensity;
	float highDensityModAContrast;
	float3 scatterTint;
	float forwardEccentricity;
	float backwardEccentricity;
	float ambientTopMultiplier;
	float ambientBottomMultiplier;
	float aoUpwardScale;
	float msAttenuation;
	float msContribution;
	float msEccentricity;
	float scatterSourceODScale;
	float scatterSourceCurvePow;
	float powderIntensity;
	uint lightSteps;
	uint primaryStepMultiplier;
	float phiFwdIntensity;
	float phiFwdDepthPow;
	float phiFwdDepthBias;
	float phiFwdBoundaryConfidence;
	float phiFwdMSBuildScale;
	float phiFwdCompress;
	float highForwardEccentricity;
	float highBackwardEccentricity;
	float highAmbientTopMultiplier;
	float highAmbientBottomMultiplier;
	float highSkyBlendStrength;
	float highMSAttenuation;
	float highMSContribution;
	float highMSEccentricity;
	float highLightAbsorption;
	float highViewAbsorption;
	float highCoverAbsorptionStrength;

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
	cloud.ndf_freq = 1.0 / max(info.weatherWorldSize.xx, 1.0);
	cloud.noise_scale_or_freq = 1.0;
	cloud.noise_offset_or_speed = info.noiseOffset;
	cloud.power = 1.0;
	cloud.scatter = info.scatterTint * info.densityMultiplier;
	cloud.absorption = max(0.01, (1.0 - info.scatterTint * 0.25) * info.densityMultiplier);
	cloud.average_density = info.densityMultiplier;
	cloud.ms_mult = 1.0;
	cloud.ms_transmittance_power = 0.5;
	cloud.ms_height_power = 0.7;
	cloud.ambient_mult = info.ambientTopMultiplier;
	cloud.density_erosion_weak = 0.35;
	cloud.density_erosion_strong = 0.18;
	cloud.noise_mip_bias_weak = 0.0;
	cloud.noise_mip_bias_strong = 0.0;
	cloud.hhf_min_blend = 1.0;
	cloud.hhf_profile_threshold = 0.1;
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

Texture3D<unorm float4> TexHpBaseNoise : register(t5);
Texture3D<unorm float4> TexHpDetailNoise : register(t6);
Texture2D<float4> TexHpLowWeather : register(t7);
Texture2D<float4> TexHpHighWeather : register(t8);
Texture2D<unorm float> TexApShadow : register(t9);
Texture2D<float4> TexSkyView : register(t10);
Texture2D<float4> TexHpProfile : register(t11);
Texture2D<float4> TexHpScCell : register(t12);
Texture2D<float4> TexHpHighCell : register(t13);
Texture2D<float4> TexHpHighWarp : register(t14);
Texture2D<float4> TexHpHighWisp : register(t15);
Texture2D<sh2> TexCloudAmbientSH : register(t16);

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

	const float near_dot = dot(float3(jitter_x, jitter_y, jitter_frame), float3(jitter_y + 31.3199997, jitter_frame + 31.3199997, jitter_x + 31.3199997));
	const float far_dot = dot(float3(jitter_x, jitter_y, jitter_x), float3(jitter_y + 19.1900005, jitter_x + 19.1900005, jitter_x + 19.1900005));

	return float2(
		frac((jitter_x + jitter_y + 2.0 * jitter_frame + near_dot * 4.0) * (jitter_frame + jitter_x + near_dot * 2.0)),
		frac((jitter_x + jitter_y + far_dot * 2.0) * (far_dot + jitter_x)));
}

float SelectNubisRayJitter(float rayDistance, float2 jitter)
{
	return jitter.x;
}

float NubisVerticalStep(float rayDistance)
{
	return rayDistance * 0.003662109375 + 0.003 / 1.428e-5f;
}

float StabilizeVerticalProfileDensity(float dimensionProfile, float noiseComposite, CloudLayer cloud)
{
	float erosionWidth = max(1.0 - noiseComposite, lerp(cloud.density_erosion_weak, cloud.density_erosion_strong, saturate(dimensionProfile)));
	return saturate((dimensionProfile - noiseComposite) / erosionWidth);
}

float sampleCoverage2D(float3 pos, CloudLayer cloud)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 uv = (pos.xy - info.weatherCenter) / max(info.weatherWorldSize, 1.0) + 0.5;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	return TexHpLowWeather.SampleLevel(TransmittanceSampler, uv, 0).r;
}

float HPPositivePow(float x, float p)
{
	return pow(max(x, 0.0), p);
}

float HPDensityRemap(float x, float a, float b, float c, float d)
{
	return (((x - a) / max(b - a, 1e-5)) * (d - c)) + c;
}

float2 HPWeatherUV(float2 worldXY, VolumetricCloudData info)
{
	return (worldXY - info.weatherCenter) / max(info.weatherWorldSize, 1.0) + 0.5;
}

float HPLowTypeValue(float cloudType, float cu, float tcu, float cb)
{
	return cloudType < 0.5 ? lerp(cu, tcu, cloudType * 2.0) : lerp(tcu, cb, (cloudType - 0.5) * 2.0);
}

float HPEvaluateTopHeightProxy(float2 worldXY)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float2 uv = HPWeatherUV(worldXY, info);
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	float4 weather = TexHpLowWeather.SampleLevel(TransmittanceSampler, uv, 0);
	float coverageHeight = saturate(HPPositivePow(weather.r, max(info.coverageHeightContrast, 0.001)) * info.coverageHeightIntensity);
	float topScale = lerp(1.0, max(info.coverTopMax, 1.0), HPPositivePow(coverageHeight, max(info.coverTopCurvePow, 0.01)) * info.coverTopStrength);
	return saturate(coverageHeight * topScale / max(info.coverTopMax, 1.0));
}

float HPEvaluateBoundaryLight(float3 pos, float3 sunDir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float sampleStep = clamp(info.weatherWorldSize * 0.001, 25.0, 200.0);
	float hL = HPEvaluateTopHeightProxy(pos.xy - float2(sampleStep, 0.0));
	float hR = HPEvaluateTopHeightProxy(pos.xy + float2(sampleStep, 0.0));
	float hD = HPEvaluateTopHeightProxy(pos.xy - float2(0.0, sampleStep));
	float hU = HPEvaluateTopHeightProxy(pos.xy + float2(0.0, sampleStep));
	float dHdx = (hR - hL) * info.cloudThickness / max(2.0 * sampleStep, 1.0);
	float dHdy = (hU - hD) * info.cloudThickness / max(2.0 * sampleStep, 1.0);
	float3 topNormal = normalize(float3(-dHdx, -dHdy, 1.0));
	float wrap = 0.5;
	float lit = saturate((dot(topNormal, sunDir) + wrap) / (1.0 + wrap));
	return lerp(1.0, lit, saturate(info.phiFwdBoundaryConfidence));
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

NDFInfo sampleNDF(float3 pos, CloudLayer cloud)
{
	NDFInfo ndf;
	initNDFInfo(ndf);

	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float planet_z = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planet_z < cloud.bottom || planet_z > cloud.bottom + cloud.thickness)
		return ndf;

	ndf.in_layer = true;
	const float2 uv = HPWeatherUV(pos.xy, info);
	if (any(uv < 0.0) || any(uv > 1.0))
		return ndf;

	float4 weather = TexHpLowWeather.SampleLevel(TransmittanceSampler, uv, 0);
	ndf.coverage = weather.r;
	if (ndf.coverage < 1e-8)
		return ndf;

	ndf.height_fraction = saturate((planet_z - cloud.bottom) / max(cloud.thickness, 1e-5));

	if (ndf.height_fraction < 0 || ndf.height_fraction > 1)
		return ndf;

	ndf.cloud_type = weather.g;
	ndf.bottom_type = saturate(weather.b);
	float radialDist = saturate(length(uv - 0.5) * 2.0);
	float3 profiles = TexHpProfile.SampleLevel(TransmittanceSampler, float2(ndf.height_fraction, radialDist), 0).rgb;
	ndf.top_value = HPLowTypeValue(ndf.cloud_type, profiles.r, profiles.g, profiles.b);
	ndf.bottom_value = lerp(1.0, profiles.r, weather.b);
	ndf.lut_value = ndf.top_value;

	ndf.dimension_profile = ndf.coverage * ndf.lut_value;

	return ndf;
}

float sampleCloudDensity(
	float3 pos, float eye_dist, CloudLayer cloud, float mip_level, bool is_expensive,
	out NDFInfo ndf)
{
	ndf = sampleNDF(pos, cloud);
	if (ndf.dimension_profile < 1e-8)
		return 0;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 weatherUV = HPWeatherUV(pos.xy, info);
	float4 weather = TexHpLowWeather.SampleLevel(TransmittanceSampler, weatherUV, 0);
	float coverageRaw = weather.r;
	float cloudType = weather.g;
	float scMask = weather.b;
	float coverage = saturate(HPPositivePow(coverageRaw, max(info.coverageCoverContrast, 0.001)) * info.coverageCoverIntensity);
	float coverageHeight = saturate(HPPositivePow(coverageRaw, max(info.coverageHeightContrast, 0.001)) * info.coverageHeightIntensity);
	float scStr = info.scWorleyStrength * scMask;
	float scCell = saturate(TexHpScCell.SampleLevel(TransmittanceSampler, weatherUV * info.scCellScale, 0).r * info.scCellNoiseStrength);
	float scCoverage = saturate(HPPositivePow(coverageRaw, max(info.scCoverageContrast, 0.001)) * info.scCoverageIntensity);
	coverage = lerp(coverage, scCoverage * scCell, scStr);
	if (coverage < 0.001)
		return 0.0;

	float topScale = lerp(1.0, max(info.coverTopMax, 1.0), HPPositivePow(coverageHeight, max(info.coverTopCurvePow, 0.01)) * info.coverTopStrength);
	float heightForLut = ndf.height_fraction / (1.0 + (topScale - 1.0) * ndf.height_fraction);
	float localHeight = lerp(heightForLut, saturate(ndf.height_fraction / max(info.scHeightScale, 0.01)), scStr);
	float radialDist = saturate(length(weatherUV - 0.5) * 2.0);
	float3 profiles = TexHpProfile.SampleLevel(TransmittanceSampler, float2(localHeight, radialDist), 0).rgb;
	float heightGradient = lerp(HPLowTypeValue(cloudType, profiles.r, profiles.g, profiles.b), profiles.r, scStr);
	float3 windOffset = float3(info.noiseWindOffset.x, info.noiseWindOffset.y, 0.0) * info.baseNoiseWindSpeed;
	float4 baseNoise = TexHpBaseNoise.SampleLevel(TileableSampler, pos * info.noiseScale + info.noiseOffset + windOffset, max(mip_level, 0.0));
	float baseShape = pow(abs(baseNoise.r), 0.6);
	float bottomNoiseFade = info.bottomSmoothHeight > 0.0 ? HPPositivePow(saturate(localHeight / info.bottomSmoothHeight), max(info.bottomSmoothPow, 0.01)) : 1.0;
	baseShape = lerp(1.0, baseShape, bottomNoiseFade);
	float billowy = 0.0;
	float wispy = 0.0;
	if (is_expensive) {
		float3 detailWind = float3(info.noiseWindOffset.x, info.noiseWindOffset.y, SharedData::FrameCountAlwaysActive * info.detailNoiseVerticalWindSpeed) * info.detailNoiseWindSpeed;
		float4 d = TexHpDetailNoise.SampleLevel(TileableSampler, float3(pos.x, pos.y, -pos.z) * info.detailNoiseScale + info.noiseOffset * 0.5 + detailWind, 0);
		billowy = (d.b * info.billowyLow + d.a * info.billowyHigh) * bottomNoiseFade;
		wispy = (d.r * info.wispyLow + d.g * info.wispyHigh) * bottomNoiseFade;
	}
	float detailStrength = lerp(HPLowTypeValue(cloudType, info.detailStrengthCu, info.detailStrengthTcu, info.detailStrengthCb), info.scDetailStrength, scStr);
	float threshold = (1.0 - coverage) + info.densityThreshold;
	float erodedBillowy = saturate(HPDensityRemap(baseShape, billowy * detailStrength, 1.0, 0.0, 1.0)) * heightGradient;
	float erodedWispy = saturate(HPDensityRemap(baseShape, wispy * detailStrength, 1.0, 0.0, 1.0)) * heightGradient;
	float densityBillowy = saturate(HPDensityRemap(erodedBillowy, threshold, threshold + 0.08, 0.0, 1.0));
	float densityWispy = saturate(HPDensityRemap(erodedWispy, threshold - info.wispyReach, threshold - info.wispyReach + 0.08, 0.0, 1.0));
	float wispyT = saturate((ndf.height_fraction - info.wispyTopHeight) / max(1.0 - info.wispyTopHeight, 0.001));
	densityWispy *= HPPositivePow(1.0 - wispyT, max(info.wispyTopHardness * 10.0, 0.01));
	float scCellShaped = HPPositivePow(max(scCell, 0.001), max(info.scCellThickPow, 0.01));
	float heightClip = lerp(1.0, lerp(1.0, scCellShaped, info.scCellThickStrength), scStr);
	float density = lerp(densityWispy, densityBillowy, smoothstep(0.0, info.wispyEdgeWidth, densityBillowy)) * heightClip;
	density *= HPLowTypeValue(cloudType, info.densityMultiplierCu, info.densityMultiplierTcu, info.densityMultiplierCb);
	return max(0.0, density * info.densityMultiplier);
}

// sample sun transmittance / shadowing
float3 sampleSunTransmittance(float3 pos, float3 sun_dir, uint3 seed, uint jitter_frame, out float3 cloud_transmittance)
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
		uint visibility_step = max(info.lightSteps, 1u);
		const static float cone_ratio = 2.0;
		const static float cone_min_step = 5.0;
		const static float cone_max_distance = 6000.0;
		const float3 jitter = Random::R3Modified(jitter_frame, seed / 4294967295.f);

		float cloud_density = 0;
		float cover_dist = cone_max_distance;
		float step_width = max(cover_dist * (cone_ratio - 1.0) / max(pow(cone_ratio, (float)visibility_step) - 1.0, 1e-4), cone_min_step);
		float cum_dist = 0.0;

		for (uint i = 0; i < visibility_step; i++) {
			float width = min(step_width, cover_dist - cum_dist);
			if (width <= 0.0)
				break;
			float dist = cum_dist + width * 0.5;
			float3 vis_pos = pos + sun_dir * dist + jitter * width * 0.25;
			NDFInfo _;
			cloud_density += sampleCloudDensity(vis_pos, 1e8, cloud, i * 0.5, true, _) * width;
			cum_dist += width;
			step_width *= cone_ratio;
		}

		// long range
		float3 vis_pos = pos + sun_dir * cum_dist;
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

float EvaluateHighCloudDensity(float3 pos, out float normalizedHeight)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	normalizedHeight = 0.0;
	if (info.highCloudEnabled <= 0.0)
		return 0.0;
	float planetZ = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planetZ < info.cloudBottom || planetZ > info.cloudBottom + info.cloudThickness)
		return 0.0;
	normalizedHeight = saturate((planetZ - info.cloudBottom) / max(info.cloudThickness, 1.0));
	float2 uv = HPWeatherUV(pos.xy, info);
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	float4 hiWeather = TexHpHighWeather.SampleLevel(TransmittanceSampler, uv, 0);
	float hiCoverage = hiWeather.r;
	float hiType = hiWeather.g;
	if (hiCoverage < 0.001)
		return 0.0;
	float2 hiWindUV = info.noiseWindOffset / max(info.weatherWorldSize, 1.0);
	float2 cellUV = uv * info.highCellScale + hiWindUV * info.highCellWindSpeed;
	float2 warpUV = uv * info.highCellWarpScale + hiWindUV * info.highCellWindSpeed * 0.5;
	float2 warp = (TexHpHighWarp.SampleLevel(TransmittanceSampler, warpUV, 0).rg * 2.0 - 1.0) * info.highCellWarpStrength;
	float hiCell = saturate(TexHpHighCell.SampleLevel(TransmittanceSampler, cellUV + warp, 0).r);
	float hiCellShaped = HPPositivePow(max(hiCell, 0.001), max(info.highCellThickPow, 0.01));
	float hiCellThick = lerp(info.highAsCellThickStrength, info.highCellThickStrength, hiType);
	float hiCoverForHeight = HPPositivePow(hiCoverage, max(info.highHeightCurvePow, 0.01));
	float hiDrivenTop = lerp(info.highCloudBottom, info.highCloudTop, hiCoverForHeight);
	float hiTop = info.highCloudBottom + (hiDrivenTop - info.highCloudBottom) * lerp(1.0, hiCellShaped, hiCellThick * 0.5);
	float hiBottom = info.highCloudBottom - (info.highCloudTop - info.highCloudBottom) * info.highBottomCoverageScale * hiCoverForHeight;
	float distXY = length((pos - FrameBuffer::CameraPosAdjust.xyz).xy);
	float horizonT = smoothstep(info.highHorizonDistanceStart, max(info.highHorizonDistanceStart + 1.0, info.highHorizonDistanceEnd), distXY);
	hiTop -= horizonT * hiBottom;
	hiBottom -= horizonT * hiBottom;
	float band = smoothstep(hiBottom - info.highCloudSoftness, hiBottom + info.highCloudSoftness, normalizedHeight) * (1.0 - smoothstep(hiTop - info.highCloudSoftness, hiTop + info.highCloudSoftness, normalizedHeight));
	float wisp = TexHpHighWisp.SampleLevel(TransmittanceSampler, uv * info.highWispScale + hiWindUV * info.highCellWindSpeed, 0).r;
	wisp = saturate(wisp * wisp);
	float soft = info.highDensitySoftness * (1.0 - HPPositivePow(saturate(hiWeather.a), max(info.highDensitySoftAContrast, 0.01)));
	float density = saturate(HPDensityRemap(hiCoverage, info.highDensityThreshold, info.highDensityThreshold + max(soft, 0.001), 0.0, 1.0));
	density = (density * lerp(1.0, hiCellShaped, hiCellThick) - wisp * info.highWispStrength * hiType) * band;
	density *= 1.0 - saturate(info.highDensityModAIntensity * (1.0 - HPPositivePow(saturate(hiWeather.a), max(info.highDensityModAContrast, 0.01))));
	return max(0.0, density * info.highDensityMultiplier);
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

VolumetricCloudResult RenderVolumetricCloudRay(float3 ray_dir, float3 eye_pos, float solid_dist, bool is_sky, uint3 seed, float2 jitter, uint jitter_frame, float ap_shadow, bool skip_below_cloud_bottom)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);
	const static float zero_density_stride_mult = 1.5;

	const float ceil = cloud.bottom + cloud.thickness;
	const float bottom = skip_below_cloud_bottom ? cloud.bottom : 0.0;

	RayMarchInfo ray;
	initRayMarchInfo(ray);

	ray.eye_pos = eye_pos;
	ray.ray_dir = ray_dir;
	const float max_march_dist = is_sky ? info.rayMarchRange : min(info.rayMarchRange, solid_dist);
	snapMarch(ray, bottom, ceil, max_march_dist);

	[branch] if (!is_sky && ray.march_dist <= 0.0)
	{
		VolumetricCloudResult result;
		result.transmittance = 1.0;
		result.lum = 0.0;
		result.cloud_depth = solid_dist;
		result.reject_depth = solid_dist;
		result.scatter_weight = 0.0;
		result.weighted_depth = 0.0;
		return result;
	}

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

	const float base_stride = 500;
	float stride = base_stride;
	const float coarse_stride = 4 * base_stride;

	advanceRay(ray, stride, SelectNubisRayJitter(ray.start_dist + ray.segment_dist, jitter));
	[loop] for (; ray.step < info.cloudMaxStep && ray.ray_dist < ray.march_dist; advanceRay(ray, stride, SelectNubisRayJitter(ray.start_dist + ray.segment_dist, jitter)))
	{
		float coverage_2d = sampleCoverage2D(ray.pos, cloud);

		[branch] if (coverage_2d < 1e-4)
		{
			// no 2D coverage: skip with coarse stride
			ap_dist += stride;
			stride = coarse_stride;
		}
		else
		{
			const float segment_end_dist = min(ray.segment_dist, ray.march_dist);
			const float dt = segment_end_dist - ray.last_segment_dist;
			[branch] if (dt <= 0.0) break;

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
				float3 sun_transmittance = sampleSunTransmittance(ray.pos, info.dirlightDir, seed + ray.step, jitter_frame, cloud_transmittance);
				float3 in_scatter = scatter * sun_transmittance * info.dirlightColor;

				sum_shadowing_weights += dt;
				mean_shadowing += sun_transmittance * dt;

				// HP multiscatter and phi_fwd diffuse field
				float3 ms_lum = 0.0;
				[unroll] for (uint octave = 0; octave < 3; ++octave)
				{
					float att = pow(info.msAttenuation, octave);
					float con = pow(info.msContribution, octave);
					ms_lum += exp(-max(0.0, -log(max(cloud_transmittance, 1e-5))) * att) * con;
				}
				in_scatter += ms_lum * cloud_scatter * cloud_secondary_phase * info.dirlightColor * sun_transmittance;
				float boundary = HPEvaluateBoundaryLight(ray.pos, info.dirlightDir);
				float bottomConfidence = info.phiFwdDepthPow > 0.0 ? 1.0 - exp(-max(ndf.height_fraction + info.phiFwdDepthBias, 0.0) / max(info.bottomSmoothHeight, 0.001) * info.phiFwdDepthPow) : 1.0;
				float phiBuild = 1.0 - exp(-cloud_density * dt * max(info.phiFwdMSBuildScale, 0.0));
				float phiScalar = info.phiFwdIntensity * boundary * bottomConfidence * phiBuild * (1.0 - dot(cloud_transmittance, float3(0.2126, 0.7152, 0.0722)));
				phiScalar = info.phiFwdCompress > 0.0 ? (1.0 - exp(-phiScalar * info.phiFwdCompress)) / info.phiFwdCompress : phiScalar;
				in_scatter += phiScalar * info.dirlightColor;

				// ambient
				float3 ambient = SampleCloudAmbientSkyView(ray.ray_dir);
				float profile_indirect = sqrt(1.0 - saturate(ndf.dimension_profile));
				float vertical_transmittance = dot(TexTransmittance.SampleLevel(TransmittanceSampler, TrLutUvPlanet(ray.pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius), info.dirlightDir), 0).rgb, float3(0.2126, 0.7152, 0.0722));
				float upwardAO = exp(-max(0.0, -log(max(dot(cloud_transmittance, float3(0.2126, 0.7152, 0.0722)), 1e-5))) * max(info.dirlightDir.z, 0.05) * info.aoUpwardScale);
				float vertical_indirect = exp(vertical_transmittance) * upwardAO;
				in_scatter += cloud_scatter * profile_indirect * vertical_indirect * info.ambientTopMultiplier * ambient;

				const float3 sample_transmittance = exp(-dt * extinction);
				const float3 scatter_factor = (1 - sample_transmittance) / max(extinction, 1e-8);
				const float3 scatter_integeral = in_scatter * scatter_factor;

				// update
				ray.lum += scatter_integeral * ray.transmittance;
				ray.transmittance *= sample_transmittance;
			}

			// stride
			const bool cloud_density_sample = cloud_density > 0.003;
			const bool empty_layer_sample = ndf.in_layer && !cloud_density_sample;
			float rcp_step = (empty_layer_sample ? zero_density_stride_mult : 1.0) / (float)info.cloudMaxStep;
			float march_prop = saturate((ray.start_dist + ray.segment_dist) / info.rayMarchRange);

			stride = (pow(sqrt(march_prop) + rcp_step, 2) - march_prop) * info.rayMarchRange;

			const float tr = max(ray.transmittance.x, max(ray.transmittance.y, ray.transmittance.z));
			const float step_opacity = saturate(1.0 - tr);
			scatter_weight += step_opacity * dt;
			weighted_depth += step_opacity * dt * (ray.start_dist + ray.ray_dist);
			ap_dist += tr * dt;
			[branch] if (tr < 1e-3) break;
		}
	}

	if (info.highCloudEnabled > 0.0) {
		uint hiSteps = max(info.cloudMaxStep / 2u, 4u);
		float hiStep = ray.march_dist / (float)hiSteps;
		float hiDist = SelectNubisRayJitter(ray.start_dist, jitter) * hiStep;
		[loop] for (uint hi = 0; hi < hiSteps && hiDist < ray.march_dist; ++hi, hiDist += hiStep)
		{
			float hiAbsDist = ray.start_dist + hiDist;
			float3 hiPos = ray.start_pos + hiDist * ray.ray_dir;
			float hiNormH;
			float hiDensity = EvaluateHighCloudDensity(hiPos, hiNormH);
			if (hiDensity <= 0.001)
				continue;
			float3 hiExtinction = hiDensity * info.highViewAbsorption * info.scatterTint;
			float3 hiTransmittance = exp(-hiExtinction * hiStep);
			float hiCos = dot(ray.ray_dir, info.dirlightDir);
			float hiPhase = Phase::HG(hiCos, info.highForwardEccentricity) + Phase::HG(hiCos, -info.highBackwardEccentricity);
			float3 hiCloudTr;
			float3 hiSun = sampleSunTransmittance(hiPos, info.dirlightDir, seed + hi + 163u, jitter_frame, hiCloudTr);
			float3 hiAmbient = SampleCloudAmbientSkyView(ray.ray_dir) * lerp(info.highAmbientBottomMultiplier, info.highAmbientTopMultiplier, hiNormH);
			float3 hiSkyBlend = TexSkyView.SampleLevel(SkyViewSampler, SkyViewLutUv(ray.ray_dir), 0).rgb;
			float3 hiLum = hiSun * info.dirlightColor * hiPhase + hiAmbient;
			hiLum = lerp(hiLum, hiSkyBlend, smoothstep(0.0, 1.0, hiNormH) * info.highSkyBlendStrength);
			float3 hiIntegral = hiLum * (1.0 - hiTransmittance) / max(hiExtinction, 1e-8);
			ray.lum += hiIntegral * ray.transmittance;
			ray.transmittance *= hiTransmittance;
			const float tr = max(ray.transmittance.x, max(ray.transmittance.y, ray.transmittance.z));
			float stepOpacity = saturate(1.0 - tr);
			scatter_weight += stepOpacity * hiStep;
			weighted_depth += stepOpacity * hiStep * hiAbsDist;
			if (tr < 1e-3)
				break;
		}
	}

	mean_shadowing = sum_shadowing_weights > 1e-8 ? mean_shadowing / sum_shadowing_weights : 1.0;

	uint3 ap_dims;
	TexAerialPerspective.GetDimensions(ap_dims.x, ap_dims.y, ap_dims.z);
	float2 ap_uv = SkyViewLutUv(ray.ray_dir);
	const float vol_depth_slice = lerp(.5 / ap_dims.z, 1 - .5 / ap_dims.z, saturate(ap_dist / info.aerialPerspectiveMaxDist));
	float4 vol_ap_sample = TexAerialPerspective.SampleLevel(SkyViewSampler, float3(ap_uv, vol_depth_slice), 0);

	const float ap_direct_visibility = 1.0 - saturate(ap_shadow);
	const float vol_ap_direct_visibility = saturate(dot(mean_shadowing, float3(0.2126, 0.7152, 0.0722))) * ap_direct_visibility;
	const float3 ap_multi_scatter = SampleApMultiScatter();
	vol_ap_sample.rgb *= GetApShadowedMultiScatterVisibility(1.0 - vol_ap_direct_visibility, ap_multi_scatter);

	vol_ap_sample = ApplyAerialPerspectiveSettings(vol_ap_sample);

	ray.transmittance *= vol_ap_sample.a;
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

float ReconstructSolidDist(uint2 full_px_coords, VolumetricCloudData info, out bool is_sky)
{
	const float depth = TexDepth[full_px_coords.xy];
	is_sky = depth > 1 - 1e-6;
	if (is_sky)
		return info.rayMarchRange;

	const float2 texture_uv = (full_px_coords + 0.5) * info.rcpFrameDim;
	const float2 logic_uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(texture_uv);

	float4 pos_world = float4(2 * float2(logic_uv.x, -logic_uv.y + 1) - 1, depth, 1);
	pos_world = mul(FrameBuffer::CameraViewProjInverse, pos_world);
	pos_world.xyz = pos_world.xyz / pos_world.w;
	return length(pos_world.xyz);
}

float ConservativeTileSolidDist(uint2 low_px_coords, VolumetricCloudData info, out bool is_sky)
{
	is_sky = false;
	float solid_dist = 0.0;

	const uint2 tile_origin = low_px_coords * 4u;
	const uint2 frame_dim = uint2(info.frameDim);

	[unroll] for (uint y = 0u; y < 4u; y++)
	{
		[unroll] for (uint x = 0u; x < 4u; x++)
		{
			bool sample_is_sky;
			const uint2 full_px_coords = min(tile_origin + uint2(x, y), frame_dim - 1u);
			const float sample_solid_dist = ReconstructSolidDist(full_px_coords, info, sample_is_sky);
			is_sky = is_sky || sample_is_sky;
			solid_dist = max(solid_dist, sample_solid_dist);
		}
	}

	return is_sky ? info.rayMarchRange : solid_dist;
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

	const uint3 seed = Random::pcg3d(uint3(full_px_coords.xy, full_px_coords.x ^ 0xf874));
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

	bool cloud_is_sky = is_sky;
	float cloud_solid_dist = solid_dist;
	if (!full_resolution)
		cloud_solid_dist = ConservativeTileSolidDist(px_coords, info, cloud_is_sky);

	const float ap_shadow = SampleFilteredApShadow(full_px_coords);
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, cloud_solid_dist, cloud_is_sky, seed, ray_jitter, SharedData::FrameCountAlwaysActive, ap_shadow, true);

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
	const float2 ray_jitter = NubisRayJitter(tid.xy + uint2(tid.z * dims.x, tid.z * dims.y), SharedData::FrameCountAlwaysActive);

	const float3 eye_pos = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float3 ray_dir = GetCubemapSamplingVector(tid, RWTexCubeTr);
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, info.rayMarchRange, true, seed, ray_jitter, SharedData::FrameCountAlwaysActive, 0.0, false);

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
	const float reprojection_depth = depth;

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
