#include "Common/SharedData.hlsli"

RWTexture2D<float4> RWTexOut : register(u0);
RWTexture2D<float> RWFocus : register(u1);
RWTexture2D<float> RWTexCoC : register(u2);

SamplerState ImageSampler : register(s0);
SamplerState DepthSampler : register(s1);

Texture2D<float4> TexColor : register(t0);
Texture2D<float> TexPreviousFocus : register(t1);

cbuffer DoFCB : register(b1)
{
    float TransitionSpeed;
    float2 FocusCoord;
    float ManualFocusPlane;
    float FocalLength;
    float FNumber;
    float BlurQuality;
    float HighlightBoost;
    float Bokeh;
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

float GetDepth(float2 uv)
{
    return SharedData::DepthTexture.SampleLevel(DepthSampler, uv, 0).x;
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

[numthreads(1, 1, 1)]
void CS_UpdateFocus(uint2 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)Width || DTid.y >= (uint)Height)
        return;

    float depth = AutoFocus? GetDepth(FocusCoord): ManualFocusPlane / 1000.0f;

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
}

