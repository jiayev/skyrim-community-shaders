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
	float marchStepScale;
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

	float2 lowNdfFrequency;
	float2 noiseWindOffset;
	float lowDensityScale;
	float coverageBottomPower;
	float coverageHeightRange;
	float bottomDensityPower;
	float bottomDensityWidth;
	float topExpansion;
	uint cirrusEnabled;
	float cirrusAltitude;
	float cirrusPatternFrequency;
	float cirrusDensityScale;
	float cirrusLightingScale;
	float lightingScale;
	float sunExtinction;
	float phaseForwardG;
	float phaseBackwardG;
	float phaseForwardWeight;
	float phaseBackwardWeight;
	float scatterVolumeStrength;
	float scatterVolumeDepth;
	float scatterVolumeHeight;
	float softScatteringStrength;
	float powderStrength;
	float ambientStrength;
	float ambientFloor;
	float ambientDensity;
	float ambientBase;

	float2 lowFrameDim;
	uint historyValid;
	float shadowVolumeBottom;
	float shadowVolumeTop;
	float2 cloudWindDelta;
	float2 cloudShapeShear;
	row_major float4x4 previousViewProj;
	float3 previousCamera;
	float2 previousFrameDim;
	float4 ndfBoundaryRect;
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

Texture3D<unorm float4> TexCloudShapeNoise : register(t5);
Texture3D<float4> TexAerialPerspectiveSun : register(t6);
Texture2D<float2> TexCloudHeight : register(t7);
Texture2D<float3> TexCloudModeling : register(t8);
Texture2D<unorm float> TexApShadow : register(t9);
Texture2D<float4> TexSkyView : register(t10);
Texture2D<float2> TexCirrusWeather : register(t11);
Texture2D<float3> TexCirrusPatterns : register(t13);
Texture2D<sh2> TexCloudAmbientSH : register(t16);
Texture2D<unorm float2> TexCloudProfileLUT : register(t17);
Texture2D<unorm float3> TexCloudAdjustmentLUT : register(t18);

Texture3D<float> TexShadowVolume : register(t23);
Texture2D<float4> TexCloudBoundary : register(t24);
Texture2D<float> TexVolHistoryTr : register(t26);
Texture2D<float4> TexVolHistoryLum : register(t27);
Texture2D<float4> TexVolHistoryAux : register(t28);
Texture2D<float> TexVolLowTr : register(t29);
Texture2D<float4> TexVolLowLum : register(t30);
Texture2D<float4> TexVolLowAux : register(t31);
TextureCube<float> TexCubeHistoryTr : register(t32);
TextureCube<float4> TexCubeHistoryLum : register(t33);
TextureCube<float4> TexCubeHistoryAux : register(t34);
TextureCube<float> TexCubeTraceTr : register(t35);
TextureCube<float4> TexCubeTraceLum : register(t36);
TextureCube<float4> TexCubeTraceAux : register(t37);

float3 GetCloudDirectionalLightColor()
{
	// Top-of-atmosphere irradiance, before ground sunset dimming or attenuation.
	if (SharedData::physSkyData.volCloudUseSun != 0)
		return SharedData::physSkyData.sunlightColor;
	return Color::Light(SharedData::DirLightColor.xyz);
}

RWTexture2D<float> RWTexTr : register(u0);
RWTexture2D<float4> RWTexLum : register(u1);
RWTexture2D<float4> RWTexAux : register(u2);

RWTexture3D<float> RWShadowVolume : register(u0);

RWTexture2DArray<float> RWTexCubeTr : register(u0);
RWTexture2DArray<float4> RWTexCubeLum : register(u1);
RWTexture2DArray<float4> RWTexCubeAux : register(u2);
RWTexture2D<sh2> RWCloudAmbientSH : register(u0);

float RayIntersectSphereCentered(float3 orig, float3 dir, float r)
{
	return RayIntersectSphere(orig, dir, 0, r);
}

float3 SampleCloudAmbientRadiance()
{
	const sh2 phase = SphericalHarmonics::EvaluatePhaseHG(float3(0, 1, 0), 0.0);
	const float r = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(0, 0)], phase);
	const float g = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(1, 0)], phase);
	const float b = SphericalHarmonics::FuncProductIntegral(TexCloudAmbientSH[int2(2, 0)], phase);
	return max(0.0, float3(r, g, b));
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
	VolumetricCloudData info)
{
	CloudRaySegments segments = (CloudRaySegments)0;
	if (topAltitude <= bottomAltitude)
		return segments;
	const float2 outer = IntersectSpherePair(origin, dir, info.planetRadius + topAltitude);
	const float entry = max(outer.x, 50.0 * GAME_UNITS_PER_METER);
	const float exit = outer.y;
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
	segments.nearLength = min(segments.nearSegment.y - segments.nearSegment.x, info.rayMarchRange);
	segments.nearSegment.y = segments.nearSegment.x + segments.nearLength;
	const float farLength = min(segments.farSegment.y - segments.farSegment.x, info.rayMarchRange - segments.nearLength);
	segments.farSegment.y = segments.farSegment.x + farLength;
	segments.length = segments.nearLength + farLength;
	return segments;
}

float CloudTemporalMarchHash(float2 pixel, float frame)
{
	const float3 seed = frac(float3(pixel, frame) * 0.1031);
	const float scramble = dot(seed, seed.zyx + 31.32);
	return frac((seed.y + seed.x + scramble * 2.0) * (scramble + seed.z));
}

float CloudSpatialMarchHash(float2 position)
{
	const float2 seed = frac(position * 0.1031);
	const float scramble = dot(seed.xyx, seed.yxx + 19.19);
	return frac((seed.y + seed.x + scramble * 2.0) * (scramble + seed.x));
}

float CloudDepthWeight(float transmittance, float density, float distance)
{
	return saturate((transmittance - 0.1) * 1.1111112) * density * (1.0 - exp2(distance * GAME_UNIT_TO_M * -0.36067376));
}

float2 LowNdfUV(float2 worldXY, VolumetricCloudData info)
{
	return (worldXY - info.noiseWindOffset) * info.lowNdfFrequency + 0.5;
}

float CloudViewStep(float distance)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	return (3.0 + distance * GAME_UNIT_TO_M * (60.0 / 16384.0)) * info.marchStepScale * GAME_UNITS_PER_METER;
}

struct NDFInfo
{
	bool in_layer;
	float dimension_profile;
	float coverage;
	float height_fraction;
	float top_type;
	float bottom_type;
};

void initNDFInfo(out NDFInfo ndf)
{
	ndf.in_layer = false;
	ndf.dimension_profile = 0.0;
	ndf.coverage = 0.0;
	ndf.height_fraction = 0.0;
	ndf.top_type = 0.0;
	ndf.bottom_type = 0.0;
}

NDFInfo sampleNDF(CloudLayer cloud, float2 worldXY, float planetHeight)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	NDFInfo ndf;
	initNDFInfo(ndf);
	const float2 heights = saturate(TexCloudHeight.SampleLevel(TileableSampler, LowNdfUV(worldXY, info), 0));
	const float minAltitude = lerp(cloud.lowestAltitude, cloud.highestAltitude, heights.r);
	const float maxAltitude = lerp(cloud.lowestAltitude, cloud.highestAltitude, heights.g);
	if (maxAltitude <= minAltitude || planetHeight <= minAltitude || planetHeight >= maxAltitude)
		return ndf;
	ndf.height_fraction = saturate((planetHeight - minAltitude) / (maxAltitude - minAltitude));
	const float2 modelingXY = worldXY - info.cloudShapeShear * smoothstep(0.0, 1.0, ndf.height_fraction);
	const float3 model = saturate(TexCloudModeling.SampleLevel(TileableSampler, LowNdfUV(modelingXY, info), 0));
	float3 upstream = model;
	if (any(info.cloudShapeShear != 0.0))
		upstream = saturate(TexCloudModeling.SampleLevel(TileableSampler, LowNdfUV(modelingXY - info.cloudShapeShear * 60.0, info), 0));
	const float shearBlend = smoothstep(0.0, 1.0, saturate((ndf.height_fraction - 0.1) * 1.5384616));
	ndf.coverage = lerp(model.r, max(model.r, upstream.r), shearBlend);
	if (ndf.coverage < 1e-8)
		return ndf;
	ndf.in_layer = true;
	ndf.top_type = lerp(model.g, upstream.g, shearBlend) * saturate(ndf.coverage * 10.0);
	ndf.bottom_type = lerp(model.b, upstream.b, shearBlend);
	const float top = TexCloudProfileLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, ndf.height_fraction), 0).g;
	const float expandedTop = TexCloudAdjustmentLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, ndf.height_fraction), 0).r;
	const float bottom = TexCloudProfileLUT.SampleLevel(TransmittanceSampler, float2(ndf.bottom_type, ndf.height_fraction), 0).r;
	const float verticalProfile = max(lerp(top, max(top, expandedTop), info.topExpansion) * bottom, 0.0);
	const float heightFade = saturate(ndf.height_fraction / info.coverageHeightRange);
	const float exponent = lerp(lerp(info.coverageBottomPower, 1.0, heightFade), 1.0, heightFade);
	ndf.dimension_profile = verticalProfile * pow(ndf.coverage, exponent);
	return ndf;
}

struct CloudDensityContext
{
	NDFInfo ndf;
	float3 noise_coordinates;
	float eye_distance;
};

float sampleCloudDensityFromContext(
	CloudDensityContext density_context, float mipBias)
{
	const NDFInfo ndf = density_context.ndf;
	if (!ndf.in_layer || ndf.dimension_profile <= 1e-10)
		return 0.0;

	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float mip = floor(ndf.dimension_profile * 3.0 + mipBias);
	const float4 noise = TexCloudShapeNoise.SampleLevel(TileableSampler, density_context.noise_coordinates, mip);
	const float eroded = ReconstructCloudNoiseDensity(noise, ndf.dimension_profile, ndf.top_type, ndf.height_fraction, density_context.eye_distance);
	return ShapeCloudBaseDensity(eroded, ndf.height_fraction, info.bottomDensityWidth, info.bottomDensityPower) * info.lowDensityScale;
}

float sampleCloudDensity(
	float3 pos, CloudLayer cloud, float mipBias, float viewDistance,
	out CloudDensityContext density_context)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	initNDFInfo(density_context.ndf);
	density_context.noise_coordinates = 0.0;
	density_context.eye_distance = 0.0;

	const float planetHeight = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	if (planetHeight < cloud.lowestAltitude || planetHeight > cloud.highestAltitude)
		return 0.0;

	density_context.ndf = sampleNDF(cloud, pos.xy, planetHeight);
	if (!density_context.ndf.in_layer || density_context.ndf.dimension_profile <= 0.0)
		return 0;

	const float bottomFade = saturate((density_context.ndf.height_fraction - 0.02) * 33.333332);
	const float2 xy = (pos.xy - info.noiseWindOffset) * GAME_UNIT_TO_M;
	const float2 displacement = TexCloudAdjustmentLUT.SampleLevel(TileableSampler, xy * 0.004, 0).gb;
	density_context.noise_coordinates = float3(xy * 0.0043545123 + (displacement * 2.0 - 1.0) * (0.125 - bottomFade * 0.125),
		(pos.z * GAME_UNIT_TO_M - 20.0 + bottomFade * 10.0) * 0.0034834063);
	density_context.eye_distance = viewDistance;
	return sampleCloudDensityFromContext(density_context, mipBias);
}

float3 sampleExternalSunTransmittance(float3 pos, float3 sun_dir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);

	// Finite scene-shadow coverage would project a camera-following boundary into the cloud layer.
	return SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, pos_planet, sun_dir);
}

#include "PhysicalSky/CloudLighting.hlsli"

struct VolumetricCloudResult
{
	float3 transmittance;
	float3 lum;
	float cloud_depth;
};

float CirrusDensity(float2 position, out float profile)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 weather = saturate(TexCirrusWeather.SampleLevel(TileableSampler, LowNdfUV(position, info), 0));
	const float3 patterns = TexCirrusPatterns.SampleLevel(TileableSampler, (position - info.noiseWindOffset) * info.cirrusPatternFrequency, 0);
	const float3 squared = patterns * patterns;
	profile = lerp(lerp(squared.b, squared.r, saturate(weather.y * 2.0)), squared.g, saturate(weather.y * 2.0 - 1.0));
	profile = pow(max(profile, 1e-10), 1.9 - weather.x * 1.8) * saturate(weather.x * weather.x * weather.x * 2.0);
	return (profile * 2.0) * info.cirrusDensityScale;
}

void IntegrateCirrus(float3 position, float3 direction, float distance, float phase, CloudAmbient cloudAmbient,
	inout float3 radiance, inout float transmittance, inout float depthSum, inout float weightSum)
{
	if (transmittance <= 0.1)
		return;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float profile;
	const float density = CirrusDensity(position.xy, profile);
	if (density <= 1e-10)
		return;
	const float angularOffset = dot(direction, info.dirlightDir) * -0.25;
	float occlusion = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const float index = i + 0.5;
		const float fraction = index * 0.25;
		const float shadowDistance = index * 30.0 * (angularOffset + 0.5 + fraction * fraction * fraction * (0.5 - angularOffset));
		float unusedProfile;
		occlusion += sqrt(CirrusDensity(position.xy + info.dirlightDir.xy * (shadowDistance * GAME_UNITS_PER_METER), unusedProfile)) * 13.5;
	}
	const float ambient = info.ambientStrength * info.cirrusLightingScale * (direction.z + 1.0) * pow(saturate(1.0 - profile * 0.3), 0.2);
	const float sun = info.cirrusLightingScale * 64.0 * exp(-info.sunExtinction * 0.1 * occlusion) * phase;
	const float3 sunlight = sampleExternalSunTransmittance(position, info.dirlightDir) * GetCloudDirectionalLightColor();
	const float3 ambientRadiance = CloudAmbientRadiance(position + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius), cloudAmbient);
	const float stepTransmittance = exp(-density * 10.0);
	const float weight = transmittance * (1.0 - stepTransmittance);
	radiance += weight * (sun * sunlight + ambient * ambientRadiance);
	const float depthWeight = CloudDepthWeight(transmittance, density, distance);
	depthSum += depthWeight * distance;
	weightSum += depthWeight;
	transmittance *= stepTransmittance;
}

bool CloudIntervalContains(CloudRaySegments segments, float distance)
{
	return (distance >= segments.nearSegment.x && distance < segments.nearSegment.y) ||
	       (distance >= segments.farSegment.x && distance < segments.farSegment.y);
}

float2 ClipCloudBoundary(float2 segment, float4 boundary)
{
	if (boundary.w <= 0.0 || segment.y <= segment.x)
		return segment;
	const bool inside = boundary.z > 0.0 && (boundary.x == 0.0 || boundary.z < boundary.x);
	if (!inside && boundary.x > 0.0)
		segment.x = max(segment.x, min(boundary.x, boundary.w));
	// An open grid does not bound the part of the ray beyond its XY footprint.
	if (boundary.y > 0.0 && segment.y <= boundary.w)
		segment.y = min(segment.y, boundary.y);
	segment.y = max(segment.x, segment.y);
	return segment;
}

VolumetricCloudResult RenderVolumetricCloudRay(float3 direction, float3 eye, float sceneDistance, float2 jitter, float apShadow, float4 boundary)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	VolumetricCloudResult result;
	result.transmittance = 1.0;
	result.lum = 0.0;
	bool sceneLimited = sceneDistance > 0.0;
	const float3 planetEye = eye + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float2 ground = IntersectSpherePair(planetEye, direction, info.planetRadius);
	if (dot(planetEye, direction) < 0.0 && ground.y > 0.0) {
		sceneDistance = sceneLimited ? min(sceneDistance, max(ground.x, 0.0)) : max(ground.x, 0.0);
		sceneLimited = true;
	}
	const CloudRaySegments bounds = GetCloudRaySegments(planetEye, direction,
		info.lowCloudBaseAltitude, info.lowCloudTraceTopAltitude, info);
	CloudRaySegments low = bounds;
	low.nearSegment = ClipCloudBoundary(low.nearSegment, boundary);
	low.farSegment = ClipCloudBoundary(low.farSegment, boundary);
	if (sceneLimited) {
		low.nearSegment.y = max(low.nearSegment.x, min(low.nearSegment.y, sceneDistance));
		low.farSegment.y = max(low.farSegment.x, min(low.farSegment.y, sceneDistance));
	}
	low.nearLength = low.nearSegment.y - low.nearSegment.x;
	low.length = low.nearLength + (low.farSegment.y - low.farSegment.x);
	result.cloud_depth = info.rayMarchRange;
	if (bounds.length > 0.0)
		result.cloud_depth = bounds.nearSegment.x > 50.0 * GAME_UNITS_PER_METER ? bounds.nearSegment.x : bounds.nearSegment.y;
	const float boundaryDepth = boundary.x > 0.0 && boundary.z > 0.0 ? min(boundary.x, boundary.z) : max(boundary.x, boundary.z);
	const bool hasBoundaryDepth = boundary.w > 0.0 && boundaryDepth > 0.0;
	if (hasBoundaryDepth)
		result.cloud_depth = boundaryDepth;
	float cirrusDistance = sceneDistance;
	bool cirrusPending = false;
	if (info.cirrusEnabled != 0u) {
		const float2 shell = IntersectSpherePair(planetEye, direction, info.planetRadius + info.cirrusAltitude);
		cirrusDistance = shell.x >= 0.0 ? shell.x : shell.y;
		if (cirrusDistance >= 0.0)
			result.cloud_depth = bounds.length > 0.0 || hasBoundaryDepth ? min(result.cloud_depth, cirrusDistance) : cirrusDistance;
		cirrusPending = cirrusDistance >= 0.0 && (!sceneLimited || cirrusDistance < sceneDistance);
	}
	if (low.length <= 0.0 && !cirrusPending)
		return result;

	// Split the volume at the sheet so camera altitude and NDF bounds cannot reverse compositing order.
	float boundaries[5] = { low.nearSegment.x, low.nearSegment.y, low.farSegment.x, low.farSegment.y,
		cirrusPending ? cirrusDistance : 0.0 };
	[unroll] for (uint sortPass = 0u; sortPass < 4u; ++sortPass)
	{
		[unroll] for (uint i = 0u; i < 4u - sortPass; ++i)
		{
			const float first = min(boundaries[i], boundaries[i + 1u]);
			boundaries[i + 1u] = max(boundaries[i], boundaries[i + 1u]);
			boundaries[i] = first;
		}
	}
	const float phase = CloudScatteringPhase(dot(direction, info.dirlightDir));
	const CloudAmbient ambient = MakeCloudAmbient();
	const CloudLayer layer = GetCloudLayer(info);
	float depthSum = 0.0;
	float weightSum = 0.0;
	float transmittance = 1.0;
	[loop] for (uint interval = 0u; interval < 4u && transmittance > 0.1; ++interval)
	{
		const float begin = boundaries[interval];
		const float end = boundaries[interval + 1u];
		if (cirrusPending && cirrusDistance <= begin) {
			IntegrateCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, phase, ambient, result.lum, transmittance, depthSum, weightSum);
			cirrusPending = false;
		}
		if (end <= begin || !CloudIntervalContains(low, 0.5 * (begin + end)))
			continue;
		float distance = begin;
		[loop] while (distance < end && transmittance > 0.1)
		{
			const float step = min(CloudViewStep(distance), end - distance);
			const float sampleDistance = distance + (distance * GAME_UNIT_TO_M < 250.0 ? jitter.x : jitter.y) * step;
			const float3 pos = eye + direction * sampleDistance;
			CloudDensityContext densityContext = (CloudDensityContext)0;
			const float viewDistance = distance * GAME_UNIT_TO_M;
			const float extinction = sampleCloudDensity(pos, layer, 0.0, viewDistance, densityContext);
			const float depthDistance = distance;
			distance += step * (densityContext.ndf.dimension_profile > 0.05 ? 1.0 : 2.0);
			if (extinction <= 0.0)
				continue;
			const float3 source = CloudLighting(pos, direction, densityContext.ndf.height_fraction, densityContext.ndf.dimension_profile,
				densityContext.ndf.coverage * densityContext.ndf.top_type, extinction, viewDistance, phase, ambient);
			const float stepTransmittance = exp(-extinction * step * GAME_UNIT_TO_M);
			const float weight = transmittance * (1.0 - stepTransmittance);
			result.lum += weight * source;
			const float depthWeight = CloudDepthWeight(transmittance, extinction, depthDistance);
			depthSum += depthWeight * depthDistance;
			weightSum += depthWeight;
			transmittance *= stepTransmittance;
		}
	}
	if (cirrusPending)
		IntegrateCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, phase, ambient, result.lum, transmittance, depthSum, weightSum);
	result.transmittance = saturate((transmittance - 0.1) / 0.9);
	if (weightSum > 0.0) {
		result.cloud_depth = depthSum / weightSum;
		const float4 ap = SampleCloudAerialPerspective(direction, result.cloud_depth, apShadow);
		result.lum = result.lum * ap.a + ap.rgb * (1.0 - result.transmittance);
	}
	return result;
}

#include "PhysicalSky/CloudBlur.hlsli"
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

	float density = 0.0;
	float accumulated_density = 0.0;
	if (is_valid) {
		const float2 center = CloudShadowVolume::GridCenter(FrameBuffer::CameraPosAdjust.xy, info.shadowVolumeRange, dims.xy);
		const float3 pos = float3(center + (thread_uv.xy - 0.5) * info.shadowVolumeRange, info.shadowVolumeBottom + shadow_thickness * thread_uv.z);

		// Fetch only density represented inside this finite shadow volume. Prefixes
		// start with zero extinction at the boundary instead of assuming a uniform
		// cloud shell outside the represented domain.
		density = SampleCloudLightDensity(pos, 2000.0) * length(ray_uv_increment * scale);

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
		RWShadowVolume[thread_px_coord] = exp(-max(accumulated_density - 0.5 * density, 0.0) * info.sunExtinction * GAME_UNIT_TO_M);
	}
}

	[numthreads(8, 8, 4)] void resampleShadowVolume(uint3 pixel : SV_DispatchThreadID)
{
	uint3 dims;
	RWShadowVolume.GetDimensions(dims.x, dims.y, dims.z);
	if (any(pixel >= dims))
		return;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 scale = float3(info.shadowVolumeRange.xx, max(info.shadowVolumeTop - info.shadowVolumeBottom, 1.0));
	float3 increment = -info.dirlightDir * rcp(scale) * float3(dims);
	const float maximum = max(max(abs(increment.x), abs(increment.y)), abs(increment.z));
	const uint axis = abs(increment.x) == maximum ? 0u : (abs(increment.y) == maximum ? 1u : 2u);
	increment /= maximum;
	const float rayStep = increment[axis] > 0.0 ? float(pixel[axis]) : float(dims[axis] - 1u - pixel[axis]);
	float3 offset = frac(0.5 + rayStep * increment) - 0.5;
	offset = float3(axis == 0u ? 0.0 : offset.x, axis == 1u ? 0.0 : offset.y, axis == 2u ? 0.0 : offset.z);
	RWShadowVolume[pixel] = TexShadowVolume.SampleLevel(TransmittanceSampler, (float3(pixel) + 0.5 - offset) / float3(dims), 0);
}
