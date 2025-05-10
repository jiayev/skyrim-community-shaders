#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float> texDepth : register(t0);

RWTexture2D<float> outDepth0 : register(u0);
RWTexture2D<float> outDepth1 : register(u1);
RWTexture2D<float> outDepth2 : register(u2);
RWTexture2D<float> outDepth3 : register(u3);
RWTexture2D<float> outLinearDepth0 : register(u4);
RWTexture2D<float> outLinearDepth1 : register(u5);
RWTexture2D<float> outLinearDepth2 : register(u6);
RWTexture2D<float> outLinearDepth3 : register(u7);

