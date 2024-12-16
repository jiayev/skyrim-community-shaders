RWTexture2D<float4> RWTexOut : register(u0);

SamplerState ImageSampler : register(s0);

Texture2D<float4> TexColor : register(t0);

cbuffer VanillaISCB : register(b1)
{
    float BlendFactor;
    float2 Resolution;
    float3 Cinematic;
};

static const float3 LuminanceWeight = float3(0.2126f, 0.7152f, 0.0722f);

float GetLuminance(float3 color)
{
    return dot(color, LuminanceWeight);
}

float3 AdjustSaturation(float3 color, float saturation)
{
    float luminance = GetLuminance(color);
    return lerp(float3(luminance, luminance, luminance), color, saturation);
}

float3 AdjustBrightness(float3 color, float brightness)
{
    return color * brightness;
}

float3 AdjustContrast(float3 color, float contrast)
{
    const float midPoint = 1.0f;
    return (color - midPoint) * contrast + midPoint;
}

[numthreads(8, 8, 1)] void main(uint2 DTid
                                : SV_DispatchThreadID) {
    if (DTid.x >= (uint)Resolution.x || DTid.y >= (uint)Resolution.y)
        return;

    float4 originalColor = TexColor.SampleLevel(ImageSampler, DTid + 0.5 / Resolution, 0);
    float3 color = originalColor.rgb;

    color = AdjustSaturation(color, Cinematic.x);
    color = AdjustBrightness(color, Cinematic.y);
    color = AdjustContrast(color, Cinematic.z);

    color = lerp(originalColor.rgb, color, BlendFactor);

    RWTexOut[DTid.xy] = float4(color, originalColor.a);
}