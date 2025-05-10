// Create depth texture and downsampled linear depth texture

#include "Common/SharedData.hlsli"

Texture2D<float> texDepth : register(t0);

RWTexture2D<float> outDepth0 : register(u0);
RWTexture2D<float> outLinearDepth0 : register(u1);

SamplerState linearSampler : register(s0);

cbuffer blurBuffer : register(b1)
{
    uint MipLevel;
    uint Steps;
    uint ResX;
    uint ResY;
};

[numthreads(8, 8, 1)] void main(const uint2 dtid : SV_DispatchThreadID) {
    float linearDepth = 0;
    float2 texCoord = (dtid + 0.5) / float2(ResX, ResY);
    for (int i = 0; i < 4 ; i++)
        for (int j = 0; j < 4 ; j++) {
            uint2 newdtid = dtid * 4 + uint2(i, j);
            float2 sampleCoord = (texCoord + float2(i, j) * (1.0 / float2(ResX, ResY)));
            float depth = texDepth.SampleLevel(linearSampler, sampleCoord, 0).x;
            float lineared = SharedData::GetScreenDepth(depth);
            outDepth0[newdtid].x = depth;
            linearDepth += lineared;
        }
    outLinearDepth0[dtid].x = linearDepth / 16.0;
}