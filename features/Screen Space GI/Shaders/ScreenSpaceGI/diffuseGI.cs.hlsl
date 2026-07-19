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
#include "Common/FastMath.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "NRD/NRDReblurSH.hlsli"
#include "ScreenSpaceGI/common.hlsli"

Texture2D<float> srcWorkingDepth : register(t0);
Texture2D<float3> srcRadiance : register(t2);
Texture2D<unorm float2> srcNoise : register(t3);
#if defined(DYNAMIC_CUBEMAPS)
TextureCube<float3> EnvTexture : register(t4);
TextureCube<float3> EnvReflectionsTexture : register(t5);
#	if defined(SKYLIGHTING)
#		define SKYLIGHTING_PROBE_REGISTER t6
#		include "Skylighting/Skylighting.hlsli"
#	endif
#endif
Texture2D<float2> srcNormal : register(t8);
Texture2D<float4> srcPrevGI : register(t9);
#ifdef SSGI_SH
Texture2D<float4> srcPrevGISH1 : register(t10);
#endif

RWTexture2D<float4> outRadianceHitDist : register(u0);
RWTexture2D<half3> outPrevGeo : register(u1);
#ifdef SSGI_SH
RWTexture2D<float4> outSH1 : register(u2);
#endif

static const float SSGI_NORMAL_BIAS = 0.01;
static const uint SSGI_MAX_RAY = 32;
static const uint SSGI_ROTATION_COUNT = 1;
static const uint SSGI_FALLBACK_SAMPLE_COUNT = 4;
static const float SSGI_EXP_FACTOR = 2.0;
static const float SSGI_FALLBACK_POWER = 1.0;
static const float SSGI_FALLBACK_MIP = 3.0;
static const float SSGI_BACKFACE_LIGHTING = 0.0;
static const float SSGI_MULTI_BOUNCE_GI = 1.0;

// Engine-specific screen & temporal noise loader
float2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)  // without TAA, temporalIndex is always 0
{
	// noise texture from https://github.com/electronicarts/fastnoise
	// 128x128x64
	uint2 noiseCoord = (pixCoord % 128) + uint2(0, (temporalIndex % 64) * 128);
	return srcNoise.Load(uint3(noiseCoord, 0));
}

uint ComputeOccludedBitfield(float minHorizon, float maxHorizon, inout uint globalOccludedBitfield, out uint numOccludedZones)
{
	uint startHorizonInt = min((uint)(saturate(minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY);
	uint angleHorizonInt = min((uint)ceil(saturate(maxHorizon - minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY - startHorizonInt);

	numOccludedZones = 0;
	if (angleHorizonInt == 0)
		return 0;

	uint angleHorizonBitfield = 0xFFFFFFFFu;
	if (angleHorizonInt < SSGI_MAX_RAY)
		angleHorizonBitfield = (1u << angleHorizonInt) - 1u;
	uint currentOccludedBitfield = angleHorizonBitfield << startHorizonInt;
	currentOccludedBitfield &= ~globalOccludedBitfield;
	globalOccludedBitfield |= currentOccludedBitfield;
	numOccludedZones = countbits(currentOccludedBitfield);
	return currentOccludedBitfield;
}

#if defined(DYNAMIC_CUBEMAPS)
float3 SampleDiffuseFallbackCubemap(float3 worldPos, float3 worldNormal, float3 worldDir)
{
	float3 envSampleRaw = EnvTexture.SampleLevel(samplerLinearClamp, worldDir, SSGI_FALLBACK_MIP);
	float3 envColor = envSampleRaw;

#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL) {
		uint dalcMode = SharedData::iblSettings.DALCMode;

		if (dalcMode >= 2) {
			// Mode 2/3: DALC-normalized env scaled by DALCAmount
			float envLum = Color::RGBToLuminance(EnvTexture.SampleLevel(samplerLinearClamp, worldDir, 15));
			float3 dalc = Color::Ambient(max(0, SharedData::GetAmbient(worldDir)));
			envColor = (envSampleRaw / max(envLum, 0.001)) * dalc * SharedData::iblSettings.DALCAmount;
		} else {
			// Mode 0/1: ratio-based
			float3 dalc0 = Color::Ambient(max(0, SharedData::GetAmbient(float3(0, 0, 0))));
			float3 envAvg = EnvTexture.SampleLevel(samplerLinearClamp, float3(0, 0, 0), 15);

			if (dalcMode == 1) {
				// Color Ratio: per-channel
				float3 ratio = dalc0 / max(envAvg, 0.001);
				envColor = envSampleRaw * lerp(1.0, ratio, SharedData::iblSettings.DALCAmount) * SharedData::iblSettings.EnvIBLScale;
			} else {
				// Luminance Ratio: scalar
				float dalcLum = Color::RGBToLuminance(dalc0);
				float envLum = Color::RGBToLuminance(envAvg);
				float ratio = (envLum > 0.001) ? (dalcLum / envLum) : 1.0;
				envColor = envSampleRaw * lerp(1.0, ratio, SharedData::iblSettings.DALCAmount) * SharedData::iblSettings.EnvIBLScale;
			}
		}
	}
#	endif

#	if defined(SKYLIGHTING)
	if (!SharedData::InInterior) {
		float fadeOutFactor = Skylighting::GetFadeOutFactor(worldPos);
		float3 skylightingNormal = normalize(float3(worldNormal.xy, max(0, worldNormal.z)));
		float skylightingBoost = 1.0 + saturate(worldNormal.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);
		sh2 skylightingSH = Skylighting::Sample(worldPos, worldDir);
		float skylightingDiffuse = Skylighting::EvaluateDiffuse(skylightingSH, skylightingNormal, fadeOutFactor) * skylightingBoost;
		float3 envSkyColor = EnvReflectionsTexture.SampleLevel(samplerLinearClamp, worldDir, SSGI_FALLBACK_MIP);
		float3 skyColor = max(envSkyColor - envSampleRaw, 0);

#		if defined(IBL)
		if (SharedData::iblSettings.EnableIBL) {
			skyColor *= SharedData::iblSettings.SkyIBLScale;
			envColor += skyColor;
			envColor *= skylightingDiffuse;
		} else
#		endif
		{
			envColor += skyColor;
			envColor *= skylightingDiffuse;
		}
	}
#	endif

	return Color::IrradianceToLinear(envColor);
}
#endif

float3 SamplePreviousGIRadiance(float2 uv, float3 viewspaceNormal, float3 viewVec, float2 frameScale)
{
	float2 prevUV = uv * frameScale;
#ifdef SSGI_SH
	NRD_SG sg = REBLUR_BackEnd_UnpackSh(
		srcPrevGI.SampleLevel(samplerPointClamp, prevUV, 0),
		srcPrevGISH1.SampleLevel(samplerPointClamp, prevUV, 0));
	return max(0, NRD_SG_ResolveDiffuse(sg, viewspaceNormal, viewVec, 1.0));
#else
	float3 radiance;
	float normHitDist;
	REBLUR_BackEnd_UnpackRadianceAndNormHitDist(srcPrevGI.SampleLevel(samplerPointClamp, prevUV, 0), radiance, normHitDist);
	return max(0, radiance);
#endif
}

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
#endif

	[loop] for (uint rotation = 0; rotation < SSGI_ROTATION_COUNT; rotation++)
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
		float3 projectedNormalNormalized = projectedNormalVec * rsqrt(max(dot(projectedNormalVec, projectedNormalVec), EPSILON_LENGTH_SQ));
		float3 realTangent = cross(projectedNormalNormalized, planeNormal);
		float cosN = clamp(dot(projectedNormalNormalized, viewVec), -1.0, 1.0);
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

				uint numOccludedZones;
				ComputeOccludedBitfield(frontBackHorizon.x, frontBackHorizon.y, globalOccludedBitfield, numOccludedZones);

#ifdef GI
				if (numOccludedZones > 0) {
					float3 sampleRadiance = srcRadiance.SampleLevel(samplerPointClamp, sampleUV * OUT_FRAME_SCALE, mipLevel).rgb;
					float3 normalSample = GBuffer::DecodeNormal(srcNormal.SampleLevel(samplerPointClamp, sampleUV * OUT_FRAME_SCALE, mipLevel));
					if (dot(samplePos, normalSample) > 0)
						normalSample = -normalSample;

					sampleRadiance += SamplePreviousGIRadiance(sampleUV, normalSample, normalize(-samplePos), frameScale) * SSGI_MULTI_BOUNCE_GI;

					if (Color::RGBToLuminance(sampleRadiance) > 0.001) {
						float3 lightDirection = sampleHorizonVec;
						float receiverNdotL = saturate(dot(viewspaceNormal, lightDirection));
						float sourceNdotL = dot(normalSample, -lightDirection);
						sourceNdotL = SSGI_BACKFACE_LIGHTING > 0 && dot(normalSample, viewVec) > 0 ?
						                  (sign(sourceNdotL) < 0 ? abs(sourceNdotL) * SSGI_BACKFACE_LIGHTING : abs(sourceNdotL)) :
						                  saturate(sourceNdotL);

						if (receiverNdotL > 0.001 && sourceNdotL > 0.001) {
							float weight = float(numOccludedZones) / float(SSGI_MAX_RAY);
							float3 diffuseRadiance = max(sampleRadiance, 0) * receiverNdotL * sourceNdotL * weight * GIStrength;
							totalRadiance += diffuseRadiance;
#	ifdef SSGI_SH
							totalDirection += lightDirection * Color::RGBToLuminance(diffuseRadiance);
#	endif
						}
					}
				}
#endif  // GI
			}
		}

		normHitDist += float(countbits(globalOccludedBitfield)) / float(SSGI_MAX_RAY);

#if defined(DYNAMIC_CUBEMAPS)
		if (UseDynamicCubemap != 0) {
			float3 worldPos = ViewToWorldPosition(pixCenterPos, FrameBuffer::CameraViewInverse);
			float3 worldNormal = ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse);
			uint globalOccludedBitfieldCopy = globalOccludedBitfield;
			[unroll] for (uint j = 0; j < SSGI_FALLBACK_SAMPLE_COUNT; j++)
			{
				uint maskSize = SSGI_MAX_RAY / SSGI_FALLBACK_SAMPLE_COUNT;
				uint mask = 0xFFFFFFFFu >> (SSGI_MAX_RAY - maskSize);
				uint hitCount = countbits(globalOccludedBitfieldCopy & mask);
				float openWeight = saturate(float(maskSize - hitCount) / float(SSGI_MAX_RAY));

				if (openWeight <= 0)
					continue;

				float cosine = (1.0 - ((float(j) + 0.5) / float(SSGI_FALLBACK_SAMPLE_COUNT))) * 2.0 - 1.0;
				float sine = sqrt(saturate(1.0 - cosine * cosine));
				float3 rayDir = normalize(realTangent * cosine + viewspaceNormal * sine);
				rayDir = normalize(rayDir - planeNormal * dot(rayDir, planeNormal));
				float3 worldDir = ViewToWorldVector(rayDir, FrameBuffer::CameraViewInverse);
				float3 contrib = SampleDiffuseFallbackCubemap(worldPos, worldNormal, worldDir) * openWeight;
				fallbackRadiance += contrib;
#	ifdef SSGI_SH
				totalDirection += rayDir * Color::RGBToLuminance(contrib);
#	endif

				globalOccludedBitfieldCopy >>= maskSize;
			}
		}
#endif
	}

	normHitDist /= SSGI_ROTATION_COUNT;
	totalRadiance /= SSGI_ROTATION_COUNT;
	fallbackRadiance /= SSGI_ROTATION_COUNT;

	normHitDist = saturate(normHitDist);
	normHitDist = 1 - pow(abs(1 - normHitDist), AOPower);
	fallbackRadiance = pow(abs(fallbackRadiance), SSGI_FALLBACK_POWER) * DiffuseCubemapMult;

	o_ao = normHitDist;
	o_radiance = totalRadiance + fallbackRadiance;
#ifdef SSGI_SH
	o_direction = normalize(totalDirection + 1e-6);
#endif
}

[numthreads(8, 8, 1)] void main(const uint2 dtid : SV_DispatchThreadID) {
	const float2 frameScale = FrameDim * RcpTexDim;

#if defined(SSGI_HALF)
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
		outPrevGeo[pxCoord] = 0;
		return;
	}

	float2 normalSample = FULLRES_LOAD(srcNormal, pxCoord, uv * OUT_FRAME_SCALE, samplerLinearClamp);
	float3 viewspaceNormal = GBuffer::DecodeNormal(normalSample);

	float3 worldNormal = ViewToWorldVector(viewspaceNormal, FrameBuffer::CameraViewInverse);
	half2 encodedWorldNormal = GBuffer::EncodeNormal(worldNormal);
	outPrevGeo[pxCoord] = half3(viewspaceZ, encodedWorldNormal);

	float ao = 0;
	float3 radiance = 0;
#ifdef SSGI_SH
	float3 direction = 0;
#endif

	CalculateGI(pxCoord, uv, viewspaceZ, viewspaceNormal, ao, radiance
#ifdef SSGI_SH
		,
		direction
#endif
	);

	radiance = filterNaN(radiance);

#ifdef SSGI_SH
	float4 sh1;
	outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackSh(radiance, ao, direction, sh1, true);
	outSH1[outCoord] = sh1;
#else
	outRadianceHitDist[outCoord] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, ao, true);
#endif
}
