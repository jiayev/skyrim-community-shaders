#define DX11

#include "Raytracing/ReSTIR/ReSTIRCommon.hlsli"

#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float4> ReservoirCurrTexture : register(t0);

RWTexture2D<float4> ReservoirSpatialTexture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 pixelCoord = DTid.xy;
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
        LightDX11 light = Lights[clamp((uint)reservoir.y, 0, LightCount - 1)];

        p_hat = GetLightWeight(light, normalWS, positionWS.xyz);

        newReservoir = UpdateReservoir(newReservoir, reservoir.y, p_hat * reservoir.w * reservoir.z, randSeed);

        float lightSamplesCount = newReservoir.z;

        int2 neighborOffset;
		uint2 neighborIndex;
		Reservoir neighborReservoir;

        for (int i = 0; i < NEIGHBOURS_COUNT; i++) {
            neighborOffset.x = int(Random(randSeed) * NEIGHBOURS_RANGE * 2.f) - NEIGHBOURS_RANGE;
            neighborOffset.y = int(Random(randSeed) * NEIGHBOURS_RANGE * 2.f) - NEIGHBOURS_RANGE;

            neighborIndex.x = max(0, min(textureSize.x - 1, pixelCoord.x + neighborOffset.x));
            neighborIndex.y = max(0, min(textureSize.y - 1, pixelCoord.y + neighborOffset.y));

            float3 neighborNormalGlossiness = NormalGlossinessTexture[neighborIndex].xyz;
            float3 neighborNormalVS = GBuffer::DecodeNormal(neighborNormalGlossiness.xy);
            float3 neighborNormalWS = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(neighborNormalVS, 0)).xyz);
            float neighborDepth = DepthTexture[neighborIndex].x;
            bool isValidNeighbor = IsValidNeighbor(neighborNormalWS, neighborDepth, normalWS, depth);
            if (isValidNeighbor) {
                neighborReservoir = ReservoirCurrTexture[neighborIndex];

                float p_hat_neighbor = GetLightWeight(Lights[(uint)neighborReservoir.y], normalWS, positionWS.xyz);

                newReservoir = UpdateReservoir(newReservoir, neighborReservoir.y, p_hat_neighbor * neighborReservoir.w * neighborReservoir.z, randSeed);

                lightSamplesCount += neighborReservoir.z;
            }
        }

        newReservoir.z = lightSamplesCount;

        light = Lights[clamp((uint)newReservoir.y, 0, LightCount - 1)];

        p_hat = GetLightWeight(light, normalWS, positionWS.xyz);

        newReservoir.w = (1 / max(p_hat, 0.0001f)) * (newReservoir.x / max(newReservoir.z, 0.0001f));
        reservoir = newReservoir;
    }

    ReservoirSpatialTexture[pixelCoord] = reservoir;
}