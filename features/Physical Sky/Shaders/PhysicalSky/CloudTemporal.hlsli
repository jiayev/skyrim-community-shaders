#ifndef PHYSICAL_SKY_CLOUD_TEMPORAL_HLSLI
#define PHYSICAL_SKY_CLOUD_TEMPORAL_HLSLI

#include "PhysicalSky/CloudPhase.hlsli"

uint2 CloudPhaseOffset()
{
	return CloudPhaseOffset(VolumetricCloudBuffer[0].cloudFrameIndex);
}

float2 CloudScreenUv(uint2 pixel)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	return FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((pixel + 0.5) * info.rcpFrameDim);
}

float3 CloudScreenRay(uint2 pixel)
{
	const float2 uv = CloudScreenUv(pixel);
	const float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), 1, 1));
	return normalize(position.xyz / position.w);
}

float CloudSceneDeviceDepth(uint2 pixel, uint footprint)
{
	const uint2 origin = (pixel / footprint) * footprint;
	const uint2 maximum = uint2(VolumetricCloudBuffer[0].activeFrameDim) - 1u;
	float depth = 0.0;
	[unroll] for (uint y = 0u; y < footprint; ++y)
		[unroll] for (uint x = 0u; x < footprint; ++x)
			depth = max(depth, TexDepth[min(origin + uint2(x, y), maximum)]);
	return depth;
}

float CloudHistoryBlend(float2 motion)
{
	return saturate((length(motion) - 0.0001) * 2500.0) * 0.5 + 0.5;
}

float3 CloudSampleDisplacement(float3 ray, float4 metadata, float opacity, out float uncertainty)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float cirrusFraction = saturate(metadata.z / max(opacity, 1e-6));
	if (cirrusFraction == 1.0) {
		uncertainty = 0.0;
		return float3(info.cirrusWindDelta, 0);
	}
	const float height = saturate(metadata.w / max(opacity - metadata.z, 1e-6));
	float lowDepth = DecodeCloudDepth(metadata.x);
	if (cirrusFraction > 0.0 && cirrusFraction < 0.99) {
		const float3 planetEye = float3(0, 0, FrameBuffer::CameraPosAdjust.z - info.bottomZ + info.planetRadius);
		const float2 shell = IntersectSpherePair(planetEye, ray, info.planetRadius + info.cirrusAltitude);
		const float cirrusDepth = shell.x >= 0.0 ? shell.x : shell.y;
		if (cirrusDepth >= 0.0)
			lowDepth = max((lowDepth - cirrusFraction * cirrusDepth) / (1.0 - cirrusFraction), 0.0);
	}
	const float2 position = (FrameBuffer::CameraPosAdjust.xy + ray.xy * lowDepth - info.noiseWindOffset) * GAME_UNIT_TO_M;
	const float2 shearChange = (info.cloudShapeShear - info.previousShapeShear) * smoothstep(0.0, 1.0, height);
	const float2 fieldDelta = CloudPreviousFieldOffset(position, -shearChange * GAME_UNIT_TO_M, info.cloudEvolution);
	const float3 lowDisplacement = float3(info.cloudWindDelta - fieldDelta * GAME_UNITS_PER_METER,
		info.cloudEvolutionDelta * (4.0 * height * (1.0 - height)));
	const float3 highDisplacement = float3(info.cirrusWindDelta, 0);
	const float3 disagreement = lowDisplacement - highDisplacement;
	const float transverseDisagreement = length(disagreement - ray * dot(disagreement, ray));
	uncertainty = (1.0 - cirrusFraction) * (abs(info.cloudEvolutionDelta) + length(shearChange) * 60.0) +
	              cirrusFraction * (1.0 - cirrusFraction) * transverseDisagreement;
	return lerp(lowDisplacement, highDisplacement, cirrusFraction);
}

float CloudEvolutionBlend(float uncertainty, float depth, float pixelsPerRadian)
{
	const float cycleError = uncertainty * pixelsPerRadian * 16.0 / max(DecodeCloudDepth(depth), 1.0);
	return saturate(cycleError - 0.5) * 0.5;
}

struct CloudTemporalSample
{
	float4 color;
	float4 metadata;
};

CloudTemporalSample ReconstructCloudSamples(float4 colors[4], float4 metadata[4], float2 fraction, bool reweightEmpty)
{
	float4 weights = float4((1.0 - fraction.x) * (1.0 - fraction.y), fraction.x * (1.0 - fraction.y),
		(1.0 - fraction.x) * fraction.y, fraction.x * fraction.y);
	if (reweightEmpty) {
		[unroll] for (uint i = 0u; i < 4u; ++i)
			weights[i] = colors[i].a == 0.0 ? 0.01 : weights[i];
		weights /= dot(weights, float4(1, 1, 1, 1));
	}
	CloudTemporalSample result = (CloudTemporalSample)0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		result.color += colors[i] * weights[i];
		result.metadata += metadata[i] * weights[i];
	}
	return result;
}

CloudTemporalSample ReconstructCloud(Texture2D<float4> color, Texture2D<float4> depth,
	float2 position, int2 maximum, bool reweightEmpty)
{
	const int2 base = int2(floor(position));
	float4 colors[4];
	float4 metadata[4];
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const int2 pixel = clamp(base + int2(i & 1u, i >> 1u), 0, maximum);
		colors[i] = color[pixel];
		metadata[i] = depth[pixel];
	}
	return ReconstructCloudSamples(colors, metadata, saturate(position - floor(position)), reweightEmpty);
}

bool CloudFootprintHasCloud(Texture2D<float4> color, float2 position, int2 maximum)
{
	const int2 base = int2(floor(position));
	float opacity = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
		opacity = max(opacity, color[clamp(base + int2(i & 1u, i >> 1u), 0, maximum)].a);
	return opacity > 0.0;
}

bool ReconstructCloudSurface(float2 position, float sceneDepth, out CloudTemporalSample result, out float opacity)
{
	const int2 base = int2(floor(position));
	const float2 fraction = saturate(position - floor(position));
	const int2 maximum = int2(VolumetricCloudBuffer[0].lowFrameDim) - 1;
	result = (CloudTemporalSample)0;
	opacity = 0.0;
	CloudTemporalSample nearest = (CloudTemporalSample)0;
	float nearestDistance = 4.0;
	float weightSum = 0.0;
	bool found = false;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const int2 pixel = clamp(base + int2(i & 1u, i >> 1u), 0, maximum);
		const float4 metadata = TexVolLowAux[pixel];
		if (abs(metadata.y - sceneDepth) > 0.064)
			continue;
		const float4 color = TexVolLowLum[pixel];
		const float weight = color.a == 0.0 ? 0.01 :
		                                      ((i & 1u) != 0u ? fraction.x : 1.0 - fraction.x) * (i >= 2u ? fraction.y : 1.0 - fraction.y);
		result.color += color * weight;
		result.metadata += metadata * weight;
		weightSum += weight;
		opacity = max(opacity, color.a);
		const float2 offset = float2(pixel) - position;
		const float distance = dot(offset, offset);
		if (!found || distance < nearestDistance) {
			nearest.color = color;
			nearest.metadata = metadata;
			nearestDistance = distance;
		}
		found = true;
	}
	if (weightSum > 0.0) {
		result.color /= weightSum;
		result.metadata /= weightSum;
	} else {
		result = nearest;
	}
	return found;
}

float4 LoadCloudBoundary(uint2 pixel, float3 ray)
{
	const float4 rect = VolumetricCloudBuffer[0].ndfBoundaryRect;
	const float2 camera = FrameBuffer::CameraPosAdjust.xy;
	const float empty = asfloat(0x7f7fffffu);
	const float exitX = abs(ray.x) > 1e-8 ? ((ray.x > 0.0 ? rect.z : rect.x) - camera.x) / ray.x : empty;
	const float exitY = abs(ray.y) > 1e-8 ? ((ray.y > 0.0 ? rect.w : rect.y) - camera.y) / ray.y : empty;
	const float3 depths = TexCloudBoundary.Load(int3(pixel, 0)).xyz;
	const float viewCosine = max(abs(mul(FrameBuffer::CameraView, float4(ray, 0)).z), 1e-6);
	return float4(depths.x != empty ? DecodeCloudDepth(depths.x) / viewCosine : 0.0,
		DecodeCloudDepth(-depths.y) / viewCosine,
		depths.z != empty ? DecodeCloudDepth(depths.z) / viewCosine : 0.0,
		max(min(exitX, exitY), 0.0));
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(tid >= uint2(info.lowFrameDim)))
		return;
	const uint2 pixel = tid * 4u + CloudPhaseOffset();
	const float3 ray = CloudScreenRay(pixel);
	const float deviceDepth = CloudSceneDeviceDepth(pixel, 4u);
	const float linearDepth = SharedData::GetScreenDepth(deviceDepth);
	const float sceneDistance = deviceDepth >= 1.0 - 1e-6 ? 0.0 :
	                                                        linearDepth / max(abs(mul(FrameBuffer::CameraView, float4(ray, 0)).z), 1e-6);
	const float3 eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float2 jitter = float2(CloudTemporalMarchHash(pixel, info.cloudFrameIndex), CloudSpatialMarchHash(pixel));
	const VolumetricCloudResult result = RenderVolumetricCloudRay(ray, eye, sceneDistance, jitter, SampleCloudApShadow(pixel), LoadCloudBoundary(tid, ray));
	RWTexTr[tid] = result.transmittance.x;
	RWTexLum[tid] = float4(min(result.lum, 65504.0), 1.0 - result.transmittance.x);
	RWTexAux[tid] = float4(EncodeCloudDepth(result.cloud_depth), EncodeCloudDepth(linearDepth), result.motionMetadata * (1.0 - result.transmittance.x));
}

	[numthreads(8, 8, 1)] void reproject(uint2 pixel : SV_DispatchThreadID)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(pixel >= uint2(info.activeFrameDim)))
		return;
	const float2 uv = CloudScreenUv(pixel);
	const float sceneDepth = EncodeCloudDepth(SharedData::GetScreenDepth(CloudSceneDeviceDepth(pixel, 1u)));
	const float2 currentPosition = (float2(pixel) - CloudPhaseOffset()) * 0.25;
	CloudTemporalSample current;
	float currentOpacity;
	const bool currentValid = ReconstructCloudSurface(currentPosition, sceneDepth, current, currentOpacity);
	uint2 traceDimensions;
	TexVolLowAux.GetDimensions(traceDimensions.x, traceDimensions.y);
	const float2 tracePixel = clamp((float2(pixel) - CloudPhaseOffset()) * 0.25 + 0.5, 0.5, info.lowFrameDim - 0.5);
	float4 motionMetadata = TexVolLowAux.SampleLevel(TransmittanceSampler, tracePixel / traceDimensions, 0);
	float motionOpacity;
	if (currentValid) {
		motionMetadata.zw = current.metadata.zw;
		motionOpacity = current.color.a;
	} else {
		motionOpacity = TexVolLowLum.SampleLevel(TransmittanceSampler, tracePixel / traceDimensions, 0).a;
	}
	const float depth = motionMetadata.x;
	const float3 ray = CloudScreenRay(pixel);
	float uncertainty;
	const float3 displacement = CloudSampleDisplacement(ray, motionMetadata, motionOpacity, uncertainty);
	const float2 projectionScale = abs(float2(FrameBuffer::CameraProjUnjittered[0][0], FrameBuffer::CameraProjUnjittered[1][1])) * info.activeFrameDim * 0.5;
	const float viewCosine = abs(mul(FrameBuffer::CameraView, float4(ray, 0)).z);
	const float evolutionBlend = CloudEvolutionBlend(uncertainty, depth, max(projectionScale.x, projectionScale.y) / max(viewCosine * viewCosine, 1e-4));
	const float3 previousPosition = ray * DecodeCloudDepth(depth) +
	                                FrameBuffer::CameraPosAdjust.xyz - info.previousCamera - displacement;
	const float4 clip = mul(info.previousViewProj, float4(previousPosition, 1));
	const float2 previousUv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
	CloudTemporalSample result;
	bool useHistory = info.historyValid != 0u && clip.w > 0.0 && all(previousUv == saturate(previousUv));
	if (useHistory) {
		result = ReconstructCloud(TexVolHistoryLum, TexVolHistoryAux,
			previousUv * info.previousFrameDim - 0.5, int2(info.previousFrameDim) - 1, false);
		useHistory = abs(sceneDepth - result.metadata.y) <= 0.064;
		const bool traced = all((pixel & 3u) == CloudPhaseOffset());
		if (useHistory && traced) {
			const float4 metadata = TexVolLowAux[pixel / 4u];
			const bool sampleValid = abs(metadata.y - sceneDepth) <= 0.064;
			if (sampleValid || currentValid) {
				const float weight = max(CloudHistoryBlend(previousUv - uv), evolutionBlend * 2.0);
				result.color = lerp(result.color, sampleValid ? TexVolLowLum[pixel / 4u] : current.color, weight);
				result.metadata = lerp(result.metadata, sampleValid ? metadata : current.metadata, weight);
			}
		}
		if (useHistory && currentValid && evolutionBlend > 0.0 && !traced) {
			result.color = lerp(result.color, current.color, evolutionBlend);
			result.metadata = lerp(result.metadata, current.metadata, evolutionBlend);
		}
		if (useHistory && result.color.a > 0.0)
			useHistory = currentValid ? currentOpacity > 0.0 :
			                            CloudFootprintHasCloud(TexVolLowLum, currentPosition, int2(info.lowFrameDim) - 1);
	}
	if (!useHistory) {
		if (currentValid)
			result = current;
		else
			result = ReconstructCloud(TexVolLowLum, TexVolLowAux, currentPosition, int2(info.lowFrameDim) - 1, true);
	}
	if (useHistory || currentValid)
		result.metadata.y = sceneDepth;
	RWTexTr[pixel] = saturate(1.0 - result.color.a);
	RWTexLum[pixel] = float4(clamp(result.color.rgb, 0.0, 65504.0), saturate(result.color.a));
	RWTexAux[pixel] = result.metadata;
}

float3 CloudCubeDirection(float2 pixel, uint face, uint size)
{
	const float2 uv = (pixel + 0.5) / size * float2(2, -2) + float2(-1, 1);
	float3 direction;
	switch (face) {
	case 0:
		direction = float3(1, uv.y, -uv.x);
		break;
	case 1:
		direction = float3(-1, uv.y, uv.x);
		break;
	case 2:
		direction = float3(uv.x, 1, -uv.y);
		break;
	case 3:
		direction = float3(uv.x, -1, uv.y);
		break;
	case 4:
		direction = float3(uv.x, uv.y, 1);
		break;
	default:
		direction = float3(-uv.x, uv.y, -1);
		break;
	}
	return normalize(direction);
}

CloudTemporalSample ReconstructCloudCube(TextureCube<float4> color, TextureCube<float4> depth,
	float2 position, uint face, uint size, bool reweightEmpty)
{
	const float2 base = floor(position);
	float4 colors[4];
	float4 metadata[4];
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const float2 pixel = clamp(base + float2(i & 1u, i >> 1u), 0.0, float(size - 1u));
		const float3 direction = CloudCubeDirection(pixel, face, size);
		colors[i] = color.SampleLevel(TransmittanceSampler, direction, 0);
		metadata[i] = depth.SampleLevel(TransmittanceSampler, direction, 0);
	}
	return ReconstructCloudSamples(colors, metadata, saturate(position - base), reweightEmpty);
}

bool CloudFootprintHasCloud(TextureCube<float4> color, float2 position, uint face, uint size)
{
	const float2 base = floor(position);
	float opacity = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const float2 pixel = clamp(base + float2(i & 1u, i >> 1u), 0.0, float(size - 1u));
		opacity = max(opacity, color.SampleLevel(TransmittanceSampler, CloudCubeDirection(pixel, face, size), 0).a);
	}
	return opacity > 0.0;
}

float2 CloudCubePosition(float3 direction, uint face)
{
	float2 position;
	float depth;
	switch (face) {
	case 0:
		position = float2(-direction.z, direction.y);
		depth = direction.x;
		break;
	case 1:
		position = float2(direction.z, direction.y);
		depth = -direction.x;
		break;
	case 2:
		position = float2(direction.x, -direction.z);
		depth = direction.y;
		break;
	case 3:
		position = float2(direction.x, direction.z);
		depth = -direction.y;
		break;
	case 4:
		position = direction.xy;
		depth = direction.z;
		break;
	default:
		position = float2(-direction.x, direction.y);
		depth = -direction.z;
		break;
	}
	position *= (depth >= 0.0 ? 1.0 : -1.0) / max(abs(depth), 1e-6);
	return position * float2(0.5, -0.5) + 0.5;
}

[numthreads(4, 4, 1)] void renderCubemap(uint3 tid : SV_DispatchThreadID) {
	uint3 dims;
	RWTexCubeTr.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims))
		return;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const uint2 pixel = tid.xy * 4u + CloudPhaseOffset();
	const float3 eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const uint2 seed = pixel + uint2(tid.z * dims.x * 4u, 0);
	const float2 jitter = float2(CloudTemporalMarchHash(seed, info.cloudFrameIndex), CloudSpatialMarchHash(seed));
	const VolumetricCloudResult result = RenderVolumetricCloudRay(CloudCubeDirection(pixel, tid.z, dims.x * 4u),
		eye, 0.0, jitter, 0.0, 0.0);
	RWTexCubeTr[tid] = result.transmittance.x;
	RWTexCubeLum[tid] = float4(min(result.lum, 65504.0), 1.0 - result.transmittance.x);
	RWTexCubeAux[tid] = float4(EncodeCloudDepth(result.cloud_depth), 0.0, result.motionMetadata * (1.0 - result.transmittance.x));
}

	[numthreads(8, 8, 1)] void reprojectCubemap(uint3 tid : SV_DispatchThreadID)
{
	uint3 dims;
	RWTexCubeTr.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims))
		return;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 direction = CloudCubeDirection(tid.xy, tid.z, dims.x);
	const float2 tracePixel = clamp((float2(tid.xy) - CloudPhaseOffset()) * 0.25, 0.0, float(dims.x / 4u - 1u));
	const float3 depthDirection = CloudCubeDirection(tracePixel, tid.z, dims.x / 4u);
	const float4 motionMetadata = TexCubeTraceAux.SampleLevel(TransmittanceSampler, depthDirection, 0);
	const float depth = motionMetadata.x;
	float uncertainty;
	const float3 displacement = CloudSampleDisplacement(direction, motionMetadata, TexCubeTraceLum.SampleLevel(TransmittanceSampler, depthDirection, 0).a, uncertainty);
	const float cubeCosine = max(max(abs(direction.x), abs(direction.y)), abs(direction.z));
	const float evolutionBlend = CloudEvolutionBlend(uncertainty, depth, 0.5 * dims.x / (cubeCosine * cubeCosine));
	const float3 previousPosition = direction * DecodeCloudDepth(depth) + FrameBuffer::CameraPosAdjust.xyz -
	                                info.previousCamera - displacement;
	const float3 previousDirection = normalize(previousPosition);
	const float3 absoluteDirection = abs(previousDirection);
	const uint face = absoluteDirection.x >= max(absoluteDirection.y, absoluteDirection.z) ? (previousDirection.x > 0 ? 0u : 1u) :
	                                                                                         (absoluteDirection.y >= absoluteDirection.z ? (previousDirection.y > 0 ? 2u : 3u) : (previousDirection.z > 0 ? 4u : 5u));
	const float2 historyUv = CloudCubePosition(previousDirection, face);
	CloudTemporalSample result;
	bool useHistory = info.historyValid != 0u && all(isfinite(previousDirection));
	if (useHistory) {
		result = ReconstructCloudCube(TexCubeHistoryLum, TexCubeHistoryAux,
			historyUv * dims.x - 0.5, face, dims.x, false);
		const bool traced = all((tid.xy & 3u) == CloudPhaseOffset());
		if (traced) {
			const float3 traceDirection = CloudCubeDirection(tid.xy / 4u, tid.z, dims.x / 4u);
			const float4 color = TexCubeTraceLum.SampleLevel(TransmittanceSampler, traceDirection, 0);
			const float2 motion = CloudCubePosition(previousDirection, tid.z) - (float2(tid.xy) + 0.5) / dims.x;
			const float weight = max(CloudHistoryBlend(motion), evolutionBlend * 2.0);
			result.color = lerp(result.color, color, weight);
			result.metadata = lerp(result.metadata, TexCubeTraceAux.SampleLevel(TransmittanceSampler, traceDirection, 0), weight);
		}
		if (!traced && evolutionBlend > 0.0) {
			const CloudTemporalSample current = ReconstructCloudCube(TexCubeTraceLum, TexCubeTraceAux,
				(float2(tid.xy) - CloudPhaseOffset()) * 0.25, tid.z, dims.x / 4u, true);
			result.color = lerp(result.color, current.color, evolutionBlend);
			result.metadata = lerp(result.metadata, current.metadata, evolutionBlend);
		}
		if (result.color.a > 0.0)
			useHistory = CloudFootprintHasCloud(TexCubeTraceLum,
				(float2(tid.xy) - CloudPhaseOffset()) * 0.25, tid.z, dims.x / 4u);
	}
	if (!useHistory) {
		result = ReconstructCloudCube(TexCubeTraceLum, TexCubeTraceAux,
			(float2(tid.xy) - CloudPhaseOffset()) * 0.25, tid.z, dims.x / 4u, true);
	}
	RWTexCubeTr[tid] = saturate(1.0 - result.color.a);
	RWTexCubeLum[tid] = float4(clamp(result.color.rgb, 0.0, 65504.0), saturate(result.color.a));
	RWTexCubeAux[tid] = result.metadata;
}

#endif
