#ifndef SSPLS_COMMON
#define SSPLS_COMMON

#include "Common/SharedData.hlsli"
#include "Common/FrameBuffer.hlsli"

#define MAX_SAMPLES 4

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

	float Raymarch(SamplerState s, float3 viewPosition, float3 lightDirectionVS, uint steps, float step, float2 stepScale, float compareToleranceScale, float radius, float2x2 rotationMatrix, uint a_eyeIndex = 0)
	{
		float shadow = 0.0;
		float opacity = 1.0;
		radius *= 0.0001 * SharedData::ssplsSettings.SoftShadowScale;
		const float3 startPosition = viewPosition;

		float compareTolerance = abs(lightDirectionVS.z - viewPosition.z) * step * compareToleranceScale;
		
		[loop] for (uint i = 0; i < steps; i++) {
			// Step the ray
			const float stepScaleMult = steps == 1 ? 1.0 : (stepScale.x + i * (stepScale.y - stepScale.x) / (steps - 1));
			viewPosition += lightDirectionVS * step * stepScaleMult;

			float2 rayUV = FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex);

			// Ensure the UV coordinates are inside the screen
			if (!(rayUV.x >= 0.0 && rayUV.x <= 1.0 && rayUV.y >= 0.0 && rayUV.y <= 1.0))
				break;

			// Compute the difference between the ray's and the camera's depth
			float rayDepth = SharedData::GetScreenDepth(rayUV, a_eyeIndex);
			if (rayDepth != startPosition.z) {
				[branch] if (i == 0 || !SharedData::ssplsSettings.EnableSoftShadows) {
					float depthDelta = viewPosition.z - rayDepth;
					bool hit = abs(depthDelta - compareTolerance) < compareTolerance;
					if (hit) {
						return 0.0;
					}
				} else {
					float distanceMult = abs(rayDepth - startPosition.z) / (abs(lightDirectionVS.z - rayDepth) + 0.00001);
					const uint sampleTimes = min(MAX_SAMPLES, (uint)(i * distanceMult * 16) + 1);
					[loop] for (uint j = 0; j < sampleTimes; j++) {
						float2 sampleOffset = mul(Random::PoissonSampleOffsets16[(j * i) % 16], rotationMatrix);
						float2 sampleUV = sampleOffset * distanceMult * radius + rayUV;
						float sampleDepth = SharedData::GetScreenDepth(sampleUV, a_eyeIndex);
						float depthDelta = viewPosition.z - sampleDepth;
						bool hit = abs(depthDelta - compareTolerance) < compareTolerance;
						if (hit) {
							opacity -= 1.0 / sampleTimes;
						}
					}
				}
				if (opacity <= 0.0) {
					return 0.0;
				}
			}
		}

		return 1.0;
	}

	float GetShadow(SamplerState s, float3 viewPosition, float noise2D, float3 lightDirectionVS, uint steps, float radius, uint a_eyeIndex = 0, bool isShadowCaster = false)
	{
		if (steps == 0)
			return 1.0;

		const float rayLength = SharedData::ssplsSettings.RayLength;
		if (rayLength <= 0.0)
			return 1.0;
		lightDirectionVS *= rayLength;

		const float3 normalizedLightDirection = normalize(lightDirectionVS);
		float3 auxVec = (abs(normalizedLightDirection.x) < 0.9) ? float3(1, 0, 0) : float3(0, 1, 0);
		float3 T = normalize(cross(normalizedLightDirection, auxVec));
		float3 B = cross(normalizedLightDirection, T);

		// Offset with interleaved gradient noise
		const float offset = noise2D - 0.5;
		const float noise = Random::InterleavedGradientNoise(normalize(viewPosition.xy) + offset);
		float2 rotation;
		sincos(Math::TAU * noise, rotation.y, rotation.x);
		float2x2 rotationMatrix = float2x2(rotation.x, rotation.y, -rotation.y, rotation.x);

		const float step = 1.0 / steps;

		viewPosition += lightDirectionVS * (noise - 0.5) * step;

		const float startDepth = viewPosition.z;

#	if defined(SKIN) || defined(HAIR) || defined(EYE)
		const float2 stepScale = float2(0.5, 1.25);
		const float scaleMult = 0.5;
		const float shadowCasterToleranceScale = SharedData::ssplsSettings.CompareToleranceScale;
#	else
		const float2 stepScale = float2(1.0, 1.0);
		const float scaleMult = 1.0;
		const float shadowCasterToleranceScale = 0.4;
#	endif

		const float compareToleranceScale = isShadowCaster ? shadowCasterToleranceScale : SharedData::ssplsSettings.CompareToleranceScale;

		// Accumulate samples
		float shadow = 1.0;

		uint2 sampleCoord = SharedData::ConvertUVToSampleCoord(FrameBuffer::ViewToUV(viewPosition, true, a_eyeIndex), a_eyeIndex).xy;
		
		shadow = Raymarch(s, viewPosition, lightDirectionVS, steps, step, stepScale, compareToleranceScale, radius, rotationMatrix, a_eyeIndex);
		return shadow;
	}
}
#endif // SSPLS_COMMON