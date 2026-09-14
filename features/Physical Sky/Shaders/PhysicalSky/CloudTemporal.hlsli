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
	RWTexAux[tid] = float4(EncodeCloudDepth(result.cloud_depth), EncodeCloudDepth(linearDepth), 0.0, 0.0);
}

	[numthreads(8, 8, 1)] void reproject(uint2 pixel : SV_DispatchThreadID)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(pixel >= uint2(info.activeFrameDim)))
		return;
	const float2 uv = CloudScreenUv(pixel);
	uint2 traceDimensions;
	TexVolLowAux.GetDimensions(traceDimensions.x, traceDimensions.y);
	const float2 tracePixel = clamp((float2(pixel) - CloudPhaseOffset()) * 0.25 + 0.5, 0.5, info.lowFrameDim - 0.5);
	const float depth = TexVolLowAux.SampleLevel(TransmittanceSampler, tracePixel / traceDimensions, 0).x;
	const float3 ray = CloudScreenRay(pixel);
	const float3 previousPosition = ray * DecodeCloudDepth(depth) +
	                                FrameBuffer::CameraPosAdjust.xyz - info.previousCamera - float3(info.cloudWindDelta, 0);
	const float4 clip = mul(info.previousViewProj, float4(previousPosition, 1));
	const float2 previousUv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
	CloudTemporalSample result;
	bool useHistory = info.historyValid != 0u && clip.w > 0.0 && all(previousUv == saturate(previousUv));
	if (useHistory) {
		result = ReconstructCloud(TexVolHistoryLum, TexVolHistoryAux,
			previousUv * info.previousFrameDim - 0.5, int2(info.previousFrameDim) - 1, false);
		const bool traced = all((pixel & 3u) == CloudPhaseOffset());
		if (traced) {
			const float4 color = TexVolLowLum[pixel / 4u];
			const float weight = CloudHistoryBlend(previousUv - uv);
			result.color = lerp(result.color, color, weight);
			result.metadata = lerp(result.metadata, TexVolLowAux[pixel / 4u], weight);
		} else {
			useHistory = abs(EncodeCloudDepth(SharedData::GetScreenDepth(CloudSceneDeviceDepth(pixel, 2u))) - result.metadata.y) <= 0.064;
		}
		if (useHistory && result.color.a > 0.0)
			useHistory = CloudFootprintHasCloud(TexVolLowLum,
				(float2(pixel) - CloudPhaseOffset()) * 0.25, int2(info.lowFrameDim) - 1);
	}
	if (!useHistory)
		result = ReconstructCloud(TexVolLowLum, TexVolLowAux,
			(float2(pixel) - CloudPhaseOffset()) * 0.25, int2(info.lowFrameDim) - 1, true);
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
	RWTexCubeAux[tid] = float4(EncodeCloudDepth(result.cloud_depth), 0.0, 0.0, 0.0);
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
	const float depth = TexCubeTraceAux.SampleLevel(TransmittanceSampler, depthDirection, 0).x;
	const float3 previousPosition = direction * DecodeCloudDepth(depth) + FrameBuffer::CameraPosAdjust.xyz -
	                                info.previousCamera - float3(info.cloudWindDelta, 0);
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
		if (all((tid.xy & 3u) == CloudPhaseOffset())) {
			const float3 traceDirection = CloudCubeDirection(tid.xy / 4u, tid.z, dims.x / 4u);
			const float4 color = TexCubeTraceLum.SampleLevel(TransmittanceSampler, traceDirection, 0);
			const float2 motion = CloudCubePosition(previousDirection, tid.z) - (float2(tid.xy) + 0.5) / dims.x;
			const float weight = CloudHistoryBlend(motion);
			result.color = lerp(result.color, color, weight);
			result.metadata = lerp(result.metadata, TexCubeTraceAux.SampleLevel(TransmittanceSampler, traceDirection, 0), weight);
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
