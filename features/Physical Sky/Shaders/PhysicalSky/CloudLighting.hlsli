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

float CloudCacheRange(bool high)
{
	return VolumetricCloudBuffer[0].lightCacheRange * (high ? 4.0 : 1.0);
}

float SampleCloudLightCache(float3 pos, bool high, out float2 opticalDepth)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float altitude = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	const float bottom = high ? info.highCloudBottom : info.lowCloudBaseAltitude;
	const float top = high ? info.highCloudTop : info.lowCloudTraceTopAltitude;
	const float2 relative = (pos.xy - info.lightCacheWindDelta - info.lightCacheOrigin) * (2.0 / CloudCacheRange(high));
	const float2 xy = sign(relative) * sqrt(abs(relative)) * 0.5 + 0.5;
	const float3 uvw = float3(xy, (altitude - bottom) / max(top - bottom, 1.0));
	opticalDepth = 0.0;
	if (any(xy <= 0.0) || any(xy >= 1.0) || altitude < bottom || altitude > top)
		return 0.0;
	if (high)
		opticalDepth = TexHighLightCache.SampleLevel(TransmittanceSampler, uvw, 0).xy;
	else
		opticalDepth = TexLowLightCache.SampleLevel(TransmittanceSampler, uvw, 0).xy;
	return saturate(min(min(xy.x, xy.y), min(1.0 - xy.x, 1.0 - xy.y)) * 64.0 - 0.5);
}

float CloudCachedSun(float3 pos, bool high)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 planetPos = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const CloudRaySegments segments = GetCloudRaySegments(planetPos, info.dirlightDir,
		high ? info.highCloudBottom : info.lowCloudBaseAltitude,
		high ? info.highCloudTop : info.lowCloudTraceTopAltitude, CLOUD_SKY_DISTANCE, info);
	if (segments.length <= 0.0 || (high && info.highCloudEnabled <= 0.0))
		return 0.0;
	float2 cached;
	const float entryOffset = segments.nearSegment.x > 0.0 ? min(segments.nearLength * 0.5, 2.0 * GAME_UNITS_PER_METER) : 0.0;
	const float3 entry = pos + info.dirlightDir * (segments.nearSegment.x + entryOffset);
	const float weight = SampleCloudLightCache(entry, high, cached);
	if (weight >= 1.0)
		return cached.x;
	return lerp(CloudColumnOpticalDepth(pos, info.dirlightDir, high, true, 4u, CLOUD_SKY_DISTANCE), cached.x, weight);
}

float CloudSunOpticalDepth(float3 pos, float3 viewDir, float height, bool high)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float cosine = dot(viewDir, info.dirlightDir);
	const float extent = (240.0 - saturate(height * 3.3333333) * 120.0) * GAME_UNITS_PER_METER;
	const float angularOffset = -0.25 * cosine;
	float opticalDepth = 0.0;
	float previousDistance = 0.0;
	// The far column begins after the two local intervals to avoid counting them twice.
	[unroll] for (uint i = 0u; i < 2u; ++i)
	{
		const float fraction = (i + 1.0) * 0.5;
		const float distance = extent * fraction * (angularOffset + 0.5 + fraction * fraction * fraction * (0.5 - angularOffset));
		const float3 samplePos = pos + info.dirlightDir * (0.5 * (previousDistance + distance));
		float extinction;
		if (high) {
			float h;
			float4 weather;
			extinction = EvaluateHighCloudDensity(samplePos, h, weather) * info.highLightAbsorption * (1.0 + weather.r * info.highCoverAbsorptionStrength);
		} else {
			CloudDensityContext context;
			extinction = sampleCloudDensity(samplePos, GetCloudLayer(info), 1.0, false, context);
		}
		opticalDepth += extinction * (distance - previousDistance) * GAME_UNIT_TO_M;
		previousDistance = distance;
	}
	return opticalDepth + CloudCachedSun(pos + info.dirlightDir * extent, high);
}

float2 CloudEnvironmentOpticalDepth(float3 pos, bool high, float profile)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	float2 cached;
	const float weight = SampleCloudLightCache(pos, high, cached);
	float upward = lerp(profile, cached.y, weight);
	float otherSun = 0.0;
	if (info.crossLayerShadows != 0u && info.highCloudEnabled > 0.0) {
		otherSun = CloudCachedSun(pos, !high);
		if (!high) {
			const float3 planetPos = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
			const float3 up = normalize(planetPos);
			const float entry = max(IntersectSpherePair(planetPos, up, info.planetRadius + info.highCloudBottom).y, 0.0);
			float2 other;
			const float otherWeight = SampleCloudLightCache(pos + up * (entry + GAME_UNITS_PER_METER), true, other);
			upward += other.y * otherWeight;
		}
	}
	return float2(upward, otherSun);
}

[numthreads(4, 4, 4)] void buildCloudLightCache(uint3 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	uint3 dims;
	RWCloudLightCache.GetDimensions(dims.x, dims.y, dims.z);
	if (info.lightCacheUpdatePhase < 8u)
		tid.z = tid.z * 8u + info.lightCacheUpdatePhase;
	if (any(tid >= dims))
		return;
#ifdef HIGH_CLOUD_CACHE
	const bool high = true;
#else
	const bool high = false;
#endif
	const float2 xy = (float2(tid.xy) + 0.5) / dims.xy * 2.0 - 1.0;
	const float2 offset = sign(xy) * xy * xy * (0.5 * CloudCacheRange(high));
	const float height = lerp(high ? info.highCloudBottom : info.lowCloudBaseAltitude,
		high ? info.highCloudTop : info.lowCloudTraceTopAltitude, (tid.z + 0.5) / dims.z);
	const float radius = info.planetRadius + height;
	const float2 worldXY = info.lightCacheOrigin + offset + info.lightCacheWindDelta;
	const float2 planetXY = worldXY - FrameBuffer::CameraPosAdjust.xy;
	const float3 pos = float3(worldXY, sqrt(max(radius * radius - dot(planetXY, planetXY), 0.0)) - info.planetRadius);
	const float3 up = normalize(float3(planetXY, pos.z + info.planetRadius));
	const float sun = CloudColumnOpticalDepth(pos, info.dirlightDir, high, true, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	const float ambient = CloudColumnOpticalDepth(pos, up, high, false, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	RWCloudLightCache[tid] = min(float2(sun, ambient), 65504.0);
}

float3 LowCloudLighting(float3 pos, float3 viewDir, NDFInfo ndf, float2 phase, float3 ambientTop, float3 ambientBottom)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float opticalDepth = CloudSunOpticalDepth(pos, viewDir, ndf.height_fraction, false);
	const float2 environment = CloudEnvironmentOpticalDepth(pos, false, ndf.dimension_profile);
	const float3 extinction = info.scatterTint * opticalDepth;
	// A fixed reference length keeps the scattering volume independent of the view budget.
	float volume = saturate((ndf.dimension_profile * 3.0 - 0.1) / 0.9);
	volume *= pow(saturate(ndf.coverage * ndf.top_type), 0.25);
	volume *= pow(max(ndf.height_fraction, 1e-4), info.msHeightPower);
	const float3 direct = exp(-extinction) * phase.x + volume * exp(-extinction * info.msDepthPower) * phase.y * info.msContribution;
	const float ambientVisibility = sqrt(saturate(1.0 - ndf.dimension_profile)) * exp(-environment.x * info.aoUpwardScale);
	const float3 ambient = EvaluateCloudEnvironmentRadiance(ambientTop, ambientBottom, ndf.height_fraction,
		info.ambientTopMultiplier, info.ambientBottomMultiplier, ambientVisibility);
	return direct * sampleExternalSunTransmittance(pos, info.dirlightDir) * exp(-info.scatterTint * environment.y) * GetCloudDirectionalLightColor() + ambient;
}

float3 HighCloudLighting(float3 pos, float3 viewDir, float height, float4 weather, float density,
	float2 phase, float3 ambientTop, float3 ambientBottom, float3 skyBlend)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float profile = saturate(density);
	const float2 environment = CloudEnvironmentOpticalDepth(pos, true, profile);
	const float3 extinction = info.scatterTint * CloudSunOpticalDepth(pos, viewDir, height, true);
	const float3 direct = exp(-extinction) * phase.x + profile * exp(-extinction * info.highMSAttenuation) * phase.y * info.highMSContribution;
	const float3 ambient = EvaluateCloudEnvironmentRadiance(ambientTop, ambientBottom, height,
		info.highAmbientTopMultiplier, info.highAmbientBottomMultiplier,
		sqrt(1.0 - profile) * exp(-environment.x * info.aoUpwardScale));
	const float3 radiance = direct * sampleExternalSunTransmittance(pos, info.dirlightDir) * exp(-info.scatterTint * environment.y) * GetCloudDirectionalLightColor() + ambient;
	return lerp(radiance, skyBlend, smoothstep(0.0, 1.0, height) * (1.0 - saturate(info.highSkyBlendStrength)));
}

#endif
