#ifndef PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI
#define PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI

float CloudLightDensity(float extinction, float height)
{
	const float h2 = height * height;
	return extinction * (1.0 + 3.0 * h2 * h2);
}

float SampleCloudLightDensity(float3 pos, bool high, float mip)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (high) {
		float height;
		float4 weather;
		const float density = EvaluateHighCloudDensity(pos, height, weather);
		return CloudLightDensity(density, height);
	}
	CloudDensityContext context;
	const float extinction = sampleCloudDensity(pos, GetCloudLayer(info), mip, false, context);
	return CloudLightDensity(extinction, context.ndf.height_fraction);
}

float CloudColumnOpticalDepth(float3 pos, float3 direction, bool high, uint steps, float maximumDistance)
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
			opticalDepth += SampleCloudLightDensity(samplePos, high, 2.0) * ((end - start) * GAME_UNIT_TO_M);
		}
	}
	return opticalDepth;
}

float CloudCacheRange(bool high)
{
	return VolumetricCloudBuffer[0].lightCacheRange * (high ? 4.0 : 1.0);
}

float2 CloudCachePosition(float2 coordinate)
{
	return coordinate * abs(coordinate);
}

float2 CloudCacheInterpolationUv(float2 relative, float2 dimensions)
{
	const float2 index = (sign(relative) * sqrt(abs(relative)) * 0.5 + 0.5) * dimensions - 0.5;
	const float2 base = clamp(floor(index), 0.0, dimensions - 2.0);
	const float2 left = CloudCachePosition((base + 0.5) / dimensions * 2.0 - 1.0);
	const float2 right = CloudCachePosition((base + 1.5) / dimensions * 2.0 - 1.0);
	// Interpolate physical distances, including the cell spanning each centre axis.
	const float2 fraction = saturate((relative - left) / (right - left));
	return (base + fraction + 0.5) / dimensions;
}

float SampleCloudLightCache(float3 pos, bool high, out float opticalDepth)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float altitude = length(pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius)) - info.planetRadius;
	const float bottom = high ? info.highCloudBottom : info.lowCloudBaseAltitude;
	const float top = high ? info.highCloudTop : info.lowCloudTraceTopAltitude;
	const float2 relative = (pos.xy - info.lightCacheWindDelta - info.lightCacheOrigin) * (2.0 / CloudCacheRange(high));
	opticalDepth = 0.0;
	if (any(abs(relative) >= 1.0) || altitude < bottom || altitude > top)
		return 0.0;
	uint3 dims;
	if (high)
		TexHighLightCache.GetDimensions(dims.x, dims.y, dims.z);
	else
		TexLowLightCache.GetDimensions(dims.x, dims.y, dims.z);
	const float2 xy = CloudCacheInterpolationUv(relative, float2(dims.xy));
	const float3 uvw = float3(xy, (altitude - bottom) / max(top - bottom, 1.0));
	if (high)
		opticalDepth = TexHighLightCache.SampleLevel(TransmittanceSampler, uvw, 0);
	else
		opticalDepth = TexLowLightCache.SampleLevel(TransmittanceSampler, uvw, 0);
	const float edge = min(1.0 - abs(relative.x), 1.0 - abs(relative.y));
	return smoothstep(0.0, 0.1, edge);
}

float CloudCachedSun(float3 pos, bool high)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (high && info.highCloudEnabled <= 0.0)
		return 0.0;
	const float3 planetPos = pos + float3(-FrameBuffer::CameraPosAdjust.xy, info.planetRadius);
	const CloudRaySegments segments = GetCloudRaySegments(planetPos, info.dirlightDir,
		high ? info.highCloudBottom : info.lowCloudBaseAltitude,
		high ? info.highCloudTop : info.lowCloudTraceTopAltitude, CLOUD_SKY_DISTANCE, info);
	if (segments.length <= 0.0)
		return 0.0;
	float cached;
	const float entryOffset = segments.nearSegment.x > 0.0 ? min(segments.nearLength * 0.5, 2.0 * GAME_UNITS_PER_METER) : 0.0;
	const float3 entry = pos + info.dirlightDir * (segments.nearSegment.x + entryOffset);
	const float weight = SampleCloudLightCache(entry, high, cached);
	if (weight >= 1.0)
		return cached;
	return lerp(CloudColumnOpticalDepth(pos, info.dirlightDir, high, 4u, CLOUD_SKY_DISTANCE), cached, weight);
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
		opticalDepth += SampleCloudLightDensity(samplePos, high, 1.0) * (distance - previousDistance) * GAME_UNIT_TO_M;
		previousDistance = distance;
	}
	return opticalDepth + CloudCachedSun(pos + info.dirlightDir * extent, high);
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
	const float2 offset = CloudCachePosition(xy) * (0.5 * CloudCacheRange(high));
	const float height = lerp(high ? info.highCloudBottom : info.lowCloudBaseAltitude,
		high ? info.highCloudTop : info.lowCloudTraceTopAltitude, (tid.z + 0.5) / dims.z);
	const float radius = info.planetRadius + height;
	const float2 worldXY = info.lightCacheOrigin + offset + info.lightCacheWindDelta;
	const float2 planetXY = worldXY - FrameBuffer::CameraPosAdjust.xy;
	const float3 pos = float3(worldXY, sqrt(max(radius * radius - dot(planetXY, planetXY), 0.0)) - info.planetRadius);
	const float sun = CloudColumnOpticalDepth(pos, info.dirlightDir, high, info.lightCacheSteps, CLOUD_SKY_DISTANCE);
	RWCloudLightCache[tid] = min(sun, 65504.0);
}

float CloudScatteringPhase(float cosine)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	return Phase::HG(cosine, info.phaseForwardG) * info.phaseForwardWeight +
	       Phase::HG(cosine, info.phaseBackwardG) * info.phaseBackwardWeight;
}

float2 CloudLightResponse(float cosine, float height, float profile, float lightDensity, float product, float occlusion)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float opticalDepth = info.sunExtinction * occlusion;
	const float threshold = (1.0 - saturate(cosine)) * 0.1;
	const float erodedProfile = saturate((profile - 0.05) * 1.0526316);
	const float potential = saturate((erodedProfile - threshold) / (1.0 - threshold));
	const float potential6 = potential * 6.0;
	const float density6 = saturate((lightDensity - 0.1) * 1.1111112) * 6.0;
	const float productPower = pow(product, 0.8);
	const float softTransmission = 1.0 - saturate(opticalDepth * 0.025);
	const float bottomResponse = saturate(height * 10.0) * 0.5 + 0.5;
	const float bottomSquared = bottomResponse * bottomResponse;
	const float lowerFade = saturate((height - 0.1) * 10.0);
	const float powderHeight = saturate((height - 0.1) * 3.3333333) * 2.0;
	const float powderOffset = powderHeight - 2.0 + (-2.0 - powderHeight) * saturate(occlusion * 0.5);
	const float powderBase = sqrt(saturate(1.0 - potential6)) * (density6 * density6 - potential6) + potential6;
	const float powderDensity = pow(max(powderBase, 0.0), 3.0 - lowerFade);
	const float broadTransmission = exp(-opticalDepth * 0.03);
	const float ambientHeight = pow(lerp(info.ambientBase, 1.0, height), pow(product, 0.3) * 1.8 + 0.2);
	float ambient = ambientHeight * 1.68 * (1.0 - sqrt(product));
	ambient *= lerp(pow(height, 0.25), 1.0, broadTransmission);
	ambient *= pow(saturate(1.0 - erodedProfile), info.ambientDensity * 7.8 + 0.2);
	ambient = max(info.ambientFloor, ambient) * info.ambientStrength;
	const float volume = info.scatterVolumeStrength * 8.0 * pow(max(height, 1e-8), info.scatterVolumeHeight) * (1.0 - saturate((cosine - 0.5) * 2.0408163) * 0.75) * saturate(potential * 6.666667 - 0.11111112) * pow(product, 0.25) * exp(-opticalDepth * info.scatterVolumeDepth);
	const float soft = info.softScatteringStrength * 42.375 * (potential + lightDensity) * ((pow(softTransmission, 8.0 - productPower * 7.0) + pow(softTransmission, 16.0 - productPower * 14.0)) * 0.3 + pow(softTransmission, 32.0 - productPower * 28.0)) * lerp(bottomSquared, 1.0, saturate((cosine - 0.25) * 4.0));
	const float powder = saturate((saturate((powderDensity - powderOffset + saturate((cosine - 0.3) * 1.4306152) * (1.0 - powderDensity)) / (1.0 - powderOffset)) + 0.333) * 0.7501876);
	return float2((volume + soft) * 1.5 * lerp(1.0, powder, info.powderStrength), ambient);
}

float3 CloudLighting(float3 pos, float3 viewDir, float height, float profile, float product, float extinction,
	bool high, float phase, float3 ambientColor)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float occlusion = CloudSunOpticalDepth(pos, viewDir, height, high);
	const float2 response = CloudLightResponse(dot(viewDir, info.dirlightDir), saturate(height), saturate(profile),
		CloudLightDensity(extinction, height), saturate(product), occlusion);
	const float otherOcclusion = info.crossLayerShadows != 0u && info.highCloudEnabled > 0.0 ? CloudCachedSun(pos, !high) : 0.0;
	const float3 sunlight = sampleExternalSunTransmittance(pos, info.dirlightDir) * GetCloudDirectionalLightColor();
	return info.lightingScale * (response.x * phase * exp(-otherOcclusion * info.sunExtinction) * sunlight + response.y * ambientColor);
}

#endif
