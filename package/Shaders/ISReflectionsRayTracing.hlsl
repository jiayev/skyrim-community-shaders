#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/PreExposure.hlsli"
#include "Common/SharedData.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color: SV_Target0;
};

#if defined(PSHADER)
SamplerState NormalSampler : register(s0);
SamplerState ColorSampler : register(s1);
SamplerState DepthSampler : register(s2);
SamplerState AlphaSampler : register(s3);

Texture2D<float4> NormalTex : register(t0);
Texture2D<float4> ColorTex : register(t1);
Texture2D<float4> DepthTex : register(t2);
Texture2D<float4> AlphaTex : register(t3);
TextureCube<float4> SkyReflectionTex : register(t4);

#	ifdef ENABLESSR
#		include "Common/ColorManagement.hlsli"
Texture2D<float4> ProbeRadianceTexture : register(t5);
Texture2D<float> ProbeDepthTexture : register(t6);
cbuffer WaterProbe : register(b9)
{
	float4 ProbeOriginOffsets[8];
	uint ProbeValidFaces;
	float ProbeFarDistance;
	float ProbeBoundsRadius;
};
// Water has no temporal denoiser. Keep the sample positions stable as the camera moves.
static const uint ProbeMaxSteps = 24;
#		define PROBE_TRACE_COARSE_HITS
#		define PROBE_TRACE_WATER
#		define PROBE_TRACE_SAMPLER ColorSampler
#		include "Common/EnvironmentProbeTrace.hlsli"
#		undef PROBE_TRACE_SAMPLER
#		undef PROBE_TRACE_WATER
#		undef PROBE_TRACE_COARSE_HITS
#	endif

cbuffer PerGeometry : register(b2)
{
	float4 SSRParams : packoffset(c0);  // fReflectionRayThickness in x, fReflectionMarchingRadius in y, fAlphaWeight in z, 1 / fReflectionMarchingRadius in w
	float3 DefaultNormal : packoffset(c1);
};

static const int iterations = 64.0;
static const int binaryIterations = ceil(log2(iterations));

static const float rayLength = 1.0;
static const float skyDepth = 1.0 - 1e-6;
#	ifdef UNIFIED_WATER
static const float maxValidDepth = skyDepth;
#	else
static const float maxValidDepth = 0.9999;
#	endif

float2 ConvertRaySample(float2 raySample)
{
	return FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(raySample);
}

float2 ConvertRaySamplePrevious(float2 raySample)
{
	return FrameBuffer::GetPreviousDynamicResolutionAdjustedScreenPosition(raySample);
}

float3 SampleScreenReflection(float2 finalSampleUV, float iterationDepth)
{
	float3 color = ColorTex.SampleLevel(ColorSampler, ConvertRaySample(finalSampleUV), 0).xyz;

	// Final sample to world-space
	float4 positionWS = float4(float2(finalSampleUV.x, 1.0 - finalSampleUV.y) * 2.0 - 1.0, iterationDepth, 1.0);
	positionWS = mul(FrameBuffer::CameraViewProjInverse, positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;
	positionWS.w = 1.0;

	// Compute camera motion vector
	float2 cameraMotionVector = MotionBlur::GetSSMotionVector(positionWS, positionWS);

	// Reproject alpha from previous frame
	float2 reprojectedRaySample = finalSampleUV + cameraMotionVector;
	float4 alpha = 0.0;

	// Check that the reprojected data is within the frame
	if (!FrameBuffer::IsOutsideFrame(reprojectedRaySample.xy))
		alpha = float4(AlphaTex.SampleLevel(AlphaSampler, ConvertRaySamplePrevious(reprojectedRaySample.xy), 0).xyz, 1.0);

	return color + SSRParams.z * PreExposure::History(alpha.xyz) * alpha.w;
}

float4 GetReflectionColor(
	float3 projReflectionDirection,
	float3 projPosition,
	float3 reflectionDirection,
	float3 viewPosition)
{
	float3 prevRaySample;
	float3 raySample = projPosition;
	float4 screen = 0;
	// The fixed screen march only proves free space across adjacent depth texels.
	float2 depthSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
	float3 clearEnd = projPosition;
	float previousDepth = projPosition.z;
	bool contiguous = max(abs(projReflectionDirection.x) * depthSize.x, abs(projReflectionDirection.y) * depthSize.y) < iterations * 2;
	bool screenSurface = false;

	for (int i = 0; i < iterations; i++) {
		prevRaySample = raySample;
		raySample = projPosition + (float(i) / float(iterations)) * projReflectionDirection;

		float2 sampleUV = raySample.xy;

		if (FrameBuffer::IsOutsideFrame(sampleUV))
			break;

		float iterationDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(sampleUV), 0).x;
		if (contiguous) {
			float2 crossed = abs(floor(raySample.xy * depthSize) - floor(clearEnd.xy * depthSize));
			contiguous = dot(crossed, 1.0.xx) <= 1 && max(clearEnd.z, raySample.z) <= min(previousDepth, iterationDepth);
			if (contiguous)
				clearEnd = raySample;
			previousDepth = iterationDepth;
		}
		if (iterationDepth > maxValidDepth && iterationDepth <= skyDepth)
			continue;

		if (saturate((raySample.z - iterationDepth) / SSRParams.y) > 0.0) {
			float3 binaryMinRaySample = prevRaySample;
			float3 binaryMaxRaySample = raySample;
			float3 binaryRaySample = raySample;
			float depthThicknessFactor;

			for (int k = 0; k < binaryIterations; k++) {
				binaryRaySample = lerp(binaryMinRaySample, binaryMaxRaySample, 0.5);

				sampleUV = binaryRaySample.xy;
				iterationDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(sampleUV), 0).x;
				if (iterationDepth > maxValidDepth && iterationDepth <= skyDepth) {
					if (iterationDepth < binaryRaySample.z)
						binaryMaxRaySample = binaryRaySample;
					else
						binaryMinRaySample = binaryRaySample;
					depthThicknessFactor = 0.0;
					continue;
				}

				// Compute expected depth vs actual depth
				depthThicknessFactor = 1.0 - saturate(abs(binaryRaySample.z - iterationDepth) / SSRParams.y);

				if (iterationDepth < binaryRaySample.z)
					binaryMaxRaySample = binaryRaySample;
				else
					binaryMinRaySample = binaryRaySample;
			}

			// Fade based on ray length
			float ssrMarchingRadiusFadeFactor = 1.0 - saturate(length(binaryRaySample - projPosition) / rayLength);

			float2 uvResultScreenCenterOffset = binaryRaySample.xy - 0.5;

			float2 centerDistance = abs(uvResultScreenCenterOffset.xy * 2.0);

			// Fade out around screen edges
			float centerDistanceFadeFactorX = smoothstep(0.0, 0.1, saturate(1.0 - centerDistance.x));
			float centerDistanceFadeFactorY = smoothstep(0.0, 0.5, saturate(1.0 - centerDistance.y));

			float fadeFactor = depthThicknessFactor * ssrMarchingRadiusFadeFactor * centerDistanceFadeFactorX * centerDistanceFadeFactorY;

			if (fadeFactor > 0.0) {
				if (iterationDepth <= skyDepth) {
					screenSurface = true;
					screen = float4(SampleScreenReflection(binaryRaySample.xy, iterationDepth), fadeFactor);
					if (screen.a >= 0.999)
						return screen;
				} else if (!SharedData::InInterior && !SharedData::HideSky) {
					// Sky has no finite intersection. The probe may still see a surface between samples.
					float3 sky = SkyReflectionTex.SampleLevel(ColorSampler, FrameBuffer::ViewToWorld(reflectionDirection, false), 0).xyz;
					screen = float4(PreExposure::Encode(sky), fadeFactor);
				}
			}

			break;
		}
	}

#	ifdef ENABLESSR
	[branch] if (ProbeValidFaces != 0)
	{
		float3 direction = normalize(reflectionDirection);
		float3 worldDirection = normalize(FrameBuffer::ViewToWorld(direction, false));
		float4 resume = mul(FrameBuffer::CameraProjInverse, float4(clearEnd.xy * float2(2, -2) + float2(-1, 1), clearEnd.z, 1));
		float startDistance = resume.w > 0 ? max(0.5, dot(resume.xyz / resume.w - viewPosition, direction)) : 1e30;
		float3 radiance;
		float hitDistance;
		float4 waterHit;
		if (TraceEnvironmentProbe(FrameBuffer::ViewToWorld(viewPosition), worldDirection, startDistance, 5.0, radiance, hitDistance, waterHit)) {
			float probeCoverage = waterHit.w;
			// If this same surface is visible, use the game's lit scene and alpha history
			// on both sides of the handoff. Reject occluders using linear view depth.
			float3 hitVS = FrameBuffer::WorldToView(waterHit.xyz);
			float4 clip = mul(FrameBuffer::CameraProj, float4(hitVS, 1));
			if (clip.w > 0) {
				float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
				if (!FrameBuffer::IsOutsideFrame(uv)) {
					float sceneDepth = DepthTex.SampleLevel(DepthSampler, ConvertRaySample(uv), 0).x;
					float error = abs(SharedData::GetScreenDepth(sceneDepth) - hitVS.z);
					uint width, height;
					ProbeDepthTexture.GetDimensions(width, height);
					float tolerance = max(5.0, length(hitVS) * 2 / width);
					if (sceneDepth <= maxValidDepth && error < tolerance) {
						float2 edge = min(uv, 1 - uv) * depthSize;
						float confidence = saturate(min(edge.x, edge.y) * 0.25) * (1 - smoothstep(0, tolerance, error));
						float3 screenRadiance = ColorManagement::SceneToLinear(PreExposure::Decode(SampleScreenReflection(uv, sceneDepth)));
						radiance = lerp(radiance, screenRadiance, confidence);
					}
				}
			}
			// A finite screen hit is in front. A sky sample fills only the open part behind the probe hit.
			float screenWeight = screen.a * (screenSurface ? 1 : 1 - probeCoverage);
			float probeWeight = probeCoverage * (screenSurface ? 1 - screen.a : 1);
			float coverage = screenWeight + probeWeight;
			float3 fallback = PreExposure::Encode(ColorManagement::LinearToScene(radiance));
			return float4(ColorManagement::SceneColor::LerpInLinear(fallback, screen.rgb, screenWeight / coverage), coverage);
		}
	}
#	endif
	return screen;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;
	psout.Color = 0;

#	ifndef ENABLESSR
	// Disable SSR raymarch
	return psout;
#	endif

	float2 uv = input.TexCoord;
	float2 screenPosition = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);

	[branch] if (NormalTex.Sample(NormalSampler, screenPosition).z <= 0)
	{
		return psout;
	}

	float3 viewNormal = DefaultNormal;

	float depth = DepthTex.SampleLevel(DepthSampler, screenPosition, 0).x;
	[branch] if (depth > maxValidDepth)
	{
		return psout;
	}

	float4 positionVS = float4(float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
	positionVS = mul(FrameBuffer::CameraProjInverse, positionVS);
	positionVS.xyz = positionVS.xyz / positionVS.w;

	float3 viewPosition = positionVS.xyz;
	float3 viewDirection = normalize(viewPosition);

	float3 reflectionDirection = reflect(viewDirection, viewNormal);
	float viewAttenuation = saturate(dot(viewDirection, reflectionDirection));
	[branch] if (viewAttenuation < 0)
	{
		return psout;
	}

	float4 clipReflectionDirection = mul(FrameBuffer::CameraProj, float4(reflectionDirection, 0.0));
	float3 ndcPosition = float3(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), depth);
	float3 projReflectionDirection = clipReflectionDirection.xyz - ndcPosition * clipReflectionDirection.w;
	projReflectionDirection.xy *= float2(0.5, -0.5);
	float directionLengthSquared = dot(projReflectionDirection, projReflectionDirection);
	if (directionLengthSquared <= 0.0)
		return psout;
	projReflectionDirection *= rsqrt(directionLengthSquared) * rayLength;

	float3 projPosition = float3(uv, depth);

	psout.Color = GetReflectionColor(projReflectionDirection, projPosition, reflectionDirection, viewPosition);

	return psout;
}
#endif
