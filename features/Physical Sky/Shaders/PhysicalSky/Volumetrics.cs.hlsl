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
#ifdef CLOUD_AP_SHADOW
#	define PS_ENABLE_DIRLIGHT_TRANSMITTANCE
#	define PS_LINEAR_SHADOW_SAMPLER
#	define AP_DIRECT_SHADOW_REGISTER t7
#	define TERRAIN_SHADOW_REGISTER t36
#	define CLOUD_SHADOW_REGISTER t37
#endif
Texture3D<float> TexShadowVolume : register(t14);
#include "Common/FlareOcclusion.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "PhysicalSky/CloudGeometry.hlsli"
#include "PhysicalSky/CloudNoise.hlsli"
#include "PhysicalSky/Common.hlsli"
#include "PhysicalSky/Lightning.hlsli"
#ifdef CLOUD_AP_SHADOW
#	include "PhysicalSky/ApShadow.hlsli"
#endif

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

#include "PhysicalSky/StormData.hlsli"

struct VolumetricCloudData
{
	float rayMarchRange;
	float shadowVolumeRange;
	float marchStepScale;
	uint cloudFrameIndex;

	float2 rcpFrameDim;
	float2 activeFrameDim;
	float2 lowFrameDim;
	float2 previousFrameDim;

	float3 dirlightDir;
	float bottomZ;
	float3 previousCamera;
	float planetRadius;

	float lowestCloudAltitude;
	float highestCloudAltitude;
	float lowCloudBaseAltitude;
	float lowCloudTopAltitude;

	float weatherBaseTopAltitude;
	float weatherStormTopAltitude;
	float cloudCurvatureRadius;
	float weatherWorldScale;

	float shadowVolumeBottom;
	float shadowVolumeTop;
	uint historyValid;
	uint cirrusEnabled;

	float2 weatherFrequency;
	float2 noiseWindOffset;
	float2 cloudWindDelta;
	float2 cirrusWindOffset;

	float2 cirrusWindDelta;
	float cirrusAltitude;
	float cirrusUvInvSize;

	float cirrusDensityScale;
	float cirrusLightThickness;
	float phaseForwardG;
	float phaseBackwardG;

	float cumulusExtinction;
	float coverageBottomPower;
	float coverageHeightRange;
	float bottomDensityPower;

	float bottomDensityWidth;
	float topExpansion;
	float scatterDensityThreshold;
	float scatterDensityPower;

	float cloudLightingScale;
	float sunExtinction;
	float viewExtinction;
	float ambientStrength;

	float msAlbedo;
	float msStrength;
	float msBoundaryWidth;
	float msTransportExtinction;

	float4 weatherDetail;
	row_major float4x4 previousViewProj;

	float4 lowCloudPhase;
	uint stormCount;
	uint voxelCount;
	float2 voxelAltitude;
	uint skipShapeNoise;
	uint useVoxelNoise;
	float2 noisePadding;  // previous curvature radius, weather-history refresh weight
	float4 middleLayer;   // bottom/top (game units), extinction/metre, weather UV/game unit
	float4 middleShape;   // edge softness, reserved, detail UV/game unit, 3D detail
};

cbuffer VolumetricCloudConstants : register(b1)
{
	VolumetricCloudData VolumetricCloudBuffer[1];
	float4 CloudMsCalibration[49];
	float4 MiddleCloudCells;      // frequency, Ac modulation, As modulation, warp
	float4 MiddleCloudBands;      // world wave vector (cycles/game unit), bend (wavelengths), organization
	float4 MiddleCloudBandShape;  // thickness contrast, displacement (game units), wavelength (game units), enabled
	uint CloudTraceScale;
	uint UpdateAmbientMip;
	float StormExtinction;
	float StratusExtinction;
	uint UsePerformanceCloudSteps;
	uint WeatherLightFirstSlice;
	uint WeatherLightSeedStride;
	uint WeatherAmbientUpdate;
	float4 WeatherLightGrid;  // projection origin and horizontal footprint radius, in game units
	float4 WeatherPreviousLightGrid;
	float CloudBaseHeightVariation;
	float WeatherProceduralWeight;
	uint FiniteCloudWeather;
	uint CloudCascadeShadows;
};
Texture2D<float4> TexTransmittance : register(t1);
Texture3D<float4> TexAerialPerspective : register(t2);

Texture2D<float> TexDepth : register(t3);

Texture3D<unorm float4> TexCloudShapeNoise : register(t4);

Texture2D<float4> TexCloudAtmosphereMs : register(t8);
Texture2D<float2> TexCirrusWeather : register(t9);
Texture2D<float3> TexCirrusPatterns : register(t10);
Texture2D<unorm float2> TexCloudProfileLUT : register(t11);
Texture2D<unorm float3> TexCloudAdjustmentLUT : register(t12);
Texture2D<sh2> TexCloudAmbientSH : register(t13);
TextureCube<float4> TexCloudGroundReflection : register(t39);

Texture2D<float2> TexLightningTrace : register(t90);
Texture2D<float2> TexLightningHistory : register(t91);
TextureCube<float2> TexLightningCubeTrace : register(t92);
TextureCube<float2> TexLightningCubeHistory : register(t93);
RWTexture2D<float2> RWLightningMask : register(u3);
RWTexture2DArray<float2> RWLightningCubeMask : register(u3);

CloudLayer GetCloudLayer(VolumetricCloudData info)
{
	CloudLayer cloud;
	cloud.lowestAltitude = info.lowCloudBaseAltitude;
	cloud.highestAltitude = info.lowCloudTopAltitude;
	return cloud;
}

Texture2D<float> TexVolHistoryTr : register(t17);
Texture2D<float4> TexVolHistoryLum : register(t18);
Texture2D<float4> TexVolHistoryAux : register(t19);
Texture2D<float> TexVolLowTr : register(t20);
Texture2D<float4> TexVolLowLum : register(t21);
Texture2D<float4> TexVolLowAux : register(t22);
Texture2D<float4> TexVolLowStormMotion : register(t58);
TextureCube<float4> TexCubeStormMotion : register(t59);
RWTexture2D<float4> RWStormMotion : register(u6);
RWTexture2DArray<float4> RWCubeStormMotion : register(u7);
TextureCube<float> TexCubeHistoryTr : register(t23);
TextureCube<float4> TexCubeHistoryLum : register(t24);
TextureCube<float4> TexCubeHistoryAux : register(t25);
TextureCube<float> TexCubeTraceTr : register(t26);
TextureCube<float4> TexCubeTraceLum : register(t27);
TextureCube<float4> TexCubeTraceAux : register(t28);
uint CloudStormCount()
{
#ifdef NO_CLOUD_STORMS
	return 0u;
#elif defined(SINGLE_CLOUD_STORM)
	return 1u;
#else
	return VolumetricCloudBuffer[0].stormCount;
#endif
}

Texture2DArray<float4> TexStormHeight : register(t55);
Texture2DArray<float4> TexStormModeling : register(t56);
#ifdef SINGLE_CLOUD_STORM
cbuffer StormInstanceConstants : register(b0) { CloudStormData CloudStorms[1]; };
#else
StructuredBuffer<CloudStormData> CloudStorms : register(t57);
#endif

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
	// The upper hemisphere's first moment gives irradiance / pi.
	return max(0.0, -float3(TexCloudAmbientSH[int2(0, 0)].y,
						TexCloudAmbientSH[int2(1, 0)].y, TexCloudAmbientSH[int2(2, 0)].y) *
						(RCP_PI / 0.4886025119));
}

// The legacy lookup needs at least two slices; use 1 for the shared RGB medium.
static const uint CloudAerialSharedVolume = 1u;

struct CloudAerialRay
{
	float2 skyUv;
	uint slices;
	float2 mix;
};

CloudAerialRay MakeCloudAerialRay(float3 viewDir)
{
	CloudAerialRay air = (CloudAerialRay)0;
	[branch] if (ViewAtmosphere::Enabled())
	{
		air.slices = CloudAerialSharedVolume;
		air.skyUv = ViewAtmosphere::ProjectUv(float4(viewDir, 0.0));
		return air;
	}
	air.mix = float2(SharedData::physSkyData.apLumMix, SharedData::physSkyData.apTrMix);
	if (!any(air.mix > 0.0))
		return air;

	uint width, height, depth;
	TexAerialPerspective.GetDimensions(width, height, depth);
	if (width > 0u && height > 0u && depth >= 2u) {
		air.skyUv = SkyViewLutUv(viewDir, height);
		air.slices = depth;
	}
	return air;
}

ViewAtmosphere::Medium SampleCloudAerialPerspective(CloudAerialRay air, float distance)
{
	[branch] if (air.slices == CloudAerialSharedVolume) return ViewAtmosphere::Sample(air.skyUv, distance, TransmittanceSampler);
	if (air.slices == 0u)
		return ViewAtmosphere::Identity();
	const float3 uvw = float3(air.skyUv, ApDepthUv(distance, air.slices));
	const float4 transfer = TexAerialPerspective.SampleLevel(SkyViewSampler, uvw, 0);
	ViewAtmosphere::Medium medium;
	medium.luminance = transfer.rgb * air.mix.x;
	medium.transmittance = lerp(1.0, transfer.a, air.mix.y);
	return medium;
}

RWTexture2D<float4> RWCloudAmbientPartial : register(u1);
Texture2D<float4> TexCloudAmbientPartial : register(t90);
groupshared float4 gCloudAmbientPartial[8][32];

float3 CloudAmbientAtmosphereSource(uint lightIndex, float3 position, float3 direction, float3 lightDirection, float3 color, float3 rayleigh, float3 mie)
{
	if (!any(color > 0.0) || dot(lightDirection, lightDirection) < 0.5)
		return 0.0;
	const float cosine = dot(direction, lightDirection);
	const float3 direct = SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, position, lightDirection, SharedData::physSkyData.diskAngularRadius[lightIndex]) *
	                      (rayleigh * DiskRayleigh(cosine, lightIndex) + mie * SampleDiskPhase(cosine, lightIndex).z);
	const float radius = length(position);
	const float3 multiple = TexCloudAtmosphereMs.SampleLevel(TransmittanceSampler, MsLutUv(radius, dot(position / radius, lightDirection)), 0).rgb;
	return color * (direct + (rayleigh + mie) * multiple);
}

float3 CloudAmbientAtmosphere(float radius, float boundaryRadius, float3 direction, uint steps, out float3 transmission, out float boundaryDistance)
{
	const SharedData::PhysSkyData sky = SharedData::physSkyData;
	const float projected = radius * direction.z;
	const float difference = (sky.rAtmosphere - radius) * (sky.rAtmosphere + radius);
	const float atmosphereRoot = sqrt(max(projected * projected + difference, 0.0));
	float distance = projected >= 0.0 ? difference / max(atmosphereRoot + projected, 1e-6) : atmosphereRoot - projected;
	const float groundOffset = (radius - boundaryRadius) * (radius + boundaryRadius);
	const float groundDiscriminant = projected * projected - groundOffset;
	boundaryDistance = projected < 0.0 && groundDiscriminant >= 0.0 ?
	                       groundOffset / max(-projected + sqrt(max(groundDiscriminant, 0.0)), 1e-6) :
	                       -1.0;
	if (boundaryDistance >= 0.0)
		distance = boundaryDistance;
	transmission = 1.0;
	if (distance <= 0.0)
		return 0.0;
	float3 radiance = 0.0;
	float start = 0.0;
	[loop] for (uint i = 0u; i < steps; ++i)
	{
		const float fraction = float(i + 1u) / float(steps);
		const float end = distance * fraction * fraction;
		const float segmentLength = end - start;
		const float3 position = float3(0, 0, radius) + direction * (0.5 * (start + end));
		float r, m, o;
		SampleAtmosphere(max(length(position) - sky.rPlanet, 0.0), r, m, o);
		const float3 rayleigh = r * sky.rayleighScatter;
		const float3 mie = m * sky.aerosolScatter;
		const float3 extinction = rayleigh + mie + m * sky.aerosolAbsorption + o * sky.ozoneAbsorption;
		const float3 stepTransmission = exp(-extinction * segmentLength);
		const float3 opticalDepth = extinction * segmentLength;
		const float3 factor = lerp((1.0 - stepTransmission) / max(extinction, 1e-20),
			segmentLength * (1.0 - 0.5 * opticalDepth), float3(opticalDepth < 1e-3));
		float3 source = CloudAmbientAtmosphereSource(0u, position, direction, sky.sunDir, sky.sunlightColor, rayleigh, mie);
		source += CloudAmbientAtmosphereSource(1u, position, direction, sky.masserDir, sky.masserColor, rayleigh, mie);
		source += CloudAmbientAtmosphereSource(2u, position, direction, sky.secundaDir, sky.secundaColor, rayleigh, mie);
		radiance += transmission * source * factor;
		transmission *= stepTransmission;
		start = end;
	}
	return radiance;
}

float3 CloudAmbientSky(float3 direction, float altitude)
{
	float3 transmission;
	float boundaryDistance;
	return CloudAmbientAtmosphere(SharedData::physSkyData.rPlanet + max(altitude, GAME_UNITS_PER_METER),
		SharedData::physSkyData.rPlanet, direction, 32u, transmission, boundaryDistance);
}

// Keep the same 256 directions, distributed over eight independent groups.
// Ground reflection is linear in the transport integral, so its average can
// be applied after reduction rather than serializing every direction on it.
[numthreads(32, 1, 1)] void buildCloudAmbientSH(uint lane : SV_GroupIndex, uint3 group : SV_GroupID) {
	const uint directionIndex = group.x * 32u + lane;
	const uint2 tid = uint2(directionIndex % 16u, directionIndex / 16u);
	uint reflectionWidth, reflectionHeight;
	TexCloudGroundReflection.GetDimensions(reflectionWidth, reflectionHeight);
	const float2 faceSize = max(float2(reflectionWidth, reflectionHeight), 1.0);
	const float2 faceTexel = floor((float2(tid) + 0.5) * faceSize / 16.0);
	const float2 faceUv = 2.0 * (faceTexel + 0.5) / faceSize - 1.0;
	const float3 reflectionDirection = float3(-faceUv.x, -faceUv.y, -1.0);
	const float inverseLength = rsqrt(1.0 + dot(faceUv, faceUv));
	const float solidAngleWeight = inverseLength * inverseLength * inverseLength;
	float3 groundRadiance;
	if (reflectionWidth > 0u && reflectionHeight > 0u)
		groundRadiance = ColorManagement::SceneToLinear(max(TexCloudGroundReflection.SampleLevel(TileableSampler, reflectionDirection, 0).rgb, 0.0));
	else
		groundRadiance = ColorManagement::SceneToLinear(Color::Ambient(max(SharedData::GetAmbient(float3(0, 0, -1)), 0.0)));
	gCloudAmbientPartial[7][lane] = float4(groundRadiance * solidAngleWeight, solidAngleWeight);

	const float2 sampleCoord = (float2(tid) + 0.5) / 16.0;
	const float elevationCoordinate = 1.0 - sampleCoord.y;
	const float3 shSampleDir = SphericalHarmonics::GetUniformSphereSample(sampleCoord.x, 0.5 * (1.0 - elevationCoordinate * elevationCoordinate));
	const float3 rayDir = shSampleDir.xzy;
	const float3 skyColor = CloudAmbientSky(rayDir, VolumetricCloudBuffer[0].highestCloudAltitude);
	const float shFactor = 4.0 * Math::PI * elevationCoordinate / 256.0;
	const sh2 sh = SphericalHarmonics::Evaluate(shSampleDir);
	gCloudAmbientPartial[0][lane] = SphericalHarmonics::Scale(sh, skyColor.r * shFactor);
	gCloudAmbientPartial[1][lane] = SphericalHarmonics::Scale(sh, skyColor.g * shFactor);
	gCloudAmbientPartial[2][lane] = SphericalHarmonics::Scale(sh, skyColor.b * shFactor);
	const float3 groundDirection = rayDir * float3(1.0, 1.0, -1.0);
	const SharedData::PhysSkyData sky = SharedData::physSkyData;
	const float radius = sky.rPlanet + max(VolumetricCloudBuffer[0].lowestCloudAltitude, GAME_UNITS_PER_METER);
	const float probeRadius = clamp(sky.zCameraPlanet, sky.rPlanet, radius);
	float3 transmission;
	float boundaryDistance;
	const float3 ground = CloudAmbientAtmosphere(radius, probeRadius, groundDirection, 16u, transmission, boundaryDistance);
	transmission *= boundaryDistance >= 0.0 ? 1.0 : 0.0;
	const float weight = 2.0 * elevationCoordinate / 256.0;
	const float cosine = -2.0 * groundDirection.z;
	gCloudAmbientPartial[3][lane] = float4(ground * weight, 0.0);
	gCloudAmbientPartial[4][lane] = float4(ground * weight * cosine, 0.0);
	gCloudAmbientPartial[5][lane] = float4(transmission * weight, 0.0);
	gCloudAmbientPartial[6][lane] = float4(transmission * weight * cosine, 0.0);
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 16u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride) {
			[unroll] for (uint coefficient = 0u; coefficient < 8u; ++coefficient)
			{
				gCloudAmbientPartial[coefficient][lane] += gCloudAmbientPartial[coefficient][lane + stride];
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane == 0u) {
		[unroll] for (uint coefficient = 0u; coefficient < 8u; ++coefficient)
		{
			RWCloudAmbientPartial[uint2(coefficient, group.x)] = gCloudAmbientPartial[coefficient][0];
		}
	}
}

	[numthreads(8, 1, 1)] void reduceCloudAmbientSH(uint lane : SV_GroupIndex)
{
	[unroll] for (uint coefficient = 0u; coefficient < 8u; ++coefficient)
	{
		gCloudAmbientPartial[coefficient][lane] = TexCloudAmbientPartial[uint2(coefficient, lane)];
	}
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 4u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride) {
			[unroll] for (uint coefficient = 0u; coefficient < 8u; ++coefficient)
			{
				gCloudAmbientPartial[coefficient][lane] += gCloudAmbientPartial[coefficient][lane + stride];
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane == 0u) {
		const float3 reflection = gCloudAmbientPartial[7][0].rgb / max(gCloudAmbientPartial[7][0].w, 1e-6);
		RWCloudAmbientSH[int2(0, 0)] = gCloudAmbientPartial[0][0];
		RWCloudAmbientSH[int2(1, 0)] = gCloudAmbientPartial[1][0];
		RWCloudAmbientSH[int2(2, 0)] = gCloudAmbientPartial[2][0];
		RWCloudAmbientSH[int2(3, 0)] = float4(gCloudAmbientPartial[3][0].rgb + reflection * gCloudAmbientPartial[5][0].rgb, 0.0);
		RWCloudAmbientSH[int2(4, 0)] = float4(gCloudAmbientPartial[4][0].rgb + reflection * gCloudAmbientPartial[6][0].rgb, 0.0);
		RWCloudAmbientSH[int2(5, 0)] = float4(reflection, 0.0);
	}
}

struct CloudRaySegments
{
	float2 nearSegment;
	float2 farSegment;
	float nearLength;
	float length;
};

CloudRaySegments ClipCloudRaySegments(float2 outer, float2 inner, float range)
{
	CloudRaySegments segments = (CloudRaySegments)0;
	const float entry = max(outer.x, 50.0 * GAME_UNITS_PER_METER);
	const float exit = min(outer.y, range);
	if (exit <= entry)
		return segments;

	segments.nearSegment = float2(entry, exit);
	// A grazing ray can cross the layer twice; skip the clear gap between crossings.
	if (inner.y > entry && inner.x < exit) {
		segments.nearSegment.y = clamp(inner.x, entry, exit);
		segments.farSegment = float2(clamp(inner.y, entry, exit), exit);
		if (segments.nearSegment.y <= entry) {
			segments.nearSegment = segments.farSegment;
			segments.farSegment = 0.0;
		}
	}
	segments.nearLength = segments.nearSegment.y - segments.nearSegment.x;
	segments.length = segments.nearLength + segments.farSegment.y - segments.farSegment.x;
	return segments;
}

CloudRaySegments GetCloudRaySegments(float3 origin, float3 dir, float bottomAltitude, float topAltitude,
	VolumetricCloudData info)
{
	if (topAltitude <= bottomAltitude)
		return (CloudRaySegments)0;
	return ClipCloudRaySegments(
		IntersectCloudBelow(origin, dir, topAltitude, info.cloudCurvatureRadius),
		IntersectCloudBelow(origin, dir, bottomAltitude, info.cloudCurvatureRadius), info.rayMarchRange);
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

// Bound the omitted background contribution without rescaling translucent rays.
static const float CloudViewTransmissionCutoff = 0.01;

float CloudViewStep(float distance, float angularFootprint)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float stepScale = angularFootprint > 0.0 ? 2.0 : info.marchStepScale;
	const float viewStep = (3.0 + distance * GAME_UNIT_TO_M * (60.0 / 16384.0)) * stepScale * GAME_UNITS_PER_METER;
	if (angularFootprint <= 0.0)
		return viewStep;
	// Resolve the final reflection texel, independently of the checkerboard spacing.
	return max(viewStep, max(3.0 * GAME_UNITS_PER_METER, distance * angularFootprint * 0.25));
}

float WeatherViewStep(float distance, float intervalLength, float altitude, VolumetricCloudData info)
{
	// V5 marches base clouds at 75 source metres, and the deep layer at 300.
	// Both layers share a ray here. At their boundary retain base-layer resolution.
	const float unit = info.weatherWorldScale * GAME_UNITS_PER_METER;
	const float aboveBase = altitude - info.weatherBaseTopAltitude;
	const float base = (aboveBase > 0.0 ? 300.0 : 75.0) * unit;
	const float nearStep = base * lerp(0.3, 1.0, saturate(intervalLength / (10.0 * base)));
	const float step = min(nearStep + 0.006 * distance, 1500.0 * unit) * info.marchStepScale;
	return aboveBase > 0.0 ? min(step, max(75.0 * unit * info.marchStepScale, aboveBase)) : step;
}

struct CloudMaterialSample
{
	bool in_layer;
	float dimension_profile;
	float referenceExtinction;
	float weatherAmbientResponse;
	float coverage;
	float height_fraction;
	float shaping_start;
	float2 altitude;
	float top_type;
	float bottom_type;
	float storm;
	float rawStorm;
	bool stormVolume;
	uint stormIndex;
	float stormBlend;
	float replacement;
#ifdef CLOUD_VOXELS
	float voxelBlend;
	float voxelDensityScale;
#endif
#ifndef NO_CLOUD_STORMS
	float stormActivation;
	float plumeCoordinate;
	float plumeDensity;
	float2 plumeEnvelope;
#	ifndef VOLCANIC_STORMS_ONLY
	float stormDensity;
	float stormAlbedo;
	float2 stormAmbientPhase;
	float3 stormEmission;
	uint stormSecondaryIndex;
	float stormSecondaryBlend;
#	endif
#endif
};

void initCloudMaterialSample(out CloudMaterialSample ndf)
{
	ndf = (CloudMaterialSample)0;
#ifndef NO_CLOUD_STORMS
	ndf.plumeDensity = 1.0;
#endif
}

bool IsVolcanic(CloudStormData storm);

float CloudNdfProfile(inout CloudMaterialSample ndf)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float top = TexCloudProfileLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, ndf.height_fraction), 0).g;
	float expandedTop = top;
	[branch] if (info.topExpansion > 0.0)
		expandedTop = TexCloudAdjustmentLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, ndf.height_fraction), 0).r;
	const float bottom = TexCloudProfileLUT.SampleLevel(TransmittanceSampler, float2(ndf.bottom_type, ndf.height_fraction), 0).r;
	// The eruption's authored top types must not inherit the weather's anvil
	// expansion, which otherwise turns low type values into full-height columns.
	float topProfile = ndf.stormVolume && IsVolcanic(CloudStorms[ndf.stormIndex]) ? top : lerp(top, max(top, expandedTop), info.topExpansion);
	[branch] if (ndf.storm > 0.15 && (!ndf.stormVolume || !IsVolcanic(CloudStorms[ndf.stormIndex])))
		topProfile = max(top, TexCloudProfileLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, 1.0 - ndf.height_fraction), 0).g);
	const float verticalProfile = max(topProfile * bottom, 0.0);
	const float heightFade = saturate(ndf.height_fraction / info.coverageHeightRange);
	const float exponent = lerp(lerp(lerp(info.coverageBottomPower, 1.0, heightFade), 1.0, saturate((ndf.storm - 0.15) * 20.0)), 1.0, heightFade);
	const float coverageProfile = pow(ndf.coverage, exponent);
	return verticalProfile * coverageProfile;
}

struct WeatherOpticalSample
{
	float2 edge;        // Unclamped erosion ramp for base and anvil
	float2 extinction;  // Inverse metres
	float2 power;       // Density exponent of the erosion ramp
};

// Replace each clipped, powered linear ramp by a slab of equal column,
// anchored at its denser end. On the rising part integrate x^power between
// the two endpoints; above saturation the integrand is constant.
float2 WeatherIntervalColumn(WeatherOpticalSample a, WeatherOpticalSample b, out float2 width)
{
	const float2 sum = a.extinction + b.extinction;
	if (all((sum == 0.0) || (min(a.edge, b.edge) >= 1.0))) {
		width = 1.0;
		return 0.5 * sum;
	}
	const float2 lo = min(a.edge, b.edge), hi = max(a.edge, b.edge);
	const float2 invRange = rcp(max(hi - lo, 1e-8));
	const float2 start = saturate(-lo * invRange), end = saturate((1.0 - lo) * invRange);
	const float2 ratio = saturate(lo) / max(saturate(hi), 1e-8);
	const float2 power = lerp(a.power, b.power, b.extinction / max(sum, 1e-12));
	const float2 relativeDensity = pow(ratio, max(power, 0.3));
	const float2 rampMean = ratio > 0.999 ? 1.0 :
	                                        (1.0 - ratio * relativeDensity) / (max(1.0 - ratio, 1e-8) * (power + 1.0));
	width = 1.0 - end + (end - start) * rampMean;
	return width * sum / (1.0 + relativeDensity);
}

struct CloudDensityContext
{
	CloudMaterialSample ndf;
	float3 noise_coordinates;
	float eye_distance;
	float light_density;
	WeatherOpticalSample weather;
	float weather_height_fraction;
	float weatherScatteringExtinction;
	float3 storm_displacement;
};

#include "PhysicalSky/WeatherCloudShape.hlsli"

float4 SampleCloudStormNoise(float3 positionMeters, float height, float mip, uint index, CloudMaterialSample ndf);

float2 WeatherFieldPosition(float2 position, VolumetricCloudData info)
{
	return (position - info.noiseWindOffset) * GAME_UNIT_TO_M;
}

float4 SampleCloudWeather(float2 fieldMeters, VolumetricCloudData info)
{
	return SampleCloudWeatherMap(fieldMeters * (info.weatherFrequency * GAME_UNITS_PER_METER), WeatherProceduralWeight, FiniteCloudWeather);
}

float2 WeatherColumnBounds(float2 position, VolumetricCloudData info)
{
	const float4 weather = SampleCloudWeather(WeatherFieldPosition(position, info), info);
	const float base = info.lowCloudBaseAltitude;
	const float layerTop = weather.x > 0.0 ? lerp(base, info.weatherBaseTopAltitude, WeatherTypeProperties(weather.y, 0).y) : base;
	const float stormTop = weather.z > 0.0 ? lerp(base, info.weatherStormTopAltitude, WeatherTypeProperties(weather.w, 1).y) : base;
	return float2(base, min(max(layerTop, stormTop), info.lowCloudTopAltitude));
}

#ifndef NO_CLOUD_STORMS
float sampleCloudDensityFromContext(
	inout CloudDensityContext density_context, float mipBias, float minimumMip)
{
	density_context.light_density = 0.0;
	const CloudMaterialSample ndf = density_context.ndf;
	if (!ndf.in_layer || ndf.dimension_profile <= 1e-10)
		return 0.0;

	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const bool risingBillows = IsVolcanic(CloudStorms[ndf.stormIndex]);
	const float biasedMip = risingBillows ? mipBias : floor(ndf.dimension_profile * 3.0 + mipBias);
	const float mip = max(biasedMip, min(minimumMip, biasedMip + 1.0));
	float4 noise = 0.0;
	[branch] if (info.skipShapeNoise == 0u)
		noise = SampleCloudStormNoise(density_context.noise_coordinates, ndf.height_fraction, mip, ndf.stormIndex, ndf);
	float profile = ndf.dimension_profile;
#	ifndef NO_CLOUD_STORMS
	if (risingBillows) {
		// G is generated by the local Alligator kernel. Use its large cells to
		// displace the carrier surface in both directions, before fine erosion.
		// A clipped gain makes every value above .4 the same displacement,
		// flattening the lobe tips onto an offset copy of the carrier surface.
		// Keep the full Alligator range; sqrt also matches the rounded noise mix.
		const float lobe = info.skipShapeNoise != 0u ? 0.0 : ndf.plumeEnvelope.y * (sqrt(noise.g) - .35);
		profile *= saturate((lobe - ndf.plumeEnvelope.x) / 90.0 + .5);
		density_context.ndf.dimension_profile = profile;
	}
#	endif
	const float eroded = info.skipShapeNoise != 0u ? saturate(profile) :
	                                                 ReconstructCloudNoiseDensity(noise, profile, ndf.top_type, ndf.height_fraction, density_context.eye_distance, ndf.storm, risingBillows);
#	ifdef VOLCANIC_STORMS_ONLY
	const float densityScale = CloudStorms[ndf.stormIndex].driftDensity.z * ndf.plumeDensity;
#	else
	const float densityScale = ndf.stormDensity * ndf.plumeDensity;
#	endif
	float lightDensity;
	const float density = ShapeCloudBaseDensity(eroded, ndf.height_fraction, info.bottomDensityWidth, info.bottomDensityPower,
							  ndf.shaping_start, ndf.rawStorm, lightDensity, risingBillows) *
	                      densityScale;
	density_context.light_density = risingBillows ? density : lerp(density, lightDensity * densityScale, ndf.stormActivation);
	return density;
}
#endif

struct WeatherDensityParameters
{
	float3 eye;
	float2 windOffset;
	float2 thickness;
	float2 layerDepth;
	float3 peakExtinction;
	float4 shaping;
	float baseAltitude;
	float curvatureRadius;
	float worldScale;
	float2 weatherFrequency;
	bool skipNoise;
};

WeatherDensityParameters MakeWeatherDensityParameters(VolumetricCloudData info)
{
	WeatherDensityParameters parameters;
	parameters.eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	parameters.windOffset = info.noiseWindOffset.xy;
	parameters.thickness = float2(info.weatherBaseTopAltitude, info.weatherStormTopAltitude) - info.lowCloudBaseAltitude;
	parameters.layerDepth = parameters.thickness * (GAME_UNIT_TO_M / info.weatherWorldScale);
	parameters.peakExtinction = float3(StratusExtinction, info.cumulusExtinction, StormExtinction);
	parameters.shaping = float4(info.weatherDetail.x, CloudBaseHeightVariation / info.weatherWorldScale, info.weatherDetail.z / info.weatherWorldScale, info.weatherDetail.w);
	parameters.baseAltitude = info.lowCloudBaseAltitude;
	parameters.curvatureRadius = info.cloudCurvatureRadius;
	parameters.worldScale = info.weatherWorldScale;
	parameters.weatherFrequency = info.weatherFrequency * GAME_UNITS_PER_METER;
	parameters.skipNoise = info.skipShapeNoise != 0u;
	return parameters;
}

#if defined(BUILD_WEATHER_BOUNDS) || defined(WEATHER_EMPTY_BOUNDS)
#	include "PhysicalSky/WeatherCloudBounds.hlsli"
#endif

WeatherOpticalSample SampleWeatherOptics(float3 pos, CloudLayer cloud, WeatherDensityParameters parameters, float mip, uint layers,
	out float4 weather, out float3 body, out float3 anvil, out float2 peak, out float relativeAltitude)
{
	WeatherOpticalSample optical;
	optical.edge = -1.0;
	optical.extinction = 0.0;
	optical.power = 1.0;
	weather = 0.0;
	body = 0.0;
	anvil = 0.0;
	peak = 0.0;
	const float altitude = WeatherCloudAltitude(pos, parameters.curvatureRadius);
	relativeAltitude = altitude - parameters.baseAltitude;
	if (altitude <= cloud.lowestAltitude || altitude >= cloud.highestAltitude)
		return optical;

	const float2 field = (pos.xy - parameters.windOffset) * GAME_UNIT_TO_M;
	weather = SampleCloudWeatherMap(field * parameters.weatherFrequency, WeatherProceduralWeight, FiniteCloudWeather);
	const bool baseActive = (layers & 1u) != 0u && weather.x > 0.0 && relativeAltitude <= parameters.thickness.x;
#ifdef WEATHER_BASE_ONLY
	const bool anvilActive = false;
#else
	const bool anvilActive = (layers & 2u) != 0u && weather.z > 0.0 && relativeAltitude <= parameters.thickness.y;
#endif
	if (!baseActive && !anvilActive)
		return optical;

	const float3 radial = WeatherNoiseDirection(field, parameters.worldScale);
	const float3 detailPosition = float3(field, relativeAltitude * GAME_UNIT_TO_M) / parameters.worldScale;
	const float detailMip = max(mip, log2(1.0 + distance(pos, parameters.eye) * (GAME_UNIT_TO_M / 1500.0)));
	float2 baseErosion = float2(-1.0, 1.0), anvilErosion = baseErosion;
	if (baseActive)
		body = SampleWeatherLayer(weather.xy, parameters.layerDepth.x, 0u, radial, detailPosition, parameters.shaping, mip, detailMip, parameters.skipNoise, baseErosion);
	if (anvilActive)
		anvil = SampleWeatherLayer(weather.zw, parameters.layerDepth.y, 1u, radial, detailPosition, parameters.shaping, mip, detailMip, parameters.skipNoise, anvilErosion);
	peak = WeatherPeakExtinction(float2(body.z, anvil.z), weather.y, parameters.peakExtinction);
	optical.edge = float2(baseErosion.x, anvilErosion.x);
	optical.power = float2(baseErosion.y, anvilErosion.y);
	optical.extinction = float2(body.x * WeatherBaseExtinctionScale(weather.x, WeatherProceduralWeight), anvil.x) * peak;
	return optical;
}

float sampleWeatherCloudDensity(
	float3 pos, CloudLayer cloud, float mipBias, float viewDistance, float minimumMip,
	out CloudDensityContext context, uint layers = 3u)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const WeatherDensityParameters parameters = MakeWeatherDensityParameters(info);
	context = (CloudDensityContext)0;
	float4 weather;
	float3 body, anvil;
	float2 peak;
	float relativeAltitude;
	context.weather = SampleWeatherOptics(pos, cloud, parameters, max(mipBias, minimumMip), layers, weather, body, anvil, peak, relativeAltitude);
	const float2 extinction = context.weather.extinction;
	const float2 height = relativeAltitude / parameters.thickness;
	const float density = extinction.x + extinction.y;
	context.ndf.dimension_profile = max(max(body.y, anvil.y), 0.0);
	context.ndf.in_layer = density > 0.0 || context.ndf.dimension_profile > 0.0;
	if (density <= 0.0)
		return 0.0;

	const float layerBlend = extinction.y / density;
	context.ndf.referenceExtinction = lerp(peak.x, peak.y, layerBlend);
	// Calibrate the diffuse response against the same condensate as extinction.
	const float baseCondensate = WeatherBaseExtinctionScale(weather.x, WeatherProceduralWeight);
	context.weatherScatteringExtinction = lerp(peak.x * baseCondensate, peak.y, layerBlend);

	context.ndf.weatherAmbientResponse = lerp(sqrt(saturate(1.0 - body.y)), sqrt(saturate(1.0 - anvil.y)), layerBlend);
	context.ndf.height_fraction = saturate(lerp(height.x, height.y, layerBlend));
	context.ndf.coverage = lerp(weather.x, weather.z, layerBlend);
	context.ndf.top_type = lerp(weather.y, weather.w, layerBlend);
	context.ndf.altitude = float2(info.lowCloudBaseAltitude, lerp(info.weatherBaseTopAltitude, info.weatherStormTopAltitude, layerBlend));
	context.weather_height_fraction = context.ndf.height_fraction;
	context.light_density = density;
	return context.light_density;
}

#ifdef CLOUD_VOXELS
#	define sampleCloudDensity sampleNonVoxelCloudDensity
#endif
#include "PhysicalSky/CloudStorm.hlsli"
#ifdef CLOUD_VOXELS
#	undef sampleCloudDensity
#endif
#include "PhysicalSky/CloudVoxel.hlsli"

#include "PhysicalSky/MidCloud.hlsli"

float3 sampleExternalSunTransmittance(float3 pos, float3 sun_dir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);

	// Finite scene-shadow coverage would project a camera-following boundary into the cloud layer.
	return SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, pos_planet, sun_dir, SharedData::physSkyData.diskAngularRadius[CloudDiskIndex()]);
}

#include "PhysicalSky/CloudAmbient.hlsli"
#include "PhysicalSky/WeatherCloudLight.hlsli"
#if defined(CLOUD_MAIN_DISPATCH) && defined(NO_CLOUD_STORMS) && !defined(CLOUD_VOXELS) && !defined(CLOUD_REFLECTION)
#	define WEATHER_VIEW
#endif
#include "PhysicalSky/CloudLighting.hlsli"

struct VolumetricCloudResult
{
	float2 lightning;
	float3 transmittance;
	float3 lum;
	float cloud_depth;
	float2 motionMetadata;
	float4 stormMotion;
#ifdef CLOUD_REFLECTION
	float3 transportLum;
	float2 rawTransmission;
	float4 integralWeights;
#endif
};

float CirrusDensity(float2 position, out float profile)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 weather = saturate(TexCirrusWeather.SampleLevel(TileableSampler, position * (GAME_UNIT_TO_M / 16384.0 * info.cirrusUvInvSize) + 0.5, 0));
	const float3 patterns = TexCirrusPatterns.SampleLevel(TileableSampler, (position - info.cirrusWindOffset) * (GAME_UNIT_TO_M * 0.0002331 * info.cirrusUvInvSize), 0);
	const float3 squared = patterns * patterns;
	profile = lerp(lerp(squared.b, squared.r, saturate(weather.y * 2.0)), squared.g, saturate(weather.y * 2.0 - 1.0));
	profile = pow(max(profile, 1e-10), 1.9 - weather.x * 1.8) * saturate(weather.x * weather.x * weather.x * 2.0);
	return (profile * 2.0) * info.cirrusDensityScale;
}

// Delta-scaled forward transport belongs to the incident background, whether
// that is sky, terrain, or another cloud sheet. Keep it in the transmission
// coefficient instead of substituting sky radiance into the scattering output.
float CloudBackgroundTransmittance(float direct, float transport)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float forwardWeight = saturate(info.ambientStrength * info.cloudLightingScale);
	return lerp(direct, max(direct, transport), forwardWeight);
}

void IntegrateCirrus(float3 position, float3 direction, float distance, float phase, CloudAmbient cloudAmbient,
	inout float3 radiance, inout float transmittance, inout float ambientTransmittance,
	inout float depthSum, inout float weightSum, inout float cirrusWeight, CloudAerialRay air = (CloudAerialRay)0)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const bool ambientEnabled = info.ambientStrength > 0.0 && (info.msAlbedo > 0.0 || CloudStormCount() > 0u);
	float profile;
	const float density = CirrusDensity(position.xy, profile);
	if (density <= 1e-10)
		return;
	const float viewDepth = density * 10.0;
	const float stepTransmittance = exp(-viewDepth);
	if (max(transmittance, ambientEnabled ? ambientTransmittance : 0.0) <= 0.1) {
		transmittance *= stepTransmittance;
		ambientTransmittance *= stepTransmittance;
		return;
	}
	const float3 shellPosition = position + float3(-FrameBuffer::CameraPosAdjust.xy, info.cloudCurvatureRadius);
	const float3 normal = normalize(shellPosition);
	const float lightCosine = dot(normal, info.dirlightDir);
	const float viewCosine = dot(normal, direction);
	const float lightStep = CloudCirrusLightDistance(lightCosine) * 0.25;
	float lightDepth = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		float unusedProfile;
		const float shadowDistance = (float(i) + 0.5) * lightStep;
		lightDepth += CirrusDensity(position.xy + info.dirlightDir.xy * (shadowDistance * GAME_UNITS_PER_METER), unusedProfile);
	}
	// The center-to-boundary samples estimate half of the layer's light optical depth.
	lightDepth *= 2.0 * lightStep * 10.0 / info.cirrusLightThickness * info.sunExtinction;
	// A capped grazing light path samples only part of the layer's vertical extent.
	const float lightSpan = clamp(lightCosine * (8.0 * lightStep / info.cirrusLightThickness), -1.0, 1.0);
	const float tangentCosine = sqrt(info.cirrusLightThickness / ((info.cloudCurvatureRadius + info.cirrusAltitude) * GAME_UNIT_TO_M));
	const float viewSpan = clamp(viewCosine / tangentCosine, -1.0, 1.0);
	const float nearFraction = 0.5 * (1.0 + viewSpan * lightSpan);
	const float transmission = CloudCirrusTransmission(viewDepth, lightDepth, nearFraction);
	const float sun = transmission * phase;
	const float3 sunlight = sampleExternalSunTransmittance(position, info.dirlightDir) * GetCloudDirectionalLightColor();
	const float ambientResponse = info.ambientStrength * (direction.z + 1.0) * pow(saturate(1.0 - profile * 0.3), 0.2);
	const float3 ambientRadiance = CloudAmbientRadiance(position + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius), cloudAmbient) * ambientResponse;
	// The collapsed forward peak transports sheet radiance as well as sky radiance.
	const float weight = CloudBackgroundTransmittance(transmittance, ambientEnabled ? ambientTransmittance : transmittance) * (1.0 - stepTransmittance);
	const ViewAtmosphere::Medium perspective = SampleCloudAerialPerspective(air, distance);
	radiance += weight * (perspective.transmittance * info.cloudLightingScale * (sun * sunlight + ambientRadiance) + perspective.luminance);
	const float depthWeight = transmittance * (1.0 - stepTransmittance);
	depthSum += depthWeight * distance;
	weightSum += depthWeight;
	cirrusWeight += depthWeight;
	transmittance *= stepTransmittance;
	ambientTransmittance *= stepTransmittance;
}

#ifdef CLOUD_REFLECTION
// Resolve the sheet once with unit incident transmission, then retain the two
// transport terms separately until the ray segments have been composed.
void IntegrateReflectionCirrus(float3 position, float3 direction, float distance, float phase, CloudAmbient ambient,
	inout VolumetricCloudResult result, inout float transmission, inout float transportTransmission,
	inout float depthSum, inout float weightSum, inout float cirrusWeight)
{
	float3 source = 0.0;
	float sheetTransmission = 1.0, sheetTransport = 1.0;
	float sheetDepth = 0.0, sheetWeight = 0.0, sheetCirrusWeight = 0.0;
	IntegrateCirrus(position, direction, distance, phase, ambient, source, sheetTransmission, sheetTransport,
		sheetDepth, sheetWeight, sheetCirrusWeight);
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const bool ambientEnabled = info.ambientStrength > 0.0 && (info.msAlbedo > 0.0 || CloudStormCount() > 0u);
	const float forwardWeight = ambientEnabled ? saturate(info.ambientStrength * info.cloudLightingScale) : 0.0;
	result.lum += (1.0 - forwardWeight) * transmission * source;
	result.transportLum += forwardWeight * transportTransmission * source;
	depthSum += transmission * sheetDepth;
	weightSum += transmission * sheetWeight;
	cirrusWeight += transmission * sheetCirrusWeight;
	transmission *= sheetTransmission;
	transportTransmission *= sheetTransport;
}
#endif

bool CloudIntervalContains(CloudRaySegments segments, float distance)
{
	return (distance >= segments.nearSegment.x && distance < segments.nearSegment.y) ||
	       (distance >= segments.farSegment.x && distance < segments.farSegment.y);
}

VolumetricCloudResult RenderVolumetricCloudRay(float3 direction, float3 eye, float sceneDistance, float2 jitter, float apShadow, float angularFootprint, float2 raySegment = float2(0.0, 1.0))
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	VolumetricCloudResult result;
	result.lightning = 0.0;
	result.transmittance = 1.0;
	result.lum = 0.0;
#ifdef CLOUD_REFLECTION
	result.transportLum = 0.0;
	result.rawTransmission = 1.0;
	result.integralWeights = 0.0;
	const float stopTransmission = 0.0001;
#elif defined(WEATHER_VIEW)
	const float scatteringCutoff = 0.001;
	// Half the smallest positive R16_FLOAT value. Any remaining background
	// transmission at or below this value rounds to zero in the trace target.
	const float stopTransmission = 2.98023223876953125e-8;
#else
	const float stopTransmission = CloudViewTransmissionCutoff;
#endif
	result.motionMetadata = 0.0;
	result.stormMotion = 0.0;
	bool sceneLimited = sceneDistance > 0.0;
	const float3 planetEye = eye + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float2 ground = IntersectSpherePair(planetEye, direction, info.planetRadius);
	if (dot(planetEye, direction) < 0.0 && ground.y > 0.0) {
		sceneDistance = sceneLimited ? min(sceneDistance, max(ground.x, 0.0)) : max(ground.x, 0.0);
		sceneLimited = true;
	}
	const CloudRaySegments bounds = ClipCloudRaySegments(
		IntersectWeatherCloudBelow(eye, direction, info.lowCloudTopAltitude, info.cloudCurvatureRadius),
		IntersectWeatherCloudBelow(eye, direction, info.lowCloudBaseAltitude, info.cloudCurvatureRadius), info.rayMarchRange);
	CloudRaySegments low = bounds;
	float2 stormInterval = CloudStormRayInterval(eye, direction);
	const float2 voxelInterval = CloudVoxelInterval(eye, direction, 0.0, false);
	const bool voxelRay = voxelInterval.y > voxelInterval.x;
	if (voxelRay)
		stormInterval = stormInterval.y > stormInterval.x ? float2(min(stormInterval.x, voxelInterval.x), max(stormInterval.y, voxelInterval.y)) : voxelInterval;
	const bool stormRay = stormInterval.y > stormInterval.x;
	const CloudRaySegments weatherSegments = bounds;
	if (stormRay) {
		// The world-anchored storm is not represented by the curved weather boundary mesh.
		float start = stormInterval.x;
		if (low.nearSegment.y > low.nearSegment.x)
			start = min(start, low.nearSegment.x);
		if (low.farSegment.y > low.farSegment.x)
			start = min(start, low.farSegment.x);
		const float end = max(stormInterval.y, max(low.nearSegment.y, low.farSegment.y));
		low.nearSegment = float2(start, end);
		low.farSegment = 0.0;
	}
	if (sceneLimited) {
		low.nearSegment.y = max(low.nearSegment.x, min(low.nearSegment.y, sceneDistance));
		low.farSegment.y = max(low.farSegment.x, min(low.farSegment.y, sceneDistance));
	}
#ifdef CLOUD_REFLECTION
	// Allocate reflection samples over occupied ray intervals. The logarithmic
	// coordinate follows the distance-based march spacing without serial steps.
	const float offset = (3.0 / (60.0 / 16384.0)) * GAME_UNITS_PER_METER;
	const float4 arc = log2(1.0 + max(float4(low.nearSegment, low.farSegment), 0.0) / offset);
	const float nearArc = max(arc.y - arc.x, 0.0);
	const float farArc = max(arc.w - arc.z, 0.0);
	const float2 coordinate = raySegment * (nearArc + farArc);
	const float2 mapped = (exp2(coordinate <= nearArc ? arc.xx + coordinate : arc.zz + coordinate - nearArc) - 1.0) * offset;
	// Include a cirrus sheet outside the finite low-cloud intervals in the first
	// or last segment, without integrating an infinite volume step.
	raySegment = float2(raySegment.x == 0.0 ? 0.0 : mapped.x, raySegment.y == 1.0 ? 1e30 : mapped.y);
	low.nearSegment = float2(max(low.nearSegment.x, raySegment.x), min(low.nearSegment.y, raySegment.y));
	low.farSegment = float2(max(low.farSegment.x, raySegment.x), min(low.farSegment.y, raySegment.y));
	low.nearSegment.y = max(low.nearSegment.y, low.nearSegment.x);
	low.farSegment.y = max(low.farSegment.y, low.farSegment.x);
#endif
	low.nearLength = low.nearSegment.y - low.nearSegment.x;
	low.length = low.nearLength + (low.farSegment.y - low.farSegment.x);
	result.cloud_depth = info.rayMarchRange;
	if (bounds.length > 0.0)
		result.cloud_depth = bounds.nearSegment.x > 50.0 * GAME_UNITS_PER_METER ? bounds.nearSegment.x : bounds.nearSegment.y;
	if (stormRay)
		result.cloud_depth = min(result.cloud_depth, stormInterval.x);
	float cirrusDistance = sceneDistance;
	bool cirrusPending = false;

	if (low.length <= 0.0 && !cirrusPending)
		return result;

	// Split the volume at the sheet so camera altitude and cloud bounds cannot reverse compositing order.
	float4 boundaries = float4(low.nearSegment, low.farSegment);
	boundaries = float4(min(boundaries.x, boundaries.y), max(boundaries.x, boundaries.y),
		min(boundaries.z, boundaries.w), max(boundaries.z, boundaries.w));
	boundaries = float4(min(boundaries.x, boundaries.z), min(boundaries.y, boundaries.w),
		max(boundaries.x, boundaries.z), max(boundaries.y, boundaries.w));
	boundaries.yz = float2(min(boundaries.y, boundaries.z), max(boundaries.y, boundaries.z));
	const float sheetBoundary = cirrusPending ? cirrusDistance : 0.0;
	const float lastBoundary = max(boundaries.w, sheetBoundary);
	boundaries = min(boundaries, float4(sheetBoundary, max(boundaries.xyz, sheetBoundary)));
	const float phase = ApplyCloudForwardPeakOcclusion(CloudLowPhase(direction));
	const float cirrusPhase = cirrusPending ? CloudCirrusPhase(direction) : 0.0;
	const CloudAmbient ambient = MakeCloudAmbient();
	const bool ambientEnabled = info.ambientStrength > 0.0 && (info.msAlbedo > 0.0 || CloudStormCount() > 0u);
	float ambientTransmittance = 1.0;
	const CloudLayer layer = GetCloudLayer(info);
	const WeatherSolarDisk solarDisk = MakeWeatherSolarDisk(layer);
	float noiseTexelsPerMeter = 0.0;
	float maximumNoiseMip = 0.0;
	if (angularFootprint > 0.0) {
		uint width, height, depth, levels;
		TexWeatherWorley.GetDimensions(0, width, height, depth, levels);
		noiseTexelsPerMeter = width / (2300.0 * info.weatherWorldScale);
		maximumNoiseMip = float(levels - 1u);
	}
	float depthSum = 0.0;
	float weightSum = 0.0;
	float cirrusWeight = 0.0;
	float heightSum = 0.0;
	float transmittance = 1.0;
#ifndef CLOUD_REFLECTION
	const CloudAerialRay air = MakeCloudAerialRay(direction);
	float3 foregroundAir = 0.0;
#endif
#if defined(CLOUD_MAIN_DISPATCH) && !defined(CLOUD_VOXELS)
#	ifdef WEATHER_VIEW
	float2 sunTransmission = 1.0;
#	else
	float lightDepth = 0.0;
#	endif
	float lightDepthEnd = -1.0;
#	ifndef WEATHER_VIEW
	uint lightMaterial = 0xffffffffu;
#	endif
#endif
#ifdef WEATHER_VIEW
	const WeatherDensityParameters weatherParameters = MakeWeatherDensityParameters(info);
	const float solarCosine = dot(direction, info.dirlightDir);
#endif
	[loop] for (uint interval = 0u; interval < 4u && max(transmittance, ambientEnabled ? ambientTransmittance : 0.0) > stopTransmission; ++interval)
	{
		const float begin = interval == 0u ? boundaries.x : (interval == 1u ? boundaries.y : (interval == 2u ? boundaries.z : boundaries.w));
		const float end = interval == 0u ? boundaries.y : (interval == 1u ? boundaries.z : (interval == 2u ? boundaries.w : lastBoundary));
		if (cirrusPending && cirrusDistance <= begin) {
#ifdef CLOUD_REFLECTION
			IntegrateReflectionCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, cirrusPhase, ambient,
				result, transmittance, ambientTransmittance, depthSum, weightSum, cirrusWeight);
#else
			IntegrateCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, cirrusPhase, ambient,
				result.lum, transmittance, ambientTransmittance, depthSum, weightSum, cirrusWeight);
#endif
			cirrusPending = false;
		}
		if (end <= begin || !CloudIntervalContains(low, 0.5 * (begin + end)))
			continue;
		float distance = begin;
#ifdef WEATHER_VIEW
		CloudDensityContext previous;
		sampleWeatherCloudDensity(eye + direction * begin, layer, 0.0, begin * GAME_UNIT_TO_M, 0.0, previous);
		CloudDensityContext resumeSample;
		float resumeDistance = begin;
#	ifdef WEATHER_EMPTY_BOUNDS
		bool previousSkipped = false;
#	endif
		lightDepthEnd = -1.0;
#endif
		float2 stormPiece = 0.0;
		float2 voxelPiece = 0.0;
		[loop] while (distance < end && max(transmittance, ambientEnabled ? ambientTransmittance : 0.0) > stopTransmission)
		{
#ifdef CLOUD_REFLECTION
			// A stratum represents its complete finite interval. Use the same
			// extinction and material blend as the view, with footprint-filtered
			// noise and the coarse voxel field supplied by CLOUD_VOXEL_COARSE.
			const float step = end - distance;
			const float sampleDistance = distance + jitter.x * step;
			const float3 pos = eye + direction * sampleDistance;
			const float viewDistance = sampleDistance * GAME_UNIT_TO_M;
			const float minimumMip = min(log2(max(viewDistance * angularFootprint * noiseTexelsPerMeter, 1.0)), maximumNoiseMip);
			CloudDensityContext densityContext;
			const float extinction = sampleCloudDensity(pos, layer, 0.0, viewDistance, minimumMip, densityContext);
			distance = end;
#else
			if (voxelRay && distance >= voxelPiece.y)
				voxelPiece = CloudVoxelInterval(eye, direction, distance, true);
			if (stormRay && !CloudIntervalContains(weatherSegments, distance)) {
				if (distance >= stormPiece.y)
					stormPiece = CloudStormNextInterval(eye, direction, distance);
				if (!(distance >= stormPiece.x && distance < stormPiece.y) && !(distance >= voxelPiece.x && distance < voxelPiece.y)) {
					float next = end;
					if (weatherSegments.nearSegment.y > weatherSegments.nearSegment.x && weatherSegments.nearSegment.x > distance)
						next = min(next, weatherSegments.nearSegment.x);
					if (weatherSegments.farSegment.y > weatherSegments.farSegment.x && weatherSegments.farSegment.x > distance)
						next = min(next, weatherSegments.farSegment.x);
					if (stormPiece.x > distance)
						next = min(next, stormPiece.x);
#	ifdef CLOUD_VOXELS
					if (voxelPiece.x > distance)
						next = min(next, voxelPiece.x);
#	endif
					distance = next;
					if (distance >= end)
						break;
				}
			}
			const float marchEnd = end;
#	ifdef CLOUD_QUALITY_VIEW_STEPS
			const bool useWeatherStep = false;
#	else
			const bool useWeatherStep = UsePerformanceCloudSteps != 0u && !stormRay && !voxelRay;
#	endif
			float step;
			if (useWeatherStep) {
				const float altitude = WeatherCloudAltitude(eye + direction * distance, info.cloudCurvatureRadius);
				step = WeatherViewStep(distance, end - begin, altitude, info);
			} else
				step = CloudViewStep(distance, angularFootprint);
			step = min(step, marchEnd - distance);
#	ifdef CLOUD_VOXELS
			const bool inVoxel = distance >= voxelPiece.x && distance < voxelPiece.y;
			uint2 occupiedVoxels = 0u;
			const bool ndfOccupied = CloudIntervalContains(weatherSegments, distance) || (distance >= stormPiece.x && distance < stormPiece.y);
			if (inVoxel) {
				const float voxelStep = CloudVoxelStep(eye + direction * distance, distance, angularFootprint, occupiedVoxels);
				step = min(ndfOccupied ? min(step, voxelStep) : voxelStep, min(marchEnd, voxelPiece.y) - distance);
				// A positive voxel SDF says nothing about weather or storm density.
				// End an empty-space jump before entering either of those media.
				if (!ndfOccupied) {
					if (weatherSegments.nearSegment.y > weatherSegments.nearSegment.x && weatherSegments.nearSegment.x > distance)
						step = min(step, weatherSegments.nearSegment.x - distance);
					if (weatherSegments.farSegment.y > weatherSegments.farSegment.x && weatherSegments.farSegment.x > distance)
						step = min(step, weatherSegments.farSegment.x - distance);
					if (stormPiece.x > distance)
						step = min(step, stormPiece.x - distance);
				}
			} else if (voxelPiece.x > distance)
				step = min(step, voxelPiece.x - distance);
#	endif
#	ifdef WEATHER_VIEW
			// Sample consecutive endpoints, including both ends of a clipped
			// interval. The first knot jitters the lattice without leaving a gap.
			if (distance == begin && step < end - begin)
				step *= 0.5 + 0.5 * (distance * GAME_UNIT_TO_M < 250.0 ? jitter.x : jitter.y);
			float nextDistance = min(distance + step, end);
			CloudDensityContext next;
			if (resumeDistance > distance) {
				nextDistance = resumeDistance;
				next = resumeSample;
			} else {
				const float3 nextPosition = eye + direction * nextDistance;
#		ifdef WEATHER_EMPTY_BOUNDS
				[branch] if (!any(previous.weather.extinction > 0.0) && CloudWeatherEmptyAt(nextPosition, layer, info, weatherParameters))
				{
					distance = nextDistance;
					previousSkipped = true;
					lightDepthEnd = -1.0;
					continue;
				}
#		endif
				sampleWeatherCloudDensity(nextPosition, layer, 0.0, nextDistance * GAME_UNIT_TO_M, 0.0, next);
#		ifdef WEATHER_EMPTY_BOUNDS
				// Reconstruct the entry with the original signed boundary of the last empty knot.
				if (previousSkipped && any(next.weather.extinction > 0.0))
					sampleWeatherCloudDensity(eye + direction * distance, layer, 0.0, distance * GAME_UNIT_TO_M, 0.0, previous);
				previousSkipped = false;
#		endif
				const bool2 entering = (previous.weather.extinction <= 0.0) && (next.weather.extinction > 0.0) &&
				                       (previous.weather.edge <= 0.0) && (next.weather.edge > 0.0);
				const float peak = next.ndf.referenceExtinction * info.viewExtinction;
				// Refine an optically thick entrance and then resume the original view lattice.
				[branch] if (any(entering) && step * GAME_UNIT_TO_M > 3.0 && step * GAME_UNIT_TO_M * peak > 1.0)
				{
					const float2 crossing = -previous.weather.edge / max(next.weather.edge - previous.weather.edge, 1e-8);
					const float entry = min(entering.x ? crossing.x : 1.0, entering.y ? crossing.y : 1.0);
					const float refinedStep = max(0.125 * step, entry * step + 0.5 * GAME_UNITS_PER_METER / peak);
					const float refinedDistance = distance + refinedStep;
					if (refinedDistance < nextDistance) {
						resumeDistance = nextDistance;
						resumeSample = next;
						nextDistance = refinedDistance;
						sampleWeatherCloudDensity(eye + direction * nextDistance, layer, 0.0, nextDistance * GAME_UNIT_TO_M, 0.0, next);
					}
				}
			}
			step = nextDistance - distance;
			if (!any(previous.weather.extinction + next.weather.extinction > 0.0)) {
				previous = next;
				distance = nextDistance;
				lightDepthEnd = -1.0;
				continue;
			}
			float2 widths;
			const WeatherOpticalSample nearSample = previous.weather;
			const float2 column = WeatherIntervalColumn(nearSample, next.weather, widths);
			const float average = column.x + column.y;
			const float fullDepth = average * info.viewExtinction * step * GAME_UNIT_TO_M;
			const float backgroundTransmission = transmittance * exp(-fullDepth);
			if (transmittance <= scatteringCutoff) {
				if (air.slices != 0u) {
					const float airDistance = distance + step * 0.5;
					foregroundAir += (transmittance - backgroundTransmission) * SampleCloudAerialPerspective(air, airDistance).luminance;
				}
				previous = next;
				distance = nextDistance;
				transmittance = backgroundTransmission;
				ambientTransmittance = backgroundTransmission;
				continue;
			}
			const float2 fractions = column / max(average, 1e-12);
			const float start = dot(fractions, (next.weather.edge > previous.weather.edge) ? 1.0 - widths : 0.0);
			const float width = dot(fractions, widths);
			const float previousDensity = dot(previous.weather.extinction, 1.0);
			const float nextDensity = dot(next.weather.extinction, 1.0);
			const float blend = nextDensity / max(previousDensity + nextDensity, 1e-12);
			CloudDensityContext densityContext = next;
			densityContext.ndf.dimension_profile = lerp(previous.ndf.dimension_profile, next.ndf.dimension_profile, blend);
			densityContext.ndf.height_fraction = lerp(previous.ndf.height_fraction, next.ndf.height_fraction, blend);
			densityContext.ndf.referenceExtinction = lerp(previous.ndf.referenceExtinction, next.ndf.referenceExtinction, blend);
			densityContext.weatherScatteringExtinction = lerp(previous.weatherScatteringExtinction, next.weatherScatteringExtinction, blend);
			densityContext.ndf.altitude = lerp(previous.ndf.altitude, next.ndf.altitude, blend);

			densityContext.ndf.weatherAmbientResponse = lerp(previous.ndf.weatherAmbientResponse, next.ndf.weatherAmbientResponse, blend);
			previous = next;
			const float segmentStart = distance + step * start;
			distance = nextDistance;
			if (average <= 0.0)
				continue;
			const float extinction = average / max(width, 1e-8);
			step *= width;
#	else
			const float sampleDistance = distance + (distance * GAME_UNIT_TO_M < 250.0 ? jitter.x : jitter.y) * step;
			const float3 pos = eye + direction * sampleDistance;
			CloudDensityContext densityContext = (CloudDensityContext)0;
			const float viewDistance = distance * GAME_UNIT_TO_M;
			const float minimumMip = min(log2(max(viewDistance * angularFootprint * noiseTexelsPerMeter, 1.0)), maximumNoiseMip);
#		ifdef CLOUD_VOXELS
			// Keep NDF out of voxel-only segments; the original voxel marcher has
			// no weather-density or weather-lighting probes in its inner loop.
			float extinction = 0.0;
			[branch] if (ndfOccupied)
				extinction = sampleNonVoxelCloudDensity(pos, layer, 0.0, viewDistance, minimumMip, densityContext);
			if (any(occupiedVoxels != 0u))
				extinction = AddVoxelDensity(pos, 0.0, viewDistance, 0.0, occupiedVoxels, extinction, densityContext);
#		else
			const float extinction = sampleCloudDensity(pos, layer, 0.0, viewDistance, minimumMip, densityContext);
#		endif
			const bool weatherSample = extinction > 0.0 && !densityContext.ndf.stormVolume;
			distance += step * (useWeatherStep || weatherSample || densityContext.ndf.dimension_profile > 0.05 || voxelRay ? 1.0 : 2.0);
#	endif
#endif
			if (extinction <= 0.0)
				continue;
#ifdef WEATHER_VIEW
			const float viewDepth = fullDepth;
#else
			const float viewDepth = extinction * info.viewExtinction * step * GAME_UNIT_TO_M;
#endif
			const float stepTransmittance = exp(-viewDepth);
			const float opacity = viewDepth < 0.001 ? CloudOpacity(viewDepth) : 1.0 - stepTransmittance;
#ifdef WEATHER_VIEW
			const float absorptionFraction = WeatherAbsorptionCentroid(viewDepth, stepTransmittance, opacity);
			const float sampleDistance = segmentStart + step * absorptionFraction;
			const float3 pos = eye + direction * sampleDistance;
			// Recover both media at the absorption centroid from the same signed
			// erosion ramps used for the view column. This is the first solar knot.
			WeatherOpticalSample local;
			local.edge = lerp(nearSample.edge, next.weather.edge, start + width * absorptionFraction);
			const float2 denserEdge = saturate(max(nearSample.edge, next.weather.edge));
			const float2 fraction = next.weather.extinction / max(nearSample.extinction + next.weather.extinction, 1e-12);
			local.power = lerp(nearSample.power, next.weather.power, fraction);
			local.extinction = (column / max(widths, 1e-8)) * pow(saturate(local.edge) / max(denserEdge, 1e-8), max(local.power, 0.3));
#endif
#ifdef WEATHER_VIEW
			// Weather has unit transport extinction. Visible opacity weights both
			// sun and sky, including the last step through an opaque cloud.
			const float weight = transmittance * opacity;
			const float2 scattering = WeatherScatteringWeights(densityContext.ndf, densityContext.weatherScatteringExtinction,
				dot(local.extinction, 1.0), viewDepth, opacity);
			const float viewMetres = sampleDistance * GAME_UNIT_TO_M;
			const bool interior = all((column == 0.0) || (min(nearSample.edge, next.weather.edge) >= 1.0 &&
															 min(nearSample.extinction, next.weather.extinction) > 0.0));
			if (!interior || viewMetres >= lightDepthEnd) {
				sunTransmission = WeatherCloudLightTransmission(pos, solarDisk, solarCosine, viewMetres, densityContext.ndf.height_fraction,
					local, info, layer, weatherParameters);
				lightDepthEnd = interior ? viewMetres + min(4.0 * step * GAME_UNIT_TO_M, 64.0) : -1.0;
			}
			const float3 source = WeatherViewSource(pos, scattering, CloudAmbientReflectance * densityContext.ndf.weatherAmbientResponse, phase, sunTransmission);
			ViewAtmosphere::Medium perspective = ViewAtmosphere::Identity();
			[branch] if (air.slices != 0u)
			{
				perspective = SampleCloudAerialPerspective(air, sampleDistance);
				result.lum += (weight * perspective.transmittance) * source;
				foregroundAir += weight * perspective.luminance;
			}
			else
			{
				result.lum += weight * source;
			}
#else
#	ifdef CLOUD_REFLECTION
			const ViewAtmosphere::Medium perspective = ViewAtmosphere::Identity();
#	else
			const ViewAtmosphere::Medium perspective = SampleCloudAerialPerspective(air, sampleDistance);
#	endif
#	ifndef CLOUD_REFLECTION
			const float backgroundBefore = CloudBackgroundTransmittance(transmittance, ambientEnabled ? ambientTransmittance : transmittance);
#	endif
			if (transmittance > stopTransmission) {
				float3 source;
				[branch] if (IsWeatherMaterial(densityContext.ndf))
				{
					const float2 scattering = WeatherScatteringWeights(densityContext.ndf, densityContext.weatherScatteringExtinction,
						extinction, viewDepth, opacity);
					const float2 weatherSun = WeatherCloudLightTransmission(pos, solarDisk, dot(direction, info.dirlightDir), viewDistance,
						densityContext.ndf.height_fraction, densityContext.weather, info, layer, MakeWeatherDensityParameters(info));
					source = opacity * ShadeWeatherScattering(pos, scattering, phase, weatherSun);
				}
				else
				{
#	if defined(CLOUD_MAIN_DISPATCH) && !defined(CLOUD_VOXELS)
					const uint material = densityContext.ndf.stormIndex + 1u;
					if (viewDistance >= lightDepthEnd || material != lightMaterial) {
						lightDepth = CloudLightOpticalDepth(pos, direction, densityContext.ndf, extinction, viewDistance, minimumMip);
						lightDepthEnd = viewDistance + min(4.0 * step * GAME_UNIT_TO_M, 64.0);
						lightMaterial = material;
					}
					source = ShadeCloudLighting(pos, densityContext.ndf, extinction, viewDepth, opacity, phase, lightDepth);
#	else
					source = CloudLighting(pos, direction, densityContext.ndf, extinction, viewDepth, opacity, viewDistance, minimumMip, phase);
#	endif
				}
				result.lum += (transmittance * perspective.transmittance) * source;
				if (densityContext.ndf.stormVolume) {
#	if !defined(NO_CLOUD_STORMS) && !defined(VOLCANIC_STORMS_ONLY)
					const float3 emission = densityContext.ndf.stormEmission * saturate(densityContext.ndf.coverage);
#	else
					const float height = StormHeight(pos.z * GAME_UNIT_TO_M, CloudStorms[densityContext.ndf.stormIndex]);
					const float glow = exp(-height / CloudStorms[densityContext.ndf.stormIndex].emission.w) * saturate(densityContext.ndf.coverage);
					const float3 emission = CloudStorms[densityContext.ndf.stormIndex].emission.rgb * glow;
#	endif
					result.lum += (transmittance * opacity * perspective.transmittance) * emission * densityContext.ndf.stormBlend;
				}
			}
			[branch] if (ambientEnabled)
			{
				// Weather uses the same extinction for visible background and for
				// scattered skylight along the view. Preserve the authored storm mix.
				const float2 ambientPhase = CloudMaterialAmbient(densityContext.ndf, float2(1.0, CloudAmbientPhase.y));
				float transport = ambientPhase.x;
#	ifdef CLOUD_VOXELS
				// NVDF opacity is the native density integral, not the NDF forward
				// transport integral. Otherwise details become translucent and the
				// marcher continues through already opaque voxel interiors.
				transport = lerp(transport, 1.0, densityContext.ndf.voxelBlend);
#	endif
				const float reducedDepth = transport * viewDepth;
				const float reducedOpacity = CloudOpacity(reducedDepth);
				const float ambientResponse = IsWeatherMaterial(densityContext.ndf) ? CloudAmbientReflectance * densityContext.ndf.weatherAmbientResponse : 1.0;
				const float3 ambientSource = ambientResponse * CloudAmbientLight::SkySource(pos);
#	ifdef CLOUD_REFLECTION
				result.transportLum += ambientTransmittance * reducedOpacity * ambientPhase.y * info.ambientStrength * info.cloudLightingScale *
#	else
				result.lum += (ambientTransmittance * perspective.transmittance) * reducedOpacity * ambientPhase.y * info.ambientStrength * info.cloudLightingScale *
#	endif
				                       ambientSource;
				ambientTransmittance *= exp(-reducedDepth);
			}
#endif

			if (Lightning::HasAnySource()) {
#ifdef CLOUD_REFLECTION
				const float airTransmission = 1.0;
#else
				// Shared RGB attenuation is applied when resolving the scalar mask.
				const float airTransmission = air.slices == CloudAerialSharedVolume ? 1.0 : perspective.transmittance.r;
#endif
				result.lightning += transmittance * opacity * stepTransmittance * airTransmission *
				                    Lightning::CloudMask(pos + float3(0, 0, info.bottomZ), densityContext.ndf.height_fraction, densityContext.ndf.dimension_profile);
			}

			const float depthWeight = transmittance * opacity;
			depthSum += depthWeight * sampleDistance;
			weightSum += depthWeight;
			if (densityContext.ndf.stormVolume) {
				result.stormMotion.xyz += depthWeight * densityContext.storm_displacement;
				result.stormMotion.w += depthWeight * densityContext.ndf.stormBlend;
			}
#ifdef CLOUD_VOXELS
			// The authored field is stationary. Only its detail noise follows wind;
			// do not apply the weather's height shear to this local volume.
			result.stormMotion.xy += depthWeight * densityContext.ndf.voxelBlend * info.cloudWindDelta;
			result.stormMotion.w += depthWeight * densityContext.ndf.voxelBlend;
#endif
#if defined(NO_CLOUD_STORMS) && !defined(CLOUD_VOXELS)
			heightSum += depthWeight * densityContext.ndf.height_fraction;
#else
			float weatherWeight = 1.0 - densityContext.ndf.stormBlend;
#	ifdef CLOUD_VOXELS
			weatherWeight -= densityContext.ndf.voxelBlend;
#	endif
			heightSum += depthWeight * weatherWeight * densityContext.weather_height_fraction;
#endif
#ifdef WEATHER_VIEW
			transmittance = backgroundTransmission;
			ambientTransmittance = transmittance;
#else
			transmittance *= stepTransmittance;
#	ifndef CLOUD_REFLECTION
			const float backgroundAfter = CloudBackgroundTransmittance(transmittance, ambientEnabled ? ambientTransmittance : transmittance);
			// Only the interval's lost background transmission exposes foreground air.
			foregroundAir += (backgroundBefore - backgroundAfter) * perspective.luminance;
#	endif
#endif
		}
	}
	if (cirrusPending) {
#ifdef CLOUD_REFLECTION
		IntegrateReflectionCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, cirrusPhase, ambient,
			result, transmittance, ambientTransmittance, depthSum, weightSum, cirrusWeight);
#else
		IntegrateCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, cirrusPhase, ambient,
			result.lum, transmittance, ambientTransmittance, depthSum, weightSum, cirrusWeight);
#endif
	}
#ifdef CLOUD_REFLECTION
	result.rawTransmission = float2(transmittance, ambientEnabled ? ambientTransmittance : transmittance);
	result.integralWeights = float4(depthSum, weightSum, cirrusWeight, heightSum);
#else
	result.transmittance = CloudBackgroundTransmittance(transmittance, ambientEnabled ? ambientTransmittance : transmittance);
	if (weightSum > 0.0) {
		result.cloud_depth = depthSum / weightSum;
		result.stormMotion *= (1.0 - result.transmittance.x) / weightSum;
		result.motionMetadata = float2(saturate(cirrusWeight / weightSum), saturate(heightSum / weightSum));
#	ifdef CLOUD_AP_SHADOW
		// Preserve the legacy shadow estimate for the foreground air integral.
		if (air.slices > CloudAerialSharedVolume && air.mix.x > 0.0)
			apShadow = AtmosphereShadow::TraceShadow(direction, result.cloud_depth, jitter.x);
#	endif
		result.lum += foregroundAir * (air.slices == CloudAerialSharedVolume ? 1.0 : 1.0 - saturate(apShadow));
	}
#endif
	return result;
}

// clang-format off
#include "PhysicalSky/UpperCloud.hlsli"
#include "PhysicalSky/CloudBlur.hlsli"
#include "PhysicalSky/CloudTemporal.hlsli"
#include "PhysicalSky/CloudVoxelView.hlsli"
#include "PhysicalSky/LightningView.hlsli"
// clang-format on

// Match the largest shadow-volume dimension (kShadowVolW/H in PhysicalSky.h).
#define NTHREADS 256
groupshared float g_density[2 * NTHREADS];
groupshared uint g_density_segment[NTHREADS];

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
	float localExtinction = 0.0;
	float accumulated_density = 0.0;
	if (is_valid) {
		const float2 center = CloudShadowVolume::GridCenter(FrameBuffer::CameraPosAdjust.xy, info.shadowVolumeRange, dims.xy);
		const float3 pos = float3(center + (thread_uv.xy - 0.5) * info.shadowVolumeRange, info.shadowVolumeBottom + shadow_thickness * thread_uv.z);

		// Fetch only density represented inside this finite shadow volume. Prefixes
		// start with zero extinction at the boundary instead of assuming a uniform
		// cloud shell outside the represented domain.
		CloudDensityContext densityContext;
		localExtinction = sampleCloudDensity(pos, GetCloudLayer(info), 2.0, 2000.0, 0.0, densityContext);
		float4 middleModel;
		float2 middleDepth;
		localExtinction += MiddleCloudDensity(pos, middleModel, middleDepth, info.shadowVolumeRange / min(dims.x, dims.y));
		density = localExtinction * length(ray_uv_increment * scale);

		accumulated_density = density;
	}
	// At 256 x 256 x 64 the ray wraps at most four times per axis.
	// Pack the exact segment identity into one shared word instead of int4.
	const uint3 wrap = asuint(int3(floor(raw_thread_uv))) & 255u;
	const uint segment = wrap.x | (wrap.y << 8u) | (wrap.z << 16u) | (column << 24u);
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
			if (g_density_segment[gtid - offset] == segment)  // no wraparound happened
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

	// Each lane integrates one light ray; neighbouring lanes sample neighbouring
	// columns. Keep the slanted sampling grid, wrap resets and half-voxel integral,
	// without a group-wide prefix scan or shared-memory barriers at every voxel.
	[numthreads(8, 8, 1)] void renderShadowColumns(uint2 ray : SV_DispatchThreadID)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	uint3 dims;
	RWShadowVolume.GetDimensions(dims.x, dims.y, dims.z);
	const float3 rcpDims = 1.0 / float3(dims);
	const float thickness = max(info.shadowVolumeTop - info.shadowVolumeBottom, 1.0);
	const float3 scale = float3(info.shadowVolumeRange.xx, thickness);
	float3 increment = -info.dirlightDir * (1.0 / scale) * dims;
	const float maximum = max(max(abs(increment.x), abs(increment.y)), abs(increment.z));
	const uint axis = abs(increment.x) == maximum ? 0u : (abs(increment.y) == maximum ? 1u : 2u);
	const uint2 columns = axis == 0u ? dims.yz : (axis == 1u ? dims.xz : dims.xy);
	if (any(ray >= columns))
		return;
	uint3 start;
	if (axis == 0u)
		start = uint3(increment.x > 0.0 ? 0u : dims.x - 1u, ray);
	else if (axis == 1u)
		start = uint3(ray.x, increment.y > 0.0 ? 0u : dims.y - 1u, ray.y);
	else
		start = uint3(ray, increment.z > 0.0 ? 0u : dims.z - 1u);
	increment /= maximum;
	const float3 uvIncrement = increment * rcpDims;
	const float3 startUv = (start + 0.5) * rcpDims;
	const float stepLength = length(uvIncrement * scale);
	const float2 center = CloudShadowVolume::GridCenter(FrameBuffer::CameraPosAdjust.xy, info.shadowVolumeRange, dims.xy);
	const CloudLayer layer = GetCloudLayer(info);
	float accumulated = 0.0;
	uint previousSegment = 0u;
	[loop] for (uint step = 0u; step < dims[axis]; ++step)
	{
		const float3 rawUv = startUv + step * uvIncrement;
		const float3 uv = rawUv - floor(rawUv);
		const uint3 pixel = uv * dims;
		const uint3 wrap = asuint(int3(floor(rawUv))) & 255u;
		const uint segment = wrap.x | (wrap.y << 8u) | (wrap.z << 16u);
		// Crossing a side of the finite volume starts a new light column.
		if (step == 0u || segment != previousSegment)
			accumulated = 0.0;
		previousSegment = segment;
		const float3 position = float3(center + (uv.xy - 0.5) * info.shadowVolumeRange, info.shadowVolumeBottom + thickness * uv.z);
		CloudDensityContext densityContext;
		float extinction = sampleCloudDensity(position, layer, 2.0, 2000.0, 0.0, densityContext);
		float4 middleModel;
		float2 middleDepth;
		extinction += MiddleCloudDensity(position, middleModel, middleDepth, info.shadowVolumeRange / min(dims.x, dims.y));
		const float density = extinction * stepLength;
		accumulated += density;
		RWShadowVolume[pixel] = exp(-max(accumulated - 0.5 * density, 0.0) * info.sunExtinction * GAME_UNIT_TO_M);
	}
}

[numthreads(8, 8, 4)] void resampleShadowVolume(uint3 pixel : SV_DispatchThreadID) {
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

RWTexture2D<float4> RWCloudShadowMean : register(u0);
// Each AP column stores step count, first boundary along the light, step length,
// then optical depth from each boundary toward the light.
RWTexture3D<float> RWProjectedAPShadow : register(u1);
groupshared float ShadowMeanSamples[64];

void IncludeShadowInterval(float2 interval, float range, inout float2 extent)
{
	interval = clamp(interval, 0.0, range);
	if (interval.y > interval.x)
		extent = float2(min(extent.x, interval.x), max(extent.y, interval.y));
}

float3 TraceProjectedCloudShadow(uint2 column, uint2 dimensions, float2 boxCenter, VolumetricCloudData info, bool recordAP)
{
	if (recordAP)
		RWProjectedAPShadow[uint3(column, 0)] = 0.0;
	const float3 direction = info.dirlightDir;
	if (direction.z <= 0.0)
		return float3(1.0, 1.0, 1e30);
	const float3 camera = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	float3 across, up;
	float2 size;
	CloudShadowVolume::LightPlane(direction, info.shadowVolumeRange, across, up, size);
	const float2 uv = (float2(column) + 0.5) / dimensions;
	const float2 offset = CloudShadowVolume::ProjectedGridCenter(camera, across, up, size, dimensions) + (uv - 0.5) * size;
	const float back = 2.0 * info.shadowVolumeRange;
	float3 pos = across * offset.x + up * offset.y + direction * (dot(camera, direction) - back);
	const float groundExit = max(0.0, IntersectWeatherCloudBelow(pos, direction, 0.0, 2.0 * info.planetRadius).y);
	pos += groundExit * direction;
	const float range = back + info.rayMarchRange - groundExit;
	if (range <= 0.0)
		return float3(1.0, 1.0, 1e30);

	const float2 outer = IntersectWeatherCloudBelow(pos, direction, info.lowCloudTopAltitude, info.cloudCurvatureRadius);
	const float2 inner = IntersectWeatherCloudBelow(pos, direction, info.lowCloudBaseAltitude, info.cloudCurvatureRadius);
	// Receivers using this projection lie below the inner shell. Only the
	// outward crossing is in front of them.
	float2 extent = float2(range, 0.0);
	IncludeShadowInterval(float2(max(outer.x, inner.y), outer.y), range, extent);
	IncludeShadowInterval(CloudStormRayInterval(pos, direction), range, extent);
	IncludeShadowInterval(CloudVoxelInterval(pos, direction, 0.0, false), range, extent);
	if (info.middleLayer.z > 0.0) {
		const float2 middleOuter = IntersectCloudBelow(pos, direction, info.middleLayer.y, info.cloudCurvatureRadius);
		const float2 middleInner = IntersectCloudBelow(pos, direction, info.middleLayer.x, info.cloudCurvatureRadius);
		IncludeShadowInterval(float2(max(middleOuter.x, middleInner.y), middleOuter.y), range, extent);
	}
	const float pathLength = extent.y - extent.x;
	if (pathLength <= 0.0)
		return float3(1.0, 1.0, 1e30);

	const float halfRange = 0.5 * info.shadowVolumeRange;
	const float2 box = CloudShadowVolume::RayBoxInterval(pos, direction, float3(boxCenter - halfRange, info.shadowVolumeBottom),
		float3(boxCenter + halfRange, info.shadowVolumeTop));
	const float boxExit = box.y > box.x ? box.y : extent.y;
	const bool hasTail = boxExit < extent.y;
	const CloudLayer layer = GetCloudLayer(info);
	const uint steps = clamp(uint(ceil(pathLength * (GAME_UNIT_TO_M / 40.0))), 1u, 96u);
	const float stepLength = pathLength / float(steps);
	const float footprint = max(stepLength, info.shadowVolumeRange / 128.0);
	const float opticalStep = stepLength * info.sunExtinction * GAME_UNIT_TO_M;
	if (recordAP) {
		RWProjectedAPShadow[uint3(column, 0)] = float(steps);
		RWProjectedAPShadow[uint3(column, 1)] = dot(pos, direction) + extent.x;
		RWProjectedAPShadow[uint3(column, 2)] = stepLength;
	}
	float depth = 0.0;
	float tailDepth = 0.0;
	float firstCloud = 1e30;
#ifdef NO_CLOUD_STORMS
	[loop]
#else
	[fastopt]
#endif
		for (uint step = 0u; step < steps && (recordAP || (hasTail ? min(depth, tailDepth) : depth) < 16.0); ++step)
	{
		const float end = extent.x + (step + 1.0) * stepLength;
		const float tailFraction = saturate((end - boxExit) / stepLength);
		const bool accumulateShared = (hasTail ? min(depth, tailDepth) : depth) < 16.0 && (depth < 16.0 || tailFraction > 0.0);
		if (!recordAP && !accumulateShared)
			continue;
		const float distance = end - 0.5 * stepLength;
		const float3 samplePos = pos + distance * direction;
		CloudDensityContext context;
		float4 middleModel;
		float2 middleDepth;
		const float extinction = sampleCloudDensity(samplePos, layer, 0.0, distance, 0.0, context) +
		                         MiddleCloudDensity(samplePos, middleModel, middleDepth, footprint);
		if (recordAP)
			RWProjectedAPShadow[uint3(column, step + 3u)] = extinction * opticalStep;
		if (accumulateShared) {
			if (extinction > 1e-5)
				firstCloud = min(firstCloud, end - stepLength);
			depth += extinction * opticalStep;
			tailDepth += extinction * opticalStep * tailFraction;
		}
	}
	return float3(exp(-depth), exp(-tailDepth), firstCloud + dot(pos, direction));
}

// One 8x8 spatial quadrature per height. Reuse complete local prefixes; where
// the finite box truncates a ray, integrate the actual curved cloud layer.
// Averaging exp(-tau), rather than tau, preserves clear gaps between clouds.
[numthreads(64, 1, 1)] void buildCloudShadowMean(uint lane : SV_GroupIndex, uint3 group : SV_GroupID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	uint width, height;
	RWCloudShadowMean.GetDimensions(width, height);
	uint3 dims;
	TexShadowVolume.GetDimensions(dims.x, dims.y, dims.z);
	const float2 center = CloudShadowVolume::GridCenter(FrameBuffer::CameraPosAdjust.xy, info.shadowVolumeRange, dims.xy);
	// The other groups project the finite box's missing sun columns. Each
	// lane owns one column, so this work needs no group synchronization.
	if (group.x > 0u) {
		const uint2 column = uint2((group.x - 1u) * 64u + lane, group.y);
		const uint2 dimensions = uint2(width - 1u, height - 1u);
		bool recordAP = false;
#ifdef NO_CLOUD_STORMS
		uint apWidth, apHeight, apDepth;
		RWProjectedAPShadow.GetDimensions(apWidth, apHeight, apDepth);
		recordAP = apDepth >= 100u && all(uint2(apWidth, apHeight) == dimensions);
#endif
		RWCloudShadowMean[column + 1u] = float4(TraceProjectedCloudShadow(column, dimensions, center, info, recordAP), 0.0);
		return;
	}
	const float2 uv = (float2(lane % 8u, lane / 8u) + 0.5) / 8.0;
	const float altitude = lerp(info.shadowVolumeBottom, info.shadowVolumeTop, float(group.y) / (height - 2u));
	const float3 pos = float3(center + (uv - 0.5) * info.shadowVolumeRange, altitude);
	const float3 direction = info.dirlightDir;
	const float2 outer = IntersectWeatherCloudBelow(pos, direction, info.shadowVolumeTop, info.cloudCurvatureRadius);
	const float2 inner = IntersectWeatherCloudBelow(pos, direction, info.shadowVolumeBottom, info.cloudCurvatureRadius);
	const float start = max(outer.x, 0.0);
	const float nearEnd = min(outer.y, max(inner.x, 0.0));
	const float farStart = max(start, inner.y);
	const float nearLength = max(nearEnd - start, 0.0);
	const float farLength = max(outer.y - farStart, 0.0);
	const float pathLength = nearLength + farLength;
	float transmittance = 1.0;
	if (pathLength > 0.0) {
		const float3 exitPosition = pos + direction * (farLength > 0.0 ? outer.y : nearEnd);
		const float2 exitUv = (exitPosition.xy - center) / info.shadowVolumeRange + 0.5;
		const float z = (pos.z - info.shadowVolumeBottom) / max(info.shadowVolumeTop - info.shadowVolumeBottom, 1.0);
		// Keep the linear-filter footprint off incomplete side prefixes too.
		const float2 margin = 2.0 / float2(dims.xy);
		if (z >= 0.0 && z <= 1.0 && exitPosition.z >= info.shadowVolumeBottom && exitPosition.z <= info.shadowVolumeTop + 1.0 && all(exitUv >= margin) && all(exitUv <= 1.0 - margin)) {
			transmittance = TexShadowVolume.SampleLevel(TransmittanceSampler, float3(uv, z), 0);
		} else {
			const float stepLength = pathLength / 32.0;
			float opticalDepth = 0.0;
			// FXC's loop analysis is expensive for storm/voxel density bodies.
			// fastopt preserves their DXBC while avoiding speculative loop work.
#ifdef NO_CLOUD_STORMS
			[loop]
#else
			[fastopt]
#endif
				for (uint step = 0u; step < 32u; ++step)
			{
				const float distanceInCloud = (step + 0.5) * stepLength;
				const float distanceAlongRay = distanceInCloud < nearLength ? start + distanceInCloud : farStart + distanceInCloud - nearLength;
				CloudDensityContext densityContext;
				float density = sampleCloudDensity(pos + direction * distanceAlongRay, GetCloudLayer(info), 2.0, 2000.0, 0.0, densityContext);
				float4 middleModel;
				float2 middleDepth;
				density += MiddleCloudDensity(pos + direction * distanceAlongRay, middleModel, middleDepth,
					max(stepLength * length(direction.xy), info.shadowVolumeRange / min(dims.x, dims.y)));
				opticalDepth += density * stepLength * info.sunExtinction * GAME_UNIT_TO_M;
			}
			transmittance = exp(-max(opticalDepth, 0.0));
		}
	}
	ShadowMeanSamples[lane] = transmittance;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (lane < stride)
			ShadowMeanSamples[lane] += ShadowMeanSamples[lane + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane == 0u) {
		RWCloudShadowMean[uint2(0, group.y + 1u)] = float4(saturate(ShadowMeanSamples[0] / 64.0), 0, 0, 0);
		if (group.y == 0u) {
			float3 across, up;
			float2 size;
			CloudShadowVolume::LightPlane(direction, info.shadowVolumeRange, across, up, size);
			const float3 camera = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
			const float2 projection = CloudShadowVolume::ProjectedGridCenter(camera, across, up, size, uint2(width - 1u, height - 1u));
			RWCloudShadowMean[uint2(0, 0)] = float4(info.shadowVolumeBottom, info.shadowVolumeTop, info.cloudCurvatureRadius, 2);
			RWCloudShadowMean[uint2(1, 0)] = float4(direction, info.lowestCloudAltitude);
			RWCloudShadowMean[uint2(2, 0)] = float4(center, info.shadowVolumeRange, 0);
			RWCloudShadowMean[uint2(3, 0)] = float4(projection, 0, 0);
		}
	}
}

	[numthreads(8, 8, 1)] void integrateProjectedAPShadow(uint2 column : SV_DispatchThreadID)
{
	const uint steps = uint(RWProjectedAPShadow[uint3(column, 0)]);
	RWProjectedAPShadow[uint3(column, steps + 3u)] = 0.0;
	float opticalDepth = 0.0;
	[loop] for (uint step = steps; step > 0u; --step)
	{
		const uint3 cell = uint3(column, step + 2u);
		opticalDepth += RWProjectedAPShadow[cell];
		RWProjectedAPShadow[cell] = opticalDepth;
	}
}
