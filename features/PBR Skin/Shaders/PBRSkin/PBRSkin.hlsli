#include "Common/PBR.hlsli"

namespace PBRSkin{
    float CalculateApproximateAO(float3 skincolor, float2 uv, float scale)
    {
        float3 baseColorApprox = skincolor;
        float luminance = 0.2126 * baseColorApprox.r + 0.7152 * baseColorApprox.g + 0.0722 * baseColorApprox.b;
        float approximateAO = pow(luminance, 2.4) * scale;
        return saturate(approximateAO);
    }
}