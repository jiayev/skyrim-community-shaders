#ifndef PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI
#define PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI

// Jendersie and d'Eon, An Approximate Mie Scattering Function (2023).
float CloudLowPhase(float3 viewDir)
{
	return SampleLightDiscPhase(viewDir, CloudDisc(), CloudDiskIndex()).x;
}

#if defined(CLOUD_MAIN_DISPATCH) || defined(CLOUD_VOXEL_VIEW)
/** @brief Fraction of the cloud light disc's screen neighbourhood showing sky, the same test as the sun glare. */
float GetCloudLightScreenVisibility()
{
	const float4 lightCS = mul(FrameBuffer::CameraViewProj, float4(CloudDisc().xyz, 0.0));
	if (lightCS.w <= 0.0)
		return 1.0;
	const float2 lightUV = lightCS.xy / lightCS.w * float2(0.5, -0.5) + 0.5;
	uint visibleSamples = 0;
	[unroll] for (uint i = 0; i < FlareOcclusion::SampleCount; i++)
	{
		const float2 sampleUV = lightUV + FlareOcclusion::GetSampleOffset(i);
		visibleSamples += FrameBuffer::IsOutsideFrame(sampleUV) || TexDepth.Load(SharedData::ConvertUVToSampleCoord(sampleUV)) >= 1.0;
	}
	return FlareOcclusion::GetVisibility(visibleSamples);
}
#endif

/** @brief Low-cloud phase with its forward peak, the part above the phase 5 deg past the disc edge, faded out while geometry hides the light. */
float ApplyCloudForwardPeakOcclusion(float phase)
{
#if defined(CLOUD_MAIN_DISPATCH) || defined(CLOUD_VOXEL_VIEW)
	static const float kPeakWidth = 0.08726646;  // 5 deg past the disc edge
	const uint index = CloudDiskIndex();
	const float edgeCosine = cos(SharedData::physSkyData.diskAngularRadius[index] + kPeakWidth);
	const float peakFloor = lerp(DISK_PHASE_ISOTROPIC.x, SampleDiskPhase(edgeCosine, index).x, CloudDisc().w);
	[branch] if (phase > peakFloor) return lerp(peakFloor, phase, GetCloudLightScreenVisibility());
#endif
	return phase;
}

float CloudCirrusPhase(float3 viewDir)
{
	return SampleLightDiscPhase(viewDir, CloudDisc(), CloudDiskIndex()).y;
}

struct CloudAmbient
{
	sh2 skyR;
	sh2 skyG;
	sh2 skyB;
	float3 groundRadiance;
	float3 skyRadiance;
	float3 lowerBoundaryTransmittance;
};

CloudAmbient MakeCloudAmbient()
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 lowerBoundary = float3(0.0, 0.0, info.planetRadius + max(info.lowestCloudAltitude, GAME_UNITS_PER_METER));
	CloudAmbient ambient;
	ambient.skyR = TexCloudAmbientSH[int2(0, 0)];
	ambient.skyG = TexCloudAmbientSH[int2(1, 0)];
	ambient.skyB = TexCloudAmbientSH[int2(2, 0)];
	ambient.groundRadiance = TexCloudAmbientSH[int2(3, 0)].rgb;
	ambient.skyRadiance = SampleCloudAmbientRadiance();
	ambient.lowerBoundaryTransmittance = SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, lowerBoundary, float3(0.0, 0.0, 1.0));
	return ambient;
}

float3 CloudAmbientRadiance(float3 planetPos, CloudAmbient ambient)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float radius = max(length(planetPos), info.planetRadius);
	const float ratio = info.planetRadius / radius;
	const float cosDisc = sqrt(saturate(1.0 - ratio * ratio));
	const float3 sampleVerticalTransmittance = SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, planetPos, float3(0.0, 0.0, 1.0));
	const float3 columnTransmittance = min(ambient.lowerBoundaryTransmittance / max(sampleVerticalTransmittance, 1e-4), 1.0);
	const float3 groundRadiance = TexCloudAmbientSH[int2(4, 0)].rgb * columnTransmittance;
	return 0.5 * (1.0 + cosDisc) * ambient.skyRadiance + 0.5 * (1.0 - cosDisc) * groundRadiance;
}

float CloudCirrusLightDistance(float lightCosine)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float radius = (info.cloudCurvatureRadius + info.cirrusAltitude) * GAME_UNIT_TO_M;
	const float halfThickness = 0.5 * info.cirrusLightThickness;
	const float b = radius * lightCosine;
	const float outerC = halfThickness * (2.0 * radius + halfThickness);
	const float outerRoot = sqrt(b * b + outerC);
	float distance = b >= 0.0 ? outerC / (outerRoot + b) : outerRoot - b;
	const float innerC = halfThickness * (2.0 * radius - halfThickness);
	const float innerDiscriminant = b * b - innerC;
	if (b < 0.0 && innerDiscriminant >= 0.0)
		distance = min(distance, innerC / (-b + sqrt(innerDiscriminant)));
	return min(distance, 3000.0);
}

float CloudMeanTransmission(float depth)
{
	return depth < 0.05 ? 1.0 - depth * (0.5 - depth * (1.0 / 6.0 - depth / 24.0)) :
	                      (1.0 - exp(-depth)) / depth;
}

// Integrate the light source against view extinction across the unresolved layer.
float CloudCirrusTransmission(float viewDepth, float lightDepth, float nearFraction)
{
	const float nearDepth = lightDepth * nearFraction;
	const float farDepth = viewDepth + lightDepth * (1.0 - nearFraction);
	const float integral = exp(-min(nearDepth, farDepth)) * CloudMeanTransmission(abs(farDepth - nearDepth));
	return integral / CloudMeanTransmission(viewDepth);
}

float CloudOpacity(float depth)
{
	return depth < 0.001 ? depth * (1.0 - 0.5 * depth) : 1.0 - exp(-depth);
}

static const float WeatherLocalLightDistance = 1200.0;  // V5 base-layer source metres

bool IsWeatherMaterial(CloudMaterialSample material)
{
#ifdef CLOUD_VOXELS
	if (material.voxelBlend > 0.0)
		return false;
#endif
	return !material.stormVolume;
}

float TraceNdfLighting(float3 pos, float3 viewDir, CloudMaterialSample ndf, float extinction, float viewDistance, float minimumMip, float transportExtinction)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float localOpticalDepth = 0.0;
	float previousDistance = 0.0;
	float previousExtinction = extinction;
	const bool weatherCloud = !ndf.stormVolume;
	const uint budget = uint(10.0 - saturate((viewDistance - 512.0) * 0.00040192925) * 6.0);
	const uint count = weatherCloud ? max(budget, 6u) : budget;
	const float extentMeters = weatherCloud ? WeatherLocalLightDistance * info.weatherWorldScale :
	                                          240.0 - saturate(ndf.height_fraction * 3.3333333) * 120.0;
	const float spacingMeters = extentMeters / count;
	const float cosine = dot(viewDir, info.dirlightDir);
	const float angularOffset = -0.25 * cosine;
	const float jitter = CloudLightProbeJitter(pos.xy * GAME_UNIT_TO_M);
	const bool eruption = ndf.stormVolume && IsVolcanic(CloudStorms[ndf.stormIndex]);
	[loop] for (uint i = 0u; i < count; ++i)
	{
		const float index = i + 0.5;
		const float fraction = index / count;
		const float angularScale = angularOffset + 0.5 + fraction * fraction * fraction * (0.5 - angularOffset);
		// Resolve the ash billows near the surface before widening the probe cone.
		// Uniform 30 m spacing plus a full-cell jitter skipped these lobes entirely.
		const float nearFraction = (index + jitter * 0.5) / count;
		// Keep short intervals at the illuminated edge and integrate the full
		// requested path through the much thicker weather cloud behind it.
		const float weatherFraction = float(i + 1u) / count;
		const float distanceMeters = weatherCloud ? weatherFraction * weatherFraction * extentMeters :
		                             eruption     ? nearFraction * nearFraction * nearFraction * extentMeters :
		                                            (fraction * extentMeters + jitter * spacingMeters) * angularScale;
		const float3 samplePos = pos + info.dirlightDir * (distanceMeters * GAME_UNITS_PER_METER);
		CloudDensityContext context;
		// Filter the light probes separately from the detailed view density.
		const float mipBias = weatherCloud ? smoothstep(40.0, 250.0, distanceMeters) : 2.0;
#ifdef CLOUD_VOXELS
		sampleNonVoxelCloudDensity(samplePos, GetCloudLayer(info), mipBias, viewDistance, minimumMip, context);
#else
		sampleCloudDensity(samplePos, GetCloudLayer(info), mipBias, viewDistance, minimumMip, context);
#endif
		const float sigma = context.light_density;
		if (ndf.stormVolume && !eruption) {
			const float forward = saturate(cosine);
			const float weight = (forward + 1.0) * (spacingMeters * 0.45) * (index * 0.4 * forward + 1.0);
			localOpticalDepth += sigma * weight;
		} else
			localOpticalDepth += 0.5 * (previousExtinction + sigma) * (distanceMeters - previousDistance);
		// Both direct and transport terms are negligible past this depth.
		// Avoid the remaining expensive density probes in opaque storm interiors.
		if (ndf.stormVolume && localOpticalDepth * info.sunExtinction * min(transportExtinction, 1.0) > 12.0)
			break;
		previousExtinction = sigma;
		previousDistance = distanceMeters;
	}
	return localOpticalDepth;
}

float TraceCloudLighting(float3 pos, float3 viewDir, CloudMaterialSample ndf, float extinction, float viewDistance, float minimumMip, float transportExtinction)
{
#ifdef CLOUD_VOXELS
	float voxelDepth = 0.0;
	[branch] if (ndf.voxelBlend > 0.0)
		voxelDepth = TraceVoxelLighting(pos, viewDistance);
	if (ndf.voxelBlend >= .9999)
		return voxelDepth;
	const float ndfDepth = TraceNdfLighting(pos, viewDir, ndf, extinction * (1.0 - ndf.voxelBlend), viewDistance, minimumMip, transportExtinction);
	return voxelDepth + ndfDepth;
#else
	return TraceNdfLighting(pos, viewDir, ndf, extinction, viewDistance, minimumMip, transportExtinction);
#endif
}

// Local isotropic closure: https://zhuanlan.zhihu.com/p/457997155
float CloudScatteringSeries(float opticalDepth, float albedo)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float transmission = exp(-opticalDepth);
	const float opacity = opticalDepth < 0.001 ? opticalDepth * (1.0 - 0.5 * opticalDepth) : 1.0 - transmission;
	const float recirculation = albedo * opacity;
	const float escapeAndAbsorption = (1.0 - albedo) + albedo * transmission;
	return (0.25 * RCP_PI) * recirculation / escapeAndAbsorption;
}

// Material transitions follow the extinction contribution, rather than the
// density winner, so partial storm replacement does not pop between two albedos.
float CloudMaterialAlbedo(CloudMaterialSample ndf)
{
	const float weather = VolumetricCloudBuffer[0].msAlbedo;
#if !defined(NO_CLOUD_STORMS) && !defined(VOLCANIC_STORMS_ONLY)
	return ndf.stormVolume ? lerp(weather, ndf.stormAlbedo, ndf.stormBlend) : weather;
#else
	return ndf.stormVolume ? lerp(weather, CloudStorms[ndf.stormIndex].driftDensity.w, ndf.stormBlend) : weather;
#endif
}

float2 CloudMaterialAmbient(CloudMaterialSample ndf, float2 result)
{
	if (ndf.stormVolume) {
#if !defined(NO_CLOUD_STORMS) && !defined(VOLCANIC_STORMS_ONLY)
		const float2 storm = ndf.stormAmbientPhase;
#else
		const float2 storm = CloudStorms[ndf.stormIndex].ambientPhase.xy;
#endif
		const float scattering = lerp(result.x * result.y, storm.x * storm.y, ndf.stormBlend);
		result.x = lerp(result.x, storm.x, ndf.stormBlend);
		result.y = scattering / max(result.x, 1e-6);
	}
	return result;
}

float2 CloudMaterialAmbient(CloudMaterialSample ndf)
{
	return CloudMaterialAmbient(ndf, CloudAmbientPhase.xy);
}

struct CloudMsProfile
{
	float densityScale;
	float referenceDepth;
	float4 coefficients;
};

CloudMsProfile PrepareCloudMsProfile(CloudMaterialSample ndf)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float width = min(info.msBoundaryWidth, max(ndf.altitude.y - ndf.altitude.x, 0.0) * (0.25 * GAME_UNIT_TO_M));
	const float weatherDensity = ndf.referenceExtinction > 0.0 ? ndf.referenceExtinction : info.cumulusExtinction;
#if !defined(NO_CLOUD_STORMS) && !defined(VOLCANIC_STORMS_ONLY)
	float densityScale = ndf.stormVolume ? lerp(weatherDensity, ndf.stormDensity, ndf.stormBlend) : weatherDensity;
#else
	float densityScale = ndf.stormVolume ? lerp(weatherDensity, CloudStorms[ndf.stormIndex].driftDensity.z, ndf.stormBlend) : weatherDensity;
#endif
#ifdef CLOUD_VOXELS
	densityScale += ndf.voxelBlend * (ndf.voxelDensityScale - weatherDensity);
#endif
	const float referenceDepth = densityScale * width;
	CloudMsProfile result = (CloudMsProfile)0;
	result.densityScale = densityScale;
	result.referenceDepth = referenceDepth;
	if (referenceDepth <= 0.0 || densityScale <= 0.0)
		return result;
	const float coordinate = clamp((log2(referenceDepth) + 12.0) * 2.0, 0.0, 48.0);
	const uint index = min((uint)coordinate, 47u);
	float4 low = ndf.stormVolume ? CloudStorms[ndf.stormIndex].msCalibration[index] : CloudMsCalibration[index];
	float4 high = ndf.stormVolume ? CloudStorms[ndf.stormIndex].msCalibration[index + 1u] : CloudMsCalibration[index + 1u];
#if !defined(NO_CLOUD_STORMS) && !defined(VOLCANIC_STORMS_ONLY)
	[branch] if (ndf.stormVolume && ndf.stormSecondaryBlend > 0.0)
	{
		low = lerp(low, CloudStorms[ndf.stormSecondaryIndex].msCalibration[index], ndf.stormSecondaryBlend);
		high = lerp(high, CloudStorms[ndf.stormSecondaryIndex].msCalibration[index + 1u], ndf.stormSecondaryBlend);
	}
#endif
	[branch] if (ndf.stormVolume && ndf.stormBlend < 1.0)
	{
		low = lerp(CloudMsCalibration[index], low, ndf.stormBlend);
		high = lerp(CloudMsCalibration[index + 1u], high, ndf.stormBlend);
	}
	result.coefficients = lerp(low, high, coordinate - float(index));
	return result;
}

float CloudMultipleScattering(float extinction, CloudMaterialSample ndf, CloudMsProfile calibration)
{
	const float referenceDepth = calibration.referenceDepth;
	const float densityScale = calibration.densityScale;
	if (referenceDepth <= 0.0 || extinction <= 0.0 || densityScale <= 0.0)
		return 0.0;
	const float exponent = CloudDensityExponent(ndf.height_fraction);
	const float4 coefficients = calibration.coefficients;
	const float profile = clamp((exponent - 0.35) * 10.0, 0.0, 3.0);
	const uint profileIndex = min((uint)profile, 2u);
	float coefficient = lerp(coefficients[profileIndex], coefficients[profileIndex + 1u], profile - float(profileIndex));
	if (referenceDepth > 4096.0)
		coefficient *= pow(referenceDepth / 4096.0, exponent / (exponent + 1.0));
	else if (referenceDepth < 0.000244140625)
		coefficient *= referenceDepth * 4096.0;
	return CloudScatteringSeries(coefficient * extinction / densityScale, CloudMaterialAlbedo(ndf));
}

float CloudMultipleScattering(float extinction, CloudMaterialSample ndf)
{
	return CloudMultipleScattering(extinction, ndf, PrepareCloudMsProfile(ndf));
}

float CloudTransportExtinction(CloudMaterialSample ndf)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float4 fit = info.lowCloudPhase;
	const float meanDraine = fit.y * (1.0 + fit.z * (3.0 + 2.0 * fit.y * fit.y) / 5.0) / (1.0 + fit.z * (1.0 + 2.0 * fit.y * fit.y) / 3.0);
	const float meanCosine = lerp(fit.x, meanDraine, fit.w);
	return ndf.stormVolume ? 1.0 - CloudMaterialAlbedo(ndf) * meanCosine : info.msTransportExtinction;
}

// Local volumes retain their authored solar sampling. Weather samples use WeatherCloudLightDepth.
float CloudLightOpticalDepth(float3 pos, float3 viewDir, CloudMaterialSample ndf, float extinction, float viewDistance, float minimumMip)
{
	return TraceCloudLighting(pos, viewDir, ndf, extinction, viewDistance, minimumMip, CloudTransportExtinction(ndf)) * VolumetricCloudBuffer[0].sunExtinction;
}

float3 CloudIncidentSunlight(float3 pos)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 planetPosition = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	return SampleAtmosphereLightTr(TexTransmittance, TransmittanceSampler, planetPosition, info.dirlightDir, SharedData::physSkyData.diskAngularRadius[CloudDiskIndex()]) * GetCloudDirectionalLightColor();
}

float CloudDirectScatterAmount(float extinction, float viewDepth)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float densityGate = saturate(extinction / info.scatterDensityThreshold);
	[branch] if (info.scatterDensityPower != 1.0)
		densityGate = pow(densityGate, info.scatterDensityPower);
	return CloudOpacity(viewDepth * densityGate);
}

float3 ShadeCloudLighting(float3 pos, CloudMaterialSample ndf, float extinction, float viewDepth, float viewOpacity, float phase, float opticalDepth, float multipleScattering, float externalSunTransmission)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float albedo = CloudMaterialAlbedo(ndf);
	const float transportExtinction = CloudTransportExtinction(ndf);
	const float directional = albedo * phase * exp(-opticalDepth);
	const float3 sunlight = CloudIncidentSunlight(pos);
	const float scatterAmount = CloudDirectScatterAmount(extinction, viewDepth);
	float multiple = 0.0;
	[branch] if (info.msStrength > 0.0 && albedo > 0.0)
		multiple = info.msStrength * albedo * multipleScattering * exp(-opticalDepth * transportExtinction);
	return info.cloudLightingScale * sunlight * externalSunTransmission * (directional * scatterAmount + multiple * viewOpacity);
}

float CloudExternalSunTransmission(float3 pos)
{
	float sunTransmission = UpperSunTransmission(pos);
#ifdef CLOUD_AP_SHADOW
	// Scene shadows follow the ground light, which can be clamped or lunar while clouds remain sunlit.
	// Both directions are normalized; allow only floating-point error when matching them.
	[branch] if (dot(VolumetricCloudBuffer[0].dirlightDir, SharedData::DirLightDirection.xyz) > 0.999999)
	{
		// Cloud-space z is relative to bottomZ; the heightmap is in world z.
		const float3 worldPos = pos + float3(0, 0, VolumetricCloudBuffer[0].bottomZ);
		sunTransmission *= TerrainShadows::GetTerrainShadow(worldPos, TransmittanceSampler);
		[branch] if (CloudCascadeShadows != 0u && sunTransmission > 0.0)
			sunTransmission *= AtmosphereShadow::SampleCascadeShadow(worldPos - FrameBuffer::CameraPosAdjust.xyz);
	}
#endif
	return sunTransmission;
}

float3 ShadeCloudLighting(float3 pos, CloudMaterialSample ndf, float extinction, float viewDepth, float viewOpacity, float phase, float opticalDepth, float multipleScattering)
{
	return ShadeCloudLighting(pos, ndf, extinction, viewDepth, viewOpacity, phase, opticalDepth, multipleScattering, CloudExternalSunTransmission(pos));
}

float3 ShadeCloudLighting(float3 pos, CloudMaterialSample ndf, float extinction, float viewDepth, float viewOpacity, float phase, float opticalDepth)
{
	float multipleScattering = 0.0;
	[branch] if (VolumetricCloudBuffer[0].msStrength > 0.0 && CloudMaterialAlbedo(ndf) > 0.0)
		multipleScattering = CloudMultipleScattering(extinction, ndf);
	return ShadeCloudLighting(pos, ndf, extinction, viewDepth, viewOpacity, phase, opticalDepth, multipleScattering);
}

// Ambient reflectance for water clouds.
static const float CloudAmbientReflectance = 120.0 / 255.0;

float2 WeatherScatteringWeights(CloudMaterialSample material, float referenceExtinction, float extinction, float viewDepth, float opacity)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	// At the UI minimum the droplets scatter their full albedo even in a dilute fringe.
	const float gateDensity = max(extinction, 0.001);
	const float direct = gateDensity >= info.scatterDensityThreshold ? 1.0 :
	                                                                   CloudDirectScatterAmount(gateDensity, viewDepth) / max(opacity, 1e-12);
	material.referenceExtinction = referenceExtinction;
	const float multiple = info.msStrength > 0.0 && info.msAlbedo > 0.0 ? CloudMultipleScattering(extinction, material) : 0.0;
	return float2(direct, multiple);
}

float3 ShadeWeatherScattering(float3 pos, float2 scattering, float phase, float2 sunTransmission)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float source = scattering.x * phase * sunTransmission.x + info.msStrength * scattering.y * sunTransmission.y;
	return source * info.msAlbedo * info.cloudLightingScale * CloudIncidentSunlight(pos) * CloudExternalSunTransmission(pos);
}

#ifdef WEATHER_VIEW
// Within a homogeneous interval absorption is proportional to exp(-tau * x).
float WeatherAbsorptionCentroid(float tau, float transmission, float opacity)
{
	if (tau < 0.25)
		return 0.5 - tau / 12.0;
	return rcp(tau) - transmission / opacity;
}

float3 WeatherViewSource(float3 pos, float2 scattering, float ambient, float phase, float2 sunTransmission)
{
	float3 radiance = ShadeWeatherScattering(pos, scattering, phase, sunTransmission);
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	[branch] if (ambient > 0.0 && info.ambientStrength > 0.0 && info.msAlbedo > 0.0)
	{
		const float3 sky = CloudAmbientLight::SkySource(pos);
		radiance += ambient * CloudAmbientPhase.y * info.ambientStrength * info.cloudLightingScale * sky;
	}
	return radiance;
}

#endif

float3 CloudLighting(float3 pos, float3 viewDir, CloudMaterialSample ndf, float extinction, float viewDepth, float viewOpacity,
	float viewDistance, float minimumMip, float phase)
{
	const float opticalDepth = CloudLightOpticalDepth(pos, viewDir, ndf, extinction, viewDistance, minimumMip);
	return ShadeCloudLighting(pos, ndf, extinction, viewDepth, viewOpacity, phase, opticalDepth);
}

#endif
