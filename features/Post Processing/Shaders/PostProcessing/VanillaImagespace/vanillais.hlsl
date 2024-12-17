#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"

RWTexture2D<float4> RWTexOut : register(u0);

SamplerState ImageSampler : register(s0);

Texture2D<float4> TexColor : register(t0);

cbuffer VanillaISCB : register(b1)
{
    float BlendFactor;
    float3 Cinematic;
    float2 Resolution;
};

#define EPSILON 1e-6

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= (uint)Resolution.x || DTid.y >= (uint)Resolution.y)
        return;
    float2 uv = (DTid.xy + 0.5f) / Resolution;
    float4 color = TexColor.SampleLevel(ImageSampler, uv, 0);

    if (Cinematic.y + Cinematic.z < EPSILON)
    {
        RWTexOut[DTid.xy] = color;
        return;
    }

    float luminance = Color::RGBToLuminance(color.rgb);
    float3 ppColor = color.rgb;
    
    float grayPoint = 0.1f;

    ppColor = Cinematic.y * lerp(luminance, ppColor, Cinematic.x);
    ppColor = clamp(pow(clamp(ppColor, 0.0f, 16.0f), pow(2.0f, Cinematic.z - 1.0f)), 0.0f, 16.0f);
    float3 finalColor = lerp(color.rgb, ppColor, BlendFactor);
    
    RWTexOut[DTid.xy] = float4(finalColor, color.a);
}