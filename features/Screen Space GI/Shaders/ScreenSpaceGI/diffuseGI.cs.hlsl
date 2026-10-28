///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>         (\_/)
// Version:         (see XeGTAO.h)                                                                            (='.'=)
// Details:         https://github.com/GameTechDev/XeGTAO                                                     (")_(")
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// with additional edits by FiveLimbedCat/ProfJack
//
// More references:
//
// Screen Space Indirect Lighting with Visibility Bitmask
//  https://arxiv.org/abs/2301.11376
//
// Exploring Raytraced Future in Metro Exodus
//  https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// New version is based on SSRT 3
// https://github.com/cdrinmatane/SSRT3
//
// MIT License
//
// Copyright (c) 2024 CDRIN
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FastMath.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "NRD/NRDReblurSH.hlsli"
#include "ScreenSpaceGI/common.hlsli"
#if defined(IBL)
#	include "IBL/IBL.hlsli"
#endif

Texture2D<float> srcWorkingDepth : register(t0);
Texture2D<float3> srcRadiance : register(t2);
Texture2D<unorm float2> srcNoise : register(t3);
#if defined(DYNAMIC_CUBEMAPS) && !defined(PROBE_CONTINUATION)
TextureCube<float3> EnvTexture : register(t4);
TextureCube<float3> ReflectionCubemap : register(t5);
#	if defined(SKYLIGHTING)
#		define SKYLIGHTING_PROBE_REGISTER t6
#		include "Skylighting/Skylighting.hlsli"
#	endif
#endif
Texture2D<float2> srcNormal : register(t8);

RWTexture2D<float4> outRadianceHitDist : register(u0);
#ifdef SSGI_SH
RWTexture2D<float4> outSH1 : register(u2);
#endif

static const float SSGI_NORMAL_BIAS = 0.01;
static const uint SSGI_ROTATION_COUNT = 1;
static const uint SSGI_FALLBACK_SAMPLE_COUNT = 4;
static const float SSGI_EXP_FACTOR = 2.0;
static const float SSGI_FALLBACK_POWER = 1.0;
static const float SSGI_FALLBACK_MIP = 3.0;

#include "ScreenSpaceGI/visibility.hlsli"
#if defined(PROBE_CONTINUATION)
#	include "ScreenSpaceGI/probeGI.hlsli"
#endif

// Engine-specific screen & temporal noise loader
float2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)  // without TAA, temporalIndex is always 0
{
	// noise texture from https://github.com/electronicarts/fastnoise
	// 128x128x64
	uint2 noiseCoord = (pixCoord % 128) + uint2(0, (temporalIndex % 64) * 128);
	return srcNoise.Load(uint3(noiseCoord, 0));
}

#if defined(DYNAMIC_CUBEMAPS) && !defined(PROBE_CONTINUATION)
float3 SampleDiffuseFallbackCubemap(float3 worldPos, float3 worldNormal, float3 worldDir)
{
	float3 envSampleRaw = EnvTexture.SampleLevel(samplerLinearClamp, worldDir, SSGI_FALLBACK_MIP);
	float3 envColor = envSampleRaw;

#	if defined(SKYLIGHTING)
	float skylightingDiffuse = 1.0;
	if (!SharedData::InInterior) {
		float fadeOutFactor = Skylighting::GetFadeOutFactor(worldPos);
		float3 skylightingNormal = normalize(float3(worldNormal.xy, max(0, worldNormal.z)));
		float skylightingBoost = 1.0 + saturate(worldNormal.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);
		sh2 skylightingSH = Skylighting::Sample(worldPos, worldDir);
		skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, skylightingNormal, fadeOutFactor) * skylightingBoost;
	}
#	endif

#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		uint dalcMode = SharedData::iblSettings.DALCMode;

		if (dalcMode >= 2) {
			// Mode 2: DALC-normalized env scaled by DALCAmount
			float envLum = Color::RGBToLuminance(EnvTexture.SampleLevel(samplerLinearClamp, worldDir, 15));
			float3 dalc = Color::Ambient(max(0, SharedData::GetAmbient(worldDir)));
			envColor = (envSampleRaw / max(envLum, 0.001)) * dalc * SharedData::iblSettings.DALCAmount;
		} else {
			// Mode 0/1: ratio-based
			float3 saturatedEnv = Color::Saturation(envSampleRaw, SharedData::iblSettings.EnvIBLSaturation);
			float3 ratio = ImageBasedLighting::GetIBLRatio();
			envColor = saturatedEnv * ratio * SharedData::iblSettings.EnvIBLScale;
		}

		float3 reflectionColor = 0;
		if (!SharedData::InInterior) {
			float3 skySample = ReflectionCubemap.SampleLevel(samplerLinearClamp, worldDir, SSGI_FALLBACK_MIP);
			float3 skyColor = Color::Saturation(skySample, SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
#		if defined(SKYLIGHTING)
			skyColor *= skylightingDiffuse;
			if (SharedData::iblSettings.SkylightingAffectsEnv != 0)
				envColor *= skylightingDiffuse;
#		endif
			reflectionColor = skyColor;
			envColor += skyColor;
		}
		envColor = ColorManagement::SceneToLinear(envColor);
		envColor = ImageBasedLighting::ApplyIBLReflectionFallback(envColor, ColorManagement::SceneToLinear(reflectionColor), worldPos);
	} else
#	endif
	{
		float3 directionalAmbient = Color::Ambient(max(0, SharedData::GetAmbient(worldDir)));
		envColor = ColorManagement::SceneToLinear(directionalAmbient);
#	if defined(SKYLIGHTING)
		if (!SharedData::InInterior)
			envColor *= skylightingDiffuse;
#	endif
	}

	return envColor;
}
#endif

void CalculateGI(
	uint2 dtid, float2 uv, float viewspaceZ, float3 viewspaceNormal,
	out float o_ao, out float3 o_radiance
#ifdef SSGI_SH
	,
	out float3 o_direction
#endif
)
{
	const float2 frameScale = FrameDim * RcpTexDim;

	float2 normalizedScreenPos = uv;
	const float rcpNumSteps = rcp((float)NumSteps);
	uint2 noiseCoord = uint2(normalizedScreenPos * OUT_FRAME_DIM);
	const float2 localNoise = SpatioTemporalNoise(noiseCoord, FrameIndex);
	const float noiseStep = localNoise.y;
	const float noiseDirection = localNoise.x;

	const float3 pixCenterPos = ScreenToViewPosition(normalizedScreenPos, viewspaceZ) + viewspaceNormal * SSGI_NORMAL_BIAS * viewspaceZ;
	const float3 viewVec = normalize(-pixCenterPos);

	if (dot(viewVec, pixCenterPos) > 0)
		viewspaceNormal = -viewspaceNormal;

	float normHitDist = 0;
	float3 totalRadiance = 0;
	float3 fallbackRadiance = 0;
#ifdef SSGI_SH
	float3 totalDirection = 0;
	float3 fallbackDirection = 0;
#endif

	[unroll] for (uint rotation = 0; rotation < SSGI_ROTATION_COUNT; rotation++)
	{
		float phi = (rotation + noiseDirection) * (Math::PI / SSGI_ROTATION_COUNT);
		float3 directionVec = 0;
		sincos(phi, directionVec.y, directionVec.x);

		float2 omega_dir = float2(directionVec.x, -directionVec.y);
		float2 pixPos = dtid + 0.5;
		float2 absDir = max(abs(omega_dir), 1e-6);

		float3 planeNormal = normalize(cross(directionVec, viewVec));
		float3 tangent = cross(viewVec, planeNormal);
		float3 projectedNormalVec = viewspaceNormal - planeNormal * dot(viewspaceNormal, planeNormal);
		float projectedNormalLengthSq = dot(projectedNormalVec, projectedNormalVec);
		float projectedNormalLength = sqrt(max(projectedNormalLengthSq, 0.0));
		float3 projectedNormalNormalized = projectedNormalVec * rsqrt(max(projectedNormalLengthSq, EPSILON_LENGTH_SQ));
		float3 projectedNormalTangent = cross(projectedNormalNormalized, planeNormal);
		float cosN = clamp(dot(projectedNormalNormalized, viewVec), -1.0, 1.0);
		float sinN = clamp(dot(projectedNormalNormalized, tangent), -1.0, 1.0);
		float n = -sign(dot(projectedNormalVec, tangent)) * FastMath::ACos(cosN);

		uint globalOccludedBitfield = 0;

		[unroll] for (int sideSign = -1; sideSign <= 1; sideSign += 2)
		{
			float2 sideDir = omega_dir * sideSign;
			float2 edgeDist;
			edgeDist.x = sideDir.x >= 0 ? (OUT_FRAME_DIM.x - pixPos.x) : pixPos.x;
			edgeDist.y = sideDir.y >= 0 ? (OUT_FRAME_DIM.y - pixPos.y) : pixPos.y;
			float screenspaceRadius = min(edgeDist.x / absDir.x, edgeDist.y / absDir.y);

			[loop] for (uint step = 0; step < NumSteps; step++)
			{
				float s = (step + noiseStep) * rcpNumSteps;
				float sampleOffset = pow(abs(s), SSGI_EXP_FACTOR) * screenspaceRadius;
				sampleOffset = max(sampleOffset, 1.0 + step);

				float2 samplePxCoord = pixPos + omega_dir * sampleOffset * sideSign;
				float2 sampleUV = samplePxCoord * RCP_OUT_FRAME_DIM;

				float2 sampleScreenPos = sampleUV;
				[branch] if (any(sampleScreenPos > 1.0) || any(sampleScreenPos < 0.0)) break;

				float mipLevel = min((step + 1) / 2, 4);

				float SZ = srcWorkingDepth.SampleLevel(samplerPointClamp, sampleUV * frameScale, mipLevel);
				if (SZ <= FP_Z)
					continue;

				float3 samplePos = ScreenToViewPosition(sampleScreenPos, SZ);
				float3 sampleDelta = samplePos - pixCenterPos;
				float3 sampleHorizonVec = normalize(sampleDelta);

				float3 sampleBackHorizonVec = normalize(sampleDelta - viewVec * Thickness * viewspaceZ);

				float2 frontBackHorizon = float2(dot(sampleHorizonVec, viewVec), dot(sampleBackHorizonVec, viewVec));
				frontBackHorizon = clamp(frontBackHorizon, -1.0, 1.0);
				frontBackHorizon = float2(FastMath::ACos(frontBackHorizon.x), FastMath::ACos(frontBackHorizon.y));
				frontBackHorizon = saturate(((sideSign * -frontBackHorizon) - n + Math::HALF_PI) * Math::INV_PI);
				frontBackHorizon = sideSign > 0 ? frontBackHorizon.yx : frontBackHorizon.xy;

				uint currentOccludedBitfield = ComputeOccludedBitfield(
					frontBackHorizon.x, frontBackHorizon.y, globalOccludedBitfield);

#ifdef GI
				if (currentOccludedBitfield != 0) {
					float3 normalSample = GBuffer::DecodeNormal(
						srcNormal.SampleLevel(samplerPointClamp, sampleUV * OUT_FRAME_SCALE, mipLevel));
					if (dot(samplePos, normalSample) > 0.0)
						normalSample = -normalSample;

					float scalarWeight;
					float3 directionWeight;
					IntegrateBitfield(
						currentOccludedBitfield,
						projectedNormalNormalized, projectedNormalTangent,
						projectedNormalLength, sinN, cosN,
						normalSample, true,
						scalarWeight, directionWeight);

					float3 sampleRadiance = max(
						srcRadiance.SampleLevel(samplerPointClamp, sampleUV * OUT_FRAME_SCALE, mipLevel).rgb,
						0);
					float sampleLuminance = _NRD_LinearToYCoCg(sampleRadiance).x;
					if (scalarWeight > 0.0) {
						float3 diffuseRadiance = sampleRadiance * scalarWeight * GIStrength;
						totalRadiance += diffuseRadiance;
#	ifdef SSGI_SH
						totalDirection += directionWeight * sampleLuminance * GIStrength;
#	endif
					}
				}
#endif  // GI
			}
		}

		normHitDist += float(countbits(globalOccludedBitfield)) / float(SSGI_MAX_RAY);

#if defined(PROBE_CONTINUATION) && defined(GI)
		// Separate noise dimension from the slice azimuth and screen-step jitter.
		float sampleNoise = SpatioTemporalNoise(noiseCoord + uint2(61, 37), FrameIndex + 17).y;
		uint rayCount = clamp(ProbeRays, 1u, SSGI_MAX_PROBE_RAYS);
		OpenBitfieldSamples samples = SampleOpenBitfield(~globalOccludedBitfield,
			projectedNormalNormalized, projectedNormalTangent, projectedNormalLength, sinN, cosN,
			rayCount, sampleNoise);
		if (samples.weight > 0) {
			float3 worldPos = ViewToWorldPosition(pixCenterPos, FrameBuffer::CameraViewInverse);
			[loop] for (uint ray = 0; ray < rayCount; ++ray)
			{
				float3 directionVS = GetOpenBitfieldDirection(samples, ray);
				float3 worldDir = normalize(ViewToWorldVector(directionVS, FrameBuffer::CameraViewInverse));
				float3 sampleRadiance = SampleProbeGI(worldPos, worldDir, max(0.5, Thickness * viewspaceZ));
				totalRadiance += sampleRadiance * samples.weight;
#	ifdef SSGI_SH
				totalDirection += directionVS * (_NRD_LinearToYCoCg(sampleRadiance).x * samples.weight);
#	endif
			}
		}
#elif defined(DYNAMIC_CUBEMAPS) && !defined(PROBE_CONTINUATION)
		if (UseDynamicCubemap != 0) {
			float3 worldPos = ViewToWorldPosition(pixCenterPos, FrameBuffer::CameraViewInverse);
			float3 worldNormal = ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse);
			[unroll] for (uint j = 0; j < SSGI_FALLBACK_SAMPLE_COUNT; j++)
			{
				uint maskSize = SSGI_MAX_RAY / SSGI_FALLBACK_SAMPLE_COUNT;
				uint mask = 0xFFFFFFFFu >> (SSGI_MAX_RAY - maskSize);
				uint bitOffset = j * maskSize;
				uint openBitfield = (~globalOccludedBitfield) & (mask << bitOffset);
				float openWeight;
				float3 openDirectionWeight;
				IntegrateBitfield(
					openBitfield,
					projectedNormalNormalized, projectedNormalTangent,
					projectedNormalLength, sinN, cosN,
					float3(0, 0, 0), false,
					openWeight, openDirectionWeight);

				if (openWeight <= 0)
					continue;

				float3 rayDir = normalize(openDirectionWeight / openWeight);
				float3 worldDir = ViewToWorldVector(rayDir, FrameBuffer::CameraViewInverse);
				float3 fallbackSample = SampleDiffuseFallbackCubemap(worldPos, worldNormal, worldDir);
				float3 contrib = fallbackSample * openWeight;
				fallbackRadiance += contrib;
#	ifdef SSGI_SH
				fallbackDirection += openDirectionWeight * _NRD_LinearToYCoCg(fallbackSample).x;
#	endif
			}
		}
#endif
	}

	normHitDist /= SSGI_ROTATION_COUNT;
	totalRadiance /= SSGI_ROTATION_COUNT;
	fallbackRadiance /= SSGI_ROTATION_COUNT;
#ifdef SSGI_SH
	totalDirection /= SSGI_ROTATION_COUNT;
	fallbackDirection /= SSGI_ROTATION_COUNT;
#endif

	normHitDist = saturate(normHitDist);
	// REBLUR defines normalized hit distance as diffuse AO/visibility: nearby occlusion tends to 0,
	// while open or distant geometry tends to 1. Exact 0 is reserved for an invalid/skipped lobe.
	normHitDist = max(saturate(1 - normHitDist), NRD_EPS);
#ifdef SSGI_SH
	float fallbackLuminance = _NRD_LinearToYCoCg(fallbackRadiance).x;
#endif
	fallbackRadiance = pow(abs(fallbackRadiance), SSGI_FALLBACK_POWER) * DiffuseCubemapMult;
	// The open-bit integration supplies directional visibility, while the final
	// GTAO visibility also represents unresolved local occlusion. Apply both to
	// ambient cubemap fallback; screen-space hit radiance remains unchanged.
	// AOPower shapes only this fallback occlusion: traced GI already carries its own visibility.
	fallbackRadiance *= pow(normHitDist, AOPower);

	o_ao = normHitDist;
	o_radiance = totalRadiance + fallbackRadiance;
#ifdef SSGI_SH
	// NRD SH stores the first directional moment. Keep its magnitude (directionality) and use
	// world space so temporal history and the world-space resolve guides share one basis.
	float fallbackLuminanceScale = _NRD_LinearToYCoCg(fallbackRadiance).x / max(fallbackLuminance, EPSILON_DIVISION);
	float3 directionalMoment = totalDirection + fallbackDirection * fallbackLuminanceScale;
	float radianceLuminance = _NRD_LinearToYCoCg(o_radiance).x;
	float3 averageDirectionVS = directionalMoment / max(radianceLuminance, EPSILON_DIVISION);
	// Preserve the first-moment magnitude; normalizing here would falsely turn
	// diffuse or opposing illumination into a fully directional lobe.
	o_direction = ViewToWorldVector(averageDirectionVS, FrameBuffer::CameraViewInverse);
#endif
}

[numthreads(8, 8, 1)] void main(const uint2 dtid : SV_DispatchThreadID) {
	const float2 frameScale = FrameDim * RcpTexDim;

#if defined(SSGI_QUARTER)
	// Trace one pixel per 2x2 block. Cycle through all four phases so REBLUR's
	// hit-distance reconstruction and temporal accumulation receive fresh data.
	uint2 phaseOffset = uint2(FrameIndex & 1u, (FrameIndex >> 1u) & 1u);
	uint2 pxCoord = dtid * 2u + phaseOffset;
	if (any(pxCoord >= uint2(FrameDim)))
		return;
	uint2 outCoord = pxCoord;
#elif defined(SSGI_HALF)
	uint colOffset = (dtid.y + FrameIndex) & 1;
	uint2 pxCoord = uint2(dtid.x * 2 + colOffset, dtid.y);
	uint2 outCoord = dtid.xy;
#else
	uint2 pxCoord = dtid;
	uint2 outCoord = dtid;
#endif

	float2 uv = (pxCoord + .5) * RCP_OUT_FRAME_DIM;

	float viewspaceZ = READ_DEPTH(srcWorkingDepth, pxCoord);
	if (viewspaceZ <= FP_Z) {
#ifdef SSGI_SH
		float4 sh1;
		outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackSh(float3(0, 0, 0), 0, float3(0, 0, 0), sh1, true);
		outSH1[outCoord] = sh1;
#else
		outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3(0, 0, 0), 0, true);
#endif
		return;
	}

	float2 normalSample = FULLRES_LOAD(srcNormal, pxCoord, uv * OUT_FRAME_SCALE, samplerLinearClamp);
	float3 viewspaceNormal = GBuffer::DecodeNormal(normalSample);

	float normHitDist = 0;
	float3 radiance = 0;
#ifdef SSGI_SH
	float3 direction = 0;
#endif

	CalculateGI(pxCoord, uv, viewspaceZ, viewspaceNormal, normHitDist, radiance
#ifdef SSGI_SH
		,
		direction
#endif
	);

	radiance = filterNaN(radiance);

#ifdef SSGI_QUARTER
	// One out of four pixels carries a sample. Compensate its energy so the
	// spatially and temporally filtered signal remains unbiased.
	radiance *= 4.0;
#endif

#ifdef SSGI_SH
	float4 sh1;
	outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackSh(radiance, normHitDist, direction, sh1, true);
	outSH1[outCoord] = sh1;
#else
	outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDist, true);
#endif
}
