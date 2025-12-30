#define DX11

#define NEIGHBOURS_COUNT 15
#define NEIGHBOURS_RANGE 5

#include "Raytracing/ReSTIR/ReSTIRCommon.hlsli"
#include "Raytracing/Includes/Types.hlsli"

#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float4> ReservoirCurrTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
Texture2D<float4> NormalGlossinessTexture : register(t2);

RWTexture2D<float4> ReservoirSpatialTexture : register(u0);

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
void ReSTIRSpatialReuseCS(uint3 DTid : SV_DispatchThreadID)
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

    Reservoir newReservoir = 0;
    Reservoir reservoir = ReservoirCurrTexture[pixelCoord];

    if (SpatialReuse)
    {
        float p_hat;
        Light light = Lights[(uint)reservoir.y];

        p_hat = GetLightWeight(light, normalWS, positionWS);

        newReservoir = UpdateReservoir(newReservoir, reservoir.y, p_hat * reservoir.w * reservoir.z, randSeed);

        float lightSamplesCount = newReservoir.z;

        int2 neighborOffset;
		int2 neighborIndex;
		Reservoir neighborReservoir;

        for (int i = 0; i < NEIGHBOURS_COUNT; i++) {
            neighborOffset.x = int(Random(randSeed) * NEIGHBOURS_RANGE * 2.f) - NEIGHBOURS_RANGE;
            neighborOffset.y = int(Random(randSeed) * NEIGHBOURS_RANGE * 2.f) - NEIGHBOURS_RANGE;

            neighborIndex.x = max(0, min(textureSize.x - 1, pixelCoord.x + neighborOffset.x));
            neighborIndex.y = max(0, min(textureSize.y - 1, pixelCoord.y + neighborOffset.y));

            neighborReservoir = ReservoirCurrTexture[neighborIndex];

            Light neighborLight = Lights[(uint)neighborReservoir.y];

            float p_hat_neighbor = GetLightWeight(neighborLight, normalWS, positionWS);

            newReservoir = UpdateReservoir(newReservoir, neighborReservoir.y, p_hat_neighbor * neighborReservoir.w * neighborReservoir.z, randSeed);

            lightSamplesCount += neighborReservoir.z;
        }

        newReservoir.z = lightSamplesCount;

        light = Lights[(uint)newReservoir.y];

        p_hat = GetLightWeight(light, normalWS, positionWS);

        newReservoir.w = (1 / max(p_hat, 0.0001f)) * (newReservoir.x / max(newReservoir.z, 0.0001f));
        reservoir = newReservoir;
    }

    ReservoirSpatialTexture[pixelCoord] = reservoir;
}