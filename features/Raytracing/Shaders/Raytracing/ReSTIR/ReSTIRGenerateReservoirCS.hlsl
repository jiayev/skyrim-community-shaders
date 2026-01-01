#define DX11

#include "Raytracing/ReSTIR/ReSTIRCommon.hlsli"

#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float4> ReservoirPrevTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
Texture2D<float4> NormalGlossinessTexture : register(t2);
Texture2D<float4> MotionVectorsTexture : register(t4);

RWTexture2D<float4> ReservoirCurrTexture : register(u0);

SamplerState LinearSampler : register(s0);

StructuredBuffer<Light> Lights : register(t3);

cbuffer ReSTIRCB : register(b1)
{
    uint SpatialReuse;
    uint TemporalReuse;
    uint InitialCandidateCount;
    uint LightCount;
    uint MaxCandidateCount;
    uint3 Padding;
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixelCoord = DTid.xy;
    uint2 textureSize;
    ReservoirCurrTexture.GetDimensions(textureSize.x, textureSize.y);

    if (pixelCoord.x >= textureSize.x || pixelCoord.y >= textureSize.y)
        return;

    uint eyeIndex = 0; // vr not supported for now

    float2 uv = float2(pixelCoord + 0.5) * SharedData::BufferDim.zw;
    float depth = DepthTexture[pixelCoord].x;
	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
    positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

    float3 normalGlossiness = NormalGlossinessTexture[pixelCoord].xyz;
    float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);
    float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);

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
        Light light = Lights[lightIndex];

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