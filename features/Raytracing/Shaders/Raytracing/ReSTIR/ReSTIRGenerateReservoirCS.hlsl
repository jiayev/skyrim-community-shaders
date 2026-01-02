#define DX11
#define COMPUTESHADER

#include "Raytracing/ReSTIR/ReSTIRCommon.hlsli"

Texture2D<float4> ReservoirPrevTexture : register(t0);
Texture2D<half2> MotionVectorsTexture : register(t4);

RWTexture2D<float4> ReservoirCurrTexture : register(u0);

SamplerState LinearSampler : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixelCoord = DTid.xy;
    uint2 textureSize;
    ReservoirCurrTexture.GetDimensions(textureSize.x, textureSize.y);

    if (pixelCoord.x >= textureSize.x || pixelCoord.y >= textureSize.y)
        return;

    const uint eyeIndex = 0; // vr not supported for now

    const float2 uv = float2(pixelCoord + 0.5) * SharedData::BufferDim.zw;
    
    const float depth = DepthTexture[pixelCoord].x;
    const float depthView = ScreenToViewDepth(depth);   
    
    const float3 positionVS = ScreenToViewPosition(uv, depthView, NDCToView);
    const float3 positionCS = FrameBuffer::ViewToWorld(positionVS);
    const float3 positionWS = positionCS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;    
    
    const float3 normalWS = NormalRoughnessTexture[pixelCoord].xyz;

    uint randSeed = InitRandomSeed(pixelCoord, textureSize, SharedData::FrameCount);

    Reservoir prevReservoir = 0;
    if (TemporalReuse)
    {
        float2 prevUV = uv;
        ReprojectHit(MotionVectorsTexture, LinearSampler, float3(uv, depth), eyeIndex, prevUV);
        if (any(prevUV) >= 0.0f && any(prevUV) <= 1.0f)
        {
            uint2 prevPixelCoord = uint2(prevUV * SharedData::BufferDim.xy);
            prevReservoir = ReservoirPrevTexture[prevPixelCoord];
        }
    }

    Reservoir reservoir = 0;
    float p_hat;

    int lightIndex;

    // Generate initial candidates
    for (int i = 0; i < min(InitialCandidateCount, LightCount); i++)
    {
        lightIndex = min(int(Random(randSeed) * LightCount), LightCount - 1);
        LightDX11 light = Lights[lightIndex];

        p_hat = GetLightWeight(light, normalWS, positionWS.xyz);
        reservoir = UpdateReservoir(reservoir, lightIndex, p_hat, randSeed);
    }

    lightIndex = (int)reservoir.y;
    p_hat = GetLightWeight(Lights[lightIndex], normalWS, positionWS.xyz);

    reservoir.w = (1 / max(p_hat, 0.0001f)) * (reservoir.x / max(reservoir.z, 0.0001f));

    if (TemporalReuse)
    {
        Reservoir temporalReservoir = 0;
        temporalReservoir = UpdateReservoir(temporalReservoir, reservoir.y, p_hat * reservoir.w * reservoir.z, randSeed);

        p_hat = GetLightWeight(Lights[prevReservoir.y], normalWS, positionWS.xyz);
        prevReservoir.z = min(MaxCandidateCount * reservoir.z, prevReservoir.z);
        temporalReservoir = UpdateReservoir(temporalReservoir, prevReservoir.y, p_hat * prevReservoir.w * prevReservoir.z, randSeed);

        temporalReservoir.z = reservoir.z + prevReservoir.z;

        p_hat = GetLightWeight(Lights[temporalReservoir.y], normalWS, positionWS.xyz);

        temporalReservoir.w = (1 / max(p_hat, 0.0001f)) * (temporalReservoir.x / max(temporalReservoir.z, 0.0001f));

        reservoir = temporalReservoir;
    }

    ReservoirCurrTexture[pixelCoord] = reservoir;
}