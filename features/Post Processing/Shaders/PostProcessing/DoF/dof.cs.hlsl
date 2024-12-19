#include "Common/SharedData.hlsli"

RWTexture2D<float4> RWTexOut : register(u0);
RWTexture2D<float> RWFocus : register(u1);
RWTexture2D<float> RWTexCoC : register(u2);

SamplerState ImageSampler : register(s0);
SamplerState DepthSampler : register(s1);

Texture2D<float4> TexColor : register(t0);
Texture2D<float> TexPreviousFocus : register(t1);
Texture2D<float> DepthTexture : register(t2);
Texture2D<float> TexCoCInput : register(t3);
Texture2D<float> TexCoCBlurredInput : register(t4);

cbuffer DoFCB : register(b1)
{
    float TransitionSpeed;
    float2 FocusCoord;
    float ManualFocusPlane;
    float FocalLength;
    float FNumber;
    float BlurQuality;
    float NearFarDistanceCompensation;
    float BokehBusyFactor;
    float HighlightBoost;
    float Width;
    float Height;
    bool AutoFocus;
};

#define EPSILON 1e-6
#define SENSOR_SIZE 0.024f

struct FOCUSINFO
{
    float2 texcoord : TEXCOORD0;
    float focusDepth : TEXCOORD1;
    float focusDepthInM : TEXCOORD2;
    float focusDepthInMM : TEXCOORD3;
    float pixelSizeLength : TEXCOORD4;
    float nearPlaneInMM : TEXCOORD5;
    float farPlaneInMM : TEXCOORD6;
};

struct DISCBLURINFO
{
    float2 texcoord : TEXCOORD0;
    float numberOfRings : TEXCOORD1;
    float farPlaneMaxBlurInPixels : TEXCOORD2;
    float nearPlaneMaxBlurInPixels : TEXCOORD3;
    float cocFactorPerPixel : TEXCOORD4;
    float highlightBoostFactor: TEXCOORD5;
};

float GetDepth(float2 uv)
{
    float depth = DepthTexture.SampleLevel(DepthSampler, uv, 0);
    float linearDepth = SharedData::GetScreenDepth(depth);
    return depth * 2.0f;
}

float PreviousFocus()
{
    return TexPreviousFocus.SampleLevel(DepthSampler, float2(0.5, 0.5), 0).x;
}

void FillFocusInfoData(inout FOCUSINFO toFill)
{
    // Reshade depth buffer ranges from 0.0->1.0, where 1.0 is 1000 in world units. All camera element sizes are in mm, so we state 1 in world units is 
    // 1 meter. This means to calculate from the linearized depth buffer value to meter we have to multiply by 1000.
    // Manual focus value is already in meter (well, sort of. This differs per game so we silently assume it's meter), so we first divide it by
    // 1000 to make it equal to a depth value read from the depth linearized depth buffer.
    // Read from sampler on current focus which is a 1x1 texture filled with the actual depth value of the focus point to use.
    toFill.focusDepth = PreviousFocus();
    toFill.focusDepthInM = toFill.focusDepth * 1000.0; 		// km to m
    toFill.focusDepthInMM = toFill.focusDepthInM * 1000.0; 	// m to mm
    toFill.pixelSizeLength = length(float2(Width, Height));	// in pixels
    
    // HyperFocal calculation, see https://photo.stackexchange.com/a/33898. Useful to calculate the edges of the depth of field area
    float hyperFocal = (FocalLength * FocalLength) / (FNumber * SENSOR_SIZE);
    float hyperFocalFocusDepthFocus = (hyperFocal * toFill.focusDepthInMM);
    toFill.nearPlaneInMM = (hyperFocalFocusDepthFocus / (hyperFocal + (toFill.focusDepthInMM - FocalLength)));	// in mm
    toFill.farPlaneInMM = hyperFocalFocusDepthFocus / (hyperFocal - (toFill.focusDepthInMM - FocalLength));		// in mm
}

float CalculateBlurDiscSize(FOCUSINFO focusInfo)
{
    float pixelDepth = GetDepth(focusInfo.texcoord);
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

float GetBlurDiscRadiusFromSource(Texture2D<float> source, SamplerState samp, float2 texcoord, bool flattenToZero)
	{
		float coc = source.SampleLevel(samp, texcoord, 0).x;
		// we're only interested in negative coc's (near plane). All coc's in focus/far plane are flattened to 0. Return the
		// absolute value of the coc as we're working with positive blurred CoCs (as the sign is no longer needed)
		return (flattenToZero && coc >= 0) ? 0 : abs(coc);
	}

float PerformSingleValueGaussianBlur(Texture2D<float> source, SamplerState samp, float2 texcoord, float2 offsetWeight, bool flattenToZero)
{
    float offset[18] = { 0.0, 1.4953705027, 3.4891992113, 5.4830312105, 7.4768683759, 9.4707125766, 11.4645656736, 13.4584295168, 15.4523059431, 17.4461967743, 19.4661974725, 21.4627427973, 23.4592916956, 25.455844494, 27.4524015179, 29.4489630909, 31.445529535, 33.4421011704 };
    float weight[18] = { 0.033245, 0.0659162217, 0.0636705814, 0.0598194658, 0.0546642566, 0.0485871646, 0.0420045997, 0.0353207015, 0.0288880982, 0.0229808311, 0.0177815511, 0.013382297, 0.0097960001, 0.0069746748, 0.0048301008, 0.0032534598, 0.0021315311, 0.0013582974 };

    float coc = GetBlurDiscRadiusFromSource(source, samp, texcoord, flattenToZero);
    coc *= weight[0];
    
    float2 factorToUse = offsetWeight * 0.8f;
    for(int i = 1; i < 18; ++i)
    {
        float2 coordOffset = factorToUse * offset[i];
        float weightSample = weight[i];
        coc += GetBlurDiscRadiusFromSource(source, samp, texcoord + coordOffset, flattenToZero) * weightSample;
        coc += GetBlurDiscRadiusFromSource(source, samp, texcoord - coordOffset, flattenToZero) * weightSample;
    }
    
    return saturate(coc);
}

float3 ConeOverlap(float3 fragment)
{
    float k = 0.4 * 0.33;
    float2 f = float2(1-2 * k, k);
    float3x3 m = float3x3(f.xyy, f.yxy, f.yyx);
    return mul(fragment, m);
}

float3 AccentuateWhites(float3 fragment)
{
    // apply small tow to the incoming fragment, so the whitepoint gets slightly lower than max.
    // De-tonemap color (reinhard). Thanks Marty :) 
    fragment = pow(abs(ConeOverlap(fragment)), 2.2);
    return fragment / max((1.001 - (HighlightBoost * fragment)), 0.001);
}

// returns 2 vectors, (x,y) are up vector, (z,w) are right vector. 
// In: pixelVector which is the current pixel converted into a vector where (0,0) is the center of the screen.
float4 CalculateAnamorphicFactor(float2 pixelVector)
{
    float HighlightAnamorphicFactor = 1.0f;
    float HighlightAnamorphicSpreadFactor = 0.0f;
    float normalizedFactor = lerp(1, HighlightAnamorphicFactor, lerp(length(pixelVector * 2), 1, HighlightAnamorphicSpreadFactor));
    return float4(0, 1 + (1-normalizedFactor), normalizedFactor, 0);
}

// Calculates a rotation matrix for the current pixel specified in texcoord, which can be used to rotate the bokeh shape to match
// a distored field around the center of the screen: it rotates the anamorphic factors with this matrix so the bokeh shapes form a circle
// around the center of the screen. 
float2x2 CalculateAnamorphicRotationMatrix(float2 texcoord)
{
    float HighlightAnamorphicAlignmentFactor = 0.0f;
    float2 pixelVector = normalize(texcoord - 0.5);
    float limiter = (1-HighlightAnamorphicAlignmentFactor)/2;
    pixelVector.y = clamp(pixelVector.y, -limiter, limiter);
    float2 refVector = normalize(float2(-0.5, 0));
    float2 sincosFactor = float2(0,0);
    // calculate the angle between the pixelvector and the ref vector and grab the sin/cos for that angle for the rotation matrix.
    sincos(atan2(pixelVector.y, pixelVector.x) - atan2(refVector.y, refVector.x), sincosFactor.x, sincosFactor.y);
    return float2x2(sincosFactor.y, sincosFactor.x, -sincosFactor.x, sincosFactor.y);
}

// calculate the sample weight based on the values specified. 
float CalculateSampleWeight(float sampleRadiusInCoC, float ringDistanceInCoC)
{
    return saturate(sampleRadiusInCoC - (ringDistanceInCoC * NearFarDistanceCompensation) + 0.5);
}

float2 MorphPointOffsetWithAnamorphicDeltas(float2 pointOffset, float4 anamorphicFactors, float2x2 anamorphicRotationMatrix)
{
    pointOffset.x = pointOffset.x * anamorphicFactors.x + pointOffset.x*anamorphicFactors.z;
    pointOffset.y = pointOffset.y * anamorphicFactors.y + pointOffset.y*anamorphicFactors.w;
    return mul(pointOffset, anamorphicRotationMatrix);
}

// Performs a small blur to the out of focus areas using a lower amount of rings. Additionally it calculates the luma of the fragment into alpha
// and makes sure the fragment post-blur has the maximum luminosity from the taken samples to preserve harder edges on highlights. 
// In:	blurInfo, the pre-calculated disc blur information from the vertex shader.
// 		source, the source buffer to read RGBA data from
// Out: RGBA fragment that's the result of the disc-blur on the pixel at texcoord in source. A contains luma of RGB.
float4 PerformPreDiscBlur(DISCBLURINFO blurInfo, Texture2D source, SamplerState samp)
{
    const float radiusFactor = 1.0/max(blurInfo.numberOfRings, 1);
    const float pointsFirstRing = max(blurInfo.numberOfRings-3, 2); 	// each ring has a multiple of this value of sample points. 
    
    float4 fragment = source.SampleLevel(samp, blurInfo.texcoord, 0);
    fragment.rgb = AccentuateWhites(fragment.rgb);
    
    float signedFragmentRadius = TexCoCInput.SampleLevel(DepthSampler, blurInfo.texcoord, 0).x * radiusFactor;
    float absoluteFragmentRadius = abs(signedFragmentRadius);
    bool isNearPlaneFragment = signedFragmentRadius < 0;
    float blurFactorToUse = 1.0f;
    // Substract 2 as we blur on a smaller range. Don't limit the rings based on radius here, as that will kill the pre-blur.
    float numberOfRings = max(blurInfo.numberOfRings-2, 1);
    float4 average = absoluteFragmentRadius == 0 ? fragment : float4(fragment.rgb * absoluteFragmentRadius, absoluteFragmentRadius);
    float2 pointOffset = float2(0,0);
    // pre blur blurs near plane fragments with near plane samples and far plane fragments with far plane samples [Jimenez2014].
    float2 ringRadiusDeltaCoords = float2(1.0f / Width, 1.0f / Height) 
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
            float signedSampleRadius = TexCoCInput.SampleLevel(DepthSampler, tapCoords.xy, 0).x * radiusFactor;
            float absoluteSampleRadius = abs(signedSampleRadius);
            float isSamePlaneAsFragment = ((signedSampleRadius > 0 && !isNearPlaneFragment) || (signedSampleRadius <= 0 && isNearPlaneFragment));
            float weight = CalculateSampleWeight(absoluteSampleRadius * blurFactorToUse, ringDistance) * isSamePlaneAsFragment * 
                            (absoluteFragmentRadius - absoluteSampleRadius < 0.001);
            float3 tap = source.SampleLevel(samp, tapCoords.xy, 0).rgb;
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

// Calculates the new RGBA fragment for a pixel at texcoord in source using a disc based blur technique described in [Jimenez2014] 
// (Though without using tiles). Blurs far plane.
// In:	blurInfo, the pre-calculated disc blur information from the vertex shader.
// 		source, the source buffer to read RGBA data from. RGB is in HDR. A not used.
//		shape, the shape sampler to use if shapes are used.
// Out: RGBA fragment that's the result of the disc-blur on the pixel at texcoord in source. A contains luma of pixel.
float4 PerformDiscBlur(DISCBLURINFO blurInfo, Texture2D source, SamplerState samp)
{
    const float pointsFirstRing = 7; 	// each ring has a multiple of this value of sample points. 
    float4 fragment = source.SampleLevel(samp, blurInfo.texcoord, 0);
    float fragmentRadius = TexCoCInput.SampleLevel(DepthSampler, blurInfo.texcoord, 0).r;
    // we'll not process near plane fragments as they're processed in a separate pass. 
    if(fragmentRadius < 0 || blurInfo.farPlaneMaxBlurInPixels <=0)
    {
        // near plane fragment, will be done in near plane pass 
        return fragment;
    }
    float bokehBusyFactorToUse = saturate(1.0-BokehBusyFactor);		// use the busy factor as an edge bias on the blur, not the highlights
    float4 average = float4(fragment.rgb * fragmentRadius * bokehBusyFactorToUse, bokehBusyFactorToUse);
    float2 pointOffset = float2(0,0);
    float2 ringRadiusDeltaCoords =  (float2(1.0f / Width, 1.0f / Height) * blurInfo.farPlaneMaxBlurInPixels * fragmentRadius) / blurInfo.numberOfRings;
    float2 currentRingRadiusCoords = ringRadiusDeltaCoords;
    float cocPerRing = (fragmentRadius * 1.0f) / blurInfo.numberOfRings;
    float pointsOnRing = pointsFirstRing;
    float4 anamorphicFactors = CalculateAnamorphicFactor(blurInfo.texcoord - 0.5); // xy are up vector, zw are right vector
    float2x2 anamorphicRotationMatrix = CalculateAnamorphicRotationMatrix(blurInfo.texcoord);
    // bool useShape = HighlightShape > 0;
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
            // shapeTap = useShape ? GetShapeTap(angle, shapeRingDistance, shapeSampler) : shapeTap;
            // now transform the offset vector with the anamorphic factors and rotate it accordingly to the rotation matrix, so we get a nice
            // bending around the center of the screen.
            pointOffset = MorphPointOffsetWithAnamorphicDeltas(pointOffset, anamorphicFactors, anamorphicRotationMatrix);
            float4 tapCoords = float4(blurInfo.texcoord + (pointOffset * currentRingRadiusCoords), 0, 0);
            float sampleRadius = TexCoCInput.SampleLevel(DepthSampler, tapCoords.xy, 0).r;
            float weight = (sampleRadius >=0) * ringWeight * CalculateSampleWeight(sampleRadius, ringDistance) * (shapeTap.a > 0.01 ? 1.0f : 0.0f);
            // adjust the weight for samples which are in front of the fragment, as they have to get their weight boosted so we don't see edges bleeding through. 
            // as otherwise they'll get a weight that's too low relatively to the pixels sampled from the plane the fragment is in.The 3.0 value is empirically determined.
            weight *= (1.0 + saturate(fragmentRadius - sampleRadius));
            float4 tap = source.SampleLevel(samp, tapCoords.xy, 0);
            tap.rgb *= 1.0f;
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

float4 PerformNearPlaneDiscBlur(DISCBLURINFO blurInfo, Texture2D source, SamplerState samp)
{
    float4 fragment = source.SampleLevel(samp, blurInfo.texcoord, 0);
    // r contains blurred CoC, g contains original CoC. Original is negative.
    float2 fragmentRadii = float2(TexCoCBlurredInput.SampleLevel(DepthSampler, blurInfo.texcoord, 0), TexCoCInput.SampleLevel(DepthSampler, blurInfo.texcoord, 0));
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
    float2 ringRadiusDeltaCoords = float2(1.0f / Width, 1.0f / Height) * (nearPlaneBlurInPixels / (numberOfRings-1));
    float pointsOnRing = pointsFirstRing;
    float2 currentRingRadiusCoords = ringRadiusDeltaCoords;
    float4 anamorphicFactors = CalculateAnamorphicFactor(blurInfo.texcoord - 0.5); // xy are up vector, zw are right vector
    float2x2 anamorphicRotationMatrix = CalculateAnamorphicRotationMatrix(blurInfo.texcoord);
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
            // shapeTap = useShape ? GetShapeTap(angle, shapeRingDistance, shapeSampler) : shapeTap;
            // now transform the offset vector with the anamorphic factors and rotate it accordingly to the rotation matrix, so we get a nice
            // bending around the center of the screen.
            pointOffset = MorphPointOffsetWithAnamorphicDeltas(pointOffset, anamorphicFactors, anamorphicRotationMatrix);
            float4 tapCoords = float4(blurInfo.texcoord + (pointOffset * currentRingRadiusCoords), 0, 0);
            float4 tap = source.SampleLevel(samp, tapCoords.xy, 0);
            tap.rgb *= 1.0f;
            // r contains blurred CoC, g contains original CoC. Original can be negative
            float2 sampleRadii = float2(TexCoCBlurredInput.SampleLevel(DepthSampler, tapCoords.xy, 0), TexCoCInput.SampleLevel(DepthSampler, tapCoords.xy, 0));
            float blurredSampleRadius = sampleRadii.r;
            float sampleWeight = weight * (shapeTap.a > 0.01 ? 1.0f : 0.0f);
            average.rgb += tap.rgb * sampleWeight;
            average.w += sampleWeight ;
            angle+=anglePerPoint;
        }
        pointsOnRing+=pointsFirstRing;
        currentRingRadiusCoords += ringRadiusDeltaCoords;
    }
    float NearPlaneMaxBlur = 1.0f;
    average.rgb/=(average.w + (average.w ==0));
    float alpha = saturate((min(2.5, NearPlaneMaxBlur) + 0.4) * (fragmentRadiusToUse > 0.1 ? (fragmentRadii.g <=0 ? 2 : 1) * fragmentRadiusToUse : max(fragmentRadiusToUse, -fragmentRadii.g)));
    fragment.rgb = average.rgb;
    fragment.a = alpha;
    return fragment;
}

[numthreads(1, 1, 1)]
void CS_UpdateFocus(uint2 DTid : SV_DispatchThreadID)
{
    float depth = AutoFocus? GetDepth(FocusCoord): ManualFocusPlane;

    RWFocus[DTid] = lerp(TexPreviousFocus.SampleLevel(DepthSampler, (0.5f, 0.5f), 0), depth, TransitionSpeed);
}

[numthreads(8, 8, 1)]
void CS_CalculateCoC(uint2 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)Width || DTid.y >= (uint)Height)
        return;

    float2 uv = (DTid.xy + 0.5f) / float2(Width, Height);
    float4 color = TexColor.SampleLevel(ImageSampler, uv, 0);

    FOCUSINFO focusInfo;
    focusInfo.texcoord = uv;
    FillFocusInfoData(focusInfo);

    float coc = CalculateBlurDiscSize(focusInfo);
    RWTexCoC[DTid] = coc;
    // RWTexCoC[DTid] = GetDepth(uv);
}

[numthreads(8, 8, 1)]
void CS_CoCGaussian1(uint2 DTid : SV_DispatchThreadID)
{
    float2 uv = (DTid.xy + 0.5f) / float2(Width, Height);
    RWTexCoC[DTid] = PerformSingleValueGaussianBlur(TexCoCInput, DepthSampler, uv, float2(1.0f / Width, 0.0f), true);
}

[numthreads(8, 8, 1)]
void CS_CoCGaussian2(uint2 DTid : SV_DispatchThreadID)
{
    float2 uv = (DTid.xy + 0.5f) / float2(Width, Height);
    RWTexCoC[DTid] = PerformSingleValueGaussianBlur(TexCoCInput, DepthSampler, uv, float2(0.0f, 1.0f / Height), false);
}

[numthreads(8, 8, 1)]
void CS_Blur(uint2 DTid : SV_DispatchThreadID)
{
    DISCBLURINFO blurInfo;
    blurInfo.texcoord = (DTid.xy + 0.5f) / float2(Width, Height);
    blurInfo.numberOfRings = round(BlurQuality);
    float pixelSizeLength = length(float2(1.0f / Width, 1.0f / Height));
    blurInfo.farPlaneMaxBlurInPixels = (1.0f / 100.0f) / pixelSizeLength;
    blurInfo.nearPlaneMaxBlurInPixels = (1.0f / 100.0f) / pixelSizeLength;
    blurInfo.cocFactorPerPixel = pixelSizeLength * blurInfo.farPlaneMaxBlurInPixels;	// not needed for near plane.
    // Pre Blur
    float4 color = PerformPreDiscBlur(blurInfo, TexColor, ImageSampler);
    RWTexOut[DTid] = color;
}

[numthreads(8, 8, 1)]
void CS_FarBlur(uint2 DTid : SV_DispatchThreadID)
{
    DISCBLURINFO blurInfo;
    blurInfo.texcoord = (DTid.xy + 0.5f) / float2(Width, Height);
    blurInfo.numberOfRings = round(BlurQuality);
    float pixelSizeLength = length(float2(1.0f / Width, 1.0f / Height));
    blurInfo.farPlaneMaxBlurInPixels = (1.0f / 100.0f) / pixelSizeLength;
    blurInfo.nearPlaneMaxBlurInPixels = (1.0f / 100.0f) / pixelSizeLength;
    blurInfo.cocFactorPerPixel = pixelSizeLength * blurInfo.farPlaneMaxBlurInPixels;	// not needed for near plane.
    float4 color = PerformDiscBlur(blurInfo, TexColor, ImageSampler);
    RWTexOut[DTid] = color;
}

[numthreads(8, 8, 1)]
void CS_NearBlur(uint2 DTid : SV_DispatchThreadID)
{
    DISCBLURINFO blurInfo;
    blurInfo.texcoord = (DTid.xy + 0.5f) / float2(Width, Height);
    blurInfo.numberOfRings = round(BlurQuality);
    float pixelSizeLength = length(float2(1.0f / Width, 1.0f / Height));
    blurInfo.farPlaneMaxBlurInPixels = (1.0f / 100.0f) / pixelSizeLength;
    blurInfo.nearPlaneMaxBlurInPixels = (1.0f / 100.0f) / pixelSizeLength;
    blurInfo.cocFactorPerPixel = pixelSizeLength * blurInfo.farPlaneMaxBlurInPixels;	// not needed for near plane.
    float4 color = PerformNearPlaneDiscBlur(blurInfo, TexColor, ImageSampler);
    RWTexOut[DTid] = color;
}

