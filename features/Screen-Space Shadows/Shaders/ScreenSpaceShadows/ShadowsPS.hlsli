cbuffer SSSData						     : register(b5)
{
    bool EnableSSS;
    uint FrameCount;
};

Texture2D<float> TexDepthSampler				: register(t20);
Texture2D<float> TexOcclusionSampler			: register(t21);

#if defined(DEFSHADOW)
    #define LinearSampler SampShadowMaskSampler
#else
    SamplerState LinearSampler					: register(s14);
#endif

float3 WorldToView(float3 x, bool is_position = true)
{
    return mul(ViewMatrix, float4(x, (float)is_position)).xyz;
}

float2 ViewToUV(float3 x, bool is_position = true)
{
    float4 uv = mul(ProjMatrix, float4(x, (float)is_position));
    return (uv.xy / uv.w) * float2(0.5f, -0.5f) + 0.5f;
}

float2 GetDynamicResolutionAdjustedScreenPosition(float2 uv)
{
    return uv * DynamicRes_WidthX_HeightY_PreviousWidthZ_PreviousHeightW.xy;
}

float PrepassScreenSpaceShadows(float3 positionWS)
{
#if defined(EYE)
    return 1;
#else
    if (EnableSSS){
        float2 texCoord = ViewToUV(WorldToView(positionWS));
        float2 coords = GetDynamicResolutionAdjustedScreenPosition(texCoord) / 2;
        float shadow = TexOcclusionSampler.SampleLevel(LinearSampler, coords, 0);
        return shadow;
    }
    return 1;
#endif
}