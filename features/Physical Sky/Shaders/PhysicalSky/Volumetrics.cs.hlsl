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
	float lowCloudBaseAltitude;
	float lowCloudTopAltitude;
	float lowCloudTraceTopAltitude;

	float2 weatherCenter;
	float weatherWorldSize;
	float highCloudEnabled;
	float2 lowNdfFrequency;
	float2 noiseWindOffset;
	float noiseFrequency;
	float3 noiseOffset;
	float extinctionCoefficient;
	float2 noiseHeightShear;
	float warpFrequency;
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
	uint cloudPhaseModel;
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
	float temporalAccumulationFactor;
	float cloudHistoryInvalidation;
	uint ghostingReduction;
	uint scatterIntegration;
	float lightStepDistanceLod;
	float shadowVolumeBottom;
	float shadowVolumeTop;
};

CloudLayer GetCloudLayer(VolumetricCloudData info)
{
	CloudLayer cloud;
	cloud.lowestAltitude = info.lowCloudBaseAltitude;
	cloud.highestAltitude = info.lowCloudTopAltitude;
	// View extinction is scalar; the tint is applied only to light-path extinction.
	// The coefficient is authored per metre while march distances are game units.
	cloud.scatter = GAME_UNIT_TO_M;
	cloud.absorption = 0.0;
	return cloud;
}

float GetCloudAltitudeRange(VolumetricCloudData info)
{
	// Step sizing follows actual generated low-cloud content, not the complete NDF
	// coordinate frame nor the union with an independent high-cloud layer.
	return max(info.lowCloudTraceTopAltitude - info.lowCloudBaseAltitude, GAME_UNITS_PER_METER);
}

StructuredBuffer<VolumetricCloudData> VolumetricCloudBuffer : register(t0);
Texture2D<float4> TexTransmittance : register(t1);
Texture2D<float4> TexMultiScatter : register(t2);
Texture3D<float4> TexAerialPerspective : register(t3);

Texture2D<float> TexDepth : register(t4);

Texture3D<unorm float4> TexNubisNoise : register(t5);
Texture3D<float4> TexAerialPerspectiveSun : register(t6);
Texture2DArray<float> TexCloudNDF : register(t7);
Texture2D<float4> TexNubisWarp : register(t8);
Texture2D<unorm float> TexApShadow : register(t9);
Texture2D<float4> TexSkyView : register(t10);
Texture2D<float4> TexHpHighWeather : register(t11);
Texture2D<float4> TexHpHighCell : register(t13);
Texture2D<float4> TexHpHighWarp : register(t14);
Texture2D<float4> TexHpHighWisp : register(t15);
Texture2D<sh2> TexCloudAmbientSH : register(t16);
Texture2D<unorm float> TexCloudTopLUT : register(t17);
Texture2D<unorm float> TexCloudBottomLUT : register(t18);

Texture3D<float> TexShadowVolume : register(t23);
Texture2D<float4> TexVolHistoryTr : register(t26);
Texture2D<float3> TexVolHistoryLum : register(t27);
Texture2D<float4> TexVolHistoryAux : register(t28);
Texture2D<float4> TexVolLowTr : register(t29);
Texture2D<float3> TexVolLowLum : register(t30);
Texture2D<float4> TexVolLowAux : register(t31);
Texture2D<float4> TexVolUpscaleTr : register(t32);
Texture2D<float3> TexVolUpscaleLum : register(t33);
Texture2D<float4> TexVolUpscaleAux : register(t34);

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

RWTexture3D<float> RWShadowVolume : register(u0);

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

// Approximate the environment integral with the phase-convolved radiance from
// the upper and lower probe directions, then interpolate by cloud height as in
// HDRP. The weights form a partition of unity: with both authoring multipliers
// at 1.0 the probe is sampled once, rather than adding two full environment
// fields and creating energy. Only the upper contribution is attenuated by the
// estimated vertical optical depth because the cloud base has an open lower
// hemisphere while the cloud top is reached through the cloud column.
float3 EvaluateCloudEnvironmentRadiance(
	float3 ambientTop, float3 ambientBottom, float normalizedHeight,
	float topMultiplier, float bottomMultiplier, float upwardTransmittance)
{
	const float height = saturate(normalizedHeight);
	const float3 lower = ambientBottom * max(bottomMultiplier, 0.0);
	const float3 upper = ambientTop * max(topMultiplier, 0.0) * saturate(upwardTransmittance);
	return lerp(lower, upper, height);
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
	const float depthSlice = ApDepthUv(distance, apDims.z);
	const float3 apUv = float3(SkyViewLutUv(viewDir), depthSlice);
	float4 ap = TexAerialPerspective.SampleLevel(SkyViewSampler, apUv, 0);
	const float3 apSun = TexAerialPerspectiveSun.SampleLevel(SkyViewSampler, apUv, 0).rgb;
	ap.rgb += apSun * (1.0 - saturate(shadow));
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
	const float2 inner = IntersectSpherePair(origin_planet, ray.ray_dir, info.planetRadius + info.lowestCloudAltitude);
	const float2 outer = IntersectSpherePair(origin_planet, ray.ray_dir, info.planetRadius + info.highestCloudAltitude);
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

float CloudLightExitDistance(float3 pos, float3 dir, float topAltitude, VolumetricCloudData info)
{
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float2 outer = IntersectSpherePair(pos_planet, dir, info.planetRadius + topAltitude);
	return max(outer.y, 0.0);
}

// Locate a light-column position inside the camera-centred shadow volume box.
// Positions outside the box are marched along the light direction to their entry
// point; the accumulated prefix sum there covers the remaining in-box column.
float3 GetShadowVolumeSampleUvw(float3 pos, float3 rayDir, VolumetricCloudData info)
{
	float3 boundsMin = float3(FrameBuffer::CameraPosAdjust.xy - 0.5 * info.shadowVolumeRange, info.shadowVolumeBottom);
	float3 boundsMax = float3(FrameBuffer::CameraPosAdjust.xy + 0.5 * info.shadowVolumeRange, info.shadowVolumeTop);

	return CloudShadowVolume::GetSampleUvw(pos, rayDir, boundsMin, boundsMax);
}

// Hash-white spatial noise with a stratified temporal sequence. IGN was briefly
// used here as an analytical substitute for blue noise, but its regular diagonal
// structure remained visible after reprojection. Hash each use independently so
// ray starts, light-cone offsets and light-step LOD rounding cannot reinforce the
// same screen-space pattern.
float CloudSpatiotemporalNoise(uint2 pixelCoord, uint sampleIndex, uint dimension)
{
	const uint pixelSeed = 0x9e3779b9u ^ (dimension * 0x85ebca6bu);
	const uint sampleSeed = 0x68bc21ebu ^ (dimension * 0xc2b2ae35u);
	const uint pixelHash = Random::pcg3d(uint3(pixelCoord, pixelSeed)).x;
	// Advance by one stratum per traced update. The per-pixel phase distributes the
	// single wrap in the sixteen-sample cycle spatially instead of making the whole
	// image take a large ray-start jump on the same frame.
	const uint stratum = (sampleIndex + (pixelHash & 15u)) & 15u;
	const uint sampleHash = Random::pcg3d(uint3(pixelCoord, sampleIndex ^ sampleSeed)).y;
	const float withinStratum = float(sampleHash >> 8u) * (1.0 / 16777216.0);
	return (float(stratum) + withinStratum) * (1.0 / 16.0);
}

float CloudRayJitter(uint2 pixelCoord, uint sampleIndex)
{
	return CloudSpatiotemporalNoise(pixelCoord, sampleIndex, 0u);
}

float CloudPositivePow(float x, float p)
{
	return pow(max(x, 0.0), p);
}

float CloudDensityRemap(float x, float a, float b, float c, float d)
{
	return (((x - a) / max(b - a, 1e-5)) * (d - c)) + c;
}

float CloudPowderEffect(float cloudDensity, float cosAngle, float intensity)
{
	float powder = saturate((1.0 - exp(-cloudDensity * 4.0)) * 2.0);
	return lerp(1.0, lerp(1.0, powder, smoothstep(0.5, -0.5, cosAngle)), intensity);
}

// The weather map is a seamlessly tiling synoptic pattern rather than a finite
// rectangle, so it is sampled with a wrapping sampler and never needs a bounds
// test. Subtracting the accumulated wind displacement advects the whole cloud
// field downwind, matching the 3D volume coordinates that use the same offset.
float2 CloudWeatherUV(float2 worldXY, VolumetricCloudData info)
{
	return (worldXY - info.weatherCenter - info.noiseWindOffset) / max(info.weatherWorldSize, 1.0) + 0.5;
}

// The pre-HP NDF is its own tileable five-layer definition. Its physical scale
// is independent of both the high-cloud weather map and the 3D detail noise.
float2 LowNdfUV(float2 worldXY, VolumetricCloudData info)
{
	return worldXY * info.lowNdfFrequency + 0.5;
}

float EvaluateCloudTopHeightProxy(float2 worldXY)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float2 uv = LowNdfUV(worldXY, info);
	return TexCloudNDF.SampleLevel(TileableSampler, float3(uv, 1), 0);
}

float EvaluateCloudBoundaryLight(float3 pos, float3 sunDir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float2 ndfPeriod = 1.0 / max(info.lowNdfFrequency, float2(1e-8, 1e-8));
	float sampleStep = clamp(min(ndfPeriod.x, ndfPeriod.y) * 0.001, 25.0 * GAME_UNITS_PER_METER, 200.0 * GAME_UNITS_PER_METER);
	float hL = EvaluateCloudTopHeightProxy(pos.xy - float2(sampleStep, 0.0));
	float hR = EvaluateCloudTopHeightProxy(pos.xy + float2(sampleStep, 0.0));
	float hD = EvaluateCloudTopHeightProxy(pos.xy - float2(0.0, sampleStep));
	float hU = EvaluateCloudTopHeightProxy(pos.xy + float2(0.0, sampleStep));
	const float cloudAltitudeRange = max(info.lowCloudTopAltitude - info.lowCloudBaseAltitude, GAME_UNITS_PER_METER);
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
	float dimension_profile;
	float coverage;
	float height_fraction;
	float local_height;
	float top_type;
	float bottom_type;
	float top_value;
	float bottom_value;
};

void initNDFInfo(out NDFInfo ndf)
{
	ndf.in_layer = false;
	ndf.dimension_profile = 0.0;
	ndf.coverage = 0.0;
	ndf.height_fraction = 0.0;
	ndf.local_height = 0.0;
	ndf.top_type = 0.0;
	ndf.bottom_type = 0.0;
	ndf.top_value = 0.0;
	ndf.bottom_value = 0.0;
}

NDFInfo sampleNDF(CloudLayer cloud, float2 ndfUV, float planet_z)
{
	NDFInfo ndf;
	initNDFInfo(ndf);

	ndf.coverage = TexCloudNDF.SampleLevel(TileableSampler, float3(ndfUV, 2), 0);
	if (ndf.coverage < 1e-8)
		return ndf;

	const float minHeight = TexCloudNDF.SampleLevel(TileableSampler, float3(ndfUV, 0), 0);
	const float maxHeight = TexCloudNDF.SampleLevel(TileableSampler, float3(ndfUV, 1), 0);
	const float minAltitude = lerp(cloud.lowestAltitude, cloud.highestAltitude, minHeight);
	const float maxAltitude = lerp(cloud.lowestAltitude, cloud.highestAltitude, maxHeight);
	if (maxAltitude <= minAltitude || planet_z < minAltitude || planet_z > maxAltitude)
		return ndf;
	ndf.height_fraction = (planet_z - minAltitude) / max(maxAltitude - minAltitude, 1e-5);

	ndf.in_layer = true;
	ndf.local_height = ndf.height_fraction;
	ndf.top_type = TexCloudNDF.SampleLevel(TileableSampler, float3(ndfUV, 3), 0);
	ndf.bottom_type = TexCloudNDF.SampleLevel(TileableSampler, float3(ndfUV, 4), 0);
	ndf.top_value = TexCloudTopLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, 1.0 - ndf.height_fraction), 0);
	ndf.bottom_value = TexCloudBottomLUT.SampleLevel(TransmittanceSampler, float2(ndf.bottom_type, 1.0 - ndf.height_fraction), 0);
	const float verticalProfile = ndf.top_value * ndf.bottom_value;
	ndf.dimension_profile = saturate(ndf.coverage * verticalProfile);

	return ndf;
}

struct CloudDensityContext
{
	NDFInfo ndf;
	float3 noise_coordinates;
	float2 warp_coordinates;
	float eye_distance;
};

float ReduceNubisErosion(float4 noise, NDFInfo ndf, float eye_distance)
{
	// nubis.dds is the authored 128^3, four-channel "Noise Composite" shown on
	// page 33 of Nubis Evolved. This project's asset stores two wispy erosion
	// variants in R/G and two billowy variants in B/A; it is not an R carrier plus
	// GBA reconstruction texture. Reduce those variants to one scalar according to
	// the dimensional profile.
	const float wispyErosion = lerp(noise.r, noise.g, saturate(ndf.dimension_profile));
	const float billowyGradient = pow(saturate(ndf.dimension_profile), 0.25);
	const float billowyErosion = lerp(noise.b * 0.3, noise.a * 0.3, billowyGradient);
	float erosion = lerp(wispyErosion, billowyErosion, saturate(ndf.bottom_value));

	const float covRamp = saturate((ndf.coverage - 0.2) * 10.0);
	const float relief = noise.a * 0.2 * (1.0 - pow(saturate(ndf.height_fraction * (5.0 + 5.0 * covRamp)), 3.0));
	erosion = max(erosion - relief, 0.0);

	const float distFade = saturate((eye_distance - 1000.0) * 0.001);
	const float smoothErosion = lerp(noise.r, noise.b * 0.3, saturate(ndf.bottom_value));
	return lerp(erosion, smoothErosion, distFade);
}

float sampleCloudDensityFromContext(
	CloudDensityContext density_context, float mip_level, bool include_detail)
{
	const NDFInfo ndf = density_context.ndf;
	if (!ndf.in_layer || ndf.dimension_profile < 1e-8)
		return 0.0;

	const VolumetricCloudData info = VolumetricCloudBuffer[0];

	// Distorts the noise UVs with a 2D warp field that is
	// strongest at the layer bottom (0.125 in noise-UV space below 2% height
	// fraction) and fades to zero by 5%, breaking up the flat base without
	// disturbing the cloud body.
	const float bottomRamp = saturate((ndf.height_fraction - 0.02) * 33.3333);
	const float2 warp = (TexNubisWarp.SampleLevel(TileableSampler, density_context.warp_coordinates, 0).rg * 2.0 - 1.0) *
	                    (0.125 * (1.0 - bottomRamp));
	const float3 noise_uv = density_context.noise_coordinates + float3(warp, 0.0);

	const float baseMip = max(mip_level + saturate(ndf.dimension_profile) * 2.0, 0.0);
	const float4 noise = saturate(TexNubisNoise.SampleLevel(TileableSampler, noise_uv, baseMip));
	float erosionComposite = ReduceNubisErosion(noise, ndf, density_context.eye_distance);

	if (include_detail) {
		const float2 rotated = float2(0.920505 * noise_uv.x - 0.390731 * noise_uv.y, 0.390731 * noise_uv.x + 0.920505 * noise_uv.y);
		const float3 detail_uv = float3(rotated * 0.345, noise_uv.z * 0.3);
		const float4 detail = saturate(TexNubisNoise.SampleLevel(TileableSampler, detail_uv, baseMip + 1.0));
		const float detailExp = 2.0 - 1.5 * saturate((ndf.height_fraction - 0.7) * 3.3333);
		erosionComposite = lerp(erosionComposite, ReduceNubisErosion(pow(max(detail, 1e-5), detailExp), ndf, density_context.eye_distance), 0.35);
	}

	const float cloudNoiseComposite = 1.0 - erosionComposite;
	const float normalizedDensity = saturate(min(ndf.dimension_profile, 0.7) - 1.0 + 0.975 * cloudNoiseComposite);
	return normalizedDensity * info.extinctionCoefficient;
}

float sampleCloudDensity(
	float3 pos, CloudLayer cloud, float mip_level, bool include_detail,
	out CloudDensityContext density_context)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	initNDFInfo(density_context.ndf);
	density_context.noise_coordinates = 0.0;
	density_context.warp_coordinates = 0.0;
	density_context.eye_distance = 0.0;

	const float planetHeight = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planetHeight < cloud.lowestAltitude || planetHeight > cloud.highestAltitude)
		return 0.0;

	const float layerFraction = saturate((planetHeight - cloud.lowestAltitude) / max(cloud.highestAltitude - cloud.lowestAltitude, 1e-5));
	const float2 heightShift = info.noiseHeightShear * layerFraction;
	const float2 ndfUV = LowNdfUV(pos.xy, info);
	density_context.ndf = sampleNDF(cloud, ndfUV, planetHeight);
	if (!density_context.ndf.in_layer || density_context.ndf.dimension_profile < 1e-8)
		return 0;

	const float3 shiftedPosition = pos + float3(heightShift, 0.0);
	density_context.noise_coordinates = shiftedPosition * info.noiseFrequency + info.noiseOffset -
	                                    float3(info.noiseWindOffset, 0.0) * info.noiseFrequency;
	density_context.warp_coordinates = pos.xy * info.warpFrequency;
	density_context.eye_distance = length(pos) * GAME_UNIT_TO_M;
	return sampleCloudDensityFromContext(density_context, mip_level, include_detail);
}

float3 sampleExternalSunTransmittance(float3 pos, float3 sun_dir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);

	[branch] if (RayIntersectSphereCentered(pos_planet, sun_dir, info.planetRadius) > 0.0) return 0.0;

	// Scene CSM coverage is camera-relative and terminates at a finite split. When
	// sampled at the view ray's entry endpoint, that split is projected from the
	// low-cloud base across the whole march and appears as a camera-following direct
	// light boundary. The reference cloud lighting contains planet occlusion and
	// smooth atmospheric attenuation here; cloud self-shadowing is integrated by
	// sampleCloudSelfShadow. Terrain and scene shadows remain on their receivers.
	return TexTransmittance.SampleLevel(TransmittanceSampler, TrLutUvPlanet(pos_planet, sun_dir), 0).rgb;
}

// Evaluate only the cloud column here. Smooth atmospheric attenuation is sampled
// at the ray endpoints and interpolated in RenderVolumetricCloudRay, matching the
// reference environment-lighting setup.
void sampleCloudSelfShadow(
	float3 pos, float local_height, float3 sun_dir, uint visibility_step, float cone_jitter,
	out float light_extinction_od, out float phi_fwd)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	light_extinction_od = 0.0;
	phi_fwd = 0.0;

	// cloud self-shadowing
	{
		visibility_step = max(visibility_step, 1u);
		const static float cone_ratio = 2.0;
		const static float cone_min_step = 5.0 * GAME_UNITS_PER_METER;
		const static float cone_max_distance = 6000.0 * GAME_UNITS_PER_METER;
		float kappa_od_sum = 0.0;
		float diffuse_survival = 1.0;
		float cover_dist = min(CloudLightExitDistance(pos, sun_dir, info.lowCloudTraceTopAltitude, info), cone_max_distance);
		float step_width = max(cover_dist * (cone_ratio - 1.0) / max(exp2((float)visibility_step) - 1.0, 1e-4), cone_min_step);
		float cum_dist = 0.0;
		const bool evaluate_phi = info.phiFwdIntensity > 0.0;
		float source_confidence = 0.0;
		[branch] if (evaluate_phi)
		{
			const float bottom_soft_height = 0.08;
			const float bottom_height = local_height + info.phiFwdDepthBias;
			const float bottom_confidence = info.phiFwdDepthPow > 0.0 ?
			                                    1.0 - exp(-max(bottom_height, 0.0) / bottom_soft_height * info.phiFwdDepthPow) :
			                                    1.0;
			source_confidence = EvaluateCloudBoundaryLight(pos, sun_dir) * bottom_confidence;
		}

		for (uint i = 0; i < visibility_step; i++) {
			float width = min(step_width, cover_dist - cum_dist);
			if (width <= 0.0)
				break;
			// A single stratified offset shared by the whole cone replaces the fixed
			// midpoint. Sampling always at the centre of a geometrically growing step
			// produces fixed-position banding in the self-shadow term; offsetting it
			// with stratified noise turns that bias into temporally resolvable noise. One
			// offset per cone (rather than per step) keeps the added variance low.
			float dist = cum_dist + width * cone_jitter;
			float3 vis_pos = pos + sun_dir * dist;
			CloudDensityContext _;
			const float mip_offset = (float)i / max((float)(visibility_step - 1u), 1.0) * 3.0;
			const float density = sampleCloudDensity(vis_pos, cloud, mip_offset, true, _);
			[branch] if (density > 0.0)
			{
				const float width_m = width * GAME_UNIT_TO_M;
				const float sigma_t_per_game_unit = density * dot(cloud.scatter + cloud.absorption, float3(0.2126, 0.7152, 0.0722));
				const float sigma_t_per_meter = sigma_t_per_game_unit * GAME_UNITS_PER_METER;
				const float local_od = sigma_t_per_meter * width_m;
				[branch] if (evaluate_phi)
				{
					const float dist_m = dist * GAME_UNIT_TO_M;
					const float q_source = sigma_t_per_meter * 0.999 * width_m;
					const float kappa_step = local_od * sqrt(3.0 * (1.0 - 0.999));
					const float kappa_to_center = kappa_od_sum + kappa_step * 0.5;
					const float ms_build = 1.0 - exp(-(light_extinction_od + local_od * 0.5) * max(info.phiFwdMSBuildScale, 0.0));
					const float inv_r = 1.0 / max(dist_m, width_m * 0.5);
					phi_fwd += diffuse_survival * q_source * sigma_t_per_meter * source_confidence * ms_build * exp(-kappa_to_center) * inv_r;
					kappa_od_sum += kappa_step;
					diffuse_survival *= exp(-local_od * (1.0 - 0.999));
				}
				light_extinction_od += local_od;
			}
			cum_dist += width;
			step_width *= cone_ratio;
		}

		// Far range (hybrid shadow volume): when the cone endpoint is represented by
		// the finite shadow volume, its prefix supplies the remaining known light
		// column without overlapping the cone march. Outside that domain there is no
		// cloud-density data, so no synthetic average-density occluder is introduced.
		// The volume stores density * path length in game units.
		const float3 tail_pos = pos + sun_dir * cum_dist;
		const float3 tail_uvw = GetShadowVolumeSampleUvw(tail_pos, sun_dir, info);
		light_extinction_od += CloudShadowVolume::SampleDensity(TexShadowVolume, tail_uvw) * GAME_UNIT_TO_M;
	}
}

float EvaluateHighCloudDensity(float3 pos, out float normalizedHeight, out float4 hiWeather)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	normalizedHeight = 0.0;
	hiWeather = 0.0;
	if (info.highCloudEnabled <= 0.0)
		return 0.0;
	float planetZ = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planetZ < info.highCloudBottom || planetZ > info.highCloudTop)
		return 0.0;
	normalizedHeight = saturate((planetZ - info.highCloudBottom) /
								max(info.highCloudTop - info.highCloudBottom, GAME_UNITS_PER_METER));
	float2 uv = CloudWeatherUV(pos.xy, info);
	hiWeather = TexHpHighWeather.SampleLevel(TileableSampler, uv, 0);
	float hiCoverage = hiWeather.r;
	float hiType = hiWeather.g;
	if (hiCoverage < 0.001)
		return 0.0;
	// The weather UV already advects with the wind, so the cell, warp, and wisp
	// patterns travel with the broad weather field. This offset is an additional
	// drift relative to that field.
	float2 hiWindUV = info.noiseWindOffset / max(info.weatherWorldSize, 1.0);
	float2 cellUV = uv * info.highCellScale + hiWindUV * info.highCellWindSpeed;
	float2 warpUV = uv * info.highCellWarpScale + hiWindUV * info.highCellWindSpeed * 0.5;
	float2 warp = (TexHpHighWarp.SampleLevel(TileableSampler, warpUV, 0).rg * 2.0 - 1.0) * info.highCellWarpStrength;
	float hiCell = saturate(TexHpHighCell.SampleLevel(TileableSampler, cellUV + warp, 0).r);
	float hiCellShaped = CloudPositivePow(max(hiCell, 0.001), max(info.highCellThickPow, 0.01));
	float hiCellThick = lerp(info.highAsCellThickStrength, info.highCellThickStrength, hiType);
	float hiCoverForHeight = CloudPositivePow(hiCoverage, max(info.highHeightCurvePow, 0.01));
	float hiDrivenTop = hiCoverForHeight;
	float hiTop = hiDrivenTop * lerp(1.0, hiCellShaped, hiCellThick * 0.5);
	float hiBottom = 0.0;
	hiTop = saturate(hiTop + info.highBottomCoverageScale * hiCoverForHeight * hiType);
	float band = smoothstep(hiBottom - info.highCloudSoftness, hiBottom + info.highCloudSoftness, normalizedHeight) * (1.0 - smoothstep(hiTop - info.highCloudSoftness, hiTop + info.highCloudSoftness, normalizedHeight));
	float wisp = TexHpHighWisp.SampleLevel(TileableSampler, uv * info.highWispScale + hiWindUV * info.highCellWindSpeed, 0).r;
	wisp = saturate(wisp * wisp);
	float soft = info.highDensitySoftness * (1.0 - CloudPositivePow(saturate(hiWeather.a), max(info.highDensitySoftAContrast, 0.01)));
	float density = saturate(CloudDensityRemap(hiCoverage, info.highDensityThreshold, info.highDensityThreshold + max(soft, 0.001), 0.0, 1.0));
	density = (density * lerp(1.0, hiCellShaped, hiCellThick) - wisp * info.highWispStrength * hiType) * band;
	density *= 1.0 - saturate(info.highDensityModAIntensity * (1.0 - CloudPositivePow(saturate(hiWeather.a), max(info.highDensityModAContrast, 0.01))));
	return max(0.0, density * info.highDensityMultiplier);
}

float EvaluateHighCloudDensity(float3 pos, out float normalizedHeight)
{
	float4 _;
	return EvaluateHighCloudDensity(pos, normalizedHeight, _);
}

// ---------------------------------------------------------------------------
// Cloud phase function models.
//   0 = dual-lobe Henyey-Greenstein (original behaviour, forward + backward g)
//   1 = approximate Mie: HG + Draine numerical fit for a 5 um droplet
//       (Jendersie & d'Eon 2023, constants as used by three-geospatial).
//       This model is physically parameterised, so the forward/backward
//       eccentricity sliders do not apply to it.
// Each octave of the multiple-scattering sum reuses the same model with its
// anisotropy attenuated by msEccentricity^octave (Frostbite).
// ---------------------------------------------------------------------------
#define PHYSKY_CLOUD_PHASE_HG_DUAL_LOBE 0u
#define PHYSKY_CLOUD_PHASE_APPROX_MIE 1u

// Per-step in-scattering integral.
//   0 = legacy scalar (1 - exp(-sigma_s * ds)) driven by luminance extinction
//   1 = Frostbite 5.6.3 energy-conserving per-channel albedo * (1 - T)
#define PHYSKY_CLOUD_SCATTER_INTEGRAL_LEGACY 0u
#define PHYSKY_CLOUD_SCATTER_INTEGRAL_ENERGY_CONSERVING 1u

float CloudPhaseSingle(float cos_theta, float fwd_g, float bwd_g, float anisotropy, uint model)
{
	// Numerical fit of Mie scattering for a water droplet (Jendersie & d'Eon 2023);
	// the constants are the ones three-geospatial uses for cloud droplets.
	const float kMieHgG = 0.988176691700256;
	const float kMieDraineG = 0.5556712547839497;
	const float kMieDraineAlpha = 21.995520856274638;
	const float kMieDraineWeight = 0.4819554318404214;

	// Both models are evaluated unconditionally: this runs once per ray as a
	// precalculation, not per march step, and a single return keeps the compiler
	// from reporting the whole call chain as possibly uninitialized.
	const float mie = lerp(
		Phase::HG(cos_theta, kMieHgG * anisotropy),
		Phase::Draine(cos_theta, kMieDraineG * anisotropy, kMieDraineAlpha),
		kMieDraineWeight);
	const float dual_lobe = Phase::HG(cos_theta, fwd_g * anisotropy) + Phase::HG(cos_theta, -bwd_g * anisotropy);
	return model == PHYSKY_CLOUD_PHASE_APPROX_MIE ? mie : dual_lobe;
}

float3 CloudPhaseOctaves(float cos_theta, float fwd_g, float bwd_g, float ms_eccentricity, uint model)
{
	const float a1 = ms_eccentricity;
	const float a2 = ms_eccentricity * ms_eccentricity;
	return float3(
		CloudPhaseSingle(cos_theta, fwd_g, bwd_g, 1.0, model),
		CloudPhaseSingle(cos_theta, fwd_g, bwd_g, a1, model),
		CloudPhaseSingle(cos_theta, fwd_g, bwd_g, a2, model));
}

// Distance LOD for the secondary (light) march. Far clouds contribute a few
// pixels each, so paying the full light-cone budget there is wasted work. The
// range matches the erosion mip ramp in sampleCloudDensity so both LODs move
// together. `jitter` performs stochastic rounding, which keeps the expected step
// count continuous and prevents a visible LOD ring in the distance.
uint CloudLightStepCount(float eye_dist, float jitter, VolumetricCloudData info)
{
	const float kLightLodStartMeters = 3000.0;
	const float kLightLodEndMeters = 100000.0;
	const uint base_steps = max(info.lightSteps, 1u);
	const float eye_dist_m = max(eye_dist, 0.0) * GAME_UNIT_TO_M;
	// A zero LOD scale collapses to lod = 0, which reproduces the fixed budget, so
	// no separate early-out branch is needed.
	const float lod = saturate((eye_dist_m - kLightLodStartMeters) / (kLightLodEndMeters - kLightLodStartMeters)) *
	                  saturate(info.lightStepDistanceLod);
	const float steps_f = lerp((float)base_steps, 1.0, lod);
	return (uint)clamp(floor(steps_f + jitter), 1.0, (float)base_steps);
}

struct VolumetricCloudResult
{
	float3 transmittance;
	float3 lum;
	float cloud_depth;
	float reject_depth;
	float scatter_weight;
};

VolumetricCloudResult RenderVolumetricCloudRay(float3 ray_dir, float3 eye_pos, float solid_dist, bool is_sky, float jitter, float2 light_jitter, float ap_shadow)
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
	const float3 cloud_phase = CloudPhaseOctaves(
		cos_theta, info.forwardEccentricity, info.backwardEccentricity, info.msEccentricity, info.cloudPhaseModel);
	const float3 ms_attenuation = float3(1.0, info.msAttenuation, info.msAttenuation * info.msAttenuation);
	const float3 ms_contribution = float3(1.0, info.msContribution, info.msContribution * info.msContribution);
	// Collapsing the 3D diffusion integral to a single light ray removes the dA
	// part of dV. Use one reference transport mean-free-path squared as that
	// effective area. Besides restoring the missing m^2, this removes the accidental
	// quadratic dependence of the diffuse-field amplitude on the authored peak
	// extinction coefficient. The remaining OD dependence is retained in the
	// source survival, MS build-up and Green-kernel attenuation terms.
	const float phi_reference_extinction = max(info.extinctionCoefficient, 1e-4);
	const float phi_transport_area = rcp(phi_reference_extinction * phi_reference_extinction);

	///////////// ray march
	float low_mean_weight = 0.0;
	float low_mean_depth = 0.0;
	bool low_valid = false;
	// These directions and multipliers are constant for the entire ray. HP prepares
	// its environment lighting before marching; sampling them inside every dense
	// step needlessly multiplied the ambient texture cost.
	const float3 ambient_sky_top = SampleCloudAmbientSkyView(float3(0, 0, 1));
	const float3 ambient_sky_bottom = SampleCloudAmbientSkyView(float3(0, 0, -1));
	// HP/HDRP evaluate environment lighting once for the ray endpoints. Keep that
	// evaluation lazy so rays that only cross empty weather regions pay nothing.
	bool external_sun_ready = false;
	float3 external_sun_start = 1.0;
	float3 external_sun_end = 1.0;

	// Distance-adaptive coarse probing followed by a quarter-size integration step
	// after a density hit. This is deliberately distance driven rather
	// than a fixed step budget so near, thin clouds cannot be skipped wholesale.
	const float step_large_raw = ray.march_dist / max((float)info.cloudMaxStep, 1.0);
	const float cloudAltitudeRange = GetCloudAltitudeRange(info);
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

		// Probe the same Nubis composite one mip coarser. Once it enters cloud,
		// retain the fine step for the full-resolution composite evaluation.
		CloudDensityContext density_context;
		const float simple_density = sampleCloudDensity(pos, cloud, 1.0, false, density_context);
		[branch] if (simple_density > 0.001)
		{
			// The coarse and full probes are at the same position. Reuse the NDF,
			// profile LUT result, height and noise coordinates; only the Nubis mip
			// differs between the two evaluations.
			const float cloud_density = sampleCloudDensityFromContext(density_context, 0.0, true);
			const NDFInfo ndf = density_context.ndf;
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
				const uint light_steps = CloudLightStepCount(absolute_dist, light_jitter.y, info);
				sampleCloudSelfShadow(pos, ndf.local_height, cloud_light_dir, light_steps, light_jitter.x, light_extinction_od, phi_fwd);
				const float relative_ray_distance = saturate(dist / max(ray.march_dist, 1.0));
				const float3 external_sun = lerp(external_sun_start, external_sun_end, relative_ray_distance);
				float3 directional_lum = 0.0;
				[unroll] for (uint octave = 0; octave < 3; ++octave)
				{
					directional_lum += exp(-info.scatterTint * light_extinction_od * ms_attenuation[octave]) * cloud_phase[octave] * ms_contribution[octave];
				}
				const float extinction_scalar = dot(extinction, float3(0.2126, 0.7152, 0.0722));
				const float scatter_od = extinction_scalar * 0.999 * step_small;
				float scatter_gate = 1.0 - exp(-scatter_od / max(info.scatterSourceODScale, 0.001));
				scatter_gate = CloudPositivePow(saturate(scatter_gate), max(info.scatterSourceCurvePow, 0.01));
				const float3 sample_transmittance = exp(-step_small * extinction);

				// The remapped gate is an edge-validity control, not the Beer-Lambert
				// integral itself. It multiplies whichever scattered fraction is chosen
				// below, so both direct and ambient radiance stay bounded by this
				// sample's optical depth.
				float3 scatter_source;
				[branch] if (info.scatterIntegration == PHYSKY_CLOUD_SCATTER_INTEGRAL_ENERGY_CONSERVING)
				{
					// Frostbite 5.6.3 analytical integration of in-scattered light over
					// the step: integral(S * sigma_s * exp(-sigma_t * t), t = 0..ds)
					//   = S * (sigma_s / sigma_t) * (1 - T).
					// sigma_s / sigma_t is the single-scattering albedo; the density
					// factor cancels, so it is a per-channel constant of the medium.
					// Unlike the legacy form this is evaluated per channel, which keeps
					// the scattered colour consistent with the coloured transmittance
					// instead of driving it from a luminance-collapsed extinction.
					const float3 albedo = cloud.scatter / max(cloud.scatter + cloud.absorption, 1e-7);
					scatter_source = albedo * (1.0 - sample_transmittance) * scatter_gate;
				}
				else
				{
					// Legacy: scalar (1 - exp(-sigma_s * ds)) with a hard-coded 0.999
					// albedo. Agrees with the form above to first order and in the
					// optically thick limit, and differs mainly by being achromatic.
					scatter_source = (1.0 - exp(-scatter_od)) * scatter_gate;
				}
				float3 in_scatter = directional_lum * external_sun * dirlightColor * scatter_source;

				// Additive isotropic diffuse field, independent from directional multiple scattering.
				// phi_fwd is a 1D collapse of the 3D Green integral and therefore still
				// carries 1 / m^2 after the ds quadrature. The effective transport area
				// above supplies the missing transverse measure. As in the HP reference,
				// constants such as 3 / (4 pi) are absorbed into the unit-scale intensity.
				float phiScalar = info.phiFwdIntensity * phi_fwd * phi_transport_area;
				phiScalar = info.phiFwdCompress > 0.0 ? (1.0 - exp(-phiScalar * info.phiFwdCompress)) / info.phiFwdCompress : phiScalar;
				const float3 phi_luminance = phiScalar * external_sun * dirlightColor;
				in_scatter += phi_luminance * (1.0 - sample_transmittance);

				// Evaluate top and bottom ambient independently. The solar optical depth
				// provides the upward AO proxy used by the reference implementation.
				const float upward_ao = exp(-light_extinction_od * max(cloud_light_dir.z, 0.05) * max(info.aoUpwardScale, 0.0));
				const float3 ambient_term = EvaluateCloudEnvironmentRadiance(
					ambient_sky_top, ambient_sky_bottom, ndf.height_fraction,
					info.ambientTopMultiplier, info.ambientBottomMultiplier, upward_ao);
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
	const float3 camera_planet = ray.eye_pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float high_cloud_inner_radius = info.planetRadius + info.highCloudBottom;
	const bool camera_at_or_above_high_clouds = dot(camera_planet, camera_planet) >= high_cloud_inner_radius * high_cloud_inner_radius;
	// Below the high layer, an already opaque low-cloud result completely masks
	// the high layer in the final low + T_low * high composition. Preserve the
	// existing transmittance cutoff and avoid an otherwise wasted nested high-cloud
	// view/light march.
	const bool high_fully_occluded = !camera_at_or_above_high_clouds && ray.transmittance.x <= 0.0;
	// Match HP's whole-ray high-cloud gate. Most rays cross weather-map regions
	// without high coverage and should not pay for 2 * primarySteps density tests.
	float high_gate_coverage = 0.0;
	[branch] if (info.highCloudEnabled > 0.0 && !high_fully_occluded)
	{
		const float2 high_gate_uv = CloudWeatherUV(ray.start_pos.xy, info);
		high_gate_coverage = TexHpHighWeather.SampleLevel(TileableSampler, high_gate_uv, 2).r;
	}
	if (info.highCloudEnabled > 0.0 && high_gate_coverage > 0.001 && !high_fully_occluded) {
		const float3 lowLum = ray.lum;
		const float3 lowTransmittance = ray.transmittance;
		float3 highLum = 0.0;
		float3 highTransmittance = 1.0;
		const float hiCos = dot(ray.ray_dir, cloud_light_dir);
		const float3 hiPhase = float3(
			Phase::HG(hiCos, info.highForwardEccentricity) + Phase::HG(hiCos, -info.highBackwardEccentricity),
			Phase::HG(hiCos, info.highForwardEccentricity * info.highMSEccentricity) + Phase::HG(hiCos, -info.highBackwardEccentricity * info.highMSEccentricity),
			Phase::HG(hiCos, info.highForwardEccentricity * info.highMSEccentricity * info.highMSEccentricity) + Phase::HG(hiCos, -info.highBackwardEccentricity * info.highMSEccentricity * info.highMSEccentricity));
		const float3 hiMsAttenuation = float3(1.0, info.highMSAttenuation, info.highMSAttenuation * info.highMSAttenuation);
		const float3 hiMsContribution = float3(1.0, info.highMSContribution, info.highMSContribution * info.highMSContribution);
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
			float4 hiWeather;
			float hiDensity = EvaluateHighCloudDensity(hiPos, hiNormH, hiWeather);
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
			const float hiMsWeight = hiWeather.a;
			float3 hiExtinction = hiDensity * info.highViewAbsorption * hiMsWeight * GAME_UNIT_TO_M;
			float3 hiTransmittance = exp(-hiExtinction * hiStep);
			const float3 hiExternalSun = lerp(external_sun_start, external_sun_end, saturate(hiDist / max(ray.march_dist, 1.0)));
			float hiExtinctionSum = 0.0;
			const float hiLightDistance = min(CloudLightExitDistance(hiPos, cloud_light_dir, info.highCloudTop, info), 3000.0 * GAME_UNITS_PER_METER);
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
				hiDirectionalLum += exp(-hiLightExtinction * hiMsAttenuation[octave]) * hiPhase[octave] * hiMsContribution[octave];
			}
			const float powder = CloudPowderEffect(hiDensity, hiCos, info.powderIntensity);
			hiDirectionalLum *= powder;
			const float hiUpwardOD = dot(hiLightExtinction, float3(0.2126, 0.7152, 0.0722)) * max(cloud_light_dir.z, 0.05);
			const float hiUpwardTransmittance = exp(-hiUpwardOD * max(info.aoUpwardScale, 0.0));
			const float3 hiAmbient = EvaluateCloudEnvironmentRadiance(
				ambient_sky_top, ambient_sky_bottom, hiNormH,
				info.highAmbientTopMultiplier, info.highAmbientBottomMultiplier, hiUpwardTransmittance);
			float3 hiLum = (hiExternalSun * dirlightColor * hiDirectionalLum + hiAmbient) * hiMsWeight;
			// A value of one is the neutral physical baseline. Values below one opt
			// into the legacy artistic blend toward the view-direction sky probe.
			const float hiEnvironmentBlend = smoothstep(0.0, 1.0, hiNormH) * (1.0 - saturate(info.highSkyBlendStrength));
			hiLum = lerp(hiLum, hiSkyBlend, hiEnvironmentBlend);
			float3 hiIntegral = hiLum * (1.0 - hiTransmittance);
			highLum += hiIntegral * highTransmittance;
			highTransmittance *= hiTransmittance;
			if (highTransmittance.x < 0.003) {
				highTransmittance = 0.0;
				break;
			}
		}

		if (high_valid) {
			if (camera_at_or_above_high_clouds)
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
	// Decorrelated from the ray-start offset so the primary march and the light
	// cone do not share the same error pattern. x offsets the light cone samples,
	// y drives the stochastic rounding of the light step count.
	const float2 light_jitter = float2(
		CloudSpatiotemporalNoise(intermediate_coord, frame_index >> 2u, 1u),
		CloudSpatiotemporalNoise(intermediate_coord, frame_index >> 2u, 2u));

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
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, solid_dist, is_sky, ray_jitter, light_jitter, ap_shadow);

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

	// Sky capture has no temporal integration, so its ray starts at the slab boundary
	// and the light cone keeps its unjittered midpoint sampling.
	const float ray_jitter = 0.0;
	const float2 light_jitter = 0.5;

	const float3 eye_pos = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float3 ray_dir = GetCubemapSamplingVector(output_tid, RWTexCubeTr);
	// Match the main view: fog cloud radiance at its mean depth before the cubemap
	// is composited over the sky. Cubemap capture has no screen-space AP shadow,
	// so zero selects the unshadowed LUT sample while retaining AP transmittance
	// and in-scattering.
	VolumetricCloudResult result = RenderVolumetricCloudRay(ray_dir, eye_pos, info.rayMarchRange, true, ray_jitter, light_jitter, 0.0);

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

#define NTHREADS 256
groupshared float g_density[NTHREADS];

// Accumulate the low-cloud extinction column along the light direction into the
// camera-centred shadow volume. Each thread group walks one light ray through the
// volume with a parallel prefix sum.
[numthreads(NTHREADS, 1, 1)] void renderShadowVolume(const uint gtid : SV_GroupThreadID, const uint2 gid : SV_GroupID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const CloudLayer cloud = GetCloudLayer(info);

	uint3 dims;
	RWShadowVolume.GetDimensions(dims.x, dims.y, dims.z);
	const float3 rcp_dims = 1.0 / float3(dims);
	const float shadow_thickness = max(info.shadowVolumeTop - info.shadowVolumeBottom, 1.0);
	const float3 scale = float3(info.shadowVolumeRange.xx, shadow_thickness);
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

	g_density[gtid] = 0.0;
	if (is_valid) {
		const float3 pos = float3(FrameBuffer::CameraPosAdjust.xy + (thread_uv.xy - 0.5) * info.shadowVolumeRange, info.shadowVolumeBottom + shadow_thickness * thread_uv.z);

		// Fetch only density represented inside this finite shadow volume. Prefixes
		// start with zero extinction at the boundary instead of assuming a uniform
		// cloud shell outside the represented domain.
		CloudDensityContext _;
		float density = sampleCloudDensity(pos, cloud, 2, false, _) * length(ray_uv_increment * scale);  // scaled by ray length

		g_density[gtid] = density;
	}
	GroupMemoryBarrierWithGroupSync();

	// parallel summation
	[unroll] for (uint offset = 1; offset < NTHREADS; offset <<= 1)
	{
		float accumulated_density = g_density[gtid];
		if (is_valid && gtid >= offset) {
			if (all(floor(raw_thread_uv - ray_uv_increment * offset) == floor(raw_thread_uv)))  // no wraparound happened
			{
				accumulated_density += g_density[gtid - offset];
			}
		}
		// All lanes must finish reading the previous scan step before any writes.
		GroupMemoryBarrierWithGroupSync();
		g_density[gtid] = accumulated_density;
		GroupMemoryBarrierWithGroupSync();
	}

	// save
	if (is_valid) {
		// Every voxel is rebuilt deterministically. The camera-centred grid moves,
		// so blending the same index from the previous frame would trail shadows.
		RWShadowVolume[thread_px_coord] = g_density[gtid];
	}
}
