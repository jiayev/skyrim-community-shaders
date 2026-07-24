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

static const float GAME_UNITS_PER_METER = 1.0 / GAME_UNIT_TO_M;
static const float MIN_CLOUD_DEPTH_TOLERANCE_KM = 0.001;

float EncodeCloudDepth(float gameUnitDepth)
{
	return gameUnitDepth * GAME_UNIT_TO_M * 0.001;
}

float DecodeCloudDepth(float encodedDepth)
{
	return encodedDepth * 1000.0 * GAME_UNITS_PER_METER;
}

SamplerState TileableSampler : register(s2);
#define TransmittanceSampler SampTr
#define SkyViewSampler SampSv

struct CloudLayer
{
	float lowestAltitude;
	float highestAltitude;
	float3 scatter;
	float3 absorption;
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
	float bottomZ;
	float planetRadius;
	float2 activeFrameDim;

	float lowestCloudAltitude;
	float highestCloudAltitude;
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
	uint _padPrimarySteps;
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
	float elapsedTimeSeconds;
	float temporalAccumulationFactor;
	float cloudHistoryInvalidation;
	uint ghostingReduction;
	uint3 padding;
};

CloudLayer GetCloudLayer(VolumetricCloudData info)
{
	CloudLayer cloud;
	cloud.lowestAltitude = info.lowestCloudAltitude;
	cloud.highestAltitude = info.highestCloudAltitude;
	// View extinction is scalar; the tint is applied only to light-path extinction.
	// The coefficient is authored per metre while march distances are game units.
	cloud.scatter = GAME_UNIT_TO_M;
	cloud.absorption = 0.0;
	return cloud;
}

float GetCloudAltitudeRange(CloudLayer cloud)
{
	return max(cloud.highestAltitude - cloud.lowestAltitude, GAME_UNITS_PER_METER);
}

StructuredBuffer<VolumetricCloudData> VolumetricCloudBuffer : register(t0);
Texture2D<float4> TexTransmittance : register(t1);
Texture2D<float4> TexMultiScatter : register(t2);
Texture3D<float4> TexAerialPerspective : register(t3);

Texture2D<float> TexDepth : register(t4);

Texture3D<unorm float4> TexBaseShapeNoise : register(t5);
Texture3D<unorm float4> TexDetailErosionNoise : register(t6);
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
Texture2D<float4> TexShadowVolume : register(t23);
Texture2D<float4> TexVolHistoryTr : register(t26);
Texture2D<float3> TexVolHistoryLum : register(t27);
Texture2D<float4> TexVolHistoryAux : register(t28);
Texture2D<float4> TexVolLowTr : register(t29);
Texture2D<float3> TexVolLowLum : register(t30);
Texture2D<float4> TexVolLowAux : register(t31);
Texture2D<float4> TexVolUpscaleTr : register(t32);
Texture2D<float3> TexVolUpscaleLum : register(t33);
Texture2D<float4> TexVolUpscaleAux : register(t34);
Texture2D<float4> TexShadowFilterInput : register(t35);

float3 GetSceneDirectionalLightColor()
{
	const float linearLightingMultiplier =
		SharedData::linearLightingSettings.enableLinearLighting &&
				!SharedData::linearLightingSettings.isDirLightLinear &&
				!SharedData::InInterior ?
			SharedData::linearLightingSettings.dirLightMult :
			1.0;
	return Color::Light(
			   SharedData::DirLightColor.xyz / max(linearLightingMultiplier, 1e-5),
			   SharedData::linearLightingSettings.isDirLightLinear) *
	       linearLightingMultiplier;
}

RWTexture2D<float4> RWTexTr : register(u0);
RWTexture2D<float3> RWTexLum : register(u1);
RWTexture2D<float4> RWTexAux : register(u2);

RWTexture2D<float4> RWShadowVolume : register(u0);

RWTexture2DArray<float3> RWTexCubeTr : register(u0);
RWTexture2DArray<float3> RWTexCubeLum : register(u1);
RWTexture2D<sh2> RWCloudAmbientSH : register(u0);

float CloudAlphaFromTransmittance(float3 transmittance)
{
	const float tr = max(transmittance.x, max(transmittance.y, transmittance.z));
	return 1.0 - saturate((tr - 0.1) * 1.1111111111);
}

float RayIntersectSphereCentered(float3 orig, float3 dir, float r)
{
	return RayIntersectSphere(orig, dir, 0, r);
}

float3 SampleCloudAmbientSkyView(float3 viewDir)
{
	const float3 shViewDir = float3(viewDir.x, viewDir.z, viewDir.y);
	// The cloud ambient probe is sky radiance pre-convolved with the cloud phase,
	// not diffuse irradiance. Match the anisotropy used by the reference pipeline.
	const sh2 phase = SphericalHarmonics::EvaluatePhaseHG(shViewDir, 0.7);

	const float r = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(0, 0)], phase);
	const float g = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(1, 0)], phase);
	const float b = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(2, 0)], phase);
	return max(0.0, float3(r, g, b));
}

float3 SampleCloudApMultiScatter()
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	float3 multiScatter = TexMultiScatter.SampleLevel(SkyViewSampler, TrLutUv(data.zCameraPlanet, data.sunDir.z), 0).rgb * data.sunlightColor;
	multiScatter += TexMultiScatter.SampleLevel(SkyViewSampler, TrLutUv(data.zCameraPlanet, data.masserDir.z), 0).rgb * data.masserColor;
	multiScatter += TexMultiScatter.SampleLevel(SkyViewSampler, TrLutUv(data.zCameraPlanet, data.secundaDir.z), 0).rgb * data.secundaColor;
	return multiScatter;
}

float SampleCloudApShadow(uint2 fullPixelCoord)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	uint2 apDims;
	TexApShadow.GetDimensions(apDims.x, apDims.y);
	if (any(apDims == 0u))
		return 0.0;
	const uint2 apCoord = min(data.halfResApShadow ? fullPixelCoord / 2u : fullPixelCoord, apDims - 1u);
	return TexApShadow[apCoord];
}

float4 SampleCloudAerialPerspective(float3 viewDir, float distance, float shadow)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	uint3 apDims;
	TexAerialPerspective.GetDimensions(apDims.x, apDims.y, apDims.z);
	if (any(apDims == 0u))
		return float4(0.0, 0.0, 0.0, 1.0);
	const float depthSlice = lerp(0.5 / apDims.z, 1.0 - 0.5 / apDims.z, saturate(distance / AP_MAX_DIST));
	float4 ap = TexAerialPerspective.SampleLevel(SkyViewSampler, float3(SkyViewLutUv(viewDir), depthSlice), 0);
	ap.rgb *= 1 - shadow;
	ap.rgb *= data.apLumMix;
	ap.a = lerp(1.0, ap.a, data.apTrMix);
	return ap;
}

groupshared sh2 gCloudAmbientSHR[256];
groupshared sh2 gCloudAmbientSHG[256];
groupshared sh2 gCloudAmbientSHB[256];

[numthreads(16, 16, 1)] void buildCloudAmbientSH(uint3 tid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 sampleCoord = (float2(tid.xy) + 0.5) / 16.0;
	const float3 shSampleDir = SphericalHarmonics::GetUniformSphereSample(sampleCoord.x, sampleCoord.y);
	const float3 rayDir = float3(shSampleDir.x, shSampleDir.z, shSampleDir.y);
	const float3 cloudCenterPlanet = float3(0.0, 0.0, info.planetRadius + 0.5 * (info.lowestCloudAltitude + info.highestCloudAltitude));
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

	ray.transmittance = 1;
	ray.lum = 0;
}

float2 IntersectSpherePair(float3 origin, float3 dir, float radius)
{
	const float b = dot(origin, dir);
	const float discriminant = b * b - dot(origin, origin) + radius * radius;
	if (discriminant < 0.0)
		return -1.0;
	const float root = sqrt(discriminant);
	return float2(-b - root, -b + root);
}

bool snapCloudShell(inout RayMarchInfo ray, CloudLayer cloud, VolumetricCloudData info, float max_dist)
{
	const float3 origin_planet = ray.eye_pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float radial_distance = length(origin_planet);
	const float cos_chi = dot(origin_planet, ray.ray_dir) / max(radial_distance, 1e-5);
	const float2 inner = IntersectSpherePair(origin_planet, ray.ray_dir, info.planetRadius + cloud.lowestAltitude);
	const float2 outer = IntersectSpherePair(origin_planet, ray.ray_dir, info.planetRadius + cloud.highestAltitude);
	if (outer.y < 0.0)
		return false;

	float entry;
	float exit;
	if (inner.x < 0.0 && inner.y >= 0.0) {
		entry = inner.y;
		exit = outer.y;
		const float horizon_cos = -sqrt(saturate(1.0 - info.planetRadius * info.planetRadius / max(radial_distance * radial_distance, 1.0)));
		if (cos_chi < horizon_cos)
			return false;
	} else {
		entry = max(outer.x, 0.0);
		exit = inner.x >= 0.0 ? inner.x : outer.y;
	}

	ray.start_dist = clamp(entry, 0.0, max_dist);
	ray.end_dist = clamp(exit, ray.start_dist, max_dist);
	ray.march_dist = ray.end_dist - ray.start_dist;
	ray.start_pos = ray.eye_pos + ray.ray_dir * ray.start_dist;
	ray.end_pos = ray.eye_pos + ray.ray_dir * ray.end_dist;
	return ray.march_dist > 0.0;
}

float CloudLightExitDistance(float3 pos, float3 dir, CloudLayer cloud, VolumetricCloudData info)
{
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float2 outer = IntersectSpherePair(pos_planet, dir, info.planetRadius + cloud.highestAltitude);
	return max(outer.y, 0.0);
}

float CloudRayJitter(uint2 pixelCoord, uint sampleIndex)
{
	// Spatial samples remain hash-white, while the sixteen temporal samples are
	// stratified and permuted per pixel. This avoids large frame-to-frame gaps in
	// ray start distance without changing the eventual sequence interface.
	const uint pixelHash = Random::pcg3d(uint3(pixelCoord, 0x9e3779b9u)).x;
	// Advance by one stratum per traced update. The per-pixel phase distributes the
	// single wrap in the sixteen-sample cycle spatially instead of making the whole
	// image take a large ray-start jump on the same frame.
	const uint stratum = (sampleIndex + (pixelHash & 15u)) & 15u;
	const uint sampleHash = Random::pcg3d(uint3(pixelCoord, sampleIndex ^ 0x68bc21ebu)).y;
	const float withinStratum = float(sampleHash >> 8u) * (1.0 / 16777216.0);
	return (float(stratum) + withinStratum) * (1.0 / 16.0);
}

float HPPositivePow(float x, float p)
{
	return pow(max(x, 0.0), p);
}

float HPDensityRemap(float x, float a, float b, float c, float d)
{
	return (((x - a) / max(b - a, 1e-5)) * (d - c)) + c;
}

float CloudPowderEffect(float cloudDensity, float cosAngle, float intensity)
{
	float powder = saturate((1.0 - exp(-cloudDensity * 4.0)) * 2.0);
	return lerp(1.0, lerp(1.0, powder, smoothstep(0.5, -0.5, cosAngle)), intensity);
}

float2 HPWeatherUV(float2 worldXY, VolumetricCloudData info)
{
	return (worldXY - info.weatherCenter) / max(info.weatherWorldSize, 1.0) + 0.5;
}

float HPLowTypeValue(float cloudType, float cu, float tcu, float cb)
{
	return cloudType < 0.5 ? lerp(cu, tcu, cloudType * 2.0) : lerp(tcu, cb, (cloudType - 0.5) * 2.0);
}

float HPLowTopDevelopment(float cloudType)
{
	// Coverage mainly develops deep convection. Shallow and towering cumulus
	// retain their species profile instead of inheriting the full-column stretch.
	return HPLowTypeValue(cloudType, 0.08, 0.42, 1.0);
}

float HPEvaluateTopHeightProxy(float2 worldXY)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float2 uv = HPWeatherUV(worldXY, info);
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	float4 weather = TexHpLowWeather.SampleLevel(TransmittanceSampler, uv, 0);
	float coverageHeight = saturate(HPPositivePow(weather.r, max(info.coverageHeightContrast, 0.001)) * info.coverageHeightIntensity);
	float development = HPLowTopDevelopment(weather.g) * (1.0 - saturate(info.scWorleyStrength * weather.b));
	float topScale = lerp(1.0, max(info.coverTopMax, 1.0), HPPositivePow(coverageHeight, max(info.coverTopCurvePow, 0.01)) * info.coverTopStrength * development);
	return saturate(coverageHeight * topScale / max(info.coverTopMax, 1.0));
}

float HPEvaluateBoundaryLight(float3 pos, float3 sunDir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float sampleStep = clamp(info.weatherWorldSize * 0.001, 25.0 * GAME_UNITS_PER_METER, 200.0 * GAME_UNITS_PER_METER);
	float hL = HPEvaluateTopHeightProxy(pos.xy - float2(sampleStep, 0.0));
	float hR = HPEvaluateTopHeightProxy(pos.xy + float2(sampleStep, 0.0));
	float hD = HPEvaluateTopHeightProxy(pos.xy - float2(0.0, sampleStep));
	float hU = HPEvaluateTopHeightProxy(pos.xy + float2(0.0, sampleStep));
	const float cloudAltitudeRange = max(info.highestCloudAltitude - info.lowestCloudAltitude, GAME_UNITS_PER_METER);
	float dHdx = (hR - hL) * cloudAltitudeRange / max(2.0 * sampleStep, 1.0);
	float dHdy = (hU - hD) * cloudAltitudeRange / max(2.0 * sampleStep, 1.0);
	float3 topNormal = normalize(float3(-dHdx, -dHdy, 1.0));
	float wrap = 0.5;
	float lit = saturate((dot(topNormal, sunDir) + wrap) / (1.0 + wrap));
	return lerp(1.0, lit, saturate(info.phiFwdBoundaryConfidence));
}

struct NDFInfo
{
	bool in_layer;
	float height_fraction;
	float local_height;
};

void initNDFInfo(out NDFInfo ndf)
{
	ndf.in_layer = false;
	ndf.height_fraction = 0.0;
	ndf.local_height = 0.0;
}

NDFInfo sampleNDF(float3 pos, CloudLayer cloud)
{
	NDFInfo ndf;
	initNDFInfo(ndf);

	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float planet_z = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planet_z < cloud.lowestAltitude || planet_z > cloud.highestAltitude)
		return ndf;

	ndf.in_layer = true;
	ndf.height_fraction = saturate((planet_z - cloud.lowestAltitude) / GetCloudAltitudeRange(cloud));

	return ndf;
}

float sampleCloudDensity(
	float3 pos, float eye_dist, CloudLayer cloud, float mip_level, bool is_expensive,
	out NDFInfo ndf)
{
	ndf = sampleNDF(pos, cloud);
	if (!ndf.in_layer)
		return 0;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 weatherUV = HPWeatherUV(pos.xy, info);
	if (any(weatherUV < 0.0) || any(weatherUV > 1.0))
		return 0.0;
	float4 weather = TexHpLowWeather.SampleLevel(TransmittanceSampler, weatherUV, 0);
	float coverageRaw = weather.r;
	float cloudType = weather.g;
	float scMask = weather.b;
	float coverage = saturate(HPPositivePow(coverageRaw, max(info.coverageCoverContrast, 0.001)) * info.coverageCoverIntensity);
	float coverageHeight = saturate(HPPositivePow(coverageRaw, max(info.coverageHeightContrast, 0.001)) * info.coverageHeightIntensity);
	float scStr = info.scWorleyStrength * scMask;
	float scCell = saturate(TexHpScCell.SampleLevel(TileableSampler, weatherUV * info.scCellScale, 0).r * info.scCellNoiseStrength);
	float scCoverage = saturate(HPPositivePow(coverageRaw, max(info.scCoverageContrast, 0.001)) * info.scCoverageIntensity);
	coverage = lerp(coverage, scCoverage * scCell, scStr);
	if (coverage < 0.1)
		return 0.0;
	const float4 highWeather = TexHpHighWeather.SampleLevel(TransmittanceSampler, weatherUV, 0);
	const float edgeSoftness = info.highDensitySoftAIntensity *
	                           (1.0 - HPPositivePow(saturate(highWeather.a), max(info.highDensitySoftAContrast, 0.01)));

	float topDevelopment = HPLowTopDevelopment(cloudType);
	float topScale = lerp(1.0, max(info.coverTopMax, 1.0), HPPositivePow(coverageHeight, max(info.coverTopCurvePow, 0.01)) * info.coverTopStrength * topDevelopment);
	float heightForLut = ndf.height_fraction / (1.0 + (topScale - 1.0) * ndf.height_fraction);
	float localHeight = lerp(heightForLut, saturate(ndf.height_fraction / max(info.scHeightScale, 0.01)), scStr);
	ndf.local_height = localHeight;
	float radialDist = saturate(length(weatherUV - 0.5) * 2.0);
	float3 profiles = TexHpProfile.SampleLevel(TransmittanceSampler, float2(localHeight, radialDist), 0).rgb;
	float heightGradient = lerp(HPLowTypeValue(cloudType, profiles.r, profiles.g, profiles.b), profiles.r, scStr);
	// Noise parameters use X/vertical/Y ordering. Convert the game's Z-up position
	// and horizontal wind displacement before applying the authored frequencies.
	float3 noisePosition = float3(pos.x, pos.z, pos.y);
	float3 windOffset = float3(info.noiseWindOffset.x, 0.0, info.noiseWindOffset.y) * info.noiseScale * info.baseNoiseWindSpeed;
	float4 baseNoise = TexBaseShapeNoise.SampleLevel(TileableSampler, noisePosition * info.noiseScale + info.noiseOffset + windOffset, max(mip_level, 0.0));
	float baseShape = pow(abs(baseNoise.r), 0.6);
	float bottomNoiseFade = info.bottomSmoothHeight > 0.0 ? HPPositivePow(saturate(localHeight / info.bottomSmoothHeight), max(info.bottomSmoothPow, 0.01)) : 1.0;
	baseShape = lerp(1.0, baseShape, bottomNoiseFade);
	float billowy = 0.0;
	float wispy = 0.0;
	if (is_expensive) {
		float3 detailWind = float3(info.noiseWindOffset.x, 0.0, info.noiseWindOffset.y) * info.detailNoiseScale * info.detailNoiseWindSpeed;
		detailWind.y += info.elapsedTimeSeconds * info.detailNoiseVerticalWindSpeed;
		const float eyeDistanceMeters = max(eye_dist, 0.0) * GAME_UNIT_TO_M;
		const float erosionMip = 4.0 * saturate((eyeDistanceMeters - 3000.0) / (100000.0 - 3000.0));
		float4 d = TexDetailErosionNoise.SampleLevel(TileableSampler, float3(pos.x, -pos.z, pos.y) * info.detailNoiseScale + info.noiseOffset * 0.5 + detailWind, erosionMip);
		billowy = (d.b * info.billowyLow + d.a * info.billowyHigh) * bottomNoiseFade;
		wispy = (d.r * info.wispyLow + d.g * info.wispyHigh) * bottomNoiseFade;
	}
	float detailStrength = lerp(HPLowTypeValue(cloudType, info.detailStrengthCu, info.detailStrengthTcu, info.detailStrengthCb), info.scDetailStrength, scStr);
	float threshold = (1.0 - coverage) + info.densityThreshold;
	float erodedBillowy = saturate(HPDensityRemap(baseShape, billowy * detailStrength, 1.0, 0.0, 1.0)) * heightGradient;
	float erodedWispy = saturate(HPDensityRemap(baseShape, wispy * detailStrength, 1.0, 0.0, 1.0)) * heightGradient;
	float densityBillowy = saturate(HPDensityRemap(erodedBillowy, threshold, threshold + max(edgeSoftness, 0.001), 0.0, 1.0));
	// Reach may lower the wispy threshold, but it must not make an eroded value of
	// zero dense again. The former negative threshold produced solid full-height
	// columns at high coverage and made both base and detail noise appear ineffective.
	const float wispyThreshold = max(threshold - info.wispyReach, 0.0);
	float densityWispy = saturate(HPDensityRemap(erodedWispy, wispyThreshold, wispyThreshold + max(edgeSoftness, 0.001), 0.0, 1.0));
	float wispyT = saturate((ndf.height_fraction - info.wispyTopHeight) / max(1.0 - info.wispyTopHeight, 0.001));
	densityWispy *= HPPositivePow(1.0 - wispyT, max(info.wispyTopHardness * 10.0, 0.01));
	float scCellShaped = HPPositivePow(max(scCell, 0.001), max(info.scCellThickPow, 0.01));
	float heightClip = lerp(1.0, lerp(1.0, scCellShaped, info.scCellThickStrength), scStr);
	float density = lerp(densityWispy, densityBillowy, smoothstep(0.0, info.wispyEdgeWidth, densityBillowy)) * heightClip;
	density *= HPLowTypeValue(cloudType, info.densityMultiplierCu, info.densityMultiplierTcu, info.densityMultiplierCb);
	density *= 1.0 - saturate(info.highDensityModAIntensity *
							  (1.0 - HPPositivePow(saturate(coverage), max(info.highDensityModAContrast, 0.01))));
	return max(0.0, density * info.densityMultiplier);
}

float3 sampleExternalSunTransmittance(float3 pos, float3 sun_dir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float3 shadow = 1.0;
	const float3 pos_world = pos + float3(0, 0, info.bottomZ);
	const float3 pos_world_relative = pos_world - FrameBuffer::CameraPosAdjust.xyz;
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);

	[branch] if (RayIntersectSphereCentered(pos_planet, sun_dir, info.planetRadius) > 0.0) return 0.0;

	DirectionalShadowLightData directionalShadowLightData = DirectionalShadowLights[0];
	const float shadow_depth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(pos_world_relative));
	[branch] if (directionalShadowLightData.EndSplitDistances.y > 0.0 &&
				 shadow_depth < directionalShadowLightData.EndSplitDistances.y)
	{
		const float cascade_select = saturate(
			(shadow_depth - directionalShadowLightData.StartSplitDistances.y) /
			(directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));
		const uint cascade_index = uint(cascade_select);
		const float3 positionLS = mul(directionalShadowLightData.ShadowProj[cascade_index], float4(pos_world, 1)).xyz;
		const float4 depths = TexDirectShadows.GatherRed(TransmittanceSampler, float3(saturate(positionLS.xy), cascade_index), 0);
		shadow *= dot(float4(depths > positionLS.z), 0.25);
	}
	[branch] if (all(shadow < 1e-8)) return 0.0;

	shadow *= TerrainShadows::GetTerrainShadow(pos_world, TransmittanceSampler);
	[branch] if (all(shadow < 1e-8)) return 0.0;

	shadow *= TexTransmittance.SampleLevel(TransmittanceSampler, TrLutUvPlanet(pos_planet, sun_dir), 0).rgb;
	return shadow;
}

// Evaluate only the cloud column here. External directional, terrain, and
// atmospheric attenuation is sampled at the ray endpoints and interpolated in
// RenderVolumetricCloudRay, matching the reference environment-lighting setup.
void sampleCloudSelfShadow(
	float3 pos, float local_height, float3 sun_dir,
	out float light_extinction_od, out float phi_fwd)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	light_extinction_od = 0.0;
	phi_fwd = 0.0;

	// cloud self-shadowing
	{
		uint visibility_step = max(info.lightSteps, 1u);
		const static float cone_ratio = 2.0;
		const static float cone_min_step = 5.0 * GAME_UNITS_PER_METER;
		const static float cone_max_distance = 6000.0 * GAME_UNITS_PER_METER;
		float kappa_od_sum = 0.0;
		float diffuse_survival = 1.0;
		float cover_dist = min(CloudLightExitDistance(pos, sun_dir, cloud, info), cone_max_distance);
		float step_width = max(cover_dist * (cone_ratio - 1.0) / max(pow(cone_ratio, (float)visibility_step) - 1.0, 1e-4), cone_min_step);
		float cum_dist = 0.0;
		const float column_thickness = HPEvaluateTopHeightProxy(pos.xy);
		const float bottom_soft_height = max(info.bottomSmoothHeight * lerp(1.0, 4.0, column_thickness), 0.001);
		const float bottom_height = local_height + info.phiFwdDepthBias;
		const float bottom_confidence = info.phiFwdDepthPow > 0.0 ?
		                                    1.0 - exp(-max(bottom_height, 0.0) / bottom_soft_height * info.phiFwdDepthPow) :
		                                    1.0;
		const float source_confidence = HPEvaluateBoundaryLight(pos, sun_dir) * bottom_confidence;

		for (uint i = 0; i < visibility_step; i++) {
			float width = min(step_width, cover_dist - cum_dist);
			if (width <= 0.0)
				break;
			float dist = cum_dist + width * 0.5;
			float3 vis_pos = pos + sun_dir * dist;
			NDFInfo _;
			const float mip_offset = (float)i / max((float)(visibility_step - 1u), 1.0) * 3.0;
			const float density = sampleCloudDensity(vis_pos, 0.0, cloud, mip_offset, true, _);
			const float width_m = width * GAME_UNIT_TO_M;
			const float dist_m = dist * GAME_UNIT_TO_M;
			const float sigma_t_per_game_unit = density * dot(cloud.scatter + cloud.absorption, float3(0.2126, 0.7152, 0.0722));
			const float sigma_t_per_meter = sigma_t_per_game_unit * GAME_UNITS_PER_METER;
			const float local_od = sigma_t_per_meter * width_m;
			const float q_source = sigma_t_per_meter * 0.999 * width_m;
			const float kappa_step = local_od * sqrt(3.0 * (1.0 - 0.999));
			const float kappa_to_center = kappa_od_sum + kappa_step * 0.5;
			const float ms_build = 1.0 - exp(-(light_extinction_od + local_od * 0.5) * max(info.phiFwdMSBuildScale, 0.0));
			const float inv_r = 1.0 / max(dist_m, width_m * 0.5);
			phi_fwd += diffuse_survival * q_source * sigma_t_per_meter * source_confidence * ms_build * exp(-kappa_to_center) * inv_r;
			light_extinction_od += local_od;
			kappa_od_sum += kappa_step;
			diffuse_survival *= exp(-local_od * (1.0 - 0.999));
			cum_dist += width;
			step_width *= cone_ratio;
		}
	}
}

float EvaluateHighCloudDensity(float3 pos, out float normalizedHeight)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	normalizedHeight = 0.0;
	if (info.highCloudEnabled <= 0.0)
		return 0.0;
	float planetZ = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planetZ < info.lowestCloudAltitude || planetZ > info.highestCloudAltitude)
		return 0.0;
	normalizedHeight = saturate((planetZ - info.lowestCloudAltitude) /
								max(info.highestCloudAltitude - info.lowestCloudAltitude, GAME_UNITS_PER_METER));
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
	float2 warp = (TexHpHighWarp.SampleLevel(TileableSampler, warpUV, 0).rg * 2.0 - 1.0) * info.highCellWarpStrength;
	float hiCell = saturate(TexHpHighCell.SampleLevel(TileableSampler, cellUV + warp, 0).r);
	float hiCellShaped = HPPositivePow(max(hiCell, 0.001), max(info.highCellThickPow, 0.01));
	float hiCellThick = lerp(info.highAsCellThickStrength, info.highCellThickStrength, hiType);
	float hiCoverForHeight = HPPositivePow(hiCoverage, max(info.highHeightCurvePow, 0.01));
	float hiDrivenTop = lerp(info.highCloudBottom, info.highCloudTop, hiCoverForHeight);
	float hiTop = info.highCloudBottom + (hiDrivenTop - info.highCloudBottom) * lerp(1.0, hiCellShaped, hiCellThick * 0.5);
	float hiBottom = info.highCloudBottom - (info.highCloudTop - info.highCloudBottom) * info.highBottomCoverageScale * hiCoverForHeight;
	float distXY = length((pos - FrameBuffer::CameraPosAdjust.xyz).xy);
	float horizonT = smoothstep(info.highHorizonDistanceStart, max(info.highHorizonDistanceStart + GAME_UNITS_PER_METER, info.highHorizonDistanceEnd), distXY);
	hiTop -= horizonT * hiBottom;
	hiBottom -= horizonT * hiBottom;
	float band = smoothstep(hiBottom - info.highCloudSoftness, hiBottom + info.highCloudSoftness, normalizedHeight) * (1.0 - smoothstep(hiTop - info.highCloudSoftness, hiTop + info.highCloudSoftness, normalizedHeight));
	float wisp = TexHpHighWisp.SampleLevel(TileableSampler, uv * info.highWispScale + hiWindUV * info.highCellWindSpeed, 0).r;
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
};

VolumetricCloudResult RenderVolumetricCloudRay(float3 ray_dir, float3 eye_pos, float solid_dist, bool is_sky, float jitter, float ap_shadow)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);
	const float3 dirlightColor = GetSceneDirectionalLightColor();
	const float3 cloud_light_dir = info.dirlightDir;
	RayMarchInfo ray;
	initRayMarchInfo(ray);

	ray.eye_pos = eye_pos;
	ray.ray_dir = ray_dir;
	const float max_march_dist = is_sky ? info.rayMarchRange : min(info.rayMarchRange, solid_dist);
	const bool intersects_clouds = snapCloudShell(ray, cloud, info, max_march_dist);

	[branch] if (!intersects_clouds || ray.march_dist <= 0.0)
	{
		const float fallback_depth = is_sky ? info.rayMarchRange : min(solid_dist, info.rayMarchRange);
		VolumetricCloudResult result;
		result.transmittance = 1.0;
		result.lum = 0.0;
		result.cloud_depth = fallback_depth;
		result.reject_depth = fallback_depth;
		result.scatter_weight = 0.0;
		return result;
	}

	///////////// precalc
	const float cos_theta = dot(ray.ray_dir, cloud_light_dir);
	const float3 cloud_phase = float3(
		Phase::HG(cos_theta, info.forwardEccentricity) + Phase::HG(cos_theta, -info.backwardEccentricity),
		Phase::HG(cos_theta, info.forwardEccentricity * info.msEccentricity) + Phase::HG(cos_theta, -info.backwardEccentricity * info.msEccentricity),
		Phase::HG(cos_theta, info.forwardEccentricity * info.msEccentricity * info.msEccentricity) + Phase::HG(cos_theta, -info.backwardEccentricity * info.msEccentricity * info.msEccentricity));

	///////////// ray march
	float low_mean_weight = 0.0;
	float low_mean_depth = 0.0;
	bool low_valid = false;
	// These directions and multipliers are constant for the entire ray. HP prepares
	// its environment lighting before marching; sampling them inside every dense
	// step needlessly multiplied the ambient texture cost.
	const float3 ambient_sky_top = SampleCloudAmbientSkyView(float3(0, 0, 1));
	const float3 ambient_sky_bottom = SampleCloudAmbientSkyView(float3(0, 0, -1));
	const float3 ambient_top = ambient_sky_top * info.ambientTopMultiplier;
	const float3 ambient_bottom = ambient_sky_bottom * info.ambientBottomMultiplier;
	// HP/HDRP evaluate environment lighting once for the ray endpoints. Keep that
	// evaluation lazy so rays that only cross empty weather regions pay nothing.
	bool external_sun_ready = false;
	float3 external_sun_start = 1.0;
	float3 external_sun_end = 1.0;

	// HPTraceVolumetricRay: distance-adaptive coarse probing followed by a quarter-size
	// integration step after a density hit.  This is deliberately distance driven rather
	// than a fixed step budget so near, thin clouds cannot be skipped wholesale.
	const float step_large_raw = ray.march_dist / max((float)info.cloudMaxStep, 1.0);
	const float cloudAltitudeRange = GetCloudAltitudeRange(cloud);
	const float step_large_near_cap = cloudAltitudeRange * 0.0625;
	const float step_large_far_cap = cloudAltitudeRange * 0.5;
	float dist = jitter * step_large_near_cap;
	const uint max_iterations = info.cloudMaxStep * 4u;
	[loop] for (uint iteration = 0u; iteration < max_iterations && dist < ray.march_dist; ++iteration)
	{
		const float dist_norm = saturate(dist / max(info.rayMarchRange, 1.0));
		const float absolute_dist = ray.start_dist + dist;
		const float view_cap = max(absolute_dist * 0.125, 1.0);
		const float slab_cap = lerp(step_large_near_cap, step_large_far_cap, dist_norm * dist_norm);
		const float step_large = min(step_large_raw, min(view_cap, slab_cap));
		const float step_small = step_large * 0.25;
		const float3 pos = ray.start_pos + dist * ray.ray_dir;

		// Probe without erosion first. Once the probe enters a cloud, retain the
		// fine step even when the full erosion sample removes density at this point.
		NDFInfo ndf;
		const float simple_density = sampleCloudDensity(pos, absolute_dist, cloud, 1.0, false, ndf);
		[branch] if (simple_density > 0.001)
		{
			const float cloud_density = sampleCloudDensity(pos, absolute_dist, cloud, 0.0, true, ndf);
			const float3 extinction = cloud_density * (cloud.scatter + cloud.absorption);

			// scattering
			[branch] if (max(extinction.x, max(extinction.y, extinction.z)) > 1e-7)
			{
				[branch] if (!external_sun_ready)
				{
					external_sun_start = sampleExternalSunTransmittance(ray.start_pos, cloud_light_dir);
					external_sun_end = sampleExternalSunTransmittance(ray.start_pos + ray.ray_dir * ray.march_dist, cloud_light_dir);
					external_sun_ready = true;
				}
				low_valid = true;
				const float transmittance_weighted_density = ray.transmittance.x * cloud_density;
				low_mean_depth += absolute_dist * transmittance_weighted_density;
				low_mean_weight += transmittance_weighted_density;

				// dir light
				float light_extinction_od;
				float phi_fwd;
				sampleCloudSelfShadow(pos, ndf.local_height, cloud_light_dir, light_extinction_od, phi_fwd);
				const float relative_ray_distance = saturate(dist / max(ray.march_dist, 1.0));
				const float3 external_sun = lerp(external_sun_start, external_sun_end, relative_ray_distance);
				float3 directional_lum = 0.0;
				[unroll] for (uint octave = 0; octave < 3; ++octave)
				{
					const float attenuation = pow(info.msAttenuation, octave);
					const float contribution = pow(info.msContribution, octave);
					directional_lum += exp(-info.scatterTint * light_extinction_od * attenuation) * cloud_phase[octave] * contribution;
				}
				const float extinction_scalar = dot(extinction, float3(0.2126, 0.7152, 0.0722));
				const float scatter_od = extinction_scalar * 0.999 * step_small;
				const float scatter_fraction = 1.0 - exp(-scatter_od);
				float scatter_gate = 1.0 - exp(-scatter_od / max(info.scatterSourceODScale, 0.001));
				scatter_gate = HPPositivePow(saturate(scatter_gate), max(info.scatterSourceCurvePow, 0.01));
				// The remapped source is an edge-validity gate, not the Beer-Lambert
				// integral itself. Multiplying by the physical scattered fraction keeps
				// both direct and ambient radiance bounded by this sample's optical depth.
				const float scatter_source = scatter_fraction * scatter_gate;
				float3 in_scatter = directional_lum * external_sun * dirlightColor * scatter_source;

				// Additive isotropic diffuse field, independent from directional multiple scattering.
				// Convert the accumulated isotropic fluence to radiance with the Green kernel's 1 / (4 pi).
				// Keeping this normalization explicit lets the intensity remain a unit-scale artistic control.
				float phiScalar = info.phiFwdIntensity * phi_fwd * (0.25 * RCP_PI);
				phiScalar = info.phiFwdCompress > 0.0 ? (1.0 - exp(-phiScalar * info.phiFwdCompress)) / info.phiFwdCompress : phiScalar;
				const float3 sample_transmittance = exp(-step_small * extinction);
				const float3 phi_luminance = phiScalar * external_sun * dirlightColor;
				in_scatter += phi_luminance * (1.0 - sample_transmittance);

				// Evaluate top and bottom ambient independently. The solar optical depth
				// provides the upward AO proxy used by the reference implementation.
				const float upward_ao = exp(-light_extinction_od * max(cloud_light_dir.z, 0.05) * max(info.aoUpwardScale, 0.0));
				const float3 ambient_term = ambient_top * upward_ao + ambient_bottom * (1.0 - ndf.height_fraction);
				in_scatter += ambient_term * scatter_source;

				// update
				ray.lum += in_scatter * ray.transmittance;
				ray.transmittance *= sample_transmittance;
				[branch] if (ray.transmittance.x < 0.003)
				{
					ray.transmittance = 0.0;
					break;
				}
			}

			dist += step_small;
		}
		else dist += step_large;
	}

	float high_mean_weight = 0.0;
	float high_mean_depth = 0.0;
	bool high_valid = false;
	// Match HP's whole-ray high-cloud gate. Most rays cross weather-map regions
	// without high coverage and should not pay for 2 * primarySteps density tests.
	float high_gate_coverage = 0.0;
	[branch] if (info.highCloudEnabled > 0.0)
	{
		const float2 high_gate_uv = HPWeatherUV(ray.start_pos.xy, info);
		if (all(high_gate_uv >= 0.0) && all(high_gate_uv <= 1.0))
			high_gate_coverage = TexHpHighWeather.SampleLevel(TransmittanceSampler, high_gate_uv, 2).r;
	}
	if (info.highCloudEnabled > 0.0 && high_gate_coverage > 0.001) {
		const float3 lowLum = ray.lum;
		const float3 lowTransmittance = ray.transmittance;
		float3 highLum = 0.0;
		float3 highTransmittance = 1.0;
		const float hiCos = dot(ray.ray_dir, cloud_light_dir);
		const float3 hiPhase = float3(
			Phase::HG(hiCos, info.highForwardEccentricity) + Phase::HG(hiCos, -info.highBackwardEccentricity),
			Phase::HG(hiCos, info.highForwardEccentricity * info.highMSEccentricity) + Phase::HG(hiCos, -info.highBackwardEccentricity * info.highMSEccentricity),
			Phase::HG(hiCos, info.highForwardEccentricity * info.highMSEccentricity * info.highMSEccentricity) + Phase::HG(hiCos, -info.highBackwardEccentricity * info.highMSEccentricity * info.highMSEccentricity));
		const float3 hiAmbientTop = ambient_sky_top * info.highAmbientTopMultiplier;
		const float3 hiAmbientBottom = ambient_sky_bottom * info.ambientBottomMultiplier * info.highAmbientBottomMultiplier;
		const float3 hiSkyBlend = SampleCloudAmbientSkyView(ray.ray_dir);
		const uint hiSteps = max(info.cloudMaxStep * 2u, 4u);
		const uint hiLightSteps = max(info.lightSteps, 1u);
		const float hiStep = ray.march_dist / (float)hiSteps;
		float hiDist = jitter * hiStep;
		[loop] for (uint hi = 0; hi < hiSteps && hiDist < ray.march_dist; ++hi, hiDist += hiStep)
		{
			float hiAbsDist = ray.start_dist + hiDist;
			float3 hiPos = ray.start_pos + hiDist * ray.ray_dir;
			float hiNormH;
			float hiDensity = EvaluateHighCloudDensity(hiPos, hiNormH);
			if (hiDensity <= 0.001)
				continue;
			[branch] if (!external_sun_ready)
			{
				external_sun_start = sampleExternalSunTransmittance(ray.start_pos, cloud_light_dir);
				external_sun_end = sampleExternalSunTransmittance(ray.start_pos + ray.ray_dir * ray.march_dist, cloud_light_dir);
				external_sun_ready = true;
			}
			high_valid = true;
			const float transmittance_weighted_density = highTransmittance.x * hiDensity;
			high_mean_depth += hiAbsDist * transmittance_weighted_density;
			high_mean_weight += transmittance_weighted_density;
			const float2 hiWeatherUv = HPWeatherUV(hiPos.xy, info);
			const float4 hiWeather = any(hiWeatherUv < 0.0) || any(hiWeatherUv > 1.0) ? 0.0 : TexHpHighWeather.SampleLevel(TransmittanceSampler, hiWeatherUv, 0);
			const float hiMsWeight = hiWeather.a;
			float3 hiExtinction = hiDensity * info.highViewAbsorption * hiMsWeight * GAME_UNIT_TO_M;
			float3 hiTransmittance = exp(-hiExtinction * hiStep);
			const float3 hiExternalSun = lerp(external_sun_start, external_sun_end, saturate(hiDist / max(ray.march_dist, 1.0)));
			float hiExtinctionSum = 0.0;
			const float hiLightDistance = min(CloudLightExitDistance(hiPos, cloud_light_dir, cloud, info), 3000.0 * GAME_UNITS_PER_METER);
			const float hiLightStep = hiLightDistance / hiLightSteps;
			[loop] for (uint j = 0u; j < hiLightSteps; ++j)
			{
				float ignoredHeight;
				hiExtinctionSum += EvaluateHighCloudDensity(hiPos + cloud_light_dir * (j + 0.5) * hiLightStep, ignoredHeight) * hiLightStep;
			}
			const float hiCover = hiWeather.r;
			const float3 hiLightExtinction = info.scatterTint * hiExtinctionSum * info.highLightAbsorption * GAME_UNIT_TO_M * (1.0 + hiCover * info.highCoverAbsorptionStrength);
			float3 hiDirectionalLum = 0.0;
			[unroll] for (uint octave = 0u; octave < 3u; ++octave)
			{
				hiDirectionalLum += exp(-hiLightExtinction * pow(info.highMSAttenuation, octave)) * hiPhase[octave] * pow(info.highMSContribution, octave);
			}
			const float powder = CloudPowderEffect(hiDensity, hiCos, info.powderIntensity);
			hiDirectionalLum *= powder;
			float3 hiAmbient = lerp(hiAmbientBottom, hiAmbientTop, hiNormH);
			float3 hiLum = (hiExternalSun * dirlightColor * hiDirectionalLum + hiAmbient) * hiMsWeight;
			hiLum = lerp(hiLum, hiSkyBlend, smoothstep(0.0, 1.0, hiNormH) * info.highSkyBlendStrength);
			float3 hiIntegral = hiLum * (1.0 - hiTransmittance);
			highLum += hiIntegral * highTransmittance;
			highTransmittance *= hiTransmittance;
			if (highTransmittance.x < 0.003) {
				highTransmittance = 0.0;
				break;
			}
		}

		if (high_valid) {
			const float cameraAltitude = length(ray.eye_pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
			const float highCloudBottomAltitude = lerp(info.lowestCloudAltitude, info.highestCloudAltitude, info.highCloudBottom);
			if (cameraAltitude >= highCloudBottomAltitude)
				ray.lum = highLum + highTransmittance * lowLum;
			else
				ray.lum = lowLum + lowTransmittance * highLum;
			ray.transmittance = lowTransmittance * highTransmittance;
		}
	}

	const float fallback_depth = is_sky ? info.rayMarchRange : min(solid_dist, info.rayMarchRange);
	float cloud_depth = fallback_depth;
	if (low_valid && low_mean_weight > 0.0)
		cloud_depth = low_mean_depth / low_mean_weight;
	if (high_valid && high_mean_weight > 0.0) {
		const float high_cloud_depth = high_mean_depth / high_mean_weight;
		cloud_depth = low_valid ? min(cloud_depth, high_cloud_depth) : high_cloud_depth;
	}
	const float scatter_weight = low_mean_weight + high_mean_weight;
	if (scatter_weight > 0.0 && ap_shadow >= 0.0) {
		// Match the host pipeline's cloud fogging order: attenuate cloud radiance
		// between the cloud mean depth and the camera, then add only the aerial
		// perspective that replaces the portion of the background occluded by cloud.
		const float4 ap = SampleCloudAerialPerspective(ray.ray_dir, cloud_depth, ap_shadow);
		ray.lum = ray.lum * ap.a + ap.rgb * (1.0 - ray.transmittance);
	}
	VolumetricCloudResult result;
	result.transmittance = ray.transmittance;
	result.lum = ray.lum;
	result.cloud_depth = cloud_depth;
	result.reject_depth = fallback_depth;
	result.scatter_weight = scatter_weight;
	return result;
}

uint2 ComputeCloudCheckerboardOffset(uint2 traceCoord, uint subPixelIndex)
{
	const uint checker = (traceCoord.x & 1u) ^ (traceCoord.y & 1u);
	subPixelIndex = (subPixelIndex + checker) & 3u;
	return uint2(((subPixelIndex >> 1u) ^ subPixelIndex) & 1u, subPixelIndex >> 1u);
}

float ReconstructSceneRayDistance(uint2 fullPixelCoord, VolumetricCloudData info)
{
	const float depth = TexDepth[fullPixelCoord];
	if (depth > 1.0 - 1e-6)
		return info.rayMarchRange;

	const float2 textureUv = (fullPixelCoord + 0.5) * info.rcpFrameDim;
	const float2 logicUv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(textureUv);
	float4 position = float4(2.0 * float2(logicUv.x, 1.0 - logicUv.y) - 1.0, depth, 1.0);
	position = mul(FrameBuffer::CameraViewProjInverse, position);
	return min(length(position.xyz / position.w), info.rayMarchRange);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];

	if (any(tid >= uint2(info.lowFrameDim)))
		return;

	const uint2 px_coords = tid;
	const bool full_resolution = info.fullResolution != 0u;
	const uint frame_index = SharedData::FrameCountAlwaysActive & 63u;
	const uint frame_subpixel = full_resolution ? 0u : (frame_index & 3u);
	const uint2 checker_offset = full_resolution ? 0u.xx : ComputeCloudCheckerboardOffset(px_coords, frame_subpixel);
	const uint2 active_intermediate_dims = (uint2(info.activeFrameDim) + 1u) / 2u;
	const uint2 intermediate_coord = full_resolution ? px_coords : min(px_coords * 2u + checker_offset, active_intermediate_dims - 1u);
	const uint2 full_px_coords = full_resolution ? px_coords : min(intermediate_coord * 2u, uint2(info.activeFrameDim) - 1u);

	const float ray_jitter = CloudRayJitter(intermediate_coord, frame_index >> 2u);

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

	const float ap_shadow = SampleCloudApShadow(full_px_coords);
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, solid_dist, is_sky, ray_jitter, ap_shadow);

	RWTexTr[px_coords] = float4(result.transmittance, CloudAlphaFromTransmittance(result.transmittance));
	RWTexLum[px_coords] = result.lum;
	RWTexAux[px_coords] = float4(
		EncodeCloudDepth(result.cloud_depth),
		EncodeCloudDepth(result.reject_depth),
		saturate(result.scatter_weight * GAME_UNIT_TO_M),
		1.0);
};

float3 GetCubemapSamplingVector(uint3 threadId, in RWTexture2DArray<float3> outputTexture)
{
	float width = 0.0f;
	float height = 0.0f;
	float depth = 0.0f;
	outputTexture.GetDimensions(width, height, depth);

	float2 st = (float2(threadId.xy) + 0.5) / float2(width, height);
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
	if (any(tid.xy >= dims.xy) || tid.z >= 2u)
		return;
	// Update one opposite-face pair per frame. All six faces are refreshed within
	// three frames, cutting the full cloud trace cost to one third without adding
	// a cubemap history/reprojection path.
	const uint face_pair = SharedData::FrameCountAlwaysActive % 3u;
	const uint3 output_tid = uint3(tid.xy, face_pair * 2u + tid.z);

	// Sky capture has no temporal integration, so its ray starts at the slab boundary.
	const float ray_jitter = 0.0;

	const float3 eye_pos = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float3 ray_dir = GetCubemapSamplingVector(output_tid, RWTexCubeTr);
	// Sky-capture clouds are combined in the sky path and do not receive the
	// main-view aerial-perspective pass.
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, info.rayMarchRange, true, ray_jitter, -1.0);

	RWTexCubeTr[output_tid] = result.transmittance;
	RWTexCubeLum[output_tid] = result.lum;
};

float2 GetPreviousCloudUv(float2 logic_uv, float depth, out bool valid)
{
	valid = false;
	const float reprojection_depth = DecodeCloudDepth(depth);

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

float4 BilinearWeights(float2 frac)
{
	return float4((1.0 - frac.x) * (1.0 - frac.y), frac.x * (1.0 - frac.y), (1.0 - frac.x) * frac.y, frac.x * frac.y);
}

float CloudBilateralDepthWeight(float sampleRejectDepth, float referenceDepth)
{
	return rcp(abs(sampleRejectDepth - referenceDepth) + MIN_CLOUD_DEPTH_TOLERANCE_KM);
}

bool CloudDepthIsSky(float rejectDepth, float encodedRayRange)
{
	// Rejection depth is stored in R16F between passes, so allow one small
	// quantization band at the configured far distance.
	return rejectDepth >= encodedRayRange - max(MIN_CLOUD_DEPTH_TOLERANCE_KM, encodedRayRange * 0.001);
}

bool CloudHistoryDepthValid(float historyDepth, float currentDepth, float encodedRayRange)
{
	const bool historyIsSky = CloudDepthIsSky(historyDepth, encodedRayRange);
	const bool currentIsSky = CloudDepthIsSky(currentDepth, encodedRayRange);
	if (historyIsSky != currentIsSky)
		return false;
	if (currentIsSky)
		return true;

	return abs(historyDepth / max(currentDepth, MIN_CLOUD_DEPTH_TOLERANCE_KM) - 1.0) <= 0.2;
}

bool LoadCloudTrace(uint2 texelCoord, uint2 dims, float referenceDepth, out float3 tr, out float3 lum, out float4 aux)
{
	const uint2 px = min(texelCoord, dims - 1u);
	aux = TexVolLowAux[px];
	if (aux.w <= 0.0) {
		tr = 1.0;
		lum = 0.0;
		aux = float4(referenceDepth, referenceDepth, 0.0, 0.0);
		return false;
	}

	tr = TexVolLowTr[px].rgb;
	lum = TexVolLowLum[px];
	return true;
}

bool SampleHistoryBilinear(float2 uv, out float3 tr, out float3 lum, out float4 aux)
{
	uint2 dims;
	TexVolHistoryTr.GetDimensions(dims.x, dims.y);
	const float2 uvMax = 1.0 - 0.5 / float2(dims) - 1e-6;
	if (any(uv < 0.0) || any(uv > uvMax)) {
		tr = 1.0;
		lum = 0.0;
		aux = 0.0;
		return false;
	}

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
	const float4 weights = BilinearWeights(frac);

	tr = TexVolHistoryTr[px00].rgb * weights.x + TexVolHistoryTr[px10].rgb * weights.y + TexVolHistoryTr[px01].rgb * weights.z + TexVolHistoryTr[px11].rgb * weights.w;
	lum = TexVolHistoryLum[px00] * weights.x + TexVolHistoryLum[px10] * weights.y + TexVolHistoryLum[px01] * weights.z + TexVolHistoryLum[px11] * weights.w;
	aux = float4(
		aux00.x * weights.x + aux10.x * weights.y + aux01.x * weights.z + aux11.x * weights.w,
		aux00.y * weights.x + aux10.y * weights.y + aux01.y * weights.z + aux11.y * weights.w,
		aux00.z * weights.x + aux10.z * weights.y + aux01.z * weights.z + aux11.z * weights.w,
		aux00.w * weights.x + aux10.w * weights.y + aux01.w * weights.z + aux11.w * weights.w);
	return aux.z >= 0.5;
}

float ClampCloudHistoryToCurrentNeighborhood(
	uint2 traceCoord, uint2 dims, bool currentIsSky, float encodedRayRange,
	inout float3 historyTr, inout float3 historyLum)
{
	if (!currentIsSky)
		return 1.0;

	float3 minTr = 1.0;
	float3 maxTr = 0.0;
	float3 minLum = 3.402823466e+38;
	float3 maxLum = 0.0;
	uint skySamples = 0u;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const int2 tap = clamp(int2(traceCoord) + int2(x, y), 0, int2(dims) - 1);
			const float4 tapAux = TexVolLowAux[tap];
			if (CloudDepthIsSky(tapAux.y, encodedRayRange)) {
				const float3 tapTr = TexVolLowTr[tap].rgb;
				const float3 tapLum = TexVolLowLum[tap];
				minTr = min(minTr, tapTr);
				maxTr = max(maxTr, tapTr);
				minLum = min(minLum, tapLum);
				maxLum = max(maxLum, tapLum);
				++skySamples;
			}
		}
	}

	if (skySamples < 5u)
		return 1.0;

	historyTr = clamp(historyTr, minTr, maxTr);
	const float3 center = 0.5 * (minLum + maxLum);
	const float3 extents = max(0.5 * (maxLum - minLum), 1e-5);
	const float3 offset = historyLum - center;
	const float maxUnit = max(abs(offset.x / extents.x), max(abs(offset.y / extents.y), abs(offset.z / extents.z)));
	const bool clipped = maxUnit > 1.0;
	if (clipped)
		historyLum = center + offset / maxUnit;

	const float skyRatio = (float)skySamples / 9.0;
	return skyRatio * (clipped ? 0.5 : 1.0);
}

bool SampleCloudFallback(uint2 intermediateCoord, uint2 dims, float referenceDepth, float encodedRayRange, out float3 tr, out float3 lum, out float4 aux)
{
	const int2 center = int2(intermediateCoord / 2u);
	const float2 subPixelCenter = (float2(intermediateCoord & 1u) - 0.5) * 0.5;
	const bool referenceIsSky = CloudDepthIsSky(referenceDepth, encodedRayRange);
	float4 trSum = 0.0;
	float3 lumSum = 0.0;
	float4 auxSum = 0.0;
	float weightSum = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const int2 tap = clamp(center + int2(x, y), 0, int2(dims) - 1);
			const float4 tapAux = TexVolLowAux[tap];
			const bool tapIsSky = CloudDepthIsSky(tapAux.y, encodedRayRange);
			const float2 delta = float2(x, y) - subPixelCenter;
			const float spatialWeight = exp(-dot(delta, delta));
			const float depthWeight = CloudBilateralDepthWeight(tapAux.y, referenceDepth);
			const float weight = spatialWeight * depthWeight * (tapIsSky == referenceIsSky ? 1.0 : 0.0) * tapAux.w;
			trSum += TexVolLowTr[tap] * weight;
			lumSum += TexVolLowLum[tap] * weight;
			auxSum += tapAux * weight;
			weightSum += weight;
		}
	}

	if (weightSum <= 1e-5) {
		tr = 1.0;
		lum = 0.0;
		aux = float4(referenceDepth, referenceDepth, 0.0, 0.0);
		return false;
	}
	tr = trSum.rgb / weightSum;
	lum = lumSum / weightSum;
	aux = auxSum / weightSum;
	return true;
}

[numthreads(8, 8, 1)] void reproject(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	uint2 intermediate_dims;
	RWTexTr.GetDimensions(intermediate_dims.x, intermediate_dims.y);
	const uint2 active_intermediate_dims = (uint2(info.activeFrameDim) + 1u) / 2u;
	if (any(tid >= active_intermediate_dims))
		return;

	uint2 low_dims;
	low_dims = uint2(info.lowFrameDim);
	const float2 texture_uv = (tid + 0.5) / intermediate_dims;
	const float2 logic_uv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition(texture_uv);
	const uint2 full_px = min(tid * 2u, uint2(info.activeFrameDim) - 1u);
	const float current_reject_depth = EncodeCloudDepth(ReconstructSceneRayDistance(full_px, info));
	const float encoded_ray_range = EncodeCloudDepth(info.rayMarchRange);
	const uint frame_subpixel = SharedData::FrameCountAlwaysActive & 3u;
	const uint2 checker_offset = ComputeCloudCheckerboardOffset(tid / 2u, frame_subpixel);
	const bool valid_tracing = all((tid & 1u) == checker_offset);
	const bool history_available = info.historyValid != 0;
	float3 current_tr;
	float3 current_lum;
	float4 current_aux;
	const bool current_valid = LoadCloudTrace(tid / 2u, low_dims, current_reject_depth, current_tr, current_lum, current_aux);

	const uint2 trace_coord = min(tid / 2u, low_dims - 1u);
	const float reprojection_cloud_depth = TexVolLowAux[trace_coord].x;
	bool projection_valid;
	float2 projected_uv = GetPreviousCloudUv(logic_uv, reprojection_cloud_depth, projection_valid);
	projection_valid = projection_valid && history_available;
	const float2 history_uv = FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(projected_uv);

	float3 tr = 1.0;
	float3 lum = 0.0;
	float4 aux = float4(current_reject_depth, current_reject_depth, 0.0, 0.0);
	float3 history_tr = 1.0;
	float3 history_lum = 0.0;
	float4 history_aux = 0.0;
	bool history_valid = false;
	if (projection_valid) {
		history_valid = SampleHistoryBilinear(history_uv, history_tr, history_lum, history_aux);
		if (history_valid)
			history_valid = CloudHistoryDepthValid(history_aux.y, current_reject_depth, encoded_ray_range);
	}
	float history_validity = 1.0;
	if (history_valid && info.ghostingReduction != 0) {
		history_validity = ClampCloudHistoryToCurrentNeighborhood(
			trace_coord, low_dims, CloudDepthIsSky(current_reject_depth, encoded_ray_range), encoded_ray_range,
			history_tr, history_lum);
	}
	if (!valid_tracing && history_valid) {
		tr = history_tr;
		lum = history_lum;
		aux = history_aux;
		aux.z = max(1.0, history_validity * clamp(history_aux.z, 1.0, 16.0) * info.cloudHistoryInvalidation);
	} else if (valid_tracing && current_valid && history_valid) {
		// Cap the effective history at sixteen samples. The local auxiliary buffer
		// carries the count in z while y remains the scene-depth rejection value.
		const float previous_count = clamp(history_aux.z, 1.0, 16.0);
		const float history_weight = history_validity * previous_count / (previous_count + 1.0) *
		                             info.temporalAccumulationFactor * info.cloudHistoryInvalidation;
		tr = lerp(current_tr, history_tr, history_weight);
		lum = lerp(current_lum, history_lum, history_weight);
		// Cloud depth belongs to the freshly traced sample. Only radiance and
		// transmittance are accumulated temporally.
		aux = current_aux;
		aux.z = min(previous_count + 1.0, 16.0);
	} else if (valid_tracing && current_valid) {
		tr = current_tr;
		lum = current_lum;
		aux = current_aux;
		aux.z = 1.0;
	} else {
		SampleCloudFallback(tid, low_dims, current_reject_depth, encoded_ray_range, tr, lum, aux);
		aux.z = 1.0;
	}
	// Scene rejection depth describes this half-resolution pixel in the current
	// frame. It is not a temporally accumulated cloud property.
	aux.y = current_reject_depth;

	RWTexTr[tid] = float4(tr, CloudAlphaFromTransmittance(tr));
	RWTexLum[tid] = lum;
	RWTexAux[tid] = aux;
};

// An 8x8 full-resolution group touches only a 6x6 region of the half-resolution
// input for its 3x3 bilateral neighborhoods. Cache that region once, as HDRP's
// cloud denoiser does, instead of issuing 27 texture reads per output pixel.
groupshared float4 CloudUpscaleTrLds[36];
groupshared float4 CloudUpscaleLumLds[36];
groupshared float4 CloudUpscaleAuxLds[36];

[numthreads(8, 8, 1)] void upscale(
	uint2 tid : SV_DispatchThreadID,
	uint3 groupThreadId : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const uint2 active_intermediate_dims = (uint2(info.activeFrameDim) + 1u) / 2u;
	const uint2 full_group_origin = tid - groupThreadId.xy;
	const int2 intermediate_group_origin = int2(full_group_origin / 2u);
	if (groupIndex < 36u) {
		const int2 local_load = int2(groupIndex % 6u, groupIndex / 6u);
		const int2 load_coord = clamp(intermediate_group_origin + local_load - 1, 0, int2(active_intermediate_dims) - 1);
		CloudUpscaleTrLds[groupIndex] = TexVolUpscaleTr[load_coord];
		CloudUpscaleLumLds[groupIndex] = float4(TexVolUpscaleLum[load_coord], 0.0);
		CloudUpscaleAuxLds[groupIndex] = TexVolUpscaleAux[load_coord];
	}
	GroupMemoryBarrierWithGroupSync();

	if (any(tid >= uint2(info.activeFrameDim)))
		return;

	const float2 subPixelCenter = (float2(tid & 1u) - 0.5) * 0.5;
	const float reference_depth = EncodeCloudDepth(ReconstructSceneRayDistance(tid, info));
	const float encoded_ray_range = EncodeCloudDepth(info.rayMarchRange);
	const bool reference_is_sky = CloudDepthIsSky(reference_depth, encoded_ray_range);
	float4 tr_sum = 0.0;
	float3 lum_sum = 0.0;
	float4 aux_sum = 0.0;
	float weight_sum = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const int2 local_tap = int2(groupThreadId.xy / 2u) + int2(x, y) + 1;
			const uint lds_index = uint(local_tap.y * 6 + local_tap.x);
			const float4 tap_aux = CloudUpscaleAuxLds[lds_index];
			const bool tap_is_sky = CloudDepthIsSky(tap_aux.y, encoded_ray_range);
			const float2 delta = float2(x, y) - subPixelCenter;
			const float spatial_weight = exp(-dot(delta, delta));
			const float depth_weight = CloudBilateralDepthWeight(tap_aux.y, reference_depth);
			const float pixel_status = saturate(tap_aux.z);
			const float weight = spatial_weight * depth_weight * (tap_is_sky == reference_is_sky ? 1.0 : 0.0) * pixel_status;
			tr_sum += CloudUpscaleTrLds[lds_index] * weight;
			lum_sum += CloudUpscaleLumLds[lds_index].rgb * weight;
			aux_sum += tap_aux * weight;
			weight_sum += weight;
		}
	}

	const int2 local_center = int2(groupThreadId.xy / 2u) + 1;
	const uint center_index = uint(local_center.y * 6 + local_center.x);
	const float4 center_tr = CloudUpscaleTrLds[center_index];
	const float3 center_lum = CloudUpscaleLumLds[center_index].rgb;
	const float4 center_aux = CloudUpscaleAuxLds[center_index];
	float4 tr = weight_sum > 1e-5 ? tr_sum / weight_sum : center_tr;
	float3 lum = weight_sum > 1e-5 ? lum_sum / weight_sum : center_lum;
	float4 aux = weight_sum > 1e-5 ? aux_sum / weight_sum : center_aux;

	// Perform a manual depth test after upscaling so reconstructed clouds cannot
	// leak in front of nearer scene geometry.
	if (!reference_is_sky && aux.x >= reference_depth) {
		tr = 1.0;
		lum = 0.0;
		aux.x = reference_depth;
	}

	RWTexTr[tid] = tr;
	RWTexLum[tid] = lum;
	RWTexAux[tid] = aux;
};

bool IntersectCloudSphere(float3 origin, float3 dir, float radius, out float2 intersections)
{
	const float b = dot(origin, dir);
	const float c = dot(origin, origin) - radius * radius;
	const float discriminant = b * b - c;
	if (discriminant < 0.0) {
		intersections = 0.0;
		return false;
	}
	const float root = sqrt(discriminant);
	intersections = float2(-b - root, -b + root);
	return intersections.y >= 0.0;
}

[numthreads(8, 8, 1)] void renderShadowVolume(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);
	const float3 shadow_light_dir = info.dirlightDir;
	uint2 dims;
	RWShadowVolume.GetDimensions(dims.x, dims.y);
	if (any(tid >= dims))
		return;
	if (shadow_light_dir.z <= 1e-4) {
		RWShadowVolume[tid] = float4(0.0, 1.0, 0.0, 0.0);
		return;
	}

	float3 right, up;
	GetCloudShadowBasis(shadow_light_dir, right, up);
	const float3 camera = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float shadow_width = max(info.shadowVolumeRange, 1.0);
	const float2 uv = float2(tid) / max(float2(dims - 1u), 1.0.xx);
	float3 origin = camera + ((uv.x - 0.5) * right + (uv.y - 0.5) * up) * shadow_width;

	// Put every ray just outside the top of the shell. A fixed displacement along
	// the light direction fails at low solar elevations because it can remain below
	// the cloud base and then trace away from the clouds.
	const float outer_radius = info.planetRadius + cloud.highestAltitude;
	float3 origin_planet = origin + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	if (length(origin_planet) < outer_radius) {
		float2 lift_hits;
		if (!IntersectCloudSphere(origin_planet, shadow_light_dir, outer_radius, lift_hits) || lift_hits.y < 0.0) {
			RWShadowVolume[tid] = float4(0.0, 1.0, 0.0, 0.0);
			return;
		}
		origin += shadow_light_dir * (lift_hits.y + GAME_UNITS_PER_METER);
		origin_planet = origin + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	}

	const float3 ray_dir = -shadow_light_dir;
	float2 inner_hits, outer_hits;
	const bool hit_inner = IntersectCloudSphere(origin_planet, ray_dir, info.planetRadius + cloud.lowestAltitude, inner_hits);
	const bool hit_outer = IntersectCloudSphere(origin_planet, ray_dir, info.planetRadius + cloud.highestAltitude, outer_hits);
	if (!hit_inner || !hit_outer) {
		RWShadowVolume[tid] = float4(0.0, 1.0, 0.0, 0.0);
		return;
	}

	const float start_dist = max(outer_hits.x, 0.0);
	const float end_dist = max(inner_hits.x, start_dist);
	const float total_dist = end_dist - start_dist;
	const float step_size = total_dist / 16.0;
	float transmittance = 1.0;
	float closest = 3.402823466e+38;
	float farthest = 0.0;
	bool valid = false;
	[loop] for (uint i = 1u; i < 16u; ++i)
	{
		const float dist = start_dist + step_size * i;
		const float3 pos = origin + ray_dir * dist;

		// Match the main-view low-cloud visibility test: the inexpensive probe may
		// reject fine density before the full erosion lookup is evaluated.
		NDFInfo ndf;
		float low_density = sampleCloudDensity(pos, dist, cloud, 1.0, false, ndf);
		if (low_density > 0.001)
			low_density = sampleCloudDensity(pos, 0.0, cloud, 0.0, true, ndf);

		// The visible result contains a separate high layer. Its ground shadow must
		// use the same density field and the direct-light absorption parameters.
		float high_height;
		const float high_density = EvaluateHighCloudDensity(pos, high_height);
		float3 optical_depth = info.scatterTint * cloud.scatter * low_density;
		if (high_density > 0.001) {
			const float2 high_uv = HPWeatherUV(pos.xy, info);
			const float high_cover = any(high_uv < 0.0) || any(high_uv > 1.0) ? 0.0 : TexHpHighWeather.SampleLevel(TransmittanceSampler, high_uv, 0).r;
			optical_depth += info.scatterTint * high_density * info.highLightAbsorption * GAME_UNIT_TO_M *
			                 (1.0 + high_cover * info.highCoverAbsorptionStrength);
		}

		if (low_density > 0.001 || high_density > 0.001) {
			closest = min(closest, total_dist - step_size * (i + 1u));
			farthest = max(farthest, total_dist - step_size * i);
			const float3 extinction = exp(-optical_depth * step_size);
			transmittance *= dot(extinction, float3(0.2126, 0.7152, 0.0722));
			valid = true;
		}
	}
	// R16F cannot represent cloud-shell distances in game units. Store the two
	// distance channels in kilometres; transmittance and validity stay unitless.
	RWShadowVolume[tid] = valid ? float4(EncodeCloudDepth(closest), transmittance, EncodeCloudDepth(farthest), 1.0) : float4(0.0, 1.0, 0.0, 0.0);
}

	[numthreads(8, 8, 1)] void filterShadowVolume(uint2 tid : SV_DispatchThreadID)
{
	uint2 dims;
	RWShadowVolume.GetDimensions(dims.x, dims.y);
	if (any(tid >= dims))
		return;
	float3 shadow_sum = 0.0;
	float3 weight_sum = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			const uint2 tap = uint2(clamp(int2(tid) + int2(x, y), 0, int2(dims) - 1));
			const float3 shadow = TexShadowFilterInput[tap].xyz;
			const float weight = exp(-float(x * x + y * y) / 0.81);
			if (shadow.y != 1.0) {
				shadow_sum.xz += shadow.xz * weight;
				weight_sum.xz += weight;
			}
			shadow_sum.y += shadow.y * weight;
			weight_sum.y += weight;
		}
	}
	const float3 center = TexShadowFilterInput[tid].xyz;
	RWShadowVolume[tid] = float4(
		weight_sum.x > 0.0 ? shadow_sum.x / weight_sum.x : center.x,
		shadow_sum.y / max(weight_sum.y, 1e-5),
		weight_sum.z > 0.0 ? shadow_sum.z / weight_sum.z : center.z,
		1.0);
}
