#ifndef SSPLS_COMMON
#define SSPLS_COMMON

#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"

namespace ScreenSpacePointLightShadows
{
    Texture2D<float4> LinearDepthTexture : register(t56);
    Texture2D<float4> BlurredLinearDepthTexture : register(t57);

    float3 ViewToScreenCoord(float3 x, bool is_position = true, uint a_eyeIndex = 0)
    {
        float4 newPosition = float4(x, (float)is_position);
        float4 uv = mul(FrameBuffer::CameraProj[a_eyeIndex], newPosition);
        return float3((uv.xy / uv.w) * float2(0.5f, -0.5f) + 0.5f, uv.z / uv.w);
    }

	float GetShadow(SamplerState s, float3 viewPosition, float noise2D, float3 lightDirectionVS, uint steps, uint a_eyeIndex = 0, bool isShadowCaster = false)
	{
		if (steps == 0)
			return 1.0;

		const float rayLength = SharedData::ssplsSettings.RayLength;
		if (rayLength <= 0.0)
			return 1.0;
		lightDirectionVS *= isShadowCaster ? 0.1 : rayLength;

		const float3 normalizedLightDirection = normalize(lightDirectionVS);

		// Offset with interleaved gradient noise
		const float offset = noise2D - 0.5;

		const float step = 1.0 / steps;

		viewPosition += lightDirectionVS * offset * step;

		const float startDepth = viewPosition.z;

		const float compareTolerance = abs(lightDirectionVS.z - viewPosition.z) * step * isShadowCaster ? 0.5f : SharedData::ssplsSettings.CompareToleranceScale;

		// Accumulate samples
		float shadow = 0.0;
		for (uint i = 0; i < steps; i++) {
			// Step the ray
			viewPosition += lightDirectionVS * step;

			float2 rayUV = FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex);

			// Ensure the UV coordinates are inside the screen
			if (!(rayUV.x >= 0.0 && rayUV.x <= 1.0 && rayUV.y >= 0.0 && rayUV.y <= 1.0))
				break;

			// Compute the difference between the ray's and the camera's depth
			float rayDepth = SharedData::GetScreenDepth(rayUV, a_eyeIndex);

			if (rayDepth != startDepth) {
				float depthDelta = viewPosition.z - rayDepth;
				bool hit = abs(depthDelta - compareTolerance) < compareTolerance;

				if (hit) {
					return 0.0;
				}
			}
		}

		return 1.0;
	}
}
#endif // SSPLS_COMMON