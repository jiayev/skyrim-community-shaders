#ifndef PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI
#define PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI

float CloudColumnOpticalDepth(float3 pos, float3 direction, bool high, bool solar, uint steps, float maximumDistance)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (high && info.highCloudEnabled <= 0.0)
		return 0.0;
	const float3 planetPos = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const float2 ground = IntersectSpherePair(planetPos, direction, info.planetRadius);
	if (dot(planetPos, direction) < 0.0 && ground.y > 0.0)
		maximumDistance = min(maximumDistance, max(ground.x, 0.0));
	const CloudRaySegments segments = GetCloudRaySegments(planetPos, direction,
		high ? info.highCloudBottom : info.lowCloudBaseAltitude,
		high ? info.highCloudTop : info.lowCloudTraceTopAltitude, maximumDistance, info);
	float opticalDepth = 0.0;
	steps = max(steps, 1u);
	[unroll] for (uint segment = 0u; segment < 2u; ++segment)
	{
		const float2 interval = segment == 0u ? segments.nearSegment : segments.farSegment;
		if (interval.y <= interval.x)
			continue;
		[loop] for (uint i = 0u; i < steps; ++i)
		{
			const float a = float(i) / steps;
			const float b = float(i + 1u) / steps;
			const float start = lerp(interval.x, interval.y, a * a);
			const float end = lerp(interval.x, interval.y, b * b);
			const float3 samplePos = pos + direction * (0.5 * (start + end));
			float extinction;
			if (high) {
				float height;
				float4 weather;
				const float density = EvaluateHighCloudDensity(samplePos, height, weather);
				extinction = density * (solar ? info.highLightAbsorption * (1.0 + weather.r * info.highCoverAbsorptionStrength) : info.highViewAbsorption * weather.a);
			} else {
				CloudDensityContext context;
				extinction = sampleCloudDensity(samplePos, GetCloudLayer(info), 2.0, false, context);
			}
			opticalDepth += max(extinction, 0.0) * ((end - start) * GAME_UNIT_TO_M);
		}
	}
	return opticalDepth;
}

void CloudLightCacheBounds(bool high, out float3 lower, out float3 upper)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float range = high ? info.highLightCacheRange : info.shadowVolumeRange;
	lower = float3(FrameBuffer::CameraPosAdjust.xy - range * 0.5, high ? info.highCloudBottom : info.lowCloudBaseAltitude);
	upper = float3(FrameBuffer::CameraPosAdjust.xy + range * 0.5, high ? info.highCloudTop : info.lowCloudTraceTopAltitude);
}

float SampleCloudLightCache(float3 pos, bool high, out float4 opticalDepth)
{
	opticalDepth = 0.0;
	if (VolumetricCloudBuffer[0].lightCacheEnabled == 0u)
		return 0.0;
	float3 lower, upper;
	CloudLightCacheBounds(high, lower, upper);
	const float3 uvw = (pos - lower) / max(upper - lower, 1.0);
	if (any(uvw <= 0.0) || any(uvw >= 1.0))
		return 0.0;
	uint3 dims;
	if (high) {
		TexHighLightCache.GetDimensions(dims.x, dims.y, dims.z);
		opticalDepth = TexHighLightCache.SampleLevel(TransmittanceSampler, uvw, 0);
	} else {
		TexLowLightCache.GetDimensions(dims.x, dims.y, dims.z);
		opticalDepth = TexLowLightCache.SampleLevel(TransmittanceSampler, uvw, 0);
	}
	const float3 edge = min(uvw, 1.0 - uvw) * float3(dims);
	return smoothstep(0.5, 2.5, min(edge.x, min(edge.y, edge.z)));
}

float CloudSunOpticalDepth(float3 pos, bool high)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float nearDistance = high ? 250.0 * GAME_UNITS_PER_METER : 0.0;
	float4 cached;
	const float weight = SampleCloudLightCache(pos + info.dirlightDir * nearDistance, high, cached);
	float cachedDepth = cached.x;
	if (weight > 0.0 && high)
		cachedDepth += CloudColumnOpticalDepth(pos, info.dirlightDir, true, true, 3u, nearDistance);
	if (weight >= 1.0)
		return cachedDepth;
	const uint steps = high ? info.highLightSteps : info.lightCacheSteps;
	const float directDepth = CloudColumnOpticalDepth(pos, info.dirlightDir, high, true, steps, CLOUD_SKY_DISTANCE);
	return lerp(directDepth, cachedDepth, weight);
}

float2 CloudEnvironmentOpticalDepth(float3 pos, bool high)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float4 cached;
	const float weight = SampleCloudLightCache(pos, high, cached);
	const bool crossLayer = info.crossLayerShadows != 0u;
	const float2 cachedDepth = float2(cached.y + (crossLayer ? cached.w : 0.0), crossLayer ? cached.z : 0.0);
	if (weight >= 1.0)
		return cachedDepth;
	const float ownUp = CloudColumnOpticalDepth(pos, float3(0, 0, 1), high, false, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	float otherUp = 0.0;
	float otherSun = 0.0;
	if (crossLayer) {
		otherUp = CloudColumnOpticalDepth(pos, float3(0, 0, 1), !high, false, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
		otherSun = CloudColumnOpticalDepth(pos, info.dirlightDir, !high, true, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	}
	return lerp(float2(ownUp + otherUp, otherSun), cachedDepth, weight);
}

[numthreads(4, 4, 4)] void buildCloudLightCache(uint3 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	uint3 dims;
	RWCloudLightCache.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims))
		return;
#ifdef HIGH_CLOUD_CACHE
	const bool high = true;
#else
	const bool high = false;
#endif
	float3 lower, upper;
	CloudLightCacheBounds(high, lower, upper);
	const float3 pos = lerp(lower, upper, (float3(tid) + 0.5) / dims);
	float4 opticalDepth;
	opticalDepth.x = CloudColumnOpticalDepth(pos, info.dirlightDir, high, true, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	opticalDepth.y = CloudColumnOpticalDepth(pos, float3(0, 0, 1), high, false, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	opticalDepth.zw = 0.0;
	if (info.crossLayerShadows != 0u) {
		opticalDepth.z = CloudColumnOpticalDepth(pos, info.dirlightDir, !high, true, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
		opticalDepth.w = CloudColumnOpticalDepth(pos, float3(0, 0, 1), !high, false, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	}
	RWCloudLightCache[tid] = min(opticalDepth, 65504.0);
}

float3 HighCloudLighting(float3 pos, float3 viewDir, float height, float4 weather, float density,
	float3 phase, float3 ambientTop, float3 ambientBottom, float3 skyBlend)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 environmentDepth = CloudEnvironmentOpticalDepth(pos, true);
	const float3 externalSun = sampleExternalSunTransmittance(pos, info.dirlightDir) * exp(-info.scatterTint * environmentDepth.y);
	const float3 lightExtinction = info.scatterTint * CloudSunOpticalDepth(pos, true);
	const float3 attenuation = float3(1.0, info.highMSAttenuation, info.highMSAttenuation * info.highMSAttenuation);
	const float3 contribution = float3(1.0, info.highMSContribution, info.highMSContribution * info.highMSContribution);
	float3 directional = 0.0;
	[unroll] for (uint octave = 0u; octave < 3u; ++octave)
		directional += exp(-lightExtinction * attenuation[octave]) * phase[octave] * contribution[octave];
	directional *= CloudPowderEffect(density, dot(viewDir, info.dirlightDir), info.powderIntensity);
	const float3 ambient = EvaluateCloudEnvironmentRadiance(ambientTop, ambientBottom, height,
		info.highAmbientTopMultiplier, info.highAmbientBottomMultiplier, exp(-environmentDepth.x * max(info.aoUpwardScale, 0.0)));
	const float3 radiance = (externalSun * GetCloudDirectionalLightColor() * directional + ambient) * weather.a;
	return lerp(radiance, skyBlend, smoothstep(0.0, 1.0, height) * (1.0 - saturate(info.highSkyBlendStrength)));
}

float HighCloudThinWeight(float3 planetOrigin, float3 direction, CloudRaySegments segments, bool sky, out float distance)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	distance = 0.0;
	if (info.highThinLayer == 0u || !sky)
		return 0.0;
	const float middle = 0.5 * (info.highCloudBottom + info.highCloudTop);
	distance = IntersectSpherePair(planetOrigin, direction, info.planetRadius + middle).y;
	const float innerRadius = info.planetRadius + info.highCloudBottom;
	const float outerExit = IntersectSpherePair(planetOrigin, direction, info.planetRadius + info.highCloudTop).y;
	if (dot(planetOrigin, planetOrigin) >= innerRadius * innerRadius ||
		distance <= 0.0 || distance < segments.nearSegment.x || distance > segments.nearSegment.y ||
		segments.farSegment.y > segments.farSegment.x || segments.nearSegment.y + 1.0 < outerExit)
		return 0.0;
	const float incidence = abs(dot(direction, normalize(planetOrigin + direction * distance)));
	const float thickness = info.highCloudTop - info.highCloudBottom;
	return smoothstep(info.highThinStart, info.highThinEnd, distance) * smoothstep(0.15, 0.35, incidence) *
	       (1.0 - smoothstep(500.0 * GAME_UNITS_PER_METER, 1000.0 * GAME_UNITS_PER_METER, thickness));
}

void IntegrateThinHighCloud(float3 eyePos, float3 planetOrigin, float3 direction, float distance,
	float3 phase, float3 ambientTop, float3 ambientBottom, float3 skyBlend,
	out float3 radiance, out float transmittance, out float meanDepth)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 normal = normalize(planetOrigin + direction * distance);
	const float3 surface = eyePos + direction * distance;
	const float thickness = info.highCloudTop - info.highCloudBottom;
	float extinctionSum = 0.0;
	float densitySum = 0.0;
	float heightSum = 0.0;
	float4 weatherSum = 0.0;
	float3 positionSum = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const float3 pos = surface + normal * (((i + 0.5) * 0.25 - 0.5) * thickness);
		float height;
		float4 weather;
		const float density = EvaluateHighCloudDensity(pos, height, weather);
		const float extinction = max(density * info.highViewAbsorption * weather.a, 0.0);
		extinctionSum += extinction;
		densitySum += density * extinction;
		heightSum += height * extinction;
		weatherSum += weather * extinction;
		positionSum += pos * extinction;
	}
	radiance = 0.0;
	transmittance = 1.0;
	meanDepth = distance;
	if (extinctionSum <= 0.0)
		return;
	const float pathLength = thickness * GAME_UNIT_TO_M / max(abs(dot(direction, normal)), 0.15);
	transmittance = exp(-extinctionSum * 0.25 * pathLength);
	const float3 meanPosition = positionSum / extinctionSum;
	const float3 lighting = HighCloudLighting(meanPosition, direction, heightSum / extinctionSum,
		weatherSum / extinctionSum, densitySum / extinctionSum, phase, ambientTop, ambientBottom, skyBlend);
	radiance = lighting * (1.0 - transmittance);
	meanDepth = max(dot(meanPosition - eyePos, direction), 0.0);
}

#endif
