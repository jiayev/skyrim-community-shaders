#ifndef PHYSICAL_SKY_CLOUD_VOXEL_VIEW_HLSLI
#define PHYSICAL_SKY_CLOUD_VOXEL_VIEW_HLSLI

#ifdef CLOUD_VOXEL_VIEW
cbuffer VoxelViewDispatch : register(b3) { uint4 VoxelScreenRect; };
Texture2D<float4> TexVoxelViewColor : register(t86);
Texture2D<float2> TexVoxelViewDepth : register(t87);
RWTexture2D<float4> RWVoxelViewColor : register(u0);
RWTexture2D<float2> RWVoxelViewDepth : register(u1);

// VoxelCloudComputeShader writes current-frame color and opacity-weighted
// distance. It does not feed the NDF 4x4 temporal reconstruction or blur.
[numthreads(8, 8, 1)] void renderVoxelView(uint2 pixel : SV_DispatchThreadID) {
	pixel += VoxelScreenRect.xy;
	if (any(pixel >= VoxelScreenRect.zw))
		return;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const uint2 screenPixel = min(pixel * 2u, uint2(info.activeFrameDim) - 1u);
	const float2 screenUv = FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((float2(pixel) + .5) * 2.0 * info.rcpFrameDim);
	const float4 viewPosition = mul(FrameBuffer::CameraViewProjInverse, float4(screenUv * float2(2, -2) + float2(-1, 1), 1, 1));
	const float3 direction = normalize(viewPosition.xyz / viewPosition.w);
	const float3 eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	// Native mode 0 alternates the minimum and maximum depth in the footprint.
	float minimumDepth = 1.0, maximumDepth = 0.0;
	[unroll] for (uint y = 0; y < 2; ++y)
		[unroll] for (uint x = 0; x < 2; ++x)
	{
		const float depth = TexDepth[min(screenPixel + uint2(x, y), uint2(info.activeFrameDim) - 1u)];
		minimumDepth = min(minimumDepth, depth);
		maximumDepth = max(maximumDepth, depth);
	}
	const float deviceDepth = ((pixel.x ^ pixel.y) & 1u) == 0u ? maximumDepth : minimumDepth;
	const float sceneDistance = deviceDepth >= 1.0 - 1e-6 ? info.rayMarchRange :
	                                                        SharedData::GetScreenDepth(deviceDepth) / max(abs(mul(FrameBuffer::CameraView, float4(direction, 0)).z), 1e-6);
	const float2 jitter = float2(CloudTemporalMarchHash(pixel, info.cloudFrameIndex), CloudSpatialMarchHash(pixel));
	const float phase = CloudLowPhase(dot(direction, info.dirlightDir));
	float3 radiance = 0.0;
	float transmission = 1.0, depthSum = 0.0, weightSum = 0.0;
	float cursor = 0.0;
	// Selecting the next box also skips gaps between independent placements.
	CLOUD_VOXEL_INSTANCE_LOOP for (uint region = 0u; region < CloudVoxelCount() && transmission > .1; ++region)
	{
		const float2 interval = CloudVoxelInterval(eye, direction, cursor, true);
		const float end = min(interval.y, sceneDistance);
		if (end <= interval.x)
			break;
		float distance = interval.x;
		[loop] while (distance < end && transmission > .1)
		{
			uint2 occupied;
			const float step = CloudVoxelStep(eye + direction * distance, distance, 0.0, occupied);
			if (any(occupied != 0u)) {
				const float metres = distance * GAME_UNIT_TO_M;
				const float3 position = eye + direction * (distance + step * (metres < 250.0 ? jitter.x : jitter.y));
				CloudDensityContext context = (CloudDensityContext)0;
				const float density = AddVoxelDensity(position, 0.0, metres, 0.0, occupied, 0.0, context);
				if (density > 1e-10) {
					context.ndf.voxelBlend = 1.0;
					const float depthWeight = CloudDepthWeight(transmission, density, distance);
					depthSum += depthWeight * distance;
					weightSum += depthWeight;
					const float opticalDepth = step * GAME_UNIT_TO_M * density;
					const float stepTransmission = exp(-opticalDepth);
					const float opacity = CloudOpacity(opticalDepth);
					float3 source = CloudLighting(position, direction, context.ndf, density, opticalDepth, opacity, metres, 0.0, phase);
					source += opacity * Lightning::ActiveMask(Lightning::CloudMask(position + float3(0, 0, info.bottomZ), context.ndf.height_fraction, context.ndf.dimension_profile)) * Lightning::Color();
					if (info.ambientStrength > 0.0) {
						const float2 material = CloudMaterialAmbient(context.ndf);
						source += opacity * material.y * info.ambientStrength * info.cloudLightingScale * CloudAmbientTransport::ViewSource(position);
					}
					// Native NVDF integrates aerial perspective at each occupied step.
					if (!ViewAtmosphere::Enabled()) {
						const float4 air = SampleCloudAerialPerspective(direction, distance, 0.0);
						source = source * air.a + air.rgb * opacity;
					}
					radiance += transmission * source;
					transmission *= stepTransmission;
				}
			}
			distance += step;
		}
		cursor = interval.y;
	}
	const float opacity = 1.0 - saturate((transmission - .1) / .9);
	RWVoxelViewColor[pixel] = float4(min(radiance, 65504.0), opacity);
	RWVoxelViewDepth[pixel] = float2(weightSum > 0.0 ? EncodeCloudDepth(depthSum / weightSum) * opacity : 0.0,
		EncodeCloudDepth(sceneDistance));
}

	// HFW's composite reconstructs NVDF with scene-depth-weighted bilinear taps.
	// Keep the premultiplied distance until after reconstruction, just like color.
	[numthreads(8, 8, 1)] void compositeVoxelView(uint2 pixel : SV_DispatchThreadID)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(pixel >= uint2(info.activeFrameDim)))
		return;
	const float ndfTransmission = TexCloudResultTr[pixel];
	const float4 ndfColor = TexCloudResultLum[pixel];
	const float2 position = (float2(pixel) + .5) * .5 - .5;
	const int2 base = int2(floor(position));
	const float2 fraction = frac(position);
	const float4 bilinear = float4((1.0 - fraction.x) * (1.0 - fraction.y), fraction.x * (1.0 - fraction.y),
		(1.0 - fraction.x) * fraction.y, fraction.x * fraction.y);
	const float3 ray = CloudScreenRay(pixel);
	const float deviceDepth = TexDepth[pixel];
	const float sceneDistance = deviceDepth >= 1.0 - 1e-6 ? info.rayMarchRange :
	                                                        SharedData::GetScreenDepth(deviceDepth) / max(abs(mul(FrameBuffer::CameraView, float4(ray, 0)).z), 1e-6);
	const float metres = sceneDistance * GAME_UNIT_TO_M;
	const float tolerance = max((saturate((metres - 150.0) * .02) * .05 + .05) * metres, 1.0);
	float4 color = 0.0;
	float depth = 0.0, weights = 0.0;
	[unroll] for (uint tap = 0; tap < 4; ++tap)
	{
		const int2 samplePixel = clamp(base + int2(tap & 1u, tap >> 1u), 0, int2((uint2(info.activeFrameDim) + 1u) / 2u) - 1);
		// Outside the current dispatch is empty, never stale previous-frame data.
		float4 value = 0.0;
		float2 metadata = float2(0.0, EncodeCloudDepth(sceneDistance));
		if (all(samplePixel >= int2(VoxelScreenRect.xy)) && all(samplePixel < int2(VoxelScreenRect.zw))) {
			value = TexVoxelViewColor[samplePixel];
			metadata = TexVoxelViewDepth[samplePixel];
		}
		const float difference = abs(metadata.y * 1000.0 - metres);
		const float weight = bilinear[tap] * (1.0 - .999 * saturate((difference - tolerance) / tolerance));
		color += value * weight;
		depth += metadata.x * weight;
		weights += weight;
	}
	color /= weights;
	depth /= weights;
	const float cloudDepth = color.a > 0.0 ? depth / color.a : 0.0;
	if (EncodeCloudDepth(sceneDistance) < cloudDepth)
		color = 0.0;
	const float voxelTransmission = 1.0 - saturate(color.a);
	// NVDF is the local foreground integral, composited over the distant NDF
	// result. Mean cloud distances describe the integral; they are not surfaces
	// and must not switch the order of two entire translucent layers.
	float4 metadata = TexCloudResultAux[pixel];
	const float3 radiance = color.rgb + voxelTransmission * ndfColor.rgb;
	const float transmission = ndfTransmission * voxelTransmission;
	RWTexFilteredTr[pixel] = transmission;
	RWTexFilteredLum[pixel] = float4(min(radiance, 65504.0), 1.0 - transmission);
	// The atmosphere and transparent-surface composite need the visible cloud
	// distance, including pixels whose only cloud is the local voxel volume.
	const float voxelWeight = color.a;
	const float ndfWeight = (1.0 - ndfTransmission) * voxelTransmission;
	metadata.x = voxelWeight + ndfWeight > 0.0 ?
	                 (cloudDepth * voxelWeight + metadata.x * ndfWeight) / (voxelWeight + ndfWeight) :
	                 metadata.x;
	RWTexFilteredAux[pixel] = metadata;
}
#endif
#endif
