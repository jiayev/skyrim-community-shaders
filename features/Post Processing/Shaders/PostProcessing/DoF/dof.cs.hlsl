////////////////////////////////////////////////////////////////////////////////////////////////////
// Modified by Jiaye
////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Cinematic Depth of Field shader, using scatter-as-gather for ReShade 3.x+
// By Frans Bouma, aka Otis / Infuse Project (Otis_Inf)
// https://fransbouma.com
//
// This shader has been released under the following license:
//
// Copyright (c) 2018-2022 Frans Bouma
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Original shader version history:
// 16-aug-2023:	   v1.2.10: Added Cone Overlap support so the HDR conversion first desaturates the colors so channels with a high value don't
//                          exponentially boost to irrealistic values. Contributed by MartyMcFly.
// 26-jun-2023:	   v1.2.9:  Found a way to compensate for edges on close to in-focus geometry shimmering through which were otherwise only removable with the NearFarDistanceCompensation added
//                          in the previous version
// 13-jun-2023:    v1.2.8:  Added the NearFarDistanceCompensation slider for compensating hard edges on geometry that's out of focus but close to the in-focus plane
// 24-jan-2023:    v1.2.7:  Added custom shape support for bokeh highlights. The included shapes were created by Moyevka, Murchalloo, K-putt and others.
// 11-nov-2022:    v1.2.6:  Added bokeh sharpening.
// 28-mar-2022:    v1.2.5:  Made the pre-blur pass optional, as it's not really needed anymore for qualities higher than 4 and reasonable blur values.
// 15-mar-2022:    v1.2.4:  Corrected the LDR to HDR and HDR to LDR conversion functions so they now apply proper gamma correct and boost, so hue shifts are limited now as long
//                          as the highlight boost is kept <= 1
//                          Added Gamma factor for advanced highlight tweaking.
// 11-mar-2022:    v1.2.3:  Changed the sampling stages to use full HDR so there's no more back/forth calculations to SDR along the way. Highlight boost is now
//                          better and upper range has been cranked up.
// 26-feb-2022:    v1.2.2:  Made the highlight boost also be able to go to -1 to dim highlights a bit in bright scenes.
// 22-feb-2022:    v1.2.1:  Removed highlight amplification and properly implemented reinhard-esk de/re-tonemapping for proper highlight calculations. Thanks Marty McFly for the tips.
//                          (1.2.1) small adjustment, added a boost for the highlights which could help in dimly lit scenes. Based on simple levels math.
// 01-jan-2021:    v1.1.19: Corrected PS_PostSmoothing2AndFocusing's signature as it contained a redundant argument which caused warnings in newer versions of reshade.
// 23-oct-2020:    v1.1.18: Near-plane bleed blurred the unblurred far plane which leads to artifacts around edges in some cases. This has been rolled back to the earlier versions of
//                          using the blurred far plane (if any). Also added mirroring to the samplers so edges of the screen aren't blurring darker into the result but should be much smoother.
// 26-mar-2020:    v1.1.17: FreeStyle support added (not yet ansel superres compatible). Fixed issue with far plane highlight causing near plane edge pixels getting highlighted.
// 15-mar-2020:    v1.1.16: Dithering added for low-luma areas to avoid banding. (Contributed by Prod80)
// 03-feb-2020:    v1.1.15: Experimental near plane edge blur improvements.
// 04-oct-2019:    v1.1.14: Fine-tuning of near plane blur using smaller tiles.
// 23-jun-2019:    v1.1.13: Cleanup of highlight code, reimplementing of luma boost / highlightblending. Removal of unnecessary controls.
// 13-jun-2019:    v1.1.12: Bugfix in maxColor blending in near/far blur: no more dirty edges on large highlighted areas.
// 10-jun-2019:	   v1.1.11: Added new weight calculation, added near-plane highlight normalization.
// 25-may-2019:	   v1.1.10: Added white boost/correction in gathering passes to have lower-intensity highlights become less prominent.
//							Added further weight adjustment tweaks. Changed highlight defaults to utilize code changed in 1.1.9/1.1.10
// 24-may-2019:		v1.1.9: Better near-plane bleed mask. Better far plane pixel weights so more samples get accepted.
// 02-mar-2019: 	v1.1.8: Added anamorphic bokeh support, so bokehs now get stretched and rotated based on the distance from the center of the screen, with various tweaks.
// 08-jan-2019:		v1.1.7: Added 9-tap tent filter as described in [Jimenez2014) for mitigating undersampling. Implementation is from KinoBokeh (see credits below).
// 02-jan-2019:		v1.1.6: When near plane max blur is set to 0, the original fragment is now used in the near plane instead of the half-res pixel.
// 19-dec-2018:		v1.1.5: Added far plane highlight normalizing for non-gained highlights. Added tooltip for reshade v4.x
// 14-dec-2018:		v1.1.4: Far plane weight calculation tweaked a bit as near-focus plane elements could lead to hard edges which looked ugly. Highlight far plane
//							adjustments have been reworked because of this.
// 10-dec-2018:		v1.1.3: Removed averaging pass for CoC values as it resulted in noticeable wrong CoC values around edges in some TAA using games. The net result
//							was minimal anyway.
// 10-nov-2018:		v1.1.2: Near plane bugfix: tile gatherer should collect min CoC, not average of min CoC: now ends of narrow lines are properly handled too.
// 30-oct-2018:		v1.1.1: Near plane bugfix for high resolutions: it's now blurring resolution independently. Highlight bleed fix in near focus.
// 21-oct-2018:		v1.1.0: Far plane weights adjustment, half-res with upscale combiner for performance, new highlights implementation, fixed
//							pre-blur highlight smoothing.
// 10-oct-2018:		v1.0.8: Improved, tile-based near-plane bleed, optimizations, far-plane large CoC bleed limitation, Highlight dimming, fixed in-focus
// 						    bleed with post-smooth blur, fixed highlight edges, fixed pre-blur.
// 21-sep-2018:		v1.0.7: Better near-plane bleed. Optimized near plane CoC storage so less reads are needed.
//							Corrected post-blur bleed. Corrected near plane highlight bleed. Overall micro-optimizations.
// 04-sep-2018:		v1.0.6: Small fix for DX9 and autofocus.
// 17-aug-2018:		v1.0.5: Much better highlighting, higher range for manual focus
// 12-aug-2018:		v1.0.4: Finetuned the workaround for d3d9 to only affect reshade 3.4 or lower.
//							Finetuned the near highlight extrapolation a bit. Removed highlight threshold as it ruined the blur
// 10-aug-2018:		v1.0.3: Daodan's crosshair code added.
// 09-aug-2018:		v1.0.2: Added workaround for d3d9 glitch in reshade 3.4.
// 08-aug-2018:		v1.0.1: namespace addition for samplers/textures.
// 08-aug-2018:		v1.0.0: beta. Feature complete.
//
////////////////////////////////////////////////////////////////////////////////////////////////////
// Additional credits:
// Reinhard de/retonemapping for highlighting information thanks to Marty McFly.
// Gaussian blur code based on the Gaussian blur ReShade shader by Ioxa
// Thanks to Daodan for the crosshair code in the focus helper.
// 9 tap tent filter is from KinoBokeh Copyright (C) 2015 Keijiro Takahashi. MIT licensed. See file below for details.
//       Ref:  https://github.com/keijiro/KinoBokeh/blob/master/Assets/Kino/Bokeh/Shader/Composition.cginc
// Thanks to Prod80 for contributing dithering in combiner to avoid banding in low-luma blurred areas.
////////////////////////////////////////////////////////////////////////////////////////////////////
// References:
//
// [Lee2008]		Sungkil Lee, Gerard Jounghyun Kim, and Seungmoon Choi: Real-Time Depth-of-Field Rendering Using Point Splatting
//					on Per-Pixel Layers.
//					https://pdfs.semanticscholar.org/80f6/f40fe971eddc810c3c86fca6fdfe5c0fdd76.pdf
//
// [Jimenez2014]	Jorge Jimenez, Sledgehammer Games: Next generation post processing in Call of Duty Advanced Warfare, SIGGRAPH2014
//					http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
//
// [Nilsson2012]	Filip Nilsson: Implementing realistic depth of field in OpenGL.
//					http://fileadmin.cs.lth.se/cs/education/edan35/lectures/12dof.pdf
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Common/Color.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

RWTexture2D<float4> RWTexOut : register(u0);
RWTexture2D<float> RWFocus : register(u1);
RWTexture2D<float> RWTexCoC : register(u2);

SamplerState LinearSampler : register(s0);

Texture2D<float4> TexColor : register(t0);
Texture2D<float> TexPreviousFocus : register(t1);
Texture2D<float> DepthTexture : register(t2);
Texture2D<float> TexCoCInput : register(t3);
Texture2D<float> TexCoCBlurredInput : register(t4);
Texture2D<float4> TexFarBlur : register(t5);
Texture2D<float4> TexNearBlur : register(t6);
Texture2D<float4> TexPostSmoothInput : register(t7);
Texture2D<float4> TexBokehShape : register(t8);

cbuffer DoFCB : register(b1)
{
	float TransitionSpeed;
	float2 FocusCoord;
	float ManualFocusPlane;
	float FocalLength;
	float FNumber;
	float FarPlaneMaxBlur;
	float NearPlaneMaxBlur;
	float BlurQuality;
	float NearFarDistanceCompensation;
	float BokehBusyFactor;
	float HighlightBoost;
	float PostBlurSmoothing;
	uint HighlightShape;
	float HighlightShapeRotationAngle;
	float PetzvalStrength;
	uint AutoFocus;
	float MaxNearCoCRadius;
	float MaxFarCoCRadius;
	uint pad;
};

// Sensor width the FocalLength control is expressed for (35mm full frame).
#define SENSOR_WIDTH_MM 36.0f

// --------------------------------------------------------------------------------------------
// CoC units
//
// A CoC value in this shader is a *signed blur disc radius expressed as a fraction of the screen
// width*: negative = near field (in front of the focal plane), positive = far field.
// This matches UE5's DiaphragmDOF convention (`InfinityBackgroundCocRadius` is in horizontal
// ViewportUV units) and makes every threshold below expressible in pixels, which is what the
// gather kernel actually operates in.
// --------------------------------------------------------------------------------------------
static const float cocToPixels = SharedData::BufferDim.x;    // CoC fraction -> full-res pixels
static const float onePixelInCoC = SharedData::BufferDim.z;  // "less than a pixel of blur" == in focus

// Largest offset in the 18-tap gaussian table below, used to normalise the near CoC dilation.
static const float gaussianMaxOffset = 33.4421011704f;
// Near CoC radius (in pixels) at which the near field layer becomes fully opaque.
static const float nearFullOpacityPixels = 8.0f;

struct FocusInfo
{
	float2 texcoord;
	float focusDepth;  // in KM, as stored in the 1x1 focus texture
	float focusDepthInM;
	float focusDepthInMM;
};

float GetDepth(float2 uv)
{
	float depth = DepthTexture.SampleLevel(LinearSampler, uv, 0);
	depth = SharedData::GetScreenDepth(depth) * GAME_UNIT_TO_M * 0.001f;  // in KM
	return max(depth, 1e-6);
}

float PreviousFocus()
{
	return TexPreviousFocus[uint2(0, 0)].x;
}

void FillFocusInfoData(inout FocusInfo toFill)
{
	// The 1x1 focus texture holds the focus distance in KM (see CS_UpdateFocus / GetDepth).
	toFill.focusDepth = PreviousFocus();
	toFill.focusDepthInM = toFill.focusDepth * 1000.0;      // km -> m
	toFill.focusDepthInMM = toFill.focusDepthInM * 1000.0;  // m -> mm
}

// Gets the tap from the shape pointed at with the shapeSampler specified, over the angle specified, from the distance of the center in shapeRingDistance
// Returns in rgb the shape sample, and in a the luma.
float4 GetShapeTap(float angle, float shapeRingDistance)
{
	float2 pointOffsetForShape = 0.f;

	// we have to add 270 degrees to the custom angle, because it's scatter via gather, so a pixel that has to show the top of our shape is *above*
	// the highlight, and the angle has to be 270 degrees to hit it (as sampling the highlight *below it* is what makes it brighter).
	sincos(angle + (Math::TAU * HighlightShapeRotationAngle) + (Math::TAU * 0.75f), pointOffsetForShape.x, pointOffsetForShape.y);
	pointOffsetForShape.y *= -1.0f;
	float2 shapeTapCoords = float2((shapeRingDistance * pointOffsetForShape) + 0.5f);  // shapeRingDistance is [0, 0.5] so no need to multiply with 0.5 again
	float4 shapeTap = TexBokehShape.SampleLevel(LinearSampler, shapeTapCoords, 0);
	shapeTap.a = Color::RGBToLuminance(shapeTap.rgb);
	return shapeTap;
}

float CalculateBlurDiscSize(FocusInfo focusInfo)
{
	float pixelDepth = GetDepth(focusInfo.texcoord);
	float pixelDepthInM = pixelDepth * 1000.0;  // in meter

	// CoC (blur disc DIAMETER on the sensor, in mm) based on [Lee2008]:
	//     CoC = ((f*f) / N) / (S1 - f) * (|Z - S1| / Z)
	// where f = FocalLength (mm), N = FNumber, S1 = focus distance, Z = pixel depth.
	// f and S1 must be in the SAME unit for the (S1 - f) term, so S1 has to be in mm.
	// The (|Z - S1| / Z) term is dimensionless and can stay in meters.
	float focalPlaneOffsetInMM = max(focusInfo.focusDepthInMM - FocalLength, 1e-3f);
	float cocDiameterInMM = (((FocalLength * FocalLength) / FNumber) / focalPlaneOffsetInMM) *
	                        (abs(pixelDepthInM - focusInfo.focusDepthInM) / max(pixelDepthInM, 1e-6f));

	// sensor-space diameter (mm) -> screen-space radius (fraction of the screen width)
	float cocRadius = (0.5f * cocDiameterInMM) * (1.0f / SENSOR_WIDTH_MM);

	// Clamp the kernel so an extreme focus setup can never blow up the gather.
	// Equivalent of UE5's r.DOF.Kernel.MaxForegroundRadius / MaxBackgroundRadius.
	bool isNearField = pixelDepth < focusInfo.focusDepth;
	cocRadius = min(cocRadius, isNearField ? MaxNearCoCRadius : MaxFarCoCRadius);

	return isNearField ? -cocRadius : cocRadius;
}

float GetBlurDiscRadiusFromSource(Texture2D<float> source, float2 texcoord, bool flattenToZero)
{
	float coc = source.SampleLevel(LinearSampler, texcoord, 0).x;
	// we're only interested in negative coc's (near plane). All coc's in focus/far plane are flattened to 0. Return the
	// absolute value of the coc as we're working with positive blurred CoCs (as the sign is no longer needed)
	return (flattenToZero && coc >= 0) ? 0 : abs(coc);
}

// Blurs (dilates) the near field CoC so the near layer can bleed over the geometry in front of it.
// `direction` is float2(1,0) or float2(0,1); the step is scaled so the widest tap lands at roughly
// the largest possible near blur radius.
float PerformSingleValueGaussianBlur(Texture2D<float> source, float2 texcoord, float2 direction, bool flattenToZero)
{
	const float offset[18] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4661974725, 21.4627427973, 23.4592916956, 25.455844494, 27.4524015179, 29.4489630909, 31.445529535, 33.4421011704 };
	const float weight[18] = { 0.033245, 0.0659162217, 0.0636705814, 0.0598194658, 0.0546642566, 0.0485871646, 0.0420045997, 0.0353207015, 0.0288880982, 0.0229808311, 0.0177815511, 0.013382297, 0.0097960001, 0.0069746748, 0.0048301008, 0.0032534598, 0.0021315311, 0.0013582974 };

	float coc = GetBlurDiscRadiusFromSource(source, texcoord, flattenToZero);
	coc *= weight[0];

	float maxNearRadiusInPixels = MaxNearCoCRadius * cocToPixels * max(NearPlaneMaxBlur, 0.0f);
	float2 factorToUse = direction * SharedData::BufferDim.zw * (maxNearRadiusInPixels / gaussianMaxOffset);
	for (int i = 1; i < 18; ++i) {
		float2 coordOffset = factorToUse * offset[i];
		float weightSample = weight[i];
		coc += GetBlurDiscRadiusFromSource(source, texcoord + coordOffset, flattenToZero) * weightSample;
		coc += GetBlurDiscRadiusFromSource(source, texcoord - coordOffset, flattenToZero) * weightSample;
	}

	return min(coc, MaxNearCoCRadius);
}

float3 AccentuateWhites(float3 fragment)
{
	// apply small tow to the incoming fragment, so the whitepoint gets slightly lower than max.
	// We don't need to de-tonemap since we are under HDR.
	return fragment / (HighlightBoost > 0.f ? max((1.001 - (HighlightBoost * fragment)), 0.001) : 1.0f);
}

// returns 2 vectors, (x,y) are up vector, (z,w) are right vector.
// In: pixelVector which is the current pixel converted into a vector where (0,0) is the center of the screen.
float2 ApplyPetzvalMorph(float2 pointOffset, float2 texcoord)
{
	float2 centeredUV = texcoord;
	float2 fromCenter = centeredUV - 0.5f;
	float distanceFromCenter = length(fromCenter);
	float radius = saturate(distanceFromCenter * 2.0f);
	if (PetzvalStrength <= 0.001f || distanceFromCenter <= 0.0001f)
		return pointOffset;

	float2 radialAxis = fromCenter / distanceFromCenter;
	float2 tangentialAxis = float2(-radialAxis.y, radialAxis.x);
	float radialComponent = dot(pointOffset, radialAxis);
	float tangentialComponent = dot(pointOffset, tangentialAxis);
	float petzvalAmount = PetzvalStrength * radius * radius * 1.35f;
	float tangentialScale = 1.0f + petzvalAmount;
	float radialScale = rcp(tangentialScale);

	return radialAxis * (radialComponent * radialScale) + tangentialAxis * (tangentialComponent * tangentialScale);
}

// Scatter-as-gather intersection test: how much of the sample's blur disc covers this fragment.
// Both arguments are in full-res PIXELS, so the +0.5 is the usual half-pixel anti-aliasing term
// (cf. UE5 DOFGatherKernel.ush ComputeSampleIntersection).
float CalculateSampleWeight(float sampleRadiusInPixels, float ringDistanceInPixels)
{
	return saturate(sampleRadiusInPixels - (ringDistanceInPixels * NearFarDistanceCompensation) + 0.5);
}

// Clamps a texel coordinate to the buffer. Without this the -1/-3 offsets below underflow the
// unsigned coordinate at the top/left edge of the screen, the out of bounds load returns 0 and the
// min gather collapses to 0, which kills the near field blur along those edges.
int2 ClampToBuffer(int2 coord)
{
	return clamp(coord, int2(0, 0), int2(SharedData::BufferDim.xy) - 1);
}

// Gathers min CoC from a horizontal range of pixels around the pixel at texcoord, for a range of -TILE_SIZE+1 to +TILE_SIZE+1.
// returns minCoC
float PerformTileGatherHorizontal(uint2 DTid)
{
	const int tileSize = 1;
	float minCoC = 10;
	int2 base = int2(DTid);
	for (int i = 1; i <= tileSize + 1; ++i) {
		minCoC = min(minCoC, TexCoCInput[ClampToBuffer(base + int2(i, 0))].r);
		minCoC = min(minCoC, TexCoCInput[ClampToBuffer(base - int2(i, 0))].r);
	}
	return minCoC;
}

// Gathers min CoC from a vertical range of pixels around the pixel at texcoord from the high-res focus plane, for a range of -TILE_SIZE+1 to +TILE_SIZE+1.
// returns min CoC
float PerformTileGatherVertical(uint2 DTid)
{
	const int tileSize = 1;
	float minCoC = 10;
	int2 base = int2(DTid);
	for (int i = 1; i <= tileSize + 1; ++i) {
		minCoC = min(minCoC, TexCoCInput[ClampToBuffer(base + int2(0, i))].r);
		minCoC = min(minCoC, TexCoCInput[ClampToBuffer(base - int2(0, i))].r);
	}
	return minCoC;
}

// Gathers the min CoC of the tile at texcoord and the 8 tiles around it.
float PerformNeighborTileGather(uint2 DTid)
{
	const int tileSizeX = 1;
	const int tileSizeY = 1;
	float minCoC = 10;
	int2 base = int2(DTid);
	// tile is TILE_SIZE*2+1 wide. So add that and substract that to get to neighbor tile right/left.
	// 3x3 around center.
	int2 baseOffset = int2(tileSizeX * 2 + 1, tileSizeY * 2 + 1);
	for (int i = -1; i < 2; i++) {
		for (int j = -1; j < 2; j++) {
			int2 coordOffset = int2(baseOffset.x * i, baseOffset.y * j);
			minCoC = min(minCoC, TexCoCInput[ClampToBuffer(base + coordOffset)].r);
		}
	}
	return minCoC;
}

float4 PerformFullFragmentGaussianBlur(Texture2D source, float2 texcoord, uint2 DTid, float2 offsetWeight)
{
	float offset[6] = { 0.0, 1.4584295168, 3.40398480678, 5.3518057801, 7.302940716, 9.2581597095 };
	float weight[6] = { 0.13298, 0.23227575, 0.1353261595, 0.0511557427, 0.01253922, 0.0019913644 };

	float coc = TexCoCInput[DTid].r;
	float4 fragment = source[DTid];
	float fragmentLuma = Color::RGBToLuminance(fragment.rgb);
	float4 originalFragment = fragment;
	float absoluteCoC = abs(coc);

	if (absoluteCoC < onePixelInCoC || PostBlurSmoothing < 0.01 || fragmentLuma < 0.3) {
		// in focus or postblur smoothing isn't enabled or not really a highlight, ignore
		return fragment;
	}

	fragment *= weight[0];
	float2 factorToUse = offsetWeight * PostBlurSmoothing;

	for (int i = 1; i < 6; ++i) {
		float2 coordOffset = factorToUse * offset[i];
		float weightSample = weight[i];
		float sampleCoC = TexCoCInput.SampleLevel(LinearSampler, texcoord + coordOffset, 0).r;
		float maskFactor = abs(sampleCoC) < onePixelInCoC;

		fragment += (originalFragment * maskFactor * weightSample) +
		            (source.SampleLevel(LinearSampler, texcoord + coordOffset, 0) * (1 - maskFactor) * weightSample);

		sampleCoC = TexCoCInput.SampleLevel(LinearSampler, texcoord - coordOffset, 0).r;
		maskFactor = abs(sampleCoC) < onePixelInCoC;

		fragment += (originalFragment * maskFactor * weightSample) +
		            (source.SampleLevel(LinearSampler, texcoord - coordOffset, 0) * (1 - maskFactor) * weightSample);
	}
	return fragment;
}

[numthreads(1, 1, 1)] void CS_UpdateFocus(uint2 DTid : SV_DispatchThreadID) {
	float depth = AutoFocus ? GetDepth(FocusCoord) : ManualFocusPlane;
	float previousFocus = TexPreviousFocus[uint2(0, 0)];
	RWFocus[DTid] = lerp(previousFocus, depth, TransitionSpeed);
}

	[numthreads(8, 8, 1)] void CS_CalculateCoC(uint2 DTid : SV_DispatchThreadID)
{
	if (DTid.x >= (uint)SharedData::BufferDim.x || DTid.y >= (uint)SharedData::BufferDim.y)
		return;

	float2 uv = (DTid.xy + 0.5f) * SharedData::BufferDim.zw;
	float4 color = TexColor[DTid];

	FocusInfo focusInfo;
	focusInfo.texcoord = uv;
	FillFocusInfoData(focusInfo);

	float coc = CalculateBlurDiscSize(focusInfo);
	RWTexCoC[DTid] = coc;
}

[numthreads(8, 8, 1)] void CS_CoCTile1(uint2 DTid : SV_DispatchThreadID) {
	RWTexCoC[DTid] = PerformTileGatherHorizontal(DTid);
}

	[numthreads(8, 8, 1)] void CS_CoCTile2(uint2 DTid : SV_DispatchThreadID)
{
	RWTexCoC[DTid] = PerformTileGatherVertical(DTid);
}

[numthreads(8, 8, 1)] void CS_CoCTileNeighbor(uint2 DTid : SV_DispatchThreadID) {
	RWTexCoC[DTid] = PerformNeighborTileGather(DTid);
}

	[numthreads(8, 8, 1)] void CS_CoCGaussian1(uint2 DTid : SV_DispatchThreadID)
{
	float2 uv = 2.0f * (DTid.xy + 0.5f) * SharedData::BufferDim.zw;
	RWTexCoC[DTid] = PerformSingleValueGaussianBlur(TexCoCInput, uv, float2(1.0f, 0.0f), true);
}

[numthreads(8, 8, 1)] void CS_CoCGaussian2(uint2 DTid : SV_DispatchThreadID) {
	float2 uv = 2.0f * (DTid.xy + 0.5f) * SharedData::BufferDim.zw;
	RWTexCoC[DTid] = PerformSingleValueGaussianBlur(TexCoCInput, uv, float2(0.0f, 1.0f), false);
}

	// Pre pass: half res downsample of the scene colour with the highlight boost applied.
	[numthreads(8, 8, 1)] void CS_Blur(uint2 DTid : SV_DispatchThreadID)
{
	float2 uv = 2.0f * (DTid.xy + 0.5f) * SharedData::BufferDim.zw;
	float4 fragment = TexColor.SampleLevel(LinearSampler, uv, 0);
	fragment.rgb = AccentuateWhites(fragment.rgb);
	RWTexOut[DTid] = fragment;
}

[numthreads(8, 8, 1)] void CS_FarBlur(uint2 DTid : SV_DispatchThreadID) {
	float2 texcoord = 2.0f * (DTid.xy + 0.5f) * SharedData::BufferDim.zw;
	float numberOfRings = round(BlurQuality);

	const float pointsFirstRing = 7;  // each ring has a multiple of this value of sample points.
	float4 color = TexColor[DTid];
	float colorRadius = TexCoCInput[2 * DTid].r;
	// we'll not process near plane fragments as they're processed in a separate pass.
	if (colorRadius < onePixelInCoC || FarPlaneMaxBlur <= 0) {
		// near plane fragment, will be done in near plane pass
		RWTexOut[DTid] = color;
		return;
	}

	// gather kernel radius for this fragment, in full-res pixels
	float kernelRadiusInPixels = colorRadius * FarPlaneMaxBlur * cocToPixels;

	float bokehBusyFactorToUse = saturate(1.0 - BokehBusyFactor);  // use the busy factor as an edge bias on the blur, not the highlights
	float4 average = float4(color.rgb * bokehBusyFactorToUse, bokehBusyFactorToUse);
	float2 pointOffset = float2(0, 0);
	float2 ringRadiusDeltaCoords = (SharedData::BufferDim.zw * kernelRadiusInPixels) / numberOfRings;
	float2 currentRingRadiusCoords = ringRadiusDeltaCoords;
	float pixelsPerRing = kernelRadiusInPixels / numberOfRings;
	float ringDistanceInPixels = 0;
	float pointsOnRing = pointsFirstRing;
	bool useShape = HighlightShape > 0;
	float4 shapeTap = float4(1.0f, 1.0f, 1.0f, 1.0f);
	for (float ringIndex = 0; ringIndex < numberOfRings; ringIndex++) {
		float anglePerPoint = Math::TAU / pointsOnRing;
		float angle = anglePerPoint;
		float ringWeight = lerp(ringIndex / numberOfRings, 1, bokehBusyFactorToUse);
		ringDistanceInPixels += pixelsPerRing;
		float shapeRingDistance = ((ringIndex + 1) / numberOfRings) * 0.5f;
		for (float pointNumber = 0; pointNumber < pointsOnRing; pointNumber++) {
			sincos(angle, pointOffset.y, pointOffset.x);
			// shapeLuma is in Alpha
			if (useShape)
				shapeTap = GetShapeTap(angle, shapeRingDistance);
			pointOffset = ApplyPetzvalMorph(pointOffset, texcoord);
			float2 tapCoords = float2(texcoord + (pointOffset * currentRingRadiusCoords));
			float sampleRadius = TexCoCInput.SampleLevel(LinearSampler, tapCoords, 0).r;
			float4 tap = 0;
			float weight = (sampleRadius >= 0) * ringWeight * CalculateSampleWeight(sampleRadius * FarPlaneMaxBlur * cocToPixels, ringDistanceInPixels) * (shapeTap.a > 0.01 ? 1.0f : 0.0f);
			// adjust the weight for samples which are in front of the fragment, as they have to get their weight boosted so we don't see edges bleeding through.
			// as otherwise they'll get a weight that's too low relatively to the pixels sampled from the plane the fragment is in.The 3.0 value is empirically determined.
			weight *= (1.0 + min(FarPlaneMaxBlur, 3.0f) * saturate((colorRadius - sampleRadius) * cocToPixels));
			if (weight > 0)
				tap = TexColor.SampleLevel(LinearSampler, tapCoords, 0);
			average.rgb += tap.rgb * weight;
			average.w += weight;
			angle += anglePerPoint;
		}
		pointsOnRing += pointsFirstRing;
		currentRingRadiusCoords += ringRadiusDeltaCoords;
	}
	color.rgb = average.rgb / (average.w + (average.w == 0));
	RWTexOut[DTid] = color;
}

	[numthreads(8, 8, 1)] void CS_NearBlur(uint2 DTid : SV_DispatchThreadID)
{
	float2 texcoord = 2.0f * (DTid.xy + 0.5f) * SharedData::BufferDim.zw;

	float4 color = TexColor[DTid];
	// blurred (dilated, always positive) near CoC and the original signed CoC for this fragment.
	float blurredCoC = TexCoCBlurredInput[DTid];
	float pixelCoC = TexCoCInput[2 * DTid];

	if (blurredCoC <= onePixelInCoC || NearPlaneMaxBlur <= 0) {
		// the blurred CoC value is still 0, we'll never end up with a pixel that has a different value than color, so abort now by
		// returning the color we already read.
		color.a = 0;
		RWTexOut[DTid] = color;
		return;
	}

	// use one extra ring as undersampling is really prominent in near-camera objects.
	float numberOfRings = max(round(BlurQuality), 1) + 1;
	float pointsFirstRing = 7;
	float bokehBusyFactorToUse = saturate(1.0 - BokehBusyFactor);  // use the busy factor as an edge bias on the blur, not the highlights
	float4 average = float4(color.rgb * bokehBusyFactorToUse, bokehBusyFactorToUse);
	float2 pointOffset = float2(0, 0);
	float kernelRadiusInPixels = blurredCoC * NearPlaneMaxBlur * cocToPixels;
	float2 ringRadiusDeltaCoords = SharedData::BufferDim.zw * (kernelRadiusInPixels / (numberOfRings - 1));
	float pointsOnRing = pointsFirstRing;
	float2 currentRingRadiusCoords = ringRadiusDeltaCoords;
	bool useShape = HighlightShape > 0;
	float4 shapeTap = float4(1.0f, 1.0f, 1.0f, 1.0f);
	for (float ringIndex = 0; ringIndex < numberOfRings; ringIndex++) {
		float anglePerPoint = Math::TAU / pointsOnRing;
		float angle = anglePerPoint;
		// no further weight needed, bleed all you want.
		float weight = lerp(ringIndex / numberOfRings, 1, smoothstep(0, 1, bokehBusyFactorToUse));
		float shapeRingDistance = ((ringIndex + 1) / numberOfRings) * 0.5f;
		for (float pointNumber = 0; pointNumber < pointsOnRing; pointNumber++) {
			sincos(angle, pointOffset.y, pointOffset.x);
			// shapeLuma is in Alpha
			if (useShape)
				shapeTap = GetShapeTap(angle, shapeRingDistance);
			pointOffset = ApplyPetzvalMorph(pointOffset, texcoord);
			float2 tapCoords = float2(texcoord + (pointOffset * currentRingRadiusCoords));
			float sampleWeight = weight * (shapeTap.a > 0.01 ? 1.0f : 0.0f);
			if (sampleWeight > 0) {
				float4 tap = TexColor.SampleLevel(LinearSampler, tapCoords, 0);
				average.rgb += tap.rgb * sampleWeight;
				average.w += sampleWeight;
			}
			angle += anglePerPoint;
		}
		pointsOnRing += pointsFirstRing;
		currentRingRadiusCoords += ringRadiusDeltaCoords;
	}
	average.rgb /= (average.w + (average.w == 0));

	// Opacity of the near field layer. Expressed in pixels so it is independent of the CoC scale.
	float blurredCoCInPixels = blurredCoC * cocToPixels;
	float pixelCoCInPixels = -pixelCoC * cocToPixels;  // > 0 when this fragment is itself in the near field
	float coverage = (blurredCoCInPixels > 1.0f) ? ((pixelCoC <= 0) ? 2.0f : 1.0f) * blurredCoCInPixels :
	                                               max(blurredCoCInPixels, pixelCoCInPixels);
	float alpha = saturate((min(2.5, NearPlaneMaxBlur) + 0.4) * coverage * (1.0f / nearFullOpacityPixels));

	color.rgb = average.rgb;
	color.a = alpha;
	RWTexOut[DTid] = color;
}

[numthreads(8, 8, 1)] void CS_TentFilter(uint2 DTid : SV_DispatchThreadID) {
	float4 average;
	uint4 offset = uint4(1, 1, -1, 0);
	average = TexColor[DTid - offset.xy];
	average += TexColor[DTid - offset.wy] * 2;
	average += TexColor[DTid - offset.zy];
	average += TexColor[DTid + offset.zw] * 2;
	average += TexColor[DTid] * 4;
	average += TexColor[DTid + offset.xw] * 2;
	average += TexColor[DTid + offset.zy];
	average += TexColor[DTid + offset.wy] * 2;
	average += TexColor[DTid + offset.xy];
	average /= 16;
	RWTexOut[DTid] = average;
}

	[numthreads(8, 8, 1)] void CS_Combiner(uint2 DTid : SV_DispatchThreadID)
{
	float2 uv = (DTid.xy + 0.5f) * SharedData::BufferDim.zw;
	// first blend far plane with original buffer, then near plane on top of that.
	float4 originalFragment = TexColor[DTid];
	originalFragment.rgb = AccentuateWhites(originalFragment.rgb);
	float4 farFragment = TexFarBlur.SampleLevel(LinearSampler, uv, 0);
	float4 nearFragment = TexNearBlur.SampleLevel(LinearSampler, uv, 0);
	float pixelCoC = TexCoCInput[DTid].r;
	// multiply with far plane max blur so if we need to have 0 blur we get full res
	float realCoC = pixelCoC * saturate(FarPlaneMaxBlur);
	// Fully use the (half res) far field once the blur disc is bigger than a couple of pixels, and
	// blend below that so leaving the focal plane doesn't pop in resolution.
	float blendFactor = smoothstep(0.0f, 1.0f, saturate(realCoC / (2.0f * onePixelInCoC)));
	float4 color;
	color = lerp(originalFragment, farFragment, blendFactor);
	color.rgb = lerp(color.rgb, nearFragment.rgb, nearFragment.a * (NearPlaneMaxBlur != 0));
	color.a = 1.0;
	RWTexOut[DTid] = color;
}

[numthreads(8, 8, 1)] void CS_PostSmoothing1(uint2 DTid : SV_DispatchThreadID) {
	float2 uv = (DTid.xy + 0.5f) * SharedData::BufferDim.zw;

	RWTexOut[DTid] = PerformFullFragmentGaussianBlur(TexColor, uv, DTid, float2((SharedData::BufferDim.z), 0.0));
}

	[numthreads(8, 8, 1)] void CS_PostSmoothing2AndFocusing(uint2 DTid : SV_DispatchThreadID)
{
	float2 uv = (DTid.xy + 0.5f) * SharedData::BufferDim.zw;

	float4 color = PerformFullFragmentGaussianBlur(TexPostSmoothInput, uv, DTid, float2(0.0, (SharedData::BufferDim.w)));
	float4 originalColor = TexColor[DTid];

	// Ramp the smoothed result back in over the first few pixels of blur so in-focus geometry is untouched.
	float cocInPixels = abs(TexCoCInput[DTid].r) * cocToPixels;
	color.rgb = lerp(originalColor.rgb, color.rgb, saturate(cocInPixels < 1.0f ? 0.0f : cocInPixels * 0.25f));

	RWTexOut[DTid] = float4(color.rgb, 1.0f);
}