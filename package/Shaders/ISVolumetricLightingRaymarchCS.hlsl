#if defined(CSHADER)
SamplerState DensitySampler : register(s0);

Texture3D<float4> DensityTex : register(t0);

RWTexture3D<float4> DensityRW : register(u0);

cbuffer PerTechnique : register(b0)
{
	float3 TextureDimensions : packoffset(c0);
	int StepIndex : packoffset(c1);
}

[numthreads(32, 32, 1)] void main(uint3 dispatchID
								  : SV_DispatchThreadID) {
	float3 previousPosition = (0.5 + float3(dispatchID.xy, StepIndex - 1)) / TextureDimensions.xyz;
	float previousDensity = DensityTex.SampleLevel(DensitySampler, previousPosition, 0).x;
	DensityRW[uint3(dispatchID.xy, StepIndex - 1)] = previousDensity;

	float3 position = (0.5 + float3(dispatchID.xy, StepIndex)) / TextureDimensions.xyz;
	float density = DensityTex.SampleLevel(DensitySampler, position, 0).x;
	// Adaptive step size
	float stepSize = 1.0 / TextureDimensions.z;
	float gradient = abs(density - previousDensity);
	float adaptiveStepSize = stepSize * lerp(1.0, 2.0, saturate(gradient * 10.0));

	float transmittance = exp(-density * adaptiveStepSize);
	DensityRW[uint3(dispatchID.xy, StepIndex)] = previousDensity + density * (1.0 - transmittance) / max(density, 1e-5);
}
#endif
