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
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "../Common/FastMath.hlsli"
#include "../Common/GBuffer.hlsli"
#include "../Common/VR.hlsli"
#include "common.hlsli"

#if USE_HALF_FLOAT_PRECISION == 0
#	define PI (3.1415926535897932384626433832795)
#	define HALF_PI (1.5707963267948966192313216916398)
#	define RCP_PI (0.31830988618)
#else
#	define PI ((lpfloat)3.1415926535897932384626433832795)
#	define HALF_PI ((lpfloat)1.5707963267948966192313216916398)
#	define RCP_PI ((lpfloat)0.31830988618)
#endif

Texture2D<lpfloat> srcWorkingDepth : register(t0);
Texture2D<lpfloat4> srcNormal : register(t1);
Texture2D<lpfloat3> srcRadiance : register(t2);  // maybe half-res
Texture2D<uint> srcHilbertLUT : register(t3);
Texture2D<unorm float> srcAccumFrames : register(t4);  // maybe half-res
Texture2D<lpfloat4> srcPrevGI : register(t5);          // maybe half-res

RWTexture2D<lpfloat4> outGI : register(u0);
RWTexture2D<unorm float2> outBentNormal : register(u1);
RWTexture2D<half3> outPrevGeo : register(u2);

lpfloat GetDepthFade(lpfloat depth)
{
	return (lpfloat)saturate((depth - DepthFadeRange.x) * DepthFadeScaleConst);
}

// Engine-specific screen & temporal noise loader
lpfloat2 SpatioTemporalNoise(uint2 pixCoord, uint temporalIndex)  // without TAA, temporalIndex is always 0
{
	float2 noise;
	uint index = srcHilbertLUT.Load(uint3(pixCoord % 64, 0)).x;
	index += 288 * (temporalIndex % 64);  // why 288? tried out a few and that's the best so far (with XE_HILBERT_LEVEL 6U) - but there's probably better :)
	// R2 sequence - see http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
	// https://www.shadertoy.com/view/mts3zN
	return lpfloat2(frac(0.5 + index * float2(0.245122333753, 0.430159709002)));
}

// HBIL pp.29
lpfloat IlIntegral(lpfloat2 integral_factor, lpfloat angle_prev, lpfloat angle_new)
{
	lpfloat sin_prev, cos_prev, sin_new, cos_new;
	sincos(angle_prev, sin_prev, cos_prev);
	sincos(angle_new, sin_new, cos_new);

	lpfloat delta_angle = angle_new - angle_prev;
	return max(0, integral_factor.x * (delta_angle + sin_prev * cos_prev - sin_new * cos_new) + integral_factor.y * (cos_prev * cos_prev - cos_new * cos_new));
}

void CalculateGI(
	uint2 dtid, float2 uv, float viewspaceZ, lpfloat3 viewspaceNormal,
	out lpfloat4 o_currGIAO, out lpfloat3 o_bentNormal)
{
	const float srcScale = SrcFrameDim * RcpTexDim;
	const float outScale = OutFrameDim * RcpTexDim;

	uint eyeIndex = GET_EYE_IDX(uv);
	float2 normalizedScreenPos = ConvertToStereoUV(uv, eyeIndex);

	const lpfloat rcpNumSlices = rcp(NumSlices);
	const lpfloat rcpNumSteps = rcp(NumSteps);

	const lpfloat falloffRange = (lpfloat)EffectFalloffRange * (lpfloat)EffectRadius;
	const lpfloat rcpFalloffRange = rcp(falloffRange);
	const lpfloat falloffFrom = (lpfloat)EffectRadius * ((lpfloat)1 - (lpfloat)EffectFalloffRange);
	const lpfloat falloffMul = -rcpFalloffRange;
	const lpfloat falloffAdd = falloffFrom * rcpFalloffRange + (lpfloat)1.0;

	// quality settings / tweaks / hacks
	// if the offset is under approx pixel size (pixelTooCloseThreshold), push it out to the minimum distance
	const lpfloat pixelTooCloseThreshold = 1.3;
	// approx viewspace pixel size at pixCoord; approximation of NDCToViewspace( uv.xy + ViewportSize.xy, pixCenterPos.z ).xy - pixCenterPos.xy;
	const float2 pixelDirRBViewspaceSizeAtCenterZ = viewspaceZ.xx * (eyeIndex == 0 ? NDCToViewMul.xy : NDCToViewMul.zw) * RcpOutFrameDim;

	lpfloat screenspaceRadius = (lpfloat)EffectRadius / (lpfloat)pixelDirRBViewspaceSizeAtCenterZ.x;
	// this is the min distance to start sampling from to avoid sampling from the center pixel (no useful data obtained from sampling center pixel)
	const lpfloat minS = (lpfloat)pixelTooCloseThreshold / screenspaceRadius;

	//////////////////////////////////////////////////////////////////

	const lpfloat2 localNoise = SpatioTemporalNoise(dtid, FrameIndex);
	const lpfloat noiseSlice = localNoise.x;
	const lpfloat noiseStep = localNoise.y;

	//////////////////////////////////////////////////////////////////

	const float3 pixCenterPos = ScreenToViewPosition(normalizedScreenPos, viewspaceZ, eyeIndex);
	const lpfloat3 viewVec = (lpfloat3)normalize(-pixCenterPos);

	lpfloat visibility = 0;
	lpfloat3 radiance = 0;
	lpfloat3 bentNormal = viewspaceNormal;

	for (uint slice = 0; slice < NumSlices; slice++) {
		lpfloat phi = (PI * rcpNumSlices) * (slice + noiseSlice);
		lpfloat3 directionVec = 0;
		sincos(phi, directionVec.y, directionVec.x);

		// convert to px units for later use
		lpfloat2 omega = lpfloat2(directionVec.x, -directionVec.y) * screenspaceRadius;

		const lpfloat3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec);
		const lpfloat3 axisVec = normalize(cross(orthoDirectionVec, viewVec));

		lpfloat3 projectedNormalVec = viewspaceNormal - axisVec * dot(viewspaceNormal, axisVec);
		lpfloat projectedNormalVecLength = length(projectedNormalVec);
		lpfloat signNorm = (lpfloat)sign(dot(orthoDirectionVec, projectedNormalVec));
		lpfloat cosNorm = saturate(dot(projectedNormalVec, viewVec) / projectedNormalVecLength);

		lpfloat n = signNorm * ACos(cosNorm);

#ifdef BITMASK
		uint bitmask = 0;
#else
		// this is a lower weight target; not using -1 as in the original paper because it is under horizon, so a 'weight' has different meaning based on the normal
		lpfloat2 sincos_n;
		sincos(n, sincos_n.x, sincos_n.y);
		lpfloat lowHorizonCos1 = sincos_n.x;
		const lpfloat lowHorizonCos0 = -lowHorizonCos1;

		lpfloat horizonCos0 = lowHorizonCos0;  //-1;
		lpfloat horizonCos1 = lowHorizonCos1;  //-1;

		lpfloat3 sampleRadiance = 0;
#endif  // BITMASK

		// R1 sequence (http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/)
		lpfloat stepNoise = frac(noiseStep + slice * 0.6180339887498948482);

		[unroll] for (int sideSign = -1; sideSign <= 1; sideSign += 2)
		{
			[loop] for (uint step = 0; step < NumSteps; step++)
			{
				lpfloat s = (step + stepNoise) * rcpNumSteps;
				s *= s;     // default 2 is fine
				s += minS;  // avoid sampling center pixel

				lpfloat2 sampleOffset = s * omega;

				float2 samplePxCoord = dtid + .5 + sampleOffset * sideSign;
				float2 sampleUV = samplePxCoord * RcpOutFrameDim;
				float2 sampleScreenPos = ConvertToStereoUV(sampleUV, eyeIndex);
				[branch] if (any(sampleScreenPos > 1.0) || any(sampleScreenPos < 0.0)) break;

				lpfloat sampleOffsetLength = length(sampleOffset);
				lpfloat mipLevel = (lpfloat)clamp(log2(sampleOffsetLength) - DepthMIPSamplingOffset, 0, 5);
#ifdef HALF_RES
				mipLevel = max(mipLevel, 1);
#endif

				float SZ = srcWorkingDepth.SampleLevel(samplerPointClamp, sampleUV * srcScale, mipLevel);
				[branch] if (SZ > DepthFadeRange.y) continue;

				float3 samplePos = ScreenToViewPosition(sampleScreenPos, SZ, eyeIndex);
				float3 sampleDelta = samplePos - float3(pixCenterPos);
				lpfloat3 sampleHorizonVec = (lpfloat3)normalize(sampleDelta);

#ifdef BITMASK
				float3 sampleBackPos = samplePos - viewVec * Thickness;
				lpfloat3 sampleBackHorizonVec = normalize(sampleBackPos - pixCenterPos);

				lpfloat angleFront = FastACos(dot(sampleHorizonVec, viewVec));  // either clamp or use lpfloat version for whatever reason
				lpfloat angleBack = FastACos(dot(sampleBackHorizonVec, viewVec));
				lpfloat2 angleRange = -sideSign * (sideSign == -1 ? lpfloat2(angleFront, angleBack) : lpfloat2(angleBack, angleFront));
				angleRange = smoothstep(0, 1, (angleRange + n) * RCP_PI + .5);  // https://discord.com/channels/586242553746030596/586245736413528082/1102228968247144570

				uint2 bitsRange = uint2(floor(angleRange.x * 32u), round((angleRange.y - angleRange.x) * 32u));  // ceil gets too gray for flat ground
				uint maskedBits = ((1 << bitsRange.y) - 1) << bitsRange.x;

#else

				// this is our own thickness heuristic that relies on sooner discarding samples behind the center
				lpfloat falloffBase = length(lpfloat3(sampleDelta) * lpfloat3(1, 1, 1 + ThinOccluderCompensation));
				lpfloat weight = saturate(falloffBase * falloffMul + falloffAdd);

				// sample horizon cos
				lpfloat shc = (lpfloat)dot(sampleHorizonVec, viewVec);

				// discard unwanted samples
				shc = lerp(sideSign == -1 ? lowHorizonCos1 : lowHorizonCos0, shc, weight);
				lpfloat horizonCos = sideSign == -1 ? horizonCos1 : horizonCos0;
#endif

#ifdef GI
				float giBoost = 1 + GIDistanceCompensation * smoothstep(0, GICompensationMaxDist, s * EffectRadius);

#	ifdef BITMASK
				bool checkGI = maskedBits;
#	else
				bool checkGI = shc > horizonCos;
#	endif

				if (checkGI) {
					// IL
					lpfloat frontBackMult = 1.f;
#	ifdef BACKFACE
					if (dot(DecodeNormal(srcNormal.SampleLevel(samplerPointClamp, sampleUV * srcScale, 0).xy), sampleHorizonVec) > 0)  // backface
						frontBackMult = BackfaceStrength;
#	endif

					if (frontBackMult > 0.f) {
#	ifdef BITMASK
						lpfloat3 sampleRadiance = srcRadiance.SampleLevel(samplerPointClamp, sampleUV * outScale, mipLevel).rgb * frontBackMult * giBoost;

						sampleRadiance *= countbits(maskedBits & ~bitmask) * (lpfloat)0.03125;  // 1/32
						sampleRadiance *= dot(viewspaceNormal, sampleHorizonVec);
						sampleRadiance = max(0, sampleRadiance);

						radiance += sampleRadiance;
#	else
						lpfloat3 newSampleRadiance = 0;
						newSampleRadiance = srcRadiance.SampleLevel(samplerPointClamp, sampleUV * outScale, mipLevel).rgb * frontBackMult * giBoost;

						lpfloat anglePrev = n + sideSign * HALF_PI - FastACos(horizonCos);  // lpfloat version is closest acos
						lpfloat angleCurr = n + sideSign * HALF_PI - FastACos(shc);
						lpfloat2 integralFactor = 0.5 * lpfloat2(dot(directionVec.xy, viewspaceNormal.xy) * sideSign, viewspaceNormal.z);
						newSampleRadiance *= IlIntegral(integralFactor, anglePrev, angleCurr);

						// depth filtering. HBIL pp.38
						lpfloat t = smoothstep(0, 1, dot(viewspaceNormal, sampleHorizonVec));
						sampleRadiance = lerp(sampleRadiance, newSampleRadiance, t);

						radiance += max(0, sampleRadiance);
#	endif
					}
#	ifndef BITMASK
					horizonCos = shc;
#	endif
				}
#else
#	ifndef BITMASK
				// 					// thickness heuristic - see "4.3 Implementation details, Height-field assumption considerations"
				// #if 0    // (disabled, not used) this should match the paper
				//                 lpfloat newhorizonCos = max( horizonCos, shc );

				//                 horizonCos = (horizonCos > shc)? lerp( newhorizonCos, shc, ThinOccluderCompensation ) :newhorizonCos ;
				// #elif 0  // (disabled, not used) this is slightly different from the paper but cheaper and provides very similar results
				// 				horizonCos = lerp(max(horizonCos, shc), shc, ThinOccluderCompensation);
				// #else  // this is a version where thicknessHeuristic is completely disabled
				horizonCos = max(horizonCos, shc);
// #endif
#	endif
#endif  // GI

#ifdef BITMASK
				bitmask |= maskedBits;
#else
				if (sideSign == -1)
					horizonCos1 = horizonCos;
				else
					horizonCos0 = horizonCos;
#endif
			}
		}

#ifdef BITMASK
		visibility += (lpfloat)1.0 - countbits(bitmask) * (lpfloat)0.03125;

		// TODO: bent normal for bitmask?
#else
#	if 1  // I can't figure out the slight overdarkening on high slopes, so I'm adding this fudge - in the training set, 0.05 is close (PSNR 21.34) to disabled (PSNR 21.45)
		projectedNormalVecLength = lerp(projectedNormalVecLength, 1, 0.05);
#	endif

		// line ~27, unrolled
		lpfloat h0 = -FastACos(horizonCos1);  // same, breaks stuff
		lpfloat h1 = FastACos(horizonCos0);
#	if 0  // we can skip clamping for a tiny little bit more performance
            h0 = n + clamp( h0-n, (lpfloat)-HALF_PI, (lpfloat)HALF_PI );
            h1 = n + clamp( h1-n, (lpfloat)-HALF_PI, (lpfloat)HALF_PI );
#	endif
		lpfloat iarc0 = ((lpfloat)cosNorm + (lpfloat)2 * (lpfloat)h0 * (lpfloat)sincos_n.x - (lpfloat)cos((lpfloat)2 * (lpfloat)h0 - n));
		lpfloat iarc1 = ((lpfloat)cosNorm + (lpfloat)2 * (lpfloat)h1 * (lpfloat)sincos_n.x - (lpfloat)cos((lpfloat)2 * (lpfloat)h1 - n));
		lpfloat localVisibility = (lpfloat)projectedNormalVecLength * (lpfloat)(iarc0 + iarc1) * (lpfloat).25;
		visibility += localVisibility;

#	ifdef BENT_NORMAL
		// see "Algorithm 2 Extension that computes bent normals b."
		lpfloat2 sincos_3h0mn, sincos_3h1mn, sincos_h0pn, sincos_h1pn;
		sincos(3 * h0 - n, sincos_3h0mn.x, sincos_3h0mn.y);
		sincos(3 * h1 - n, sincos_3h1mn.x, sincos_3h1mn.y);
		sincos(h0 + n, sincos_h0pn.x, sincos_h0pn.y);
		sincos(h1 + n, sincos_h1pn.x, sincos_h1pn.y);

		lpfloat t0 = (6 * sin(h0 - n) - sincos_3h0mn.x + 6 * sin(h1 - n) - sincos_3h1mn.x + 16 * sincos_n.x - 3 * (sincos_h0pn.x + sincos_h1pn.x)) * 0.08333333333;  // 1/12
		lpfloat t1 = (-sincos_3h0mn.y - sincos_3h1mn.y + 8 * sincos_n.y - 3 * (sincos_h0pn.y + sincos_h1pn.y)) * 0.08333333333;
		lpfloat3 localBentNormal = lpfloat3(directionVec.x * t0, directionVec.y * t0, -t1);
		localBentNormal = (lpfloat3)mul(RotFromToMatrix(lpfloat3(0, 0, -1), viewVec), localBentNormal) * projectedNormalVecLength;
		bentNormal += localBentNormal;
#	endif
#endif  // BITMASK
	}

	lpfloat depthFade = GetDepthFade(viewspaceZ);

	visibility *= rcpNumSlices;
	visibility = lerp(saturate(visibility), 1, depthFade);
	visibility = pow(visibility, AOPower);

#ifdef GI
	radiance *= rcpNumSlices;
	radiance = lerp(radiance, 0, depthFade);
	radiance *= GIStrength;
#endif

#ifdef BENT_NORMAL
	bentNormal = normalize(bentNormal);
#endif

	o_currGIAO = lpfloat4(radiance, visibility);
	o_bentNormal = bentNormal;
}

[numthreads(8, 8, 1)] void main(const uint2 dtid
								: SV_DispatchThreadID) {
	const float srcScale = SrcFrameDim * RcpTexDim;
	const float outScale = OutFrameDim * RcpTexDim;

	float2 uv = (dtid + .5f) * RcpOutFrameDim;
	uint eyeIndex = GET_EYE_IDX(uv);

	float viewspaceZ = READ_DEPTH(srcWorkingDepth, dtid);

	lpfloat2 normalSample = FULLRES_LOAD(srcNormal, dtid, uv * srcScale, samplerLinearClamp).xy;
	lpfloat3 viewspaceNormal = (lpfloat3)DecodeNormal(normalSample);

	half2 encodedWorldNormal = EncodeNormal(ViewToWorldVector(viewspaceNormal, InvViewMatrix[eyeIndex]));
	outPrevGeo[dtid] = half3(viewspaceZ, encodedWorldNormal);

// Move center pixel slightly towards camera to avoid imprecision artifacts due to depth buffer imprecision; offset depends on depth texture format used
#if USE_HALF_FLOAT_PRECISION == 1
	viewspaceZ *= 0.99920h;  // this is good for FP16 depth buffer
#else
	viewspaceZ *= 0.99999;  // this is good for FP32 depth buffer
#endif

	lpfloat4 currGIAO = lpfloat4(0, 0, 0, 1);
	lpfloat3 bentNormal = viewspaceNormal;
	[branch] if (viewspaceZ < DepthFadeRange.y)
		CalculateGI(
			dtid, uv, viewspaceZ, viewspaceNormal,
			currGIAO, bentNormal);

#ifdef BENT_NORMAL
	outBentNormal[dtid] = EncodeNormal(bentNormal);
#endif

#ifdef TEMPORAL_DENOISER
	if (viewspaceZ < DepthFadeRange.y) {
		lpfloat4 prevGIAO = srcPrevGI[dtid];
		uint accumFrames = srcAccumFrames[dtid] * 255;

		currGIAO = lerp(prevGIAO, currGIAO, rcp(accumFrames));
	}
#endif

	currGIAO = any(ISNAN(currGIAO)) ? lpfloat4(0, 0, 0, 1) : currGIAO;

	outGI[dtid] = currGIAO;
}