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
#	define CLOUD_SELFSHADOW_REGISTER t38
#endif
Texture3D<float> TexShadowVolume : register(t14);
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "PhysicalSky/CloudGeometry.hlsli"
#include "PhysicalSky/CloudMotion.hlsli"
#include "PhysicalSky/CloudNoise.hlsli"
#include "PhysicalSky/Common.hlsli"
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

	float lowCloudTraceBottomAltitude;
	float lowCloudTraceTopAltitude;
	float cloudCurvatureRadius;
	float cloudEvolutionDelta;

	float shadowVolumeBottom;
	float shadowVolumeTop;
	uint historyValid;
	uint cirrusEnabled;

	float2 lowNdfFrequency;
	float2 noiseWindOffset;
	float2 cloudWindDelta;
	float2 cloudShapeShear;
	float2 previousShapeShear;
	float2 cirrusWindOffset;

	float2 cirrusWindDelta;
	float cirrusAltitude;
	float cirrusUvInvSize;

	float cirrusDensityScale;
	float cirrusLightThickness;
	float phaseForwardG;
	float phaseBackwardG;

	float lowDensityScale;
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

	float4 cloudEvolution;
	row_major float4x4 previousViewProj;

	float4 ndfBoundaryRect;
	float4 localNdfRect;
	float4 localNdfAltitude;
	float4 localNdfAuxiliary;
	float4 lowCloudPhase;
};

CloudLayer GetCloudLayer(VolumetricCloudData info)
{
	CloudLayer cloud;
	cloud.lowestAltitude = info.lowCloudTraceBottomAltitude;
	cloud.highestAltitude = info.shadowVolumeTop;
	return cloud;
}

cbuffer VolumetricCloudConstants : register(b1)
{
	VolumetricCloudData VolumetricCloudBuffer[1];
	float4 CloudMsCalibration[49];
};
Texture2D<float4> TexTransmittance : register(t1);
Texture3D<float4> TexAerialPerspective : register(t2);

Texture2D<float> TexDepth : register(t3);

Texture3D<unorm float4> TexCloudShapeNoise : register(t4);
Texture2D<float4> TexCloudHeight : register(t5);
Texture2D<float3> TexCloudModeling : register(t6);
Texture2D<float4> TexCloudAtmosphereMs : register(t8);
Texture2D<float2> TexCirrusWeather : register(t9);
Texture2D<float3> TexCirrusPatterns : register(t10);
Texture2D<unorm float2> TexCloudProfileLUT : register(t11);
Texture2D<unorm float3> TexCloudAdjustmentLUT : register(t12);
Texture2D<sh2> TexCloudAmbientSH : register(t13);
TextureCube<float4> TexCloudGroundReflection : register(t39);

Texture2D<float4> TexCloudBoundary : register(t15);
Texture2D<float2> TexCloudHeightBounds : register(t16);
Texture2D<float> TexVolHistoryTr : register(t17);
Texture2D<float4> TexVolHistoryLum : register(t18);
Texture2D<float4> TexVolHistoryAux : register(t19);
Texture2D<float> TexVolLowTr : register(t20);
Texture2D<float4> TexVolLowLum : register(t21);
Texture2D<float4> TexVolLowAux : register(t22);
TextureCube<float> TexCubeHistoryTr : register(t23);
TextureCube<float4> TexCubeHistoryLum : register(t24);
TextureCube<float4> TexCubeHistoryAux : register(t25);
TextureCube<float> TexCubeTraceTr : register(t26);
TextureCube<float4> TexCubeTraceLum : register(t27);
TextureCube<float4> TexCubeTraceAux : register(t28);
Texture2DArray<float4> TexLocalNdfHeight : register(t32);
Texture2DArray<float4> TexLocalNdfModeling : register(t33);
Texture2DArray<float4> TexLocalNdfMaximum : register(t34);
StructuredBuffer<float4> LocalNdfWeights : register(t35);

float2 LocalNdfUV(float2 worldXY, VolumetricCloudData info)
{
	return (worldXY * GAME_UNIT_TO_M - info.localNdfRect.xy) * info.localNdfRect.zw;
}

bool InLocalNdf(float2 uv, VolumetricCloudData info)
{
	return info.localNdfAltitude.z > 0.0 && all(uv >= 0.0) && all(uv <= 1.0);
}

struct LocalNdfModelSample
{
	float2 uv;
	uint count;
};

LocalNdfModelSample SampleLocalNdfModel(float2 worldXY, VolumetricCloudData info)
{
	LocalNdfModelSample value;
	value.uv = LocalNdfUV(worldXY, info);
	value.count = InLocalNdf(value.uv, info) ? (uint)info.localNdfAltitude.w : 0u;
	return value;
}

float3 ApplyLocalNdfModel(float3 model, LocalNdfModelSample local)
{
	float3 result = model;
	[loop] for (uint i = 0u; i < local.count; ++i)
	{
		const float4 endpoint = LocalNdfWeights[i];
		float3 shaped;
		[branch] if (endpoint.y > 0.0)
			shaped = max(model, TexLocalNdfMaximum.SampleLevel(TransmittanceSampler, float3(local.uv, i), 0).rgb);
		else
		{
			const float4 layer = TexLocalNdfModeling.SampleLevel(TransmittanceSampler, float3(local.uv, i), 0);
			shaped = model * (1.0 - layer.a) + layer.rgb;
		}
		result += endpoint.x * (shaped - model);
	}
	return saturate(result);
}

bool HasLocalNdfInfluence(LocalNdfModelSample local)
{
	[loop] for (uint i = 0u; i < local.count; ++i)
	{
		[branch] if (LocalNdfWeights[i].y > 0.0)
		{
			if (any(TexLocalNdfMaximum.SampleLevel(TransmittanceSampler, float3(local.uv, i), 0).rgb > 0.0))
				return true;
		}
		else if (TexLocalNdfModeling.SampleLevel(TransmittanceSampler, float3(local.uv, i), 0).a > 0.0) return true;
	}
	return false;
}

float3 LocalNdfHeight(float3 height, float2 worldXY, VolumetricCloudData info)
{
	const float2 uv = LocalNdfUV(worldXY, info);
	[branch] if (info.localNdfAltitude.y > 0.0 && InLocalNdf(uv, info))
	{
		float4 local = 0.0;
		[loop] for (uint i = 0u; i < (uint)info.localNdfAltitude.w; ++i)
			local += LocalNdfWeights[i].x * TexLocalNdfHeight.SampleLevel(TransmittanceSampler, float3(uv, i), 0);
		height.xy = height.xy * (1.0 - local.b) + (local.rg * info.localNdfAltitude.y + info.localNdfAltitude.x * local.b) * GAME_UNITS_PER_METER;
	}
	return height;
}

float LocalNdfMotionWeight(float2 worldXY, float height, VolumetricCloudData info)
{
	const float2 shear = info.cloudShapeShear * smoothstep(0.0, 1.0, height);
	const LocalNdfModelSample local = SampleLocalNdfModel(worldXY - shear, info);
	if (local.count == 0u)
		return 0.0;
	float coverage = -1.0;
	float weight = 0.0;
	[loop] for (uint i = 0u; i < local.count; ++i)
	{
		const float4 endpoint = LocalNdfWeights[i];
		[branch] if (endpoint.y > 0.0)
		{
			if (coverage < 0.0) {
				const float2 field = CloudFieldPosition((worldXY - info.noiseWindOffset) * GAME_UNIT_TO_M, info.cloudEvolution.xy) * GAME_UNITS_PER_METER;
				coverage = saturate(TexCloudModeling.SampleLevel(TileableSampler, (field - shear) * info.lowNdfFrequency + 0.5, 0).r);
			}
			const float floor = TexLocalNdfMaximum.SampleLevel(TransmittanceSampler, float3(local.uv, i), 0).r;
			weight += endpoint.x * (floor > coverage ? 1.0 : 0.0);
		}
		else weight += endpoint.x * TexLocalNdfModeling.SampleLevel(TransmittanceSampler, float3(local.uv, i), 0).a;
	}
	return saturate(weight);
}

bool LocalNdfIntersectsRay(float2 origin, float2 direction, VolumetricCloudData info)
{
	if (info.localNdfAltitude.z <= 0.0)
		return false;
	const float2 uv = LocalNdfUV(origin, info);
	const float2 velocity = direction * GAME_UNIT_TO_M * info.localNdfRect.zw;
	const float2 shear = info.cloudShapeShear * 61.0 * GAME_UNIT_TO_M * info.localNdfRect.zw;
	const float2 lower = min(shear, 0.0);
	const float2 upper = 1.0 + max(shear, 0.0);
	float enter = 0.0;
	float rayExit = 3.402823466e+38;
	[unroll] for (uint axis = 0u; axis < 2u; ++axis)
	{
		if (abs(velocity[axis]) < 1e-20) {
			if (uv[axis] < lower[axis] || uv[axis] > upper[axis])
				return false;
		} else {
			const float2 interval = (float2(lower[axis], upper[axis]) - uv[axis]) / velocity[axis];
			enter = max(enter, min(interval.x, interval.y));
			rayExit = min(rayExit, max(interval.x, interval.y));
		}
	}
	return rayExit >= enter;
}

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

float4 SampleCloudAerialPerspective(float3 viewDir, float distance, float shadow)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;
	uint3 apDims;
	TexAerialPerspective.GetDimensions(apDims.x, apDims.y, apDims.z);
	if (any(apDims == 0u))
		return float4(0.0, 0.0, 0.0, 1.0);
	const float depthSlice = ApDepthUv(distance, apDims.z);
	const float3 apUv = float3(SkyViewLutUv(viewDir, apDims.y), depthSlice);
	float4 ap = TexAerialPerspective.SampleLevel(SkyViewSampler, apUv, 0);
	ap.rgb *= 1.0 - saturate(shadow);
	ap.rgb *= data.apLumMix;
	ap.a = lerp(1.0, ap.a, data.apTrMix);
	return ap;
}

groupshared sh2 gCloudAmbientSHR[256];
groupshared sh2 gCloudAmbientSHG[256];
groupshared sh2 gCloudAmbientSHB[256];
groupshared float3 gCloudGroundRadiance[256];
groupshared float3 gCloudGroundIrradiance[256];

float3 CloudAmbientAtmosphereSource(float3 position, float3 direction, float3 lightDirection, float3 color, float3 rayleigh, float3 mie)
{
	if (!any(color > 0.0) || dot(lightDirection, lightDirection) < 0.5)
		return 0.0;
	const float cosine = dot(direction, lightDirection);
	const float3 direct = SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, position, lightDirection) *
	                      (rayleigh * Phase::Rayleigh(cosine) + mie * Phase::CornetteShanks(cosine, SharedData::physSkyData.aerosolPhaseG));
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
		float3 source = CloudAmbientAtmosphereSource(position, direction, sky.sunDir, sky.sunlightColor, rayleigh, mie);
		source += CloudAmbientAtmosphereSource(position, direction, sky.masserDir, sky.masserColor, rayleigh, mie);
		source += CloudAmbientAtmosphereSource(position, direction, sky.secundaDir, sky.secundaColor, rayleigh, mie);
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

float3 CloudAmbientGround(float3 direction, float3 groundRadiance, float altitude)
{
	const SharedData::PhysSkyData sky = SharedData::physSkyData;
	const float radius = sky.rPlanet + max(altitude, GAME_UNITS_PER_METER);
	// The reflection already includes transport below the probe.
	const float probeRadius = clamp(sky.zCameraPlanet, sky.rPlanet, radius);
	float3 transmission;
	float boundaryDistance;
	float3 radiance = CloudAmbientAtmosphere(radius, probeRadius, direction, 16u, transmission, boundaryDistance);
	if (boundaryDistance >= 0.0)
		radiance += transmission * groundRadiance;
	return radiance;
}

[numthreads(16, 16, 1)] void buildCloudAmbientSH(uint3 tid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	uint reflectionWidth, reflectionHeight;
	TexCloudGroundReflection.GetDimensions(reflectionWidth, reflectionHeight);
	const float2 faceSize = max(float2(reflectionWidth, reflectionHeight), 1.0);
	const float2 faceTexel = floor((float2(tid.xy) + 0.5) * faceSize / 16.0);
	const float2 faceUv = 2.0 * (faceTexel + 0.5) / faceSize - 1.0;
	const float3 reflectionDirection = float3(-faceUv.x, -faceUv.y, -1.0);
	const float inverseLength = rsqrt(1.0 + dot(faceUv, faceUv));
	const float solidAngleWeight = inverseLength * inverseLength * inverseLength;
	float3 groundRadiance;
	if (reflectionWidth > 0u && reflectionHeight > 0u)
		groundRadiance = ColorManagement::SceneToLinear(max(TexCloudGroundReflection.SampleLevel(TileableSampler, reflectionDirection, 0).rgb, 0.0));
	else
		groundRadiance = ColorManagement::SceneToLinear(Color::Ambient(max(SharedData::GetAmbient(float3(0, 0, -1)), 0.0)));
	gCloudGroundRadiance[groupIndex] = groundRadiance * solidAngleWeight;
	gCloudGroundIrradiance[groupIndex] = solidAngleWeight;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128u; stride > 0u; stride >>= 1u)
	{
		if (groupIndex < stride) {
			gCloudGroundRadiance[groupIndex] += gCloudGroundRadiance[groupIndex + stride];
			gCloudGroundIrradiance[groupIndex] += gCloudGroundIrradiance[groupIndex + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	groundRadiance = gCloudGroundRadiance[0] / max(gCloudGroundIrradiance[0].x, 1e-6);
	if (groupIndex == 0u)
		RWCloudAmbientSH[int2(5, 0)] = float4(groundRadiance, 0.0);
	GroupMemoryBarrierWithGroupSync();

	const float2 sampleCoord = (float2(tid.xy) + 0.5) / 16.0;
	const float elevationCoordinate = 1.0 - sampleCoord.y;
	const float3 shSampleDir = SphericalHarmonics::GetUniformSphereSample(sampleCoord.x, 0.5 * (1.0 - elevationCoordinate * elevationCoordinate));
	const float3 rayDir = shSampleDir.xzy;
	const float3 skyColor = CloudAmbientSky(rayDir, VolumetricCloudBuffer[0].highestCloudAltitude);
	const float shFactor = 4.0 * Math::PI * elevationCoordinate / 256.0;
	const sh2 sh = SphericalHarmonics::Evaluate(shSampleDir);
	gCloudAmbientSHR[groupIndex] = SphericalHarmonics::Scale(sh, skyColor.r * shFactor);
	gCloudAmbientSHG[groupIndex] = SphericalHarmonics::Scale(sh, skyColor.g * shFactor);
	gCloudAmbientSHB[groupIndex] = SphericalHarmonics::Scale(sh, skyColor.b * shFactor);
	const float3 groundDirection = rayDir * float3(1.0, 1.0, -1.0);
	const float3 ground = CloudAmbientGround(groundDirection, groundRadiance, VolumetricCloudBuffer[0].lowestCloudAltitude);
	gCloudGroundRadiance[groupIndex] = ground * (2.0 * elevationCoordinate / 256.0);
	gCloudGroundIrradiance[groupIndex] = gCloudGroundRadiance[groupIndex] * (-2.0 * groundDirection.z);
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint shStride = 128u; shStride > 0u; shStride >>= 1u)
	{
		if (groupIndex < shStride) {
			gCloudAmbientSHR[groupIndex] += gCloudAmbientSHR[groupIndex + shStride];
			gCloudAmbientSHG[groupIndex] += gCloudAmbientSHG[groupIndex + shStride];
			gCloudAmbientSHB[groupIndex] += gCloudAmbientSHB[groupIndex + shStride];
			gCloudGroundRadiance[groupIndex] += gCloudGroundRadiance[groupIndex + shStride];
			gCloudGroundIrradiance[groupIndex] += gCloudGroundIrradiance[groupIndex + shStride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (groupIndex == 0u) {
		RWCloudAmbientSH[int2(0, 0)] = gCloudAmbientSHR[0];
		RWCloudAmbientSH[int2(1, 0)] = gCloudAmbientSHG[0];
		RWCloudAmbientSH[int2(2, 0)] = gCloudAmbientSHB[0];
		RWCloudAmbientSH[int2(3, 0)] = float4(gCloudGroundRadiance[0], 0.0);
		RWCloudAmbientSH[int2(4, 0)] = float4(gCloudGroundIrradiance[0], 0.0);
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
	const float2 outer = IntersectCloudBelow(origin, dir, topAltitude, info.cloudCurvatureRadius);
	const float entry = max(outer.x, 50.0 * GAME_UNITS_PER_METER);
	const float exit = min(outer.y, info.rayMarchRange);
	if (exit <= entry)
		return segments;

	const float2 inner = IntersectCloudBelow(origin, dir, bottomAltitude, info.cloudCurvatureRadius);
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
	return worldXY * info.lowNdfFrequency + 0.5;
}

float CloudViewStep(float distance, float angularFootprint)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float viewStep = (3.0 + distance * GAME_UNIT_TO_M * (60.0 / 16384.0)) * info.marchStepScale * GAME_UNITS_PER_METER;
	if (angularFootprint <= 0.0)
		return viewStep;
	// Resolve the final reflection texel, independently of the checkerboard spacing.
	return max(viewStep, max(3.0 * GAME_UNITS_PER_METER, distance * angularFootprint * 0.25));
}

struct NDFInfo
{
	bool in_layer;
	float dimension_profile;
	float coverage;
	float height_fraction;
	float shaping_start;
	float2 altitude;
	float top_type;
	float bottom_type;
};

void initNDFInfo(out NDFInfo ndf)
{
	ndf.in_layer = false;
	ndf.dimension_profile = 0.0;
	ndf.coverage = 0.0;
	ndf.height_fraction = 0.0;
	ndf.shaping_start = 0.0;
	ndf.altitude = 0.0;
	ndf.top_type = 0.0;
	ndf.bottom_type = 0.0;
}

float3 SampleCloudHeights(float2 worldXY, float2 anchoredXY)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float3 heights = saturate(TexCloudHeight.SampleLevel(TileableSampler, LowNdfUV(worldXY, info), 0).rgb);
	heights.xy = lerp(info.lowCloudBaseAltitude, info.lowCloudTopAltitude, heights.xy);
	return LocalNdfHeight(heights, anchoredXY, info);
}

float CloudNdfProfile(inout NDFInfo ndf)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float top = TexCloudProfileLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, ndf.height_fraction), 0).g;
	const float expandedTop = TexCloudAdjustmentLUT.SampleLevel(TransmittanceSampler, float2(ndf.top_type, ndf.height_fraction), 0).r;
	const float bottom = TexCloudProfileLUT.SampleLevel(TransmittanceSampler, float2(ndf.bottom_type, ndf.height_fraction), 0).r;
	const float verticalProfile = max(lerp(top, max(top, expandedTop), info.topExpansion) * bottom, 0.0);
	const float heightFade = saturate(ndf.height_fraction / info.coverageHeightRange);
	const float exponent = lerp(lerp(info.coverageBottomPower, 1.0, heightFade), 1.0, heightFade);
	const float coverageProfile = pow(ndf.coverage, exponent);
	return verticalProfile * coverageProfile;
}

NDFInfo sampleNDF(float2 worldXY, float2 anchoredXY, float profileAltitude)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	NDFInfo ndf;
	initNDFInfo(ndf);
	const float3 heights = SampleCloudHeights(worldXY, anchoredXY);
	const float minAltitude = heights.r;
	const float maxAltitude = heights.g;
	if (maxAltitude <= minAltitude || profileAltitude <= minAltitude || profileAltitude >= maxAltitude)
		return ndf;
	ndf.shaping_start = heights.b;
	ndf.altitude = heights.rg;
	const float4 range = info.localNdfAuxiliary;
	[branch] if (range.z != range.w && InLocalNdf(LocalNdfUV(anchoredXY, info), info))
	{
		const LocalNdfModelSample local = SampleLocalNdfModel(anchoredXY, info);
		if (HasLocalNdfInfluence(local)) {
			const float3 baseModel = saturate(TexCloudModeling.SampleLevel(TileableSampler, LowNdfUV(worldXY, info), 0));
			const float topType = ApplyLocalNdfModel(baseModel, local).g;
			ndf.shaping_start = saturate(lerp(range.z, range.w, saturate((topType - range.x) / (range.y - range.x))));
		}
	}
	ndf.height_fraction = saturate((profileAltitude - minAltitude) / (maxAltitude - minAltitude));
	const float2 modelingXY = worldXY - info.cloudShapeShear * smoothstep(0.0, 1.0, ndf.height_fraction);
	float3 model = saturate(TexCloudModeling.SampleLevel(TileableSampler, LowNdfUV(modelingXY, info), 0));
	float3 upstream = model;
	if (any(info.cloudShapeShear != 0.0))
		upstream = saturate(TexCloudModeling.SampleLevel(TileableSampler, LowNdfUV(modelingXY - info.cloudShapeShear * 60.0, info), 0));
	const float2 anchoredModelXY = anchoredXY - info.cloudShapeShear * smoothstep(0.0, 1.0, ndf.height_fraction);
	model = ApplyLocalNdfModel(model, SampleLocalNdfModel(anchoredModelXY, info));
	upstream = any(info.cloudShapeShear != 0.0) ? ApplyLocalNdfModel(upstream, SampleLocalNdfModel(anchoredModelXY - info.cloudShapeShear * 60.0, info)) : model;
	const float shearBlend = smoothstep(0.0, 1.0, saturate((ndf.height_fraction - 0.1) * 1.5384616));
	ndf.coverage = lerp(model.r, max(model.r, upstream.r), shearBlend);
	if (ndf.coverage < 1e-8)
		return ndf;
	ndf.in_layer = true;
	ndf.top_type = lerp(model.g, upstream.g, shearBlend) * saturate(ndf.coverage * 10.0);
	ndf.bottom_type = lerp(model.b, upstream.b, shearBlend);
	ndf.dimension_profile = CloudNdfProfile(ndf);
	return ndf;
}

struct CloudDensityContext
{
	NDFInfo ndf;
	float3 noise_coordinates;
	float eye_distance;
};

float sampleCloudDensityFromContext(
	inout CloudDensityContext density_context, float mipBias, float minimumMip)
{
	const NDFInfo ndf = density_context.ndf;
	if (!ndf.in_layer || ndf.dimension_profile <= 1e-10)
		return 0.0;

	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float profileMip = floor(ndf.dimension_profile * 3.0);
	// Density is nonlinear in noise; coarse averages must not erase the cloud shape.
	const float biasedMip = profileMip + mipBias;
	const float mip = max(biasedMip, min(minimumMip, biasedMip + 1.0));
	const float4 noise = TexCloudShapeNoise.SampleLevel(TileableSampler, density_context.noise_coordinates, mip);
	const float eroded = ReconstructCloudNoiseDensity(noise, ndf.dimension_profile, ndf.top_type, ndf.height_fraction, density_context.eye_distance);
	return ShapeCloudBaseDensity(eroded, ndf.height_fraction, info.bottomDensityWidth, info.bottomDensityPower, ndf.shaping_start) * info.lowDensityScale;
}

float sampleCloudDensity(
	float3 pos, CloudLayer cloud, float mipBias, float viewDistance, float minimumMip,
	out CloudDensityContext density_context)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	initNDFInfo(density_context.ndf);
	density_context.noise_coordinates = 0.0;
	density_context.eye_distance = 0.0;

	const float profileAltitude = CloudProfileAltitude(pos, FrameBuffer::CameraPosAdjust.xy, info.cloudCurvatureRadius);
	if (profileAltitude < cloud.lowestAltitude || profileAltitude > cloud.highestAltitude)
		return 0.0;

	const float2 fieldMeters = CloudFieldPosition((pos.xy - info.noiseWindOffset) * GAME_UNIT_TO_M, info.cloudEvolution.xy);
	density_context.ndf = sampleNDF(fieldMeters * GAME_UNITS_PER_METER, pos.xy, profileAltitude);
	if (!density_context.ndf.in_layer || density_context.ndf.dimension_profile <= 0.0)
		return 0;

	const float bottomFade = saturate((density_context.ndf.height_fraction - 0.02) * 33.333332);
	const float2 xy = fieldMeters;
	float2 noiseXY = xy * 0.0043545123;
	[branch] if (bottomFade < 1.0)
	{
		const float2 displacement = TexCloudAdjustmentLUT.SampleLevel(TileableSampler, xy * 0.004, 0).gb;
		noiseXY += (displacement * 2.0 - 1.0) * (0.125 - bottomFade * 0.125);
	}
	density_context.noise_coordinates = float3(noiseXY,
		(pos.z * GAME_UNIT_TO_M - 20.0 + bottomFade * 10.0) * 0.0034834063 - info.cloudEvolution.x * (1.0 / 6.283185307179586));
	density_context.eye_distance = viewDistance;
	return sampleCloudDensityFromContext(density_context, mipBias, minimumMip);
}

float3 sampleExternalSunTransmittance(float3 pos, float3 sun_dir)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 pos_planet = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);

	// Finite scene-shadow coverage would project a camera-following boundary into the cloud layer.
	return SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, pos_planet, sun_dir);
}

#include "PhysicalSky/CloudAmbient.hlsli"
#include "PhysicalSky/CloudLighting.hlsli"

struct VolumetricCloudResult
{
	float3 transmittance;
	float3 lum;
	float cloud_depth;
	float2 motionMetadata;
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

void IntegrateCirrus(float3 position, float3 direction, float distance, float phase, CloudAmbient cloudAmbient,
	inout float3 radiance, inout float transmittance, inout float ambientTransmittance,
	inout float depthSum, inout float weightSum, inout float cirrusWeight)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const bool ambientEnabled = info.ambientStrength > 0.0 && info.msAlbedo > 0.0;
	if (max(transmittance, ambientEnabled ? ambientTransmittance : 0.0) <= 0.1)
		return;
	float profile;
	const float density = CirrusDensity(position.xy, profile);
	if (density <= 1e-10)
		return;
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
	const float viewDepth = density * 10.0;
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
	const float stepTransmittance = exp(-viewDepth);
	// The collapsed forward peak transports sheet radiance as well as sky radiance.
	const float forwardTransmittance = ambientEnabled ? max(ambientTransmittance - transmittance, 0.0) * info.ambientStrength * info.cloudLightingScale : 0.0;
	const float weight = (transmittance + forwardTransmittance) * (1.0 - stepTransmittance);
	radiance += weight * info.cloudLightingScale * (sun * sunlight + ambientRadiance);
	const float depthWeight = CloudDepthWeight(transmittance, density, distance);
	depthSum += depthWeight * distance;
	weightSum += depthWeight;
	cirrusWeight += depthWeight;
	transmittance *= stepTransmittance;
	ambientTransmittance *= stepTransmittance;
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

VolumetricCloudResult RenderVolumetricCloudRay(float3 direction, float3 eye, float sceneDistance, float2 jitter, float apShadow, float4 boundary, float angularFootprint)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	VolumetricCloudResult result;
	result.transmittance = 1.0;
	result.lum = 0.0;
	result.motionMetadata = 0.0;
	bool sceneLimited = sceneDistance > 0.0;
	const float3 planetEye = eye + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float2 ground = IntersectSpherePair(planetEye, direction, info.planetRadius);
	if (dot(planetEye, direction) < 0.0 && ground.y > 0.0) {
		sceneDistance = sceneLimited ? min(sceneDistance, max(ground.x, 0.0)) : max(ground.x, 0.0);
		sceneLimited = true;
	}
	const bool localRay = LocalNdfIntersectsRay(eye.xy, direction.xy, info);
	const float traceBottom = localRay ? info.lowCloudTraceBottomAltitude : info.lowCloudBaseAltitude;
	const float traceTop = localRay ? info.lowCloudTraceTopAltitude : info.lowCloudTopAltitude;
	const float3 cloudEye = float3(eye.xy - FrameBuffer::CameraPosAdjust.xy, eye.z);
	const CloudRaySegments bounds = GetCloudRaySegments(cloudEye, direction, traceBottom, traceTop, info);
	CloudRaySegments low = bounds;
	if (angularFootprint > 0.0) {
		const float2 heights = TexCloudHeightBounds[uint2(0, 0)];
		float2 altitude = lerp(info.lowCloudBaseAltitude, info.lowCloudTopAltitude, heights);
		if (localRay && info.localNdfAltitude.y > 0.0) {
			altitude.x = min(altitude.x, info.localNdfAltitude.x * GAME_UNITS_PER_METER);
			altitude.y = max(altitude.y, (info.localNdfAltitude.x + info.localNdfAltitude.y) * GAME_UNITS_PER_METER);
		}
		// Enclose bilinear NDF samples.
		const float padding = GAME_UNITS_PER_METER;
		low = GetCloudRaySegments(cloudEye, direction, max(traceBottom, altitude.x - padding),
			min(traceTop, altitude.y + padding), info);
		const float end = bounds.farSegment.y > bounds.farSegment.x ? bounds.farSegment.y : bounds.nearSegment.y;
		low.nearSegment.y = max(low.nearSegment.x, min(low.nearSegment.y, end));
		low.farSegment.y = max(low.farSegment.x, min(low.farSegment.y, end));
	}
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
		const float3 cirrusEye = cloudEye + float3(0.0, 0.0, info.cloudCurvatureRadius);
		const float2 shell = IntersectSpherePair(cirrusEye, direction, info.cloudCurvatureRadius + info.cirrusAltitude);
		cirrusDistance = shell.x >= 0.0 ? shell.x : shell.y;
		if (cirrusDistance >= 0.0)
			result.cloud_depth = bounds.length > 0.0 || hasBoundaryDepth ? min(result.cloud_depth, cirrusDistance) : cirrusDistance;
		cirrusPending = cirrusDistance >= 0.0 && (!sceneLimited || cirrusDistance < sceneDistance);
	}
	if (low.length <= 0.0 && !cirrusPending)
		return result;

	// Split the volume at the sheet so camera altitude and NDF bounds cannot reverse compositing order.
	float4 boundaries = float4(low.nearSegment, low.farSegment);
	boundaries = float4(min(boundaries.x, boundaries.y), max(boundaries.x, boundaries.y),
		min(boundaries.z, boundaries.w), max(boundaries.z, boundaries.w));
	boundaries = float4(min(boundaries.x, boundaries.z), min(boundaries.y, boundaries.w),
		max(boundaries.x, boundaries.z), max(boundaries.y, boundaries.w));
	boundaries.yz = float2(min(boundaries.y, boundaries.z), max(boundaries.y, boundaries.z));
	const float sheetBoundary = cirrusPending ? cirrusDistance : 0.0;
	const float lastBoundary = max(boundaries.w, sheetBoundary);
	boundaries = min(boundaries, float4(sheetBoundary, max(boundaries.xyz, sheetBoundary)));
	const float lightCosine = dot(direction, info.dirlightDir);
	const float phase = CloudLowPhase(lightCosine);
	const float cirrusPhase = cirrusPending ? CloudCirrusPhase(lightCosine) : 0.0;
	const CloudAmbient ambient = MakeCloudAmbient();
	const bool ambientEnabled = info.ambientStrength > 0.0 && info.msAlbedo > 0.0;
	float ambientTransmittance = 1.0;
	float ambientBoundaryAltitude = info.highestCloudAltitude;
	const CloudLayer layer = GetCloudLayer(info);
	float noiseTexelsPerMeter = 0.0;
	float maximumNoiseMip = 0.0;
	if (angularFootprint > 0.0) {
		uint width, height, depth, levels;
		TexCloudShapeNoise.GetDimensions(0, width, height, depth, levels);
		noiseTexelsPerMeter = max(max(width, height) * 0.0043545123, depth * 0.0034834063);
		maximumNoiseMip = float(levels - 1u);
	}
	float depthSum = 0.0;
	float weightSum = 0.0;
	float cirrusWeight = 0.0;
	float heightSum = 0.0;
	float transmittance = 1.0;
	[loop] for (uint interval = 0u; interval < 4u && max(transmittance, ambientEnabled ? ambientTransmittance : 0.0) > 0.1; ++interval)
	{
		const float begin = interval == 0u ? boundaries.x : (interval == 1u ? boundaries.y : (interval == 2u ? boundaries.z : boundaries.w));
		const float end = interval == 0u ? boundaries.y : (interval == 1u ? boundaries.z : (interval == 2u ? boundaries.w : lastBoundary));
		if (cirrusPending && cirrusDistance <= begin) {
			IntegrateCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, cirrusPhase, ambient,
				result.lum, transmittance, ambientTransmittance, depthSum, weightSum, cirrusWeight);
			cirrusPending = false;
		}
		if (end <= begin || !CloudIntervalContains(low, 0.5 * (begin + end)))
			continue;
		float distance = begin;
		[loop] while (distance < end && max(transmittance, ambientEnabled ? ambientTransmittance : 0.0) > 0.1)
		{
			const float step = min(CloudViewStep(distance, angularFootprint), end - distance);
			const float sampleDistance = distance + (distance * GAME_UNIT_TO_M < 250.0 ? jitter.x : jitter.y) * step;
			const float3 pos = eye + direction * sampleDistance;
			CloudDensityContext densityContext = (CloudDensityContext)0;
			const float viewDistance = distance * GAME_UNIT_TO_M;
			const float minimumMip = min(log2(max(viewDistance * angularFootprint * noiseTexelsPerMeter, 1.0)), maximumNoiseMip);
			const float extinction = sampleCloudDensity(pos, layer, 0.0, viewDistance, minimumMip, densityContext);
			const float depthDistance = distance;
			distance += step * (densityContext.ndf.dimension_profile > 0.05 ? 1.0 : 2.0);
			if (extinction <= 0.0)
				continue;
			const float viewDepth = extinction * info.viewExtinction * step * GAME_UNIT_TO_M;
			const float stepTransmittance = exp(-viewDepth);
			const float opacity = viewDepth < 0.001 ? CloudOpacity(viewDepth) : 1.0 - stepTransmittance;
			if (transmittance > 0.1) {
				const float3 source = CloudLighting(pos, direction, densityContext.ndf, extinction, viewDepth, opacity,
					viewDistance, minimumMip, phase);
				result.lum += transmittance * source;
			}
			[branch] if (ambientEnabled)
			{
				const float reducedDepth = CloudAmbientPhase.x * viewDepth;
				const float reducedOpacity = CloudOpacity(reducedDepth);
				result.lum += ambientTransmittance * reducedOpacity * CloudAmbientPhase.y * info.ambientStrength * info.cloudLightingScale *
				              CloudAmbientTransport::ViewSource(pos);
				ambientTransmittance *= exp(-reducedDepth);
				ambientBoundaryAltitude = pos.z;
			}

			const float depthWeight = CloudDepthWeight(transmittance, extinction, depthDistance);
			depthSum += depthWeight * depthDistance;
			weightSum += depthWeight;
			heightSum += depthWeight * densityContext.ndf.height_fraction;
			transmittance *= stepTransmittance;
		}
	}
	if (cirrusPending) {
		IntegrateCirrus(eye + direction * cirrusDistance, direction, cirrusDistance, cirrusPhase, ambient,
			result.lum, transmittance, ambientTransmittance, depthSum, weightSum, cirrusWeight);
	}
	if (ambientEnabled)
		result.lum += max(ambientTransmittance - transmittance, 0.0) * info.ambientStrength * info.cloudLightingScale *
		              CloudAmbientTransport::Environment(direction, ambientBoundaryAltitude);
	result.transmittance = saturate((transmittance - 0.1) / 0.9);
	// Keep radiance premultiplied by the remapped opacity.
	result.lum /= 1.0 - min(transmittance, 0.1);
	if (weightSum > 0.0) {
		result.cloud_depth = depthSum / weightSum;
		result.motionMetadata = float2(saturate(cirrusWeight / weightSum), saturate(heightSum / weightSum));
#ifdef CLOUD_AP_SHADOW
		if (!ViewAtmosphere::Enabled()) {
			// Foreground air must not inherit visibility averaged through the scene behind the cloud.
			if (SharedData::physSkyData.apLumMix > 0.0)
				apShadow = AtmosphereShadow::TraceShadow(direction, result.cloud_depth, jitter.x);
#endif
			const float4 ap = SampleCloudAerialPerspective(direction, result.cloud_depth, apShadow);
			result.lum = result.lum * ap.a + ap.rgb * (1.0 - result.transmittance);
#ifdef CLOUD_AP_SHADOW
		}
#endif
	}
	return result;
}

#include "PhysicalSky/CloudBlur.hlsli"
#include "PhysicalSky/CloudBounds.hlsli"
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
		density = localExtinction * length(ray_uv_increment * scale);

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
