Texture2D<float4> SSSResult : register(t0);

RWTexture2D<float4> MainTexture : register(u0);
RWTexture2D<float> SSSGuide : register(u1);

float luminance(float3 color)
{
    return (color.x + 2 * color.y + color.z) / 4.0;
}

[numthreads(8, 8, 1)] void main(uint2 DTid : SV_DispatchThreadID)
{
    float4 sssColor = SSSResult[DTid];
    float4 beforeSSS = MainTexture[DTid];

    float guideValue = luminance(sssColor.xyz - beforeSSS.xyz);

    MainTexture[DTid] = sssColor;
    SSSGuide[DTid] = guideValue;
}