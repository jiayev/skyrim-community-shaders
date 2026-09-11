#ifndef PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI
#define PHYSICAL_SKY_CLOUD_LIGHTING_HLSLI

float CloudLightDensity(float extinction, float height)
{
	const float h2 = height * height;
	return extinction * (1.0 + 3.0 * h2 * h2);
}

float SampleCloudLightDensity(float3 pos, float mip)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	CloudDensityContext context;
	const float extinction = sampleCloudDensity(pos, GetCloudLayer(info), mip, false, context);
	return CloudLightDensity(extinction, context.ndf.height_fraction);
}

float CloudLightProbeJitter(float2 positionMeters)
{
	const float2 seed = frac(positionMeters * 0.1031);
	const float scramble = dot(seed.xyx, seed.yxx + 19.19);
	return frac((seed.y + seed.x + scramble * 2.0) * (scramble + seed.x));
}

float CloudLocalSunOcclusion(float3 pos, float3 viewDir, float height)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float viewDistance = length(pos - eye) * GAME_UNIT_TO_M;
	const uint count = uint(10.0 - saturate((viewDistance - 512.0) * 0.00040192925) * 6.0);
	const float extentMeters = 240.0 - saturate(height * 3.3333333) * 120.0;
	const float spacingMeters = extentMeters / count;
	const float cosine = dot(viewDir, info.dirlightDir);
	const float forward = saturate(cosine);
	const float angularOffset = -0.25 * cosine;
	const float jitter = CloudLightProbeJitter(pos.xy * GAME_UNIT_TO_M);
	float occlusion = 0.0;
	[loop] for (uint i = 0u; i < count; ++i)
	{
		const float index = i + 0.5;
		const float fraction = index / count;
		const float angularScale = angularOffset + 0.5 + fraction * fraction * fraction * (0.5 - angularOffset);
		const float distanceMeters = (fraction * extentMeters + jitter * spacingMeters) * angularScale;
		const float3 samplePos = pos + info.dirlightDir * (distanceMeters * GAME_UNITS_PER_METER);
		const float weightMeters = (forward + 1.0) * spacingMeters * 0.45 * (index * 0.4 * forward + 1.0);
		occlusion += SampleCloudLightDensity(samplePos, 2.0) * weightMeters;
	}
	return occlusion;
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
	float phase, float3 ambientColor)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float occlusion = CloudLocalSunOcclusion(pos, viewDir, height);
	const float2 response = CloudLightResponse(dot(viewDir, info.dirlightDir), saturate(height), saturate(profile),
		CloudLightDensity(extinction, height), saturate(product), occlusion);
	const float3 sunlight = sampleExternalSunTransmittance(pos, info.dirlightDir) * GetCloudDirectionalLightColor();
	return info.lightingScale * (response.x * phase * sunlight + response.y * ambientColor);
}

#endif
