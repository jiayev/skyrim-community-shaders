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
// Modified by Jiaye for use in Community Shaders
//
////////////////////////////////////////////////////////////////////////////////////////////////////

struct VSFOCUSINFO
{
    float4 vpos : SV_Position;
    float2 texcoord : TEXCOORD0;
    float focusDepth : TEXCOORD1;
    float focusDepthInM : TEXCOORD2;
    float focusDepthInMM : TEXCOORD3;
    float pixelSizeLength : TEXCOORD4;
    float nearPlaneInMM : TEXCOORD5;
    float farPlaneInMM : TEXCOORD6;
};

struct VSDISCBLURINFO
{
    float4 vpos : SV_Position;
    float2 texcoord : TEXCOORD0;
    float numberOfRings : TEXCOORD1;
    float farPlaneMaxBlurInPixels : TEXCOORD2;
    float nearPlaneMaxBlurInPixels : TEXCOORD3;
    float cocFactorPerPixel : TEXCOORD4;
    float highlightBoostFactor: TEXCOORD5;
};

#define SENSOR_SIZE			0.024		// Height of the 35mm full-frame format (36mm x 24mm)
#define PI 					3.1415926535897932
#define TILE_SIZE			1			// amount of pixels left/right/up/down of the current pixel. So 4 is 9x9

Texture2D<float4> InputTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
Texture2D<float> CurrentFocusTexture : register(t2);
Texture2D<float> PreviousFocusTexture : register(t3);
Texture2D<float4> Buffer1Texture : register(t4);
Texture2D<float4> Buffer2Texture : register(t5);
Texture2D<float4> Buffer3Texture : register(t6);
Texture2D<float4> Buffer4Texture : register(t7);
Texture2D<float4> Buffer5Texture : register(t8);
Texture2D<float> CoCTexture : register(t9);
Texture2D<float> CoCTmp1Texture : register(t10);
Texture2D<float2> CoCBlurredTexture : register(t11);
Texture2D<float> CoCTileTmpTexture : register(t12);
Texture2D<float> CoCTileTexture : register(t13);
Texture2D<float> CoCTileNeighborTexture : register(t14);
Texture2D<float4> ShapeTexture : register(t15);
Texture2D<float4> NoiseTexture : register(t16);

RWTexture2D<float4> OutputTexture : register(u0);

SamplerState ColorSampler : register(s0);
SamplerState BufferSampler : register(s1);
SamplerState CoCSampler : register(s2);
SamplerState NoiseSampler : register(s3);

cbuffer DoFCBuffer : register(b1)
{
    bool UseAutoFocus;
    float2 AutoFocusPoint;
    float AutoFocusTransitionSpeed;
    float ManualFocusPlane;
    float FocalLength;
    float FNumber;
    float FarPlaneMaxBlur;
    float NearPlaneMaxBlur;
    float BlurQuality;
    float BokehBusyFactor;
    float PostBlurSmoothing;
    float NearFarDistanceCompensation;
    float HighlightAnamorphicFactor;
    float HighlightAnamorphicSpreadFactor;
    float HighlightAnamorphicAlignmentFactor;
    float HighlightBoost;
    float HighlightGammaFactor;
    float HighlightSharpeningFactor;
    int HighlightShape;
    float HighlightShapeRotationAngle;
    float HighlightShapeGamma;
    bool MitigateUndersampling;
    float ScreenWidth;
    float ScreenHeight;
}


#define BUFFER_SIZE float2(ScreenWidth, ScreenHeight)
#define BUFFER_WIDTH ScreenWidth
#define BUFFER_HEIGHT ScreenHeight
#define BUFFER_SCREEN_SIZE float2(ScreenWidth, ScreenHeight)
#define GROUND_TRUTH_SCREEN_WIDTH 1920
#define GROUND_TRUTH_SCREEN_HEIGHT 1080
#define BUFFER_PIXEL_SIZE float2(1.0/ScreenWidth, 1.0/ScreenHeight)
#define TEXCOORD float2((DTid.xy + 0.5f) / BUFFER_SCREEN_SIZE)

//////////////////////////////////////////////////
//
// Functions
//
//////////////////////////////////////////////////

float GetLinearizedDepth(float2 texcoord)
{
    float depth = DepthTexture.SampleLevel(ColorSampler, texcoord, 0).r;
    return depth;
}

float3 ConeOverlap(float3 fragment)
{
    float k = 0.4 * 0.33;
    float2 f = float2(1-2 * k, k);
    float3x3 m = float3x3(f.xyy, f.yxy, f.yyx);
    return mul(fragment, m);
}

float3 ConeOverlapInverse(float3 fragment)
{
    float k = 0.4 * 0.33;
    float2 f = float2(k-1, k) * rcp(3 * k-1);
    float3x3 m = float3x3(f.xyy, f.yxy, f.yyx);
    return mul(fragment, m);
}

float3 AccentuateWhites(float3 fragment)
{
    // apply small tow to the incoming fragment, so the whitepoint gets slightly lower than max.
    // De-tonemap color (reinhard). Thanks Marty :) 
    fragment = pow(abs(ConeOverlap(fragment)), HighlightGammaFactor);
    return fragment / max((1.001 - (HighlightBoost * fragment)), 0.001);
}

float3 CorrectForWhiteAccentuation(float3 fragment)
{
    // Re-tonemap color (reinhard). Thanks Marty :) 
    float3 toReturn = fragment / (1.001 + (HighlightBoost * fragment));
    return ConeOverlapInverse(pow(abs(toReturn), 1.0/ HighlightGammaFactor));
}

// returns 2 vectors, (x,y) are up vector, (z,w) are right vector. 
// In: pixelVector which is the current pixel converted into a vector where (0,0) is the center of the screen.
float4 CalculateAnamorphicFactor(float2 pixelVector)
{
    float normalizedFactor = lerp(1, HighlightAnamorphicFactor, lerp(length(pixelVector * 2), 1, HighlightAnamorphicSpreadFactor));
    return float4(0, 1 + (1-normalizedFactor), normalizedFactor, 0);
}

// Calculates a rotation matrix for the current pixel specified in texcoord, which can be used to rotate the bokeh shape to match
// a distored field around the center of the screen: it rotates the anamorphic factors with this matrix so the bokeh shapes form a circle
// around the center of the screen. 
float2x2 CalculateAnamorphicRotationMatrix(float2 texcoord)
{
    float2 pixelVector = normalize(texcoord - 0.5);
    float limiter = (1-HighlightAnamorphicAlignmentFactor)/2;
    pixelVector.y = clamp(pixelVector.y, -limiter, limiter);
    float2 refVector = normalize(float2(-0.5, 0));
    float2 sincosFactor = float2(0,0);
    // calculate the angle between the pixelvector and the ref vector and grab the sin/cos for that angle for the rotation matrix.
    sincos(atan2(pixelVector.y, pixelVector.x) - atan2(refVector.y, refVector.x), sincosFactor.x, sincosFactor.y);
    return float2x2(sincosFactor.y, sincosFactor.x, -sincosFactor.x, sincosFactor.y);
}

float2 MorphPointOffsetWithAnamorphicDeltas(float2 pointOffset, float4 anamorphicFactors, float2x2 anamorphicRotationMatrix)
{
    pointOffset.x = pointOffset.x * anamorphicFactors.x + pointOffset.x*anamorphicFactors.z;
    pointOffset.y = pointOffset.y * anamorphicFactors.y + pointOffset.y*anamorphicFactors.w;
    return mul(pointOffset, anamorphicRotationMatrix);
}

// Gathers min CoC from a horizontal range of pixels around the pixel at texcoord, for a range of -TILE_SIZE+1 to +TILE_SIZE+1.
// returns minCoC
float PerformTileGatherHorizontal(Texture2D<float> tex, float2 texcoord)
{
    float tileSize = TILE_SIZE * (BUFFER_SCREEN_SIZE.x / GROUND_TRUTH_SCREEN_WIDTH);
    float minCoC = 10;
    float coc;
    float2 coordOffset = float2(BUFFER_PIXEL_SIZE.x, 0);
    for(float i = 0; i <= tileSize; ++i) 
    {
        coc = tex.SampleLevel(CoCSampler, texcoord + coordOffset, 0);
        minCoC = min(minCoC, coc);
        coc = tex.SampleLevel(CoCSampler, texcoord - coordOffset, 0);
        minCoC = min(minCoC, coc);
        coordOffset.x+=BUFFER_PIXEL_SIZE.x;
    }
    return minCoC;
}

// Gathers min CoC from a vertical range of pixels around the pixel at texcoord from the high-res focus plane, for a range of -TILE_SIZE+1 to +TILE_SIZE+1.
// returns min CoC
float PerformTileGatherVertical(Texture2D<float> tex, float2 texcoord)
{
    float tileSize = TILE_SIZE * (BUFFER_SCREEN_SIZE.y / GROUND_TRUTH_SCREEN_HEIGHT);
    float minCoC = 10;
    float coc;
    float2 coordOffset = float2(0, BUFFER_PIXEL_SIZE.y);
    for(float i = 0; i <= tileSize; ++i) 
    {
        coc = tex.SampleLevel(CoCSampler, texcoord + coordOffset, 0).r;
        minCoC = min(minCoC, coc);
        coc = tex.SampleLevel(CoCSampler, texcoord - coordOffset, 0).r;
        minCoC = min(minCoC, coc);
        coordOffset.y+=BUFFER_PIXEL_SIZE.y;
    }
    return minCoC;
}

// Gathers the min CoC of the tile at texcoord and the 8 tiles around it. 
float PerformNeighborTileGather(Texture2D<float> tex, float2 texcoord)
{
    float minCoC = 10;
    float tileSizeX = TILE_SIZE * (BUFFER_SCREEN_SIZE.x / GROUND_TRUTH_SCREEN_WIDTH);
    float tileSizeY = TILE_SIZE * (BUFFER_SCREEN_SIZE.y / GROUND_TRUTH_SCREEN_HEIGHT);
    // tile is TILE_SIZE*2+1 wide. So add that and substract that to get to neighbor tile right/left.
    // 3x3 around center.
    float2 baseCoordOffset = float2(BUFFER_PIXEL_SIZE.x * (tileSizeX*2+1), BUFFER_PIXEL_SIZE.x * (tileSizeY*2+1));
    for(float i=-1;i<2;i++)
    {
        for(float j=-1;j<2;j++)
        {
            float2 coordOffset = float2(baseCoordOffset.x * i, baseCoordOffset.y * j);
            float coc = tex.SampleLevel(CoCSampler, texcoord + coordOffset, 0).r;
            minCoC = min(minCoC, coc);
        }
    }
    return minCoC;
}

// Calculates an RGBA fragment based on the CoC radius specified, for debugging purposes.
// In: 	radius, the CoC radius to calculate the fragment for
//		showInFocus, flag which will give a blue edge at the focus plane if true
// Out:	RGBA fragment for color buffer based on the radius specified. 
float4 GetDebugFragment(float radius, bool showInFocus)
{
    float4 toReturn = (radius/2 <= length(BUFFER_PIXEL_SIZE)) && showInFocus ? float4(0.0, 0.0, 1.0, 1.0) : float4(radius, radius, radius, 1.0);
    if(radius < 0)
    {
        toReturn = float4(-radius, 0, 0, 1);
    }
    return toReturn;
}

// Gets the tap from the shape pointed at with the shapeSampler specified, over the angle specified, from the distance of the center in shapeRingDistance
// Returns in rgb the shape sample, and in a the luma.
float4 GetShapeTap(float angle, float shapeRingDistance, Texture2D shapetex)
{
    float2 pointOffsetForShape = float2(0,0);
    
    // we have to add 270 degrees to the custom angle, because it's scatter via gather, so a pixel that has to show the top of our shape is *above*
    // the highlight, and the angle has to be 270 degrees to hit it (as sampling the highlight *below it* is what makes it brighter).
    sincos(angle + (6.28318530717958 * HighlightShapeRotationAngle) + (6.28318530717958 * 0.75f), pointOffsetForShape.x, pointOffsetForShape.y);
    pointOffsetForShape.y*=-1.0f;
    float4 shapeTapCoords = float4((shapeRingDistance * pointOffsetForShape) + 0.5f, 0, 0);	// shapeRingDistance is [0, 0.5] so no need to multiply with 0.5 again
    float4 shapeTap = shapetex.SampleLevel(ColorSampler, shapeTapCoords.xy, 0);
    shapeTap.a = dot(shapeTap.rgb, float3(0.3, 0.59, 0.11));
    return shapeTap;
}

// Calculates the blur disc size for the pixel at the texcoord specified. A blur disc is the CoC size at the image plane.
// In:	VSFOCUSINFO struct filled by the vertex shader VS_Focus
// Out:	The blur disc size for the pixel at texcoord. Format: near plane: < 0. In-focus: 0. Far plane: > 0. Range: [-1, 1].
float CalculateBlurDiscSize(VSFOCUSINFO focusInfo)
{
    float pixelDepth = GetLinearizedDepth(focusInfo.texcoord);
    float pixelDepthInM = pixelDepth * 1000.0;			// in meter

    // CoC (blur disc size) calculation based on [Lee2008]
    // CoC = ((EF / Zf - F) * (abs(Z-Zf) / Z)
    // where E is aperture size in mm, F is focal length in mm, Zf is depth of focal plane in mm, Z is depth of pixel in mm.
    // To calculate aperture in mm, we use D = F/N, where F is focal length and N is f-number
    // For the people getting confused: 
    // Remember element sizes are in mm, our depth sizes are in meter, so we have to divide S1 by 1000 to get from meter -> mm. We don't have to
    // divide the elements in the 'abs(x-S1)/x' part, as the 1000.0 will then simply be muted out (as  a / (x/1000) == a * (1000/x))
    // formula: (((f*f) / N) / ((S1/1000.0) -f)) * (abs(x - S1) / x)
    // where f = FocalLength, N = FNumber, S1 = focusInfo.focusDepthInM, x = pixelDepthInM. In-lined to save on registers. 
    float cocInMM = (((FocalLength*FocalLength) / FNumber) / ((focusInfo.focusDepthInM/1000.0) -FocalLength)) * 
                    (abs(pixelDepthInM - focusInfo.focusDepthInM) / (pixelDepthInM + (pixelDepthInM==0)));
    float toReturn = clamp(saturate(abs(cocInMM) * SENSOR_SIZE), 0, 1); // divide by sensor size to get coc in % of screen (or better: in sampler units)
    return (pixelDepth < focusInfo.focusDepth) ? -toReturn : toReturn;
}


// calculate the sample weight based on the values specified. 
float CalculateSampleWeight(float sampleRadiusInCoC, float ringDistanceInCoC)
{
    return saturate(sampleRadiusInCoC - (ringDistanceInCoC * NearFarDistanceCompensation) + 0.5);
}


// Same as PerformDiscBlur but this time for the near plane. It's in a separate function to avoid a lot of if/switch statements as
// the near plane blur requires different semantics.
// Based on [Nilsson2012] and a variant of [Jimenez2014] where far/in-focus pixels are receiving a higher weight so they bleed into the near plane, 
// In:	blurInfo, the pre-calculated disc blur information from the vertex shader.
// 		source, the source to read RGBA fragments from. Luma in alpha
//		shape, the shape sampler to use if shapes are used.
// Out: RGBA fragment for the pixel at texcoord in source, which is the blurred variant of it if it's in the near plane. A is alpha
// to blend with.
float4 PerformNearPlaneDiscBlur(VSDISCBLURINFO blurInfo, Texture2D tex, Texture2D shapetex)
{
    float4 fragment = tex.SampleLevel(BufferSampler, blurInfo.texcoord, 0);
    // r contains blurred CoC, g contains original CoC. Original is negative.
    float2 fragmentRadii = CoCBlurredTexture.SampleLevel(CoCSampler, blurInfo.texcoord, 0).rg;
    float fragmentRadiusToUse = fragmentRadii.r;

    if(fragmentRadii.r <=0)
    {
        // the blurred CoC value is still 0, we'll never end up with a pixel that has a different value than fragment, so abort now by
        // returning the fragment we already read.
        fragment.a = 0;
        return fragment;
    }
    
    // use one extra ring as undersampling is really prominent in near-camera objects.
    float numberOfRings = max(blurInfo.numberOfRings, 1) + 1;
    float pointsFirstRing = 7;
    // luma is stored in alpha
    float bokehBusyFactorToUse = saturate(1.0-BokehBusyFactor);		// use the busy factor as an edge bias on the blur, not the highlights
    float4 average = float4(fragment.rgb * fragmentRadiusToUse * bokehBusyFactorToUse, bokehBusyFactorToUse);
    float2 pointOffset = float2(0,0);
    float nearPlaneBlurInPixels = blurInfo.nearPlaneMaxBlurInPixels * fragmentRadiusToUse;
    float2 ringRadiusDeltaCoords = BUFFER_PIXEL_SIZE * (nearPlaneBlurInPixels / (numberOfRings-1));
    float pointsOnRing = pointsFirstRing;
    float2 currentRingRadiusCoords = ringRadiusDeltaCoords;
    float4 anamorphicFactors = CalculateAnamorphicFactor(blurInfo.texcoord - 0.5); // xy are up vector, zw are right vector
    float2x2 anamorphicRotationMatrix = CalculateAnamorphicRotationMatrix(blurInfo.texcoord);
    bool useShape = HighlightShape > 0;
    float4 shapeTap = float4(1.0f, 1.0f, 1.0f, 1.0f);
    for(float ringIndex = 0; ringIndex < numberOfRings; ringIndex++)
    {
        float anglePerPoint = 6.28318530717958 / pointsOnRing;
        float angle = anglePerPoint;
        // no further weight needed, bleed all you want. 
        float weight = lerp(ringIndex/numberOfRings, 1, smoothstep(0, 1, bokehBusyFactorToUse));
        float shapeRingDistance = ((ringIndex+1)/numberOfRings) * 0.5f;
        for(float pointNumber = 0; pointNumber < pointsOnRing; pointNumber++)
        {
            sincos(angle, pointOffset.y, pointOffset.x);
            // shapeLuma is in Alpha
            shapeTap = useShape ? GetShapeTap(angle, shapeRingDistance, shapetex) : shapeTap;
            // now transform the offset vector with the anamorphic factors and rotate it accordingly to the rotation matrix, so we get a nice
            // bending around the center of the screen.
            pointOffset = useShape ? pointOffset : MorphPointOffsetWithAnamorphicDeltas(pointOffset, anamorphicFactors, anamorphicRotationMatrix);
            float4 tapCoords = float4(blurInfo.texcoord + (pointOffset * currentRingRadiusCoords), 0, 0);
            float4 tap = tex.SampleLevel(BufferSampler, tapCoords.xy, 0);
            tap.rgb *= useShape ? (shapeTap.rgb * HighlightShapeGamma) : 1.0f;
            // r contains blurred CoC, g contains original CoC. Original can be negative
            float2 sampleRadii = CoCBlurredTexture.SampleLevel(CoCSampler, tapCoords.xy, 0).rg;
            float blurredSampleRadius = sampleRadii.r;
            float sampleWeight = weight * (shapeTap.a > 0.01 ? 1.0f : 0.0f);
            average.rgb += tap.rgb * sampleWeight;
            average.w += sampleWeight ;
            angle+=anglePerPoint;
        }
        pointsOnRing+=pointsFirstRing;
        currentRingRadiusCoords += ringRadiusDeltaCoords;
    }
    
    average.rgb/=(average.w + (average.w ==0));
    float alpha = saturate((min(2.5, NearPlaneMaxBlur) + 0.4) * (fragmentRadiusToUse > 0.1 ? (fragmentRadii.g <=0 ? 2 : 1) * fragmentRadiusToUse : max(fragmentRadiusToUse, -fragmentRadii.g)));
    fragment.rgb = average.rgb;
    fragment.a = alpha;
// #if CD_DEBUG
//     if(ShowNearPlaneAlpha)
//     {
//         fragment = float4(alpha, alpha, alpha, 1.0);
//     }
// #endif
// #if CD_DEBUG
//     if(ShowNearPlaneBlurred)
//     {
//         fragment.a = 1.0;
//     }
// #endif
    return fragment;
}


// Calculates the new RGBA fragment for a pixel at texcoord in source using a disc based blur technique described in [Jimenez2014] 
// (Though without using tiles). Blurs far plane.
// In:	blurInfo, the pre-calculated disc blur information from the vertex shader.
// 		source, the source buffer to read RGBA data from. RGB is in HDR. A not used.
//		shape, the shape sampler to use if shapes are used.
// Out: RGBA fragment that's the result of the disc-blur on the pixel at texcoord in source. A contains luma of pixel.
float4 PerformDiscBlur(VSDISCBLURINFO blurInfo, Texture2D tex, Texture2D shapetex)
{
    const float pointsFirstRing = 7; 	// each ring has a multiple of this value of sample points. 
    float4 fragment = tex.SampleLevel(BufferSampler, blurInfo.texcoord, 0);
    float fragmentRadius = CoCTexture.SampleLevel(CoCSampler, blurInfo.texcoord, 0).r;
    // we'll not process near plane fragments as they're processed in a separate pass. 
    if(fragmentRadius < 0 || blurInfo.farPlaneMaxBlurInPixels <=0)
    {
        // near plane fragment, will be done in near plane pass 
        return fragment;
    }
    float bokehBusyFactorToUse = saturate(1.0-BokehBusyFactor);		// use the busy factor as an edge bias on the blur, not the highlights
    float4 average = float4(fragment.rgb * fragmentRadius * bokehBusyFactorToUse, bokehBusyFactorToUse);
    float2 pointOffset = float2(0,0);
    float2 ringRadiusDeltaCoords =  (BUFFER_PIXEL_SIZE * blurInfo.farPlaneMaxBlurInPixels * fragmentRadius) / blurInfo.numberOfRings;
    float2 currentRingRadiusCoords = ringRadiusDeltaCoords;
    float cocPerRing = (fragmentRadius * FarPlaneMaxBlur) / blurInfo.numberOfRings;
    float pointsOnRing = pointsFirstRing;
    float4 anamorphicFactors = CalculateAnamorphicFactor(blurInfo.texcoord - 0.5); // xy are up vector, zw are right vector
    float2x2 anamorphicRotationMatrix = CalculateAnamorphicRotationMatrix(blurInfo.texcoord);
    bool useShape = HighlightShape > 0;
    float4 shapeTap = float4(1.0f, 1.0f, 1.0f, 1.0f);
    for(float ringIndex = 0; ringIndex < blurInfo.numberOfRings; ringIndex++)
    {
        float anglePerPoint = 6.28318530717958 / pointsOnRing;
        float angle = anglePerPoint;
        float ringWeight = lerp(ringIndex/blurInfo.numberOfRings, 1, bokehBusyFactorToUse);
        float ringDistance = cocPerRing * ringIndex;
        float shapeRingDistance = ((ringIndex+1)/blurInfo.numberOfRings) * 0.5f;
        for(float pointNumber = 0; pointNumber < pointsOnRing; pointNumber++)
        {
            sincos(angle, pointOffset.y, pointOffset.x);
            // shapeLuma is in Alpha
            shapeTap = useShape ? GetShapeTap(angle, shapeRingDistance, shapetex) : shapeTap;
            // now transform the offset vector with the anamorphic factors and rotate it accordingly to the rotation matrix, so we get a nice
            // bending around the center of the screen.
            pointOffset = useShape ? pointOffset : MorphPointOffsetWithAnamorphicDeltas(pointOffset, anamorphicFactors, anamorphicRotationMatrix);
            float4 tapCoords = float4(blurInfo.texcoord + (pointOffset * currentRingRadiusCoords), 0, 0);
            float sampleRadius = CoCTexture.SampleLevel(CoCSampler, tapCoords.xy, 0).r;
            float weight = (sampleRadius >=0) * ringWeight * CalculateSampleWeight(sampleRadius * FarPlaneMaxBlur, ringDistance) * (shapeTap.a > 0.01 ? 1.0f : 0.0f);
            // adjust the weight for samples which are in front of the fragment, as they have to get their weight boosted so we don't see edges bleeding through. 
            // as otherwise they'll get a weight that's too low relatively to the pixels sampled from the plane the fragment is in.The 3.0 value is empirically determined.
            weight *= (1.0 + min(FarPlaneMaxBlur, 3.0f) * saturate(fragmentRadius - sampleRadius));
            float4 tap = tex.SampleLevel(BufferSampler, tapCoords.xy, 0);
            tap.rgb *= useShape ? (shapeTap.rgb * HighlightShapeGamma) : 1.0f;
            average.rgb += tap.rgb * weight;
            average.w += weight;
            angle+=anglePerPoint;
        }
        pointsOnRing+=pointsFirstRing;
        currentRingRadiusCoords += ringRadiusDeltaCoords;
    }
    fragment.rgb = average.rgb / (average.w + (average.w==0));
    return fragment;
}


// Performs a small blur to the out of focus areas using a lower amount of rings. Additionally it calculates the luma of the fragment into alpha
// and makes sure the fragment post-blur has the maximum luminosity from the taken samples to preserve harder edges on highlights. 
// In:	blurInfo, the pre-calculated disc blur information from the vertex shader.
// 		source, the source buffer to read RGBA data from
// Out: RGBA fragment that's the result of the disc-blur on the pixel at texcoord in source. A contains luma of RGB.
float4 PerformPreDiscBlur(VSDISCBLURINFO blurInfo, Texture2D tex)
{
    const float radiusFactor = 1.0/max(blurInfo.numberOfRings, 1);
    const float pointsFirstRing = max(blurInfo.numberOfRings-3, 2); 	// each ring has a multiple of this value of sample points. 
    
    float4 fragment = tex.SampleLevel(BufferSampler, blurInfo.texcoord, 0);
    fragment.rgb = AccentuateWhites(fragment.rgb);
    if(!MitigateUndersampling)
    {
        // early out as we don't need this step
        return fragment;
    }

    float signedFragmentRadius = CoCTexture.SampleLevel(CoCSampler, blurInfo.texcoord, 0).r;
    float absoluteFragmentRadius = abs(signedFragmentRadius);
    bool isNearPlaneFragment = signedFragmentRadius < 0;
    float blurFactorToUse = isNearPlaneFragment ? NearPlaneMaxBlur : FarPlaneMaxBlur;
    // Substract 2 as we blur on a smaller range. Don't limit the rings based on radius here, as that will kill the pre-blur.
    float numberOfRings = max(blurInfo.numberOfRings-2, 1);
    float4 average = absoluteFragmentRadius == 0 ? fragment : float4(fragment.rgb * absoluteFragmentRadius, absoluteFragmentRadius);
    float2 pointOffset = float2(0,0);
    // pre blur blurs near plane fragments with near plane samples and far plane fragments with far plane samples [Jimenez2014].
    float2 ringRadiusDeltaCoords = BUFFER_PIXEL_SIZE 
                                            * ((isNearPlaneFragment ? blurInfo.nearPlaneMaxBlurInPixels : blurInfo.farPlaneMaxBlurInPixels) *  absoluteFragmentRadius) 
                                            * rcp((numberOfRings-1) + (numberOfRings==1));
    float pointsOnRing = pointsFirstRing;
    float2 currentRingRadiusCoords = ringRadiusDeltaCoords;
    float cocPerRing = (signedFragmentRadius * blurFactorToUse) / numberOfRings;
    for(float ringIndex = 0; ringIndex < numberOfRings; ringIndex++)
    {
        float anglePerPoint = 6.28318530717958 / pointsOnRing;
        float angle = anglePerPoint;
        float ringDistance = cocPerRing * ringIndex;
        for(float pointNumber = 0; pointNumber < pointsOnRing; pointNumber++)
        {
            sincos(angle, pointOffset.y, pointOffset.x);
            float4 tapCoords = float4(blurInfo.texcoord + (pointOffset * currentRingRadiusCoords), 0, 0);
            float signedSampleRadius = CoCTexture.SampleLevel(CoCSampler, tapCoords.xy, 0).r;
            float absoluteSampleRadius = abs(signedSampleRadius);
            float isSamePlaneAsFragment = ((signedSampleRadius > 0 && !isNearPlaneFragment) || (signedSampleRadius <= 0 && isNearPlaneFragment));
            float weight = CalculateSampleWeight(absoluteSampleRadius * blurFactorToUse, ringDistance) * isSamePlaneAsFragment * 
                            (absoluteFragmentRadius - absoluteSampleRadius < 0.001);
            float3 tap = tex.SampleLevel(BufferSampler, tapCoords.xy, 0).rgb;
            average.rgb += AccentuateWhites(tap.rgb) * weight;
            average.w += weight;
            angle+=anglePerPoint;
        }
        pointsOnRing+=pointsFirstRing;
        currentRingRadiusCoords += ringRadiusDeltaCoords;
    }
    fragment.rgb = average.rgb/(average.w + (average.w==0));
    return fragment;
}


// Function to obtain the blur disc radius from the source sampler specified and optionally flatten it to zero. Used to blur the blur disc radii using a 
// separated gaussian blur function.
// In:	source, the source to read the blur disc radius value to process from
//		texcoord, the coordinate of the pixel which blur disc radius value we have to process
//		flattenToZero, flag which if true will make this function convert a blur disc radius value bigger than 0 to 0. 
//		Radii bigger than 0 are in the far plane and we only want near plane radii in our blurred buffer.
// Out: processed blur disc radius for the pixel at texcoord in source.
float GetBlurDiscRadiusFromSource(Texture2D<float> tex, float2 texcoord, bool flattenToZero)
{
    float coc = tex.SampleLevel(CoCSampler, texcoord, 0).r;
    // we're only interested in negative coc's (near plane). All coc's in focus/far plane are flattened to 0. Return the
    // absolute value of the coc as we're working with positive blurred CoCs (as the sign is no longer needed)
    return (flattenToZero && coc >= 0) ? 0 : abs(coc);
}

// Performs a single value gaussian blur pass in 1 direction (18 taps). Based on Ioxa's Gaussian blur shader. Used for near plane CoC blur.
// Used on tiles so not expensive.
// In:	source, the source sampler to read blur disc radius values to blur from
//		texcoord, the coordinate of the pixel to blur the blur disc radius for
// 		offsetWeight, a weight to multiple the coordinate with, containing typically the x or y value of the pixel size
//		flattenToZero, a flag to pass on to the actual blur disc radius read function to make sure in this pass the positive values are squashed to 0.
// 					   This flag is needed as the gaussian blur is used separably here so the second pass should not look for positive blur disc radii
//					   as all values are already positive (due to the first pass).
// Out: the blurred value for the blur disc radius of the pixel at texcoord. Greater than 0 if the original CoC is in the near plane, 0 otherwise.
float PerformSingleValueGaussianBlur(Texture2D<float> tex, float2 texcoord, float2 offsetWeight, bool flattenToZero)
{
    float offset[18] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4661974725, 21.4627427973, 23.4592916956, 25.455844494, 27.4524015179, 29.4489630909, 31.445529535, 33.4421011704 };
    float weight[18] = { 0.033245, 0.0659162217, 0.0636705814, 0.0598194658, 0.0546642566, 0.0485871646, 0.0420045997, 0.0353207015, 0.0288880982, 0.0229808311, 0.0177815511, 0.013382297, 0.0097960001, 0.0069746748, 0.0048301008, 0.0032534598, 0.0021315311, 0.0013582974 };

    float coc = GetBlurDiscRadiusFromSource(tex, texcoord, flattenToZero);
    coc *= weight[0];
    
    float2 factorToUse = offsetWeight * NearPlaneMaxBlur * 0.8f;
    for(int i = 1; i < 18; ++i)
    {
        float2 coordOffset = factorToUse * offset[i];
        float weightSample = weight[i];
        coc += GetBlurDiscRadiusFromSource(tex, texcoord + coordOffset, flattenToZero) * weightSample;
        coc += GetBlurDiscRadiusFromSource(tex, texcoord - coordOffset, flattenToZero) * weightSample;
    }
    
    return saturate(coc);
}

// Performs a full fragment (RGBA) gaussian blur pass in 1 direction (16 taps). Based on Ioxa's Gaussian blur shader.
// Will skip any pixels which are in-focus. It will also apply the pixel's blur disc radius to further limit the blur range for near-focused pixels.
// In:	source, the source sampler to read RGBA values to blur from
//		texcoord, the coordinate of the pixel to blur. 
// 		offsetWeight, a weight to multiple the coordinate with, containing typically the x or y value of the pixel size
// Out: the blurred fragment(RGBA) for the pixel at texcoord. 
float4 PerformFullFragmentGaussianBlur(Texture2D tex, float2 texcoord, float2 offsetWeight)
{
    float offset[6] = { 0.0, 1.4584295168, 3.40398480678, 5.3518057801, 7.302940716, 9.2581597095 };
    float weight[6] = { 0.13298, 0.23227575, 0.1353261595, 0.0511557427, 0.01253922, 0.0019913644 };
    const float3 lumaDotWeight = float3(0.3, 0.59, 0.11);
    
    float coc = CoCTexture.SampleLevel(CoCSampler, texcoord, 0).r;
    float4 fragment = tex.SampleLevel(BufferSampler, texcoord, 0);
    float fragmentLuma = dot(fragment.rgb, lumaDotWeight);
    float4 originalFragment = fragment;
    float absoluteCoC = abs(coc);
    float lengthPixelSize = length(BUFFER_PIXEL_SIZE);
    if(absoluteCoC < 0.2 || PostBlurSmoothing < 0.01 || fragmentLuma < 0.3)
    {
        // in focus or postblur smoothing isn't enabled or not really a highlight, ignore
        return fragment;
    }
    fragment.rgb *= weight[0];
    float2 factorToUse = offsetWeight * PostBlurSmoothing;
    for(int i = 1; i < 6; ++i)
    {
        float2 coordOffset = factorToUse * offset[i];
        float weightSample = weight[i];
        float sampleCoC = CoCTexture.SampleLevel(CoCSampler, texcoord + coordOffset, 0).r;
        float maskFactor = abs(sampleCoC) < 0.2;		// mask factor to avoid near/in focus bleed.
        fragment.rgb += (originalFragment.rgb * maskFactor * weightSample) + 
                        (tex.SampleLevel(BufferSampler, texcoord + coordOffset, 0).rgb * (1-maskFactor) * weightSample);
        sampleCoC = CoCTexture.SampleLevel(CoCSampler, texcoord - coordOffset, 0).r;
        maskFactor = abs(sampleCoC) < 0.2;
        fragment.rgb += (originalFragment.rgb * maskFactor * weightSample) + 
                        (tex.SampleLevel(BufferSampler, texcoord - coordOffset, 0).rgb * (1-maskFactor) * weightSample);
    }
    return saturate(fragment);
}

// Functions which fills the passed in struct with focus data. This code is factored out to be able to call it either from a vertex shader
// (in d3d10+) or from a pixel shader (d3d9) to work around compilation issues in reshade.
void FillFocusInfoData(inout VSFOCUSINFO toFill)
{
    // Reshade depth buffer ranges from 0.0->1.0, where 1.0 is 1000 in world units. All camera element sizes are in mm, so we state 1 in world units is 
    // 1 meter. This means to calculate from the linearized depth buffer value to meter we have to multiply by 1000.
    // Manual focus value is already in meter (well, sort of. This differs per game so we silently assume it's meter), so we first divide it by
    // 1000 to make it equal to a depth value read from the depth linearized depth buffer.
    // Read from sampler on current focus which is a 1x1 texture filled with the actual depth value of the focus point to use.
    toFill.focusDepth = CurrentFocusTexture.SampleLevel(ColorSampler, float2(0.5, 0.5), 0).r;
    toFill.focusDepthInM = toFill.focusDepth * 1000.0; 		// km to m
    toFill.focusDepthInMM = toFill.focusDepthInM * 1000.0; 	// m to mm
    toFill.pixelSizeLength = length(BUFFER_PIXEL_SIZE);
    
    // HyperFocal calculation, see https://photo.stackexchange.com/a/33898. Useful to calculate the edges of the depth of field area
    float hyperFocal = (FocalLength * FocalLength) / (FNumber * SENSOR_SIZE);
    float hyperFocalFocusDepthFocus = (hyperFocal * toFill.focusDepthInMM);
    toFill.nearPlaneInMM = (hyperFocalFocusDepthFocus / (hyperFocal + (toFill.focusDepthInMM - FocalLength)));	// in mm
    toFill.farPlaneInMM = hyperFocalFocusDepthFocus / (hyperFocal - (toFill.focusDepthInMM - FocalLength));		// in mm
}

// From: https://www.shadertoy.com/view/4lfGDs
// Adjusted for dof usage. Returns in a the # of taps accepted: a tap is accepted if it has a coc in the same plane as center.
float4 SharpeningPass_BlurSample(in Texture2D tex, in float2 texcoord, in float2 xoff, in float2 yoff, in float centerCoC, inout float3 minv, inout float3 maxv)
{
    float3 v11 = tex.SampleLevel(BufferSampler, texcoord + xoff, 0).rgb;
    float3 v12 = tex.SampleLevel(BufferSampler, texcoord + yoff, 0).rgb;
    float3 v21 = tex.SampleLevel(BufferSampler, texcoord - xoff, 0).rgb;
    float3 v22 = tex.SampleLevel(BufferSampler, texcoord - yoff, 0).rgb;
    float3 center = tex.SampleLevel(BufferSampler, texcoord, 0).rgb;

    float v11CoC = CoCTexture.SampleLevel(BufferSampler, texcoord + xoff, 0).r;
    float v12CoC = CoCTexture.SampleLevel(BufferSampler, texcoord + yoff, 0).r;
    float v21CoC = CoCTexture.SampleLevel(BufferSampler, texcoord - xoff, 0).r;
    float v22CoC = CoCTexture.SampleLevel(BufferSampler, texcoord - yoff, 0).r;

    float accepted = sign(centerCoC)==sign(v11CoC)? 1.0f: 0.0f;
    accepted+= sign(centerCoC)==sign(v12CoC)? 1.0f: 0.0f;
    accepted+= sign(centerCoC)==sign(v21CoC)? 1.0f: 0.0f;
    accepted+= sign(centerCoC)==sign(v22CoC)? 1.0f: 0.0f;

    // keep track of min/max for clamping later on so we don't get dark halos.
    minv = min(minv, v11);
    minv = min(minv, v12);
    minv = min(minv, v21);
    minv = min(minv, v22);

    maxv = max(maxv, v11);
    maxv = max(maxv, v12);
    maxv = max(maxv, v21);
    maxv = max(maxv, v22);
    return float4((v11 + v12 + v21 + v22 + 2.0 * center) * 0.166667, accepted);
}

// From: https://www.shadertoy.com/view/4lfGDs
// Adjusted for dof usage. Returns in a the # of taps accepted: a tap is accepted if it has a coc in the same plane as center.
float3 SharpeningPass_EdgeStrength(in float3 fragment, in Texture2D tex, in float2 texcoord, in float sharpeningFactor)
{
    const float spread = 0.5;
    float2 offset = float2(1.0, 1.0) / BUFFER_SCREEN_SIZE.xy;
    float2 up    = float2(0.0, offset.y) * spread;
    float2 right = float2(offset.x, 0.0) * spread;

    float3 minv = 1000000000;
    float3 maxv = 0;

    float centerCoC = CoCTexture.SampleLevel(CoCSampler, texcoord, 0).r;
    float4 v12 = SharpeningPass_BlurSample(tex, texcoord + up, 	right, up, centerCoC, minv, maxv);
    float4 v21 = SharpeningPass_BlurSample(tex, texcoord - right,right, up, centerCoC, minv, maxv);
    float4 v22 = SharpeningPass_BlurSample(tex, texcoord, 		right, up, centerCoC, minv, maxv);
    float4 v23 = SharpeningPass_BlurSample(tex, texcoord + right,right, up, centerCoC, minv, maxv);
    float4 v32 = SharpeningPass_BlurSample(tex, texcoord - up, 	right, up, centerCoC, minv, maxv);
    // rest of the pixels aren't used
    float accepted = v12.a + v21.a + v23.a + v32.a;
    if(accepted < 15.5)
    {
        // contains rejected tap, reject the entire operation. This is ok, as it's not necessary for the final pixel color.
        return fragment;
    }
    // all pixels accepted, calculated edge strength.
    float3 laplacian_of_g = v12.rgb + v21.rgb + v22.rgb * -4.0 + v23.rgb + v32.rgb;
    return clamp(fragment - laplacian_of_g.rgb * sharpeningFactor, minv, maxv);
}

//////////////////////////////////////////////////
//
// Compute shaders
//
//////////////////////////////////////////////////

// Determines the focus depth for the current frame, which will be stored in the currentfocus texture.
[numthreads(8, 8, 1)]
void CS_DetermineCurrentFocus(uint3 DTid : SV_DispatchThreadID)
{
    float2 autoFocusPointToUse = AutoFocusPoint;
	float result = UseAutoFocus ? lerp(PreviousFocusTexture.SampleLevel(ColorSampler, float2(0.0f, 0.0f), 0), GetLinearizedDepth(autoFocusPointToUse), AutoFocusTransitionSpeed) 
							: (ManualFocusPlane / 1000);
    OutputTexture[DTid.xy] = result;
}

// Copy the single value of the current focus texture to the previous focus texture so it's preserved for the next frame.
[numthreads(8, 8, 1)]
void CS_CopyCurrentFocus(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = CurrentFocusTexture.SampleLevel(ColorSampler, float2(0.0f, 0.0f), 0);
}

// Compute shader to calculate the CoC values for the entire screen.
[numthreads(8, 8, 1)]
void CS_CalculateCoCValues(uint3 DTid : SV_DispatchThreadID)
{
    VSFOCUSINFO focusInfo;

    focusInfo.texcoord = (DTid.xy + 0.5f) / BUFFER_SCREEN_SIZE;
    
    focusInfo.vpos = float4(focusInfo.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    
    FillFocusInfoData(focusInfo);
    
    float coc = CalculateBlurDiscSize(focusInfo);
    
    OutputTexture[DTid.xy] = coc;
}

// Compute shader which will perform a pre-blur on the frame buffer using a blur disc smaller than the original blur disc of the pixel. 
// This is done to overcome the undersampling gaps we have in the main blur disc sampler [Jimenez2014].
[numthreads(8, 8, 1)]
void CS_PreBlur(uint3 DTid : SV_DispatchThreadID)
{
    VSDISCBLURINFO blurInfo;
    blurInfo.texcoord.x = (DTid.x == 2) ? 2.0 : 0.0;
	blurInfo.texcoord.y = (DTid.y == 1) ? 2.0 : 0.0;
    blurInfo.vpos = float4(blurInfo.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    blurInfo.numberOfRings = round(BlurQuality);
    float pixelSizeLength = length(BUFFER_PIXEL_SIZE);
    blurInfo.farPlaneMaxBlurInPixels = (FarPlaneMaxBlur / 100.0) / pixelSizeLength;
    blurInfo.nearPlaneMaxBlurInPixels = (NearPlaneMaxBlur / 100.0) / pixelSizeLength;
    blurInfo.cocFactorPerPixel = length(BUFFER_PIXEL_SIZE) * blurInfo.farPlaneMaxBlurInPixels;	// not needed for near plane.

    OutputTexture[DTid.xy] = PerformPreDiscBlur(blurInfo, InputTexture);
}

// Compute shader which performs the far plane blur pass.
[numthreads(8, 8, 1)]
void CS_BokehBlur(uint3 DTid : SV_DispatchThreadID)
{
    VSDISCBLURINFO blurInfo;
    blurInfo.texcoord.x = (DTid.x == 2) ? 2.0 : 0.0;
	blurInfo.texcoord.y = (DTid.y == 1) ? 2.0 : 0.0;
    blurInfo.vpos = float4(blurInfo.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    blurInfo.numberOfRings = round(BlurQuality);
    float pixelSizeLength = length(BUFFER_PIXEL_SIZE);
    blurInfo.farPlaneMaxBlurInPixels = (FarPlaneMaxBlur / 100.0) / pixelSizeLength;
    blurInfo.nearPlaneMaxBlurInPixels = (NearPlaneMaxBlur / 100.0) / pixelSizeLength;
    blurInfo.cocFactorPerPixel = length(BUFFER_PIXEL_SIZE) * blurInfo.farPlaneMaxBlurInPixels;	// not needed for near plane.

    OutputTexture[DTid.xy] = PerformDiscBlur(blurInfo, Buffer4Texture, Buffer4Texture);
}

// Compute shader which performs the near plane blur pass. Uses a blurred buffer of blur disc radii, based on a combination of [Jimenez2014] (tiles)
// and [Nilsson2012] (blurred CoC).
[numthreads(8, 8, 1)]
void CS_NearBokehBlur(uint3 DTid : SV_DispatchThreadID)
{
    VSDISCBLURINFO blurInfo;
    blurInfo.texcoord.x = (DTid.x == 2) ? 2.0 : 0.0;
	blurInfo.texcoord.y = (DTid.y == 1) ? 2.0 : 0.0;
    blurInfo.vpos = float4(blurInfo.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    blurInfo.numberOfRings = round(BlurQuality);
    float pixelSizeLength = length(BUFFER_PIXEL_SIZE);
    blurInfo.farPlaneMaxBlurInPixels = (FarPlaneMaxBlur / 100.0) / pixelSizeLength;
    blurInfo.nearPlaneMaxBlurInPixels = (NearPlaneMaxBlur / 100.0) / pixelSizeLength;
    blurInfo.cocFactorPerPixel = length(BUFFER_PIXEL_SIZE) * blurInfo.farPlaneMaxBlurInPixels;	// not needed for near plane.

    OutputTexture[DTid.xy] = PerformNearPlaneDiscBlur(blurInfo, Buffer2Texture, Buffer2Texture);
}

// Compute shader which performs the CoC tile creation (horizontal gather of min CoC)
[numthreads(8, 8, 1)]
void CS_CoCTile1(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = PerformTileGatherHorizontal(CoCTexture, TEXCOORD);
}

// Compute shader which performs the CoC tile creation (vertical gather of min CoC)
[numthreads(8, 8, 1)]
void CS_CoCTile2(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = PerformTileGatherVertical(CoCTileTmpTexture, TEXCOORD);
}

// Compute shader which performs the CoC tile creation with neighbor tile info (horizontal and vertical gather of min CoC)
[numthreads(8, 8, 1)]
void CS_CoCTileNeighbor(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = PerformNeighborTileGather(CoCTileTexture, TEXCOORD);
}

// Compute shader which performs the first part of the gaussian blur on the blur disc values
[numthreads(8, 8, 1)]
void CS_CoCGaussian1(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = PerformSingleValueGaussianBlur(CoCTileNeighborTexture, TEXCOORD, float2(BUFFER_PIXEL_SIZE.x * (BUFFER_SCREEN_SIZE.x/GROUND_TRUTH_SCREEN_WIDTH), 0.0f), true);
}

// Compute shader which performs the second part of the gaussian blur on the blur disc values
[numthreads(8, 8, 1)]
void CS_CoCGaussian2(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = float2(PerformSingleValueGaussianBlur(CoCTmp1Texture, TEXCOORD, float2(0.0f, BUFFER_PIXEL_SIZE.y * (BUFFER_SCREEN_SIZE.y/GROUND_TRUTH_SCREEN_HEIGHT)), false), CoCTileNeighborTexture.SampleLevel(CoCSampler, TEXCOORD, 0).r);
}

// Compute shader which combines 2 half-res sources to a full res output. From texCDBuffer1 & 2 to texCDBuffer4.
[numthreads(8, 8, 1)]
void CS_Combiner(uint3 DTid : SV_DispatchThreadID)
{
    float4 fragment;
    // first blend far plane with original buffer, then near plane on top of that. 
    float4 originalFragment = InputTexture.SampleLevel(ColorSampler, TEXCOORD, 0);
    originalFragment.rgb = AccentuateWhites(originalFragment.rgb);
    float4 farFragment = Buffer3Texture.SampleLevel(BufferSampler, TEXCOORD, 0);
    float4 nearFragment = Buffer1Texture.SampleLevel(BufferSampler, TEXCOORD, 0);
    float pixelCoC = CoCTexture.SampleLevel(CoCSampler, TEXCOORD, 0).r;
    // multiply with far plane max blur so if we need to have 0 blur we get full res
    float realCoC = pixelCoC * clamp(0, 1, FarPlaneMaxBlur);
    if(HighlightSharpeningFactor > 0.0f)
    {
        // sharpen the fragments pre-combining
        float sharpeningFactor = abs(pixelCoC) * 80.0 * HighlightSharpeningFactor;		// 80 is a handpicked number, just to get high sharpening.
        farFragment.rgb = SharpeningPass_EdgeStrength(farFragment.rgb, Buffer3Texture, TEXCOORD, sharpeningFactor * realCoC);
        nearFragment.rgb = SharpeningPass_EdgeStrength(nearFragment.rgb, Buffer1Texture, TEXCOORD, sharpeningFactor * (abs(pixelCoC) * clamp(0, 1, NearPlaneMaxBlur)));
    }
    // all CoC's > 0.1 are full far fragment, below that, we're going to blend. This avoids shimmering far plane without the need of a 
    // 'magic' number to boost up the alpha.
    float blendFactor = (realCoC > 0.1) ? 1 : smoothstep(0, 1, (realCoC / 0.1));
    fragment = lerp(originalFragment, farFragment, blendFactor);
    fragment.rgb = lerp(fragment.rgb, nearFragment.rgb, nearFragment.a * (NearPlaneMaxBlur != 0));
    fragment.rgb = CorrectForWhiteAccentuation(fragment.rgb);
	fragment.a = 1.0;
    OutputTexture[DTid.xy] = fragment;
}

// Compute shader which performs a 9 tap tentfilter on the far plane blur result. This tent filter is from the composition pass 
// in KinoBokeh: https://github.com/keijiro/KinoBokeh/blob/master/Assets/Kino/Bokeh/Shader/Composition.cginc
// See also [Jimenez2014] for a discussion about this filter.
[numthreads(8, 8, 1)]
void CS_TentFilter(uint3 DTid : SV_DispatchThreadID)
{
    float4 coord = BUFFER_PIXEL_SIZE.xyxy * float4(1, 1, -1, 0);
	float4 average;
    average = Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD - coord.xy, 0);
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD - coord.wy, 0) * 2;
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD - coord.zy, 0);
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD + coord.zw, 0) * 2;
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD, 0) * 4;
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD + coord.xw, 0) * 2;
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD + coord.zy, 0);
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD + coord.wy, 0) * 2;
    average += Buffer2Texture.SampleLevel(BufferSampler, TEXCOORD + coord.xy, 0);

    OutputTexture[DTid.xy] = average / 16;
}

// Compute shader which performs the first part of the gaussian post-blur smoothing pass, to iron out undersampling issues with the disc blur
[numthreads(8, 8, 1)]
void CS_PostSmoothing1(uint3 DTid : SV_DispatchThreadID)
{
    OutputTexture[DTid.xy] = PerformFullFragmentGaussianBlur(Buffer4Texture, TEXCOORD, float2(BUFFER_PIXEL_SIZE.x, 0.0));
}

// Compute shader which performs the second part of the gaussian post-blur smoothing pass, to iron out undersampling issues with the disc blur
// It also displays the focusing overlay helpers if the mouse button is down and the user enabled ShowOutOfFocusPlaneOnMouseDown.
// it displays the near and far plane at the hyperfocal planes (calculated in vertex shader) with the overlay color and the in-focus area in between
// as normal. It then also blends the focus plane as a separate color to make focusing really easy. 
[numthreads(8, 8, 1)]
void CS_PostSmoothing2AndFocusing(uint3 DTid : SV_DispatchThreadID)
{
    VSFOCUSINFO focusInfo;
    
    focusInfo.texcoord = TEXCOORD;
    
    focusInfo.vpos = float4(focusInfo.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    
    FillFocusInfoData(focusInfo);
    
    float4 fragment;
    
    fragment = PerformFullFragmentGaussianBlur(Buffer5Texture, focusInfo.texcoord, float2(0.0, BUFFER_PIXEL_SIZE.y));
    float4 originalFragment = Buffer4Texture.SampleLevel(BufferSampler, focusInfo.texcoord, 0);
    
    float2 uv = float2(BUFFER_WIDTH, BUFFER_HEIGHT) / float2(512.0f, 512.0f);
    uv.xy = uv.xy * focusInfo.texcoord.xy;
    float noise = NoiseTexture.SampleLevel(NoiseSampler, uv, 0).x;
    fragment.xyz = saturate(fragment.xyz + lerp(-0.5/255.0, 0.5/255.0, noise));
    
    float coc = abs(CoCTexture.SampleLevel(CoCSampler, focusInfo.texcoord, 0).r);
    fragment.rgb = lerp(originalFragment.rgb, fragment.rgb, saturate(coc < length(BUFFER_PIXEL_SIZE) ? 0 : 4 * coc));
    fragment.w = 1.0;

    OutputTexture[DTid.xy] = fragment;
}

