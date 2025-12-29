#define DX11

#include "Raytracing/ReSTIR/ReSTIRCommon.hlsli"
#include "Raytracing/Includes/Types.hlsli"

#include "Common/SharedData.hlsli"

Texture2D<float4> ReservoirCurrTexture : register(t0);

RWTexture2D<float4> ReservoirSpatialTexture : register(u0);

StructuredBuffer<Light> Lights : register(t1);

cbuffer ReSTIRCB : register(b1)
{
    uint SpatialReuse;
    uint TemporalReuse;
    uint InitialCandidateCount;
    uint MaxCandidateCount;
    uint LightCount;
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

    uint randSeed = InitRandomSeed(pixelCoord, textureSize, SharedData::FrameCount);

    // Load current reservoir
    Reservoir currentReservoir = ReservoirCurrTexture[pixelCoord];

    // Initialize new reservoir
    Reservoir newReservoir = currentReservoir;

    // Spatial reuse from neighboring pixels
    if (SpatialReuse != 0)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                if (x == 0 && y == 0) continue; // Skip current pixel

                uint2 neighborCoord = pixelCoord + uint2(x, y);
                if (neighborCoord.x < textureSize.x && neighborCoord.y < textureSize.y)
                {
                    Reservoir neighborReservoir = ReservoirCurrTexture[neighborCoord];

                    newReservoir = UpdateReservoir(newReservoir, (int)neighborReservoir.y, neighborReservoir.x, randSeed);
                }
            }
        }
    }

    // Store updated reservoir
    ReservoirSpatialTexture[pixelCoord] = newReservoir;
}