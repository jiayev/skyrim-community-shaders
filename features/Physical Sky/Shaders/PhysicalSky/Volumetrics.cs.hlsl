// Community Shaders
// Authors: Profjack, Jiaye
//
// References: Nubis Evolved (2022), Nubis Cubed (2023).

#define PHYSICAL_SKY_VOLUMETRICS
#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#define PS_SKY_SAMPLERS
#define PS_PREPASS_RSRCS
#define PS_NO_RSRCS
#define OMIT_PS_NAMESPACE
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "PhysicalSky/CloudNoise.hlsli"
#include "PhysicalSky/Common.hlsli"

static const float GAME_UNITS_PER_METER = 1.0 / GAME_UNIT_TO_M;
// Separate scene/sky classification from the distance spent marching cloud.
// 10000 km is exactly representable in the R16F auxiliary buffer.
static const float CLOUD_SKY_DEPTH_KM = 10000.0;
static const float CLOUD_SKY_DISTANCE = CLOUD_SKY_DEPTH_KM * 1000.0 * GAME_UNITS_PER_METER;

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
};

struct VolumetricCloudData
{
	float rayMarchRange;
	float shadowVolumeRange;
	uint lowViewSteps;
	uint cloudFrameIndex;
	float2 rcpFrameDim;
	float3 dirlightDir;
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
	float noiseRoundness;
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
	float highDensitySoftAContrast;
	float highDensityModAIntensity;
	float highDensityModAContrast;
	float3 scatterTint;
	float forwardEccentricity;
	float backwardEccentricity;
	float ambientTopMultiplier;
	float ambientBottomMultiplier;
	float aoUpwardScale;
	float msDepthPower;
	float msContribution;
	float msEccentricity;
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
	uint historyValid;
	float temporalAccumulationFactor;
	float cloudHistoryInvalidation;
	float shadowVolumeBottom;
	float shadowVolumeTop;
	float2 cloudWindDelta;
	float2 cloudShapeShear;
	float lowHistoryConfidence;
	float highHistoryConfidence;
	uint highViewSteps;
	uint ndfAccelerationValid;
	uint crossLayerShadows;
	uint lightCacheSteps;
	float msHeightPower;
	float2 lightCacheOrigin;
	float2 lightCacheWindDelta;
	float lightCacheRange;
	uint lightCacheUpdatePhase;
	row_major float4x4 previousViewProj;
	float3 previousCamera;
	float2 previousFrameDim;
};

CloudLayer GetCloudLayer(VolumetricCloudData info)
{
	CloudLayer cloud;
	cloud.lowestAltitude = info.lowCloudBaseAltitude;
	cloud.highestAltitude = info.lowCloudTopAltitude;
	return cloud;
}

StructuredBuffer<VolumetricCloudData> VolumetricCloudBuffer : register(t0);
Texture2D<float4> TexTransmittance : register(t1);
Texture2D<float4> TexMultiScatter : register(t2);
Texture3D<float4> TexAerialPerspective : register(t3);

Texture2D<float> TexDepth : register(t4);

Texture3D<unorm float4> TexNubisNoise : register(t5);
Texture3D<float4> TexAerialPerspectiveSun : register(t6);
Texture2D<float2> TexCloudHeight : register(t7);
Texture2D<float3> TexCloudModeling : register(t8);
Texture2D<unorm float> TexApShadow : register(t9);
Texture2D<float4> TexSkyView : register(t10);
Texture2D<float4> TexHpHighWeather : register(t11);
Texture2D<float> TexCloudDistance : register(t12);
Texture2D<float4> TexHpHighCell : register(t13);
Texture2D<float4> TexHpHighWarp : register(t14);
Texture2D<float4> TexHpHighWisp : register(t15);
Texture2D<sh2> TexCloudAmbientSH : register(t16);
Texture2D<unorm float> TexCloudTopLUT : register(t17);
Texture2D<unorm float> TexCloudBottomLUT : register(t18);

Texture3D<float> TexShadowVolume : register(t23);
Texture3D<float2> TexLowLightCache : register(t24);
Texture3D<float2> TexHighLightCache : register(t25);
Texture2D<float> TexVolHistoryTr : register(t26);
Texture2D<float3> TexVolHistoryLum : register(t27);
Texture2D<float4> TexVolHistoryAux : register(t28);
Texture2D<float> TexVolLowTr : register(t29);
Texture2D<float3> TexVolLowLum : register(t30);
Texture2D<float4> TexVolLowAux : register(t31);
TextureCube<float> TexCubeHistoryTr : register(t32);
TextureCube<float3> TexCubeHistoryLum : register(t33);
TextureCube<float4> TexCubeHistoryAux : register(t34);
TextureCube<float> TexCubeTraceTr : register(t35);
TextureCube<float3> TexCubeTraceLum : register(t36);
TextureCube<float4> TexCubeTraceAux : register(t37);

float3 GetCloudDirectionalLightColor()
{
	// Top-of-atmosphere irradiance, before ground sunset dimming or attenuation.
	if (SharedData::physSkyData.volCloudUseSun != 0)
		return SharedData::physSkyData.sunlightColor;
	return Color::Light(SharedData::DirLightColor.xyz);
}

RWTexture2D<float> RWTexTr : register(u0);
RWTexture2D<float3> RWTexLum : register(u1);
RWTexture2D<float4> RWTexAux : register(u2);

RWTexture3D<float> RWShadowVolume : register(u0);
RWTexture3D<float2> RWCloudLightCache : register(u0);

RWTexture2DArray<float> RWTexCubeTr : register(u0);
RWTexture2DArray<float3> RWTexCubeLum : register(u1);
RWTexture2DArray<float4> RWTexCubeAux : register(u2);
RWTexture2D<sh2> RWCloudAmbientSH : register(u0);

float RayIntersectSphereCentered(float3 orig, float3 dir, float r)
{
	return RayIntersectSphere(orig, dir, 0, r);
}

float3 SampleCloudAmbientSkyView(float3 viewDir)
{
	const float3 shViewDir = float3(viewDir.x, viewDir.z, viewDir.y);
	// Convolve sky radiance with the cloud phase before evaluating ambient light.
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
	const float3 upper = ambientTop * max(topMultiplier, 0.0);
	return lerp(lower, upper, height) * saturate(upwardTransmittance);
}

float SampleCloudApShadow(uint2 fullPixelCoord)
{
	uint2 apDims;
	TexApShadow.GetDimensions(apDims.x, apDims.y);
	if (any(apDims == 0u))
		return 0.0;
	const uint2 apCoord = min(fullPixelCoord, apDims - 1u);
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

struct CloudRaySegments
{
	float2 nearSegment;
	float2 farSegment;
	float nearLength;
	float length;
};

CloudRaySegments GetCloudRaySegments(float3 origin, float3 dir, float bottomAltitude, float topAltitude,
	float sceneDistance, VolumetricCloudData info)
{
	CloudRaySegments segments = (CloudRaySegments)0;
	if (topAltitude <= bottomAltitude)
		return segments;
	const float2 outer = IntersectSpherePair(origin, dir, info.planetRadius + topAltitude);
	const float entry = max(outer.x, 0.0);
	const float exit = min(outer.y, sceneDistance);
	if (exit <= entry)
		return segments;

	const float2 inner = IntersectSpherePair(origin, dir, info.planetRadius + bottomAltitude);
	segments.nearSegment = float2(entry, exit);
	// Subtract the inner sphere. A grazing ray can cross the shell twice without
	// hitting the planet; keep both pieces and jump over the clear gap between them.
	if (inner.y > entry && inner.x < exit) {
		segments.nearSegment.y = clamp(inner.x, entry, exit);
		segments.farSegment = float2(clamp(inner.y, entry, exit), exit);
		if (segments.nearSegment.y <= entry) {
			segments.nearSegment = segments.farSegment;
			segments.farSegment = 0.0;
		}
	}
	// The range limits distance inside this layer, not distance from the camera.
	segments.nearLength = min(segments.nearSegment.y - segments.nearSegment.x, info.rayMarchRange);
	segments.nearSegment.y = segments.nearSegment.x + segments.nearLength;
	const float farLength = min(segments.farSegment.y - segments.farSegment.x, info.rayMarchRange - segments.nearLength);
	segments.farSegment.y = segments.farSegment.x + farLength;
	segments.length = segments.nearLength + farLength;
	return segments;
}

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

// The weather map is a seamlessly tiling synoptic pattern rather than a finite
// rectangle, so it is sampled with a wrapping sampler and never needs a bounds
// test. Subtracting the accumulated wind displacement advects the whole cloud
// field downwind, matching the 3D volume coordinates that use the same offset.
float2 CloudWeatherUV(float2 worldXY, VolumetricCloudData info)
{
	return (worldXY - info.weatherCenter - info.noiseWindOffset) / max(info.weatherWorldSize, 1.0) + 0.5;
}

// The NDF has five independent shape attributes. Its physical scale
// is independent of both the high-cloud weather map and the 3D detail noise.
float2 LowNdfUV(float2 worldXY, VolumetricCloudData info)
{
	return (worldXY - info.noiseWindOffset) * info.lowNdfFrequency + 0.5;
}

float CloudEmptyDistance(float3 pos, float3 direction, VolumetricCloudData info)
{
	if (info.ndfAccelerationValid == 0u)
		return 0.0;
	uint2 dims;
	TexCloudDistance.GetDimensions(dims.x, dims.y);
	const float2 uv = frac(LowNdfUV(pos.xy, info));
	const uint2 pixel = min(uint2(uv * dims), dims - 1u);
	const float2 cellSize = rcp(float2(dims) * info.lowNdfFrequency);
	const float emptyRadius = max(TexCloudDistance[pixel] * min(cellSize.x, cellSize.y) - length(info.cloudShapeShear), 0.0);
	return emptyRadius / max(length(direction.xy), 1e-6);
}

float CloudViewStep(float distance)
{
	return (3.0 + distance * GAME_UNIT_TO_M * (60.0 / 16384.0)) * GAME_UNITS_PER_METER;
}

float CloudBudgetStep(float distance, float remaining, uint budget)
{
	const float origin = 819.2 * GAME_UNITS_PER_METER + distance;
	const float growth = exp2(log2(1.0 + remaining / origin) / max(float(budget), 1.0)) - 1.0;
	return max(CloudViewStep(distance), origin * growth);
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

	const float3 model = TexCloudModeling.SampleLevel(TileableSampler, ndfUV, 0);
	ndf.coverage = model.r;
	if (ndf.coverage < 1e-8)
		return ndf;
	const float2 heights = TexCloudHeight.SampleLevel(TileableSampler, ndfUV, 0);
	const float minHeight = heights.r;
	const float maxHeight = heights.g;
	const float minAltitude = lerp(cloud.lowestAltitude, cloud.highestAltitude, minHeight);
	const float maxAltitude = lerp(cloud.lowestAltitude, cloud.highestAltitude, maxHeight);
	if (maxAltitude <= minAltitude || planet_z < minAltitude || planet_z > maxAltitude)
		return ndf;
	ndf.height_fraction = (planet_z - minAltitude) / max(maxAltitude - minAltitude, 1e-5);

	ndf.in_layer = true;
	ndf.local_height = ndf.height_fraction;
	ndf.top_type = model.g;
	ndf.bottom_type = model.b;
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
	float eye_distance;
};

float sampleCloudDensityFromContext(
	CloudDensityContext density_context, float mip_level, bool include_detail)
{
	const NDFInfo ndf = density_context.ndf;
	const float verticalProfile = saturate(ndf.top_value * ndf.bottom_value);
	if (!ndf.in_layer || ndf.coverage <= 0.0 || verticalProfile <= 0.0)
		return 0.0;

	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float4 noise = TexNubisNoise.SampleLevel(TileableSampler, density_context.noise_coordinates, max(mip_level, 0.0));
	const float density = ReconstructCloudNoiseDensity(noise, ndf.coverage, info.noiseRoundness, verticalProfile,
		density_context.eye_distance, include_detail);
	return density * info.extinctionCoefficient;
}

float sampleCloudDensity(
	float3 pos, CloudLayer cloud, float mip_level, bool include_detail,
	out CloudDensityContext density_context)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	initNDFInfo(density_context.ndf);
	density_context.noise_coordinates = 0.0;
	density_context.eye_distance = 0.0;

	const float planetHeight = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planetHeight < cloud.lowestAltitude || planetHeight > cloud.highestAltitude)
		return 0.0;

	const float layerFraction = saturate((planetHeight - cloud.lowestAltitude) / max(cloud.highestAltitude - cloud.lowestAltitude, 1e-5));
	const float2 ndfUV = LowNdfUV(pos.xy - info.cloudShapeShear * smoothstep(0.0, 1.0, layerFraction), info);
	density_context.ndf = sampleNDF(cloud, ndfUV, planetHeight);
	if (!density_context.ndf.in_layer || density_context.ndf.dimension_profile <= 0.0)
		return 0;

	density_context.noise_coordinates = pos * info.noiseFrequency + info.noiseOffset -
	                                    float3(info.noiseWindOffset, 0.0) * info.noiseFrequency;
	const float3 eyePos = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	density_context.eye_distance = length(pos - eyePos) * GAME_UNIT_TO_M;
	return sampleCloudDensityFromContext(density_context, mip_level, include_detail);
}

float3 sampleExternalSunTransmittance(float3 pos, float3 sun_dir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);

	// Finite scene-shadow coverage would project a camera-following boundary into the cloud layer.
	return SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, pos_planet, sun_dir);
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
	float soft = info.highDensitySoftness * (1.0 - CloudPositivePow(saturate(hiWeather.a), max(info.highDensitySoftAContrast, 0.01)));
	float density = saturate(CloudDensityRemap(hiCoverage, info.highDensityThreshold, info.highDensityThreshold + max(soft, 0.001), 0.0, 1.0));
	// With no base density, nonnegative wisp erosion can only remove density.
	[branch] if (density <= 0.0 && hiType >= 0.0 && info.highWispStrength >= 0.0 && info.highDensityMultiplier >= 0.0) return 0.0;
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
	[branch] if (band == 0.0) return 0.0;
	float wisp = TexHpHighWisp.SampleLevel(TileableSampler, uv * info.highWispScale + hiWindUV * info.highCellWindSpeed, 0).r;
	wisp = saturate(wisp * wisp);
	density = (density * lerp(1.0, hiCellShaped, hiCellThick) - wisp * info.highWispStrength * hiType) * band;
	density *= 1.0 - saturate(info.highDensityModAIntensity * (1.0 - CloudPositivePow(saturate(hiWeather.a), max(info.highDensityModAContrast, 0.01))));
	return max(0.0, density * info.highDensityMultiplier);
}

float EvaluateHighCloudDensity(float3 pos, out float normalizedHeight)
{
	float4 _;
	return EvaluateHighCloudDensity(pos, normalizedHeight, _);
}

#include "PhysicalSky/CloudLighting.hlsli"

struct VolumetricCloudResult
{
	float3 transmittance;
	float3 lum;
	float cloud_depth;
	float high_fraction;
};

bool CloudIntervalContains(CloudRaySegments segments, float distance)
{
	return (distance >= segments.nearSegment.x && distance < segments.nearSegment.y) ||
	       (distance >= segments.farSegment.x && distance < segments.farSegment.y);
}

VolumetricCloudResult RenderVolumetricCloudRay(float3 direction, float3 eye, float sceneDistance, bool sky, float2 jitter, float apShadow)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	VolumetricCloudResult result;
	result.transmittance = 1.0;
	result.lum = 0.0;
	result.cloud_depth = sky ? CLOUD_SKY_DISTANCE : sceneDistance;
	result.high_fraction = 0.0;
	const float3 planetEye = eye + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float2 ground = IntersectSpherePair(planetEye, direction, info.planetRadius);
	if (dot(planetEye, direction) < 0.0 && ground.y > 0.0)
		sceneDistance = min(sceneDistance, max(ground.x, 0.0));
	const CloudRaySegments low = GetCloudRaySegments(planetEye, direction,
		info.lowCloudBaseAltitude, info.lowCloudTraceTopAltitude, sceneDistance, info);
	CloudRaySegments high = (CloudRaySegments)0;
	if (info.highCloudEnabled > 0.0)
		high = GetCloudRaySegments(planetEye, direction, info.highCloudBottom, info.highCloudTop, sceneDistance, info);
	if (low.length + high.length <= 0.0)
		return result;

	float boundaries[8] = { low.nearSegment.x, low.nearSegment.y, low.farSegment.x, low.farSegment.y,
		high.nearSegment.x, high.nearSegment.y, high.farSegment.x, high.farSegment.y };
	[unroll] for (uint sortPass = 0u; sortPass < 7u; ++sortPass)
	{
		[unroll] for (uint i = 0u; i < 7u - sortPass; ++i)
		{
			const float first = min(boundaries[i], boundaries[i + 1u]);
			boundaries[i + 1u] = max(boundaries[i], boundaries[i + 1u]);
			boundaries[i] = first;
		}
	}
	float remainingLength = 0.0;
	uint remainingIntervals = 0u;
	[unroll] for (uint i = 0u; i < 7u; ++i)
	{
		const float middle = 0.5 * (boundaries[i] + boundaries[i + 1u]);
		if (boundaries[i + 1u] > boundaries[i] && (CloudIntervalContains(low, middle) || CloudIntervalContains(high, middle))) {
			remainingLength += boundaries[i + 1u] - boundaries[i];
			++remainingIntervals;
		}
	}
	const float cosine = dot(direction, info.dirlightDir);
	const float2 phase = float2(
		Phase::HGDualLobe(cosine, info.forwardEccentricity, -info.backwardEccentricity, 0.5),
		Phase::HGDualLobe(cosine, info.forwardEccentricity * info.msEccentricity, -info.backwardEccentricity * info.msEccentricity, 0.5));
	const float2 highPhase = float2(
		Phase::HGDualLobe(cosine, info.highForwardEccentricity, -info.highBackwardEccentricity, 0.5),
		Phase::HGDualLobe(cosine, info.highForwardEccentricity * info.highMSEccentricity, -info.highBackwardEccentricity * info.highMSEccentricity, 0.5));
	const float3 ambientTop = SampleCloudAmbientSkyView(float3(0, 0, 1));
	const float3 ambientBottom = SampleCloudAmbientSkyView(float3(0, 0, -1));
	const float3 skyBlend = SampleCloudAmbientSkyView(direction);
	const CloudLayer layer = GetCloudLayer(info);
	uint budget = (low.length > 0.0 ? info.lowViewSteps : 0u) + (high.length > 0.0 ? info.highViewSteps : 0u);
	float depthSum = 0.0;
	float weightSum = 0.0;
	float highWeight = 0.0;
	float transmittance = 1.0;
	[loop] for (uint interval = 0u; interval < 7u && transmittance > 0.1; ++interval)
	{
		const float begin = boundaries[interval];
		const float end = boundaries[interval + 1u];
		const float middle = 0.5 * (begin + end);
		const bool sampleLow = CloudIntervalContains(low, middle);
		const bool sampleHigh = CloudIntervalContains(high, middle);
		if (end <= begin || (!sampleLow && !sampleHigh))
			continue;
		--remainingIntervals;
		float distance = begin;
		bool empty = false;
		[loop] while (distance < end && budget > remainingIntervals && transmittance > 0.1)
		{
			const uint available = budget - remainingIntervals;
			float step = CloudBudgetStep(distance, remainingLength, available);
			if (!sampleLow)
				step = max(step, high.length / max(float(info.highViewSteps), 1.0));
			step = min(step, end - distance);
			if (available == 1u)
				step = end - distance;
			if (sampleLow && !sampleHigh) {
				const float skip = min(CloudEmptyDistance(eye + direction * distance, direction, info), end - distance);
				if (skip > step) {
					distance += skip;
					remainingLength -= skip;
					--budget;
					empty = true;
					continue;
				}
			}
			step = min(step * (empty ? 2.0 : 1.0), end - distance);
			const float sampleDistance = distance + (distance * GAME_UNIT_TO_M < 250.0 ? jitter.x : jitter.y) * step;
			const float3 pos = eye + direction * sampleDistance;
			CloudDensityContext densityContext = (CloudDensityContext)0;
			float lowExtinction = 0.0;
			if (sampleLow) {
				const float mip = clamp(log2(max(step * info.noiseFrequency * 128.0, 1.0)), 0.0, 2.0);
				lowExtinction = sampleCloudDensity(pos, layer, mip, true, densityContext);
			}
			float highHeight = 0.0;
			float4 highWeather = 0.0;
			float highDensity = 0.0;
			[branch] if (sampleHigh)
				highDensity = EvaluateHighCloudDensity(pos, highHeight, highWeather);
			const float highExtinction = highDensity * info.highViewAbsorption * highWeather.a;
			const float extinction = lowExtinction + highExtinction;
			--budget;
			if (empty && extinction > 0.0 && available > 2u) {
				empty = false;
				continue;
			}
			distance += step;
			remainingLength = max(remainingLength - step, 0.0);
			empty = extinction <= 0.0;
			if (extinction <= 0.0)
				continue;
			float3 source = 0.0;
			[branch] if (lowExtinction > 0.0)
				source += lowExtinction * LowCloudLighting(pos, direction, densityContext.ndf, phase, ambientTop, ambientBottom);
			[branch] if (highExtinction > 0.0)
				source += highExtinction * HighCloudLighting(pos, direction, highHeight, highWeather, highDensity, highPhase, ambientTop, ambientBottom, skyBlend);
			const float stepTransmittance = exp(-extinction * step * GAME_UNIT_TO_M);
			const float weight = transmittance * (1.0 - stepTransmittance);
			result.lum += weight * (source / extinction);
			transmittance *= stepTransmittance;
			depthSum += weight * sampleDistance;
			weightSum += weight;
			highWeight += weight * highExtinction / extinction;
		}
	}
	result.transmittance = saturate((transmittance - 0.1) / 0.9);
	result.lum *= (1.0 - result.transmittance.x) / max(1.0 - transmittance, 1e-6);
	if (weightSum > 0.0) {
		result.cloud_depth = depthSum / weightSum;
		result.high_fraction = highWeight / weightSum;
		const float4 ap = SampleCloudAerialPerspective(direction, result.cloud_depth, apShadow);
		result.lum = result.lum * ap.a + ap.rgb * (1.0 - result.transmittance);
	}
	return result;
}

#include "PhysicalSky/CloudTemporal.hlsli"

// Match the largest shadow-volume dimension (kShadowVolW/H in PhysicalSky.h).
#define NTHREADS 256
groupshared float g_density[2 * NTHREADS];
groupshared int4 g_density_segment[NTHREADS];

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
	uint ray_step = gtid;
	uint ray_steps = NTHREADS;
	uint column = 0;
	if (abs(ray_px_increment.x) == dir_max_component) {
		start_px = uint3(ray_px_increment.x > 0 ? 0 : dims.x - 1, gid);
		component_mask.x = true;
	} else if (abs(ray_px_increment.y) == dir_max_component) {
		start_px = uint3(gid.x, ray_px_increment.y > 0 ? 0 : dims.y - 1, gid.y);
		component_mask.y = true;
	} else {
		// Pack four 64-voxel columns into the 256-lane group instead of
		// launching 192 inactive lanes for every vertical column.
		ray_steps = dims.z;
		column = gtid / ray_steps;
		ray_step = gtid % ray_steps;
		start_px = uint3(gid.x * (NTHREADS / ray_steps) + column, gid.y, ray_px_increment.z > 0 ? 0 : dims.z - 1);
		component_mask.z = true;
	}
	ray_px_increment /= dir_max_component;
	const float3 ray_uv_increment = ray_px_increment * rcp_dims;
	const float3 start_uv = (start_px + 0.5) * rcp_dims;
	const float3 raw_thread_uv = start_uv + ray_step * ray_uv_increment;

	const bool is_valid_x = component_mask.x && raw_thread_uv.x > 0 && raw_thread_uv.x < 1;
	const bool is_valid_y = component_mask.y && raw_thread_uv.y > 0 && raw_thread_uv.y < 1;
	const bool is_valid_z = component_mask.z && raw_thread_uv.z > 0 && raw_thread_uv.z < 1;
	const bool is_valid = is_valid_x || is_valid_y || is_valid_z;

	const float3 thread_uv = raw_thread_uv - floor(raw_thread_uv);  // wraparound
	const uint3 thread_px_coord = thread_uv * dims;

	float accumulated_density = 0.0;
	if (is_valid) {
		const float3 pos = float3(FrameBuffer::CameraPosAdjust.xy + (thread_uv.xy - 0.5) * info.shadowVolumeRange, info.shadowVolumeBottom + shadow_thickness * thread_uv.z);

		// Fetch only density represented inside this finite shadow volume. Prefixes
		// start with zero extinction at the boundary instead of assuming a uniform
		// cloud shell outside the represented domain.
		CloudDensityContext _;
		float density = sampleCloudDensity(pos, cloud, 2, false, _) * length(ray_uv_increment * scale);  // scaled by ray length

		accumulated_density = density;
	}
	const int4 segment = int4(int3(floor(raw_thread_uv)), int(column));
	g_density_segment[gtid] = segment;

	// Ping-pong scan storage lets each step publish its input with one barrier.
	// The next step writes the other bank, so it cannot overwrite pending reads.
	uint read_base = 0;
	[unroll] for (uint offset = 1; offset < NTHREADS; offset <<= 1)
	{
		// Uniform across the group; vertical columns need only six scan steps.
		if (offset >= ray_steps)
			continue;
		g_density[read_base + gtid] = accumulated_density;
		GroupMemoryBarrierWithGroupSync();
		if (is_valid && gtid >= offset) {
			if (all(g_density_segment[gtid - offset] == segment))  // no wraparound happened
			{
				accumulated_density += g_density[read_base + gtid - offset];
			}
		}
		read_base = NTHREADS - read_base;
	}

	// save
	if (is_valid) {
		// Every voxel is rebuilt deterministically. The camera-centred grid moves,
		// so blending the same index from the previous frame would trail shadows.
		RWShadowVolume[thread_px_coord] = accumulated_density;
	}
}
