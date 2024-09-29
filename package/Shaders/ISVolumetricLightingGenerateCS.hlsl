#include "Common/Constants.hlsli"
#include "Common/Random.hlsli"

#if defined(CSHADER)
SamplerState ShadowmapSampler : register(s0);
SamplerState ShadowmapVLSampler : register(s1);
SamplerState InverseRepartitionSampler : register(s2);
SamplerState NoiseSampler : register(s3);

Texture2DArray<float4> ShadowmapTex : register(t0);
Texture2DArray<float4> ShadowmapVLTex : register(t1);
Texture1D<float4> InverseRepartitionTex : register(t2);
Texture3D<float4> NoiseTex : register(t3);

RWTexture3D<float4> DensityRW : register(u0);
RWTexture3D<float4> DensityCopyRW : register(u1);

cbuffer PerTechnique : register(b0)
{
	float4x4 CameraViewProj : packoffset(c0);
	float4x4 CameraViewProjInverse : packoffset(c4);
	float4x3 ShadowMapProj[3] : packoffset(c8);
	float3 EndSplitDistances : packoffset(c17.x);
	float ShadowMapCount : packoffset(c17.w);
	float EnableShadowCasting : packoffset(c18);
	float3 DirLightDirection : packoffset(c19);
	float3 TextureDimensions : packoffset(c20);
	float3 WindInput : packoffset(c21);
	float InverseDensityScale : packoffset(c21.w);
	float3 PosAdjust : packoffset(c22);
	float IterationIndex : packoffset(c22.w);
	float PhaseContribution : packoffset(c23.x);
	float PhaseScattering : packoffset(c23.y);
	float DensityContribution : packoffset(c23.z);
}

/**
 * @brief Computes the Schlick phase function for approximating scattering.
 *
 * The Schlick phase function is a simplified approximation of the Henyey-Greenstein phase function.
 * It provides a computationally efficient way to model anisotropic scattering effects in real-time applications.
 * 
 * @param cosTheta The cosine of the scattering angle (cosine of the angle between the light and the view direction).
 * @param g The anisotropy factor (-1 for full backward scattering, 1 for full forward scattering, and 0 for isotropic).
 * @return The Schlick phase function value for the given parameters.
 *
 * @note Performance vs Quality:
 * - Schlick is a faster approximation compared to Henyey-Greenstein because it avoids the expensive power operations.
 * - It provides visually similar results but sacrifices some accuracy, especially for complex anisotropic scattering.
 * - Ideal for real-time rendering where performance is critical, such as games (default for Skyrim).
 */
float SchlickPhase(float cosTheta, float g)
{
	float k = (1 - g * g) / (4 * M_PI * pow(1 + g * cosTheta, 2));
	return k;
}

/**
 * @brief Computes the Henyey-Greenstein phase function for scattering.
 *
 * The Henyey-Greenstein phase function is a widely used model for simulating light scattering in participating media.
 * It accurately models the anisotropy of light scattering, providing good realism for both forward and backward scattering.
 * 
 * @param cosTheta The cosine of the scattering angle (cosine of the angle between the light and the view direction).
 * @param g The anisotropy factor (-1 for full backward scattering, 1 for full forward scattering, and 0 for isotropic).
 * @return The Henyey-Greenstein phase function value for the given parameters.
 *
 * @note Performance vs Quality:
 * - More computationally expensive than Schlick due to the use of power operations.
 * - Offers a more physically accurate representation of scattering, particularly for media with pronounced forward or backward scattering behavior.
 * - Suitable for applications where visual fidelity is more important than real-time performance.
 */
float HenyeyGreensteinPhase(float cosTheta, float g)
{
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * M_PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

/**
 * @brief Computes the Mie phase function for light scattering.
 *
 * The Mie phase function models the scattering of light by larger particles, such as water droplets or fog.
 * It is more complex than the Henyey-Greenstein phase function and is often used to model more realistic atmospheric effects.
 * 
 * @param cosTheta The cosine of the scattering angle (cosine of the angle between the light and the view direction).
 * @param g The anisotropy factor (-1 for full backward scattering, 1 for full forward scattering, and 0 for isotropic).
 * @return The Mie phase function value for the given parameters.
 *
 * @note Performance vs Quality:
 * - More computationally expensive than both Schlick and Henyey-Greenstein due to the complex denominator and power operations.
 * - Produces the most physically accurate results, especially for scenes involving larger particles such as clouds or fog.
 * - Best suited for high-fidelity rendering applications, but might be too slow for real-time gaming without optimization.
 */
float MiePhase(float cosTheta, float g)
{
	float denom = pow(1 + g * g - 2 * g * cosTheta, 1.5);
	return (1.0 / (4.0 * M_PI)) * (1 + cosTheta * cosTheta) / denom;
}

[numthreads(32, 32, 1)] void main(uint3 dispatchID
								  : SV_DispatchThreadID) {
	const float3 StepCoefficients[] = { { 0, 0, 0 },
		{ 0, 0, 1.000000 },
		{ 0, 1.000000, 0 },
		{ 0, 1.000000, 1.000000 },
		{ 1.000000, 0, 0 },
		{ 1.000000, 0, 1.000000 },
		{ 1.000000, 1.000000, 0 },
		{ 1.000000, 1.000000, 1.000000 } };

	float3 depthUv = dispatchID.xyz / TextureDimensions.xyz + 0.001 * StepCoefficients[IterationIndex].xyz;
	float depth = InverseRepartitionTex.SampleLevel(InverseRepartitionSampler, depthUv.z, 0).x;
	float4 positionCS = float4(2 * depthUv.x - 1, 1 - 2 * depthUv.y, depth, 1);  // UVDepthToView(depthUv)

	float4 positionWS = mul(transpose(CameraViewProjInverse), positionCS);
	positionWS /= positionWS.w;

	float4 positionCSShifted = mul(transpose(CameraViewProj), positionWS);
	positionCSShifted /= positionCSShifted.w;

	float shadowMapDepth = positionCSShifted.z;

	float shadowContribution = 1;
	if (EndSplitDistances.z >= shadowMapDepth) {
		float4x3 lightProjectionMatrix = ShadowMapProj[0];
		float shadowMapThreshold = 0.01;
		float cascadeIndex = 0;
		if (2.5 < ShadowMapCount && EndSplitDistances.y < shadowMapDepth) {
			lightProjectionMatrix = ShadowMapProj[2];
			shadowMapThreshold = 0;
			cascadeIndex = 2;
		} else if (EndSplitDistances.x < shadowMapDepth) {
			lightProjectionMatrix = ShadowMapProj[1];
			shadowMapThreshold = 0;
			cascadeIndex = 1;
		}

		float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionWS.xyz, 1)).xyz;
		float shadowMapValue = ShadowmapTex.SampleLevel(ShadowmapSampler, float3(positionLS.xy, cascadeIndex), 0).x;

		if (EnableShadowCasting < 0.5) {
			float shadowMapVLValue = ShadowmapVLTex.SampleLevel(ShadowmapVLSampler, float3(positionLS.xy, cascadeIndex), 0).x;

			float baseShadowVisibility = 0;
			if (shadowMapValue >= positionLS.z - shadowMapThreshold) {
				baseShadowVisibility = 1;
			}

			float vlShadowVisibility = 0;
			if (shadowMapVLValue >= positionLS.z - shadowMapThreshold) {
				vlShadowVisibility = 1;
			}

			shadowContribution = min(baseShadowVisibility, vlShadowVisibility);
		} else if (shadowMapValue >= positionLS.z - shadowMapThreshold) {
			shadowContribution = 1;
		} else {
			shadowContribution = 0;
		}
	}

	float3 noiseUv = 0.0125 * (InverseDensityScale * (positionWS.xyz + WindInput));
	float noise = NoiseTex.SampleLevel(NoiseSampler, noiseUv, 0).x;
	noise = lerp(noise, InterleavedGradientNoise(positionWS.xy), 0.5);
	float densityFactor = noise * (1 - 0.75 * smoothstep(0, 1, saturate(2 * positionWS.z / 300)));
	float densityContribution = lerp(1, densityFactor, DensityContribution);

	float LdotN = dot(normalize(-positionWS.xyz), normalize(DirLightDirection));
	float forwardPhase = SchlickPhase(LdotN, PhaseScattering);
	float backwardPhase = SchlickPhase(LdotN, -PhaseScattering * 0.5);
	float phaseFactor = lerp(forwardPhase, backwardPhase, 0.5);
	float phaseContribution = lerp(1, phaseFactor, PhaseContribution);

	float vl = shadowContribution * densityContribution * phaseContribution;

	DensityRW[dispatchID.xyz] = vl;
	DensityCopyRW[dispatchID.xyz] = vl;
}
#endif
