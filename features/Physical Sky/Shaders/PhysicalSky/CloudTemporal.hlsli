#ifndef PHYSICAL_SKY_CLOUD_TEMPORAL_HLSLI
#define PHYSICAL_SKY_CLOUD_TEMPORAL_HLSLI

uint2 CloudPhaseOffset()
{
	static const uint order[16] = { 0u, 10u, 2u, 8u, 5u, 15u, 7u, 13u, 1u, 11u, 3u, 9u, 4u, 14u, 6u, 12u };
	const uint phase = order[VolumetricCloudBuffer[0].cloudFrameIndex & 15u];
	return uint2(phase & 3u, phase >> 2u);
}

float2 CloudScreenUv(uint2 pixel)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	return FrameBuffer::GetDynamicResolutionUnadjustedScreenPosition((pixel + 0.5) * info.rcpFrameDim);
}

float3 CloudScreenRay(uint2 pixel)
{
	const float2 uv = CloudScreenUv(pixel);
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), 1, 1));
	return normalize(position.xyz / position.w);
}

float ReconstructSceneRayDistance(uint2 pixel)
{
	const float depth = TexDepth[pixel];
	if (depth >= 1.0 - 1e-6)
		return CLOUD_SKY_DISTANCE;
	const float2 uv = CloudScreenUv(pixel);
	float4 position = mul(FrameBuffer::CameraViewProjInverse, float4(uv * float2(2, -2) + float2(-1, 1), depth, 1));
	return min(length(position.xyz / position.w), CLOUD_SKY_DISTANCE * 0.99);
}

bool CloudDepthCompatible(float a, float b)
{
	const bool skyA = a >= CLOUD_SKY_DEPTH_KM * 0.999;
	const bool skyB = b >= CLOUD_SKY_DEPTH_KM * 0.999;
	return skyA == skyB && (skyA || abs(a - b) <= max(0.01, b * 0.1));
}

float CloudHistoryBlend(float3 history, float3 current, float2 motion)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float difference = length(history - current) / max(max(length(history), length(current)), 0.1);
	const float colorWeight = 1.0 - (1.0 - saturate(sqrt(difference))) * 0.8;
	const float motionWeight = saturate((length(motion) - 0.0001) * 2500.0) * 0.5 + 0.5;
	return lerp(1.0, max(colorWeight, motionWeight), info.temporalAccumulationFactor);
}

[numthreads(8, 8, 1)] void main(uint2 tid : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(tid >= uint2(info.lowFrameDim)))
		return;
	const uint2 pixel = min(tid * 4u + CloudPhaseOffset(), uint2(info.activeFrameDim) - 1u);
	const float sceneDistance = ReconstructSceneRayDistance(pixel);
	const float3 eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const float2 jitter = float2(CloudRayJitter(pixel, info.cloudFrameIndex >> 4u), CloudRayJitter(pixel, 0u));
	const VolumetricCloudResult result = RenderVolumetricCloudRay(CloudScreenRay(pixel), eye,
		sceneDistance, sceneDistance >= CLOUD_SKY_DISTANCE, jitter, SampleCloudApShadow(pixel));
	RWTexTr[tid] = result.transmittance.x;
	RWTexLum[tid] = min(result.lum, 65504.0);
	RWTexAux[tid] = float4(EncodeCloudDepth(result.cloud_depth), EncodeCloudDepth(sceneDistance), 1.0, result.high_fraction);
}

bool CloudTraceFallback(uint2 pixel, float sceneDepth, out float3 tr, out float3 lum, out float4 aux)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float2 position = (float2(pixel) - CloudPhaseOffset()) * 0.25;
	const int2 base = int2(floor(position));
	const float2 fraction = frac(position);
	float weightSum = 0.0;
	float depthWeight = 0.0;
	float depthSum = 0.0;
	tr = 0.0;
	lum = 0.0;
	aux = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const uint2 corner = uint2(i & 1u, i >> 1u);
		const int2 tap = clamp(base + int2(corner), 0, int2(info.lowFrameDim) - 1);
		const float4 sampleAux = TexVolLowAux[tap];
		const float2 w = lerp(1.0 - fraction, fraction, float2(corner));
		const float weight = w.x * w.y * (CloudDepthCompatible(sampleAux.y, sceneDepth) ? 1.0 : 0.0);
		const float3 sampleTr = TexVolLowTr[tap];
		const float opacityWeight = weight * (1.0 - sampleTr.x);
		depthSum += sampleAux.x * opacityWeight;
		depthWeight += opacityWeight;
		tr += sampleTr * weight;
		lum += TexVolLowLum[tap] * weight;
		aux += sampleAux * weight;
		weightSum += weight;
	}
	if (weightSum <= 1e-6) {
		tr = 1.0;
		lum = 0.0;
		aux = float4(sceneDepth, sceneDepth, 0.0, 0.0);
		return false;
	}
	tr /= weightSum;
	lum /= weightSum;
	aux /= weightSum;
	aux.x = depthWeight > 1e-6 ? depthSum / depthWeight : sceneDepth;
	return true;
}

bool CloudHistory(float2 uv, float sceneDepth, out float3 tr, out float3 lum, out float4 aux)
{
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	tr = 0.0;
	lum = 0.0;
	aux = 0.0;
	if (any(uv < 0.0) || any(uv >= 1.0) || any(info.previousFrameDim < 1.0))
		return false;
	const float2 position = uv * info.previousFrameDim - 0.5;
	const int2 base = int2(floor(position));
	const float2 fraction = frac(position);
	float weightSum = 0.0;
	float depthWeight = 0.0;
	float depthSum = 0.0;
	[unroll] for (uint i = 0u; i < 4u; ++i)
	{
		const uint2 corner = uint2(i & 1u, i >> 1u);
		const int2 tap = clamp(base + int2(corner), 0, int2(info.previousFrameDim) - 1);
		const float4 sampleAux = TexVolHistoryAux[tap];
		const float2 w = lerp(1.0 - fraction, fraction, float2(corner));
		const float weight = w.x * w.y * (sampleAux.z > 0.0 && CloudDepthCompatible(sampleAux.y, sceneDepth) ? 1.0 : 0.0);
		const float3 sampleTr = TexVolHistoryTr[tap];
		const float opacityWeight = weight * (1.0 - sampleTr.x);
		depthSum += sampleAux.x * opacityWeight;
		depthWeight += opacityWeight;
		tr += sampleTr * weight;
		lum += TexVolHistoryLum[tap] * weight;
		aux += sampleAux * weight;
		weightSum += weight;
	}
	if (weightSum <= 0.01)
		return false;
	tr /= weightSum;
	lum /= weightSum;
	aux /= weightSum;
	aux.x = depthWeight > 1e-6 ? depthSum / depthWeight : sceneDepth;
	return true;
}

[numthreads(8, 8, 1)] void reproject(uint2 pixel : SV_DispatchThreadID) {
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	if (any(pixel >= uint2(info.activeFrameDim)))
		return;
	const float sceneDepth = EncodeCloudDepth(ReconstructSceneRayDistance(pixel));
	float3 currentTr, currentLum;
	float4 currentAux;
	bool currentValid = CloudTraceFallback(pixel, sceneDepth, currentTr, currentLum, currentAux);
	const bool traced = all(pixel == min((pixel / 4u) * 4u + CloudPhaseOffset(), uint2(info.activeFrameDim) - 1u));
	if (traced) {
		currentTr = TexVolLowTr[pixel / 4u];
		currentLum = TexVolLowLum[pixel / 4u];
		currentAux = TexVolLowAux[pixel / 4u];
		currentValid = true;
	}
	const float2 uv = CloudScreenUv(pixel);
	const float3 ray = CloudScreenRay(pixel);
	float3 previousPosition = ray * DecodeCloudDepth(currentAux.x);
	previousPosition += FrameBuffer::CameraPosAdjust.xyz - info.previousCamera;
	previousPosition.xy -= info.cloudWindDelta;
	const float4 clip = mul(info.previousViewProj, float4(previousPosition, 1));
	const float2 previousUv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
	float3 historyTr = 1.0, historyLum = 0.0;
	float4 historyAux = 0.0;
	bool historyValid = false;
	if (info.historyValid != 0u && clip.w > 0.0 && info.cloudHistoryInvalidation > 0.5)
		historyValid = CloudHistory(previousUv, sceneDepth, historyTr, historyLum, historyAux);
	historyValid = historyValid && lerp(info.lowHistoryConfidence, info.highHistoryConfidence, historyAux.w) > 0.5;
	float3 tr = currentTr;
	float3 lum = currentLum;
	float4 aux = currentAux;
	if (historyValid) {
		const float weight = traced ? (historyAux.z < 0.99 ? 1.0 : max(CloudHistoryBlend(historyLum, currentLum, previousUv - uv), saturate(abs(historyTr.x - currentTr.x) * 4.0))) : 0.0;
		tr = lerp(historyTr, currentTr, weight);
		lum = lerp(historyLum, currentLum, weight);
		aux = lerp(historyAux, currentAux, weight);
	}
	aux.y = sceneDepth;
	aux.z = traced ? 1.0 : (historyValid ? historyAux.z : (currentValid ? 0.5 : 0.0));
	RWTexTr[pixel] = saturate(tr.x);
	RWTexLum[pixel] = clamp(lum, 0.0, 65504.0);
	RWTexAux[pixel] = aux;
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

[numthreads(8, 8, 1)] void renderCubemap(uint3 tid : SV_DispatchThreadID) {
	uint3 dims;
	RWTexCubeTr.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims))
		return;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const uint2 pixel = tid.xy * 4u + CloudPhaseOffset();
	const float3 eye = FrameBuffer::CameraPosAdjust.xyz - float3(0, 0, info.bottomZ);
	const uint2 seed = pixel + uint2(tid.z * dims.x * 4u, 0);
	const float2 jitter = float2(CloudRayJitter(seed, info.cloudFrameIndex >> 4u), CloudRayJitter(seed, 0u));
	const VolumetricCloudResult result = RenderVolumetricCloudRay(CloudCubeDirection(pixel, tid.z, dims.x * 4u),
		eye, CLOUD_SKY_DISTANCE, true, jitter, 0.0);
	RWTexCubeTr[tid] = result.transmittance.x;
	RWTexCubeLum[tid] = min(result.lum, 65504.0);
	const float opacity = 1.0 - result.transmittance.x;
	RWTexCubeAux[tid] = float4(EncodeCloudDepth(result.cloud_depth) * opacity, opacity, 1.0, result.high_fraction * opacity);
}

	[numthreads(8, 8, 1)] void reprojectCubemap(uint3 tid : SV_DispatchThreadID)
{
	uint3 dims;
	RWTexCubeTr.GetDimensions(dims.x, dims.y, dims.z);
	if (any(tid >= dims))
		return;
	const VolumetricCloudData info = VolumetricCloudBuffer[0];
	const float3 direction = CloudCubeDirection(tid.xy, tid.z, dims.x);
	const bool traced = all((tid.xy & 3u) == CloudPhaseOffset());
	const float2 tracePosition = clamp((float2(tid.xy) - CloudPhaseOffset()) * 0.25, 0.0, float(dims.x / 4u - 1u));
	const float3 traceDirection = CloudCubeDirection(tracePosition, tid.z, dims.x / 4u);
	const float3 currentTr = TexCubeTraceTr.SampleLevel(TransmittanceSampler, traceDirection, 0);
	const float3 currentLum = TexCubeTraceLum.SampleLevel(TransmittanceSampler, traceDirection, 0);
	const float4 currentAux = TexCubeTraceAux.SampleLevel(TransmittanceSampler, traceDirection, 0);
	const float depth = currentAux.y > 1e-6 ? currentAux.x / currentAux.y : CLOUD_SKY_DEPTH_KM;
	float3 previousPosition = direction * DecodeCloudDepth(depth);
	previousPosition += FrameBuffer::CameraPosAdjust.xyz - info.previousCamera;
	previousPosition.xy -= info.cloudWindDelta;
	const float3 previousDirection = normalize(previousPosition);
	float3 tr = currentTr, lum = currentLum;
	float4 aux = currentAux;
	aux.z = traced ? 1.0 : 0.5;
	if (info.historyValid != 0u && info.cloudHistoryInvalidation > 0.5) {
		const float4 historyAux = TexCubeHistoryAux.SampleLevel(TransmittanceSampler, previousDirection, 0);
		if (historyAux.z > 0.0 && lerp(info.lowHistoryConfidence, info.highHistoryConfidence, historyAux.w / max(historyAux.y, 1e-6)) > 0.5) {
			const float3 historyTr = TexCubeHistoryTr.SampleLevel(TransmittanceSampler, previousDirection, 0);
			const float3 historyLum = TexCubeHistoryLum.SampleLevel(TransmittanceSampler, previousDirection, 0);
			const float weight = traced ? (historyAux.z < 0.99 ? 1.0 : max(CloudHistoryBlend(historyLum, currentLum, float2(length(direction - previousDirection), 0.0)), saturate(abs(historyTr.x - currentTr.x) * 4.0))) : 0.0;
			tr = lerp(historyTr, currentTr, weight);
			lum = lerp(historyLum, currentLum, weight);
			aux = lerp(historyAux, currentAux, weight);
			aux.z = traced ? 1.0 : historyAux.z;
		}
	}
	RWTexCubeTr[tid] = saturate(tr.x);
	RWTexCubeLum[tid] = clamp(lum, 0.0, 65504.0);
	RWTexCubeAux[tid] = aux;
}

#endif
