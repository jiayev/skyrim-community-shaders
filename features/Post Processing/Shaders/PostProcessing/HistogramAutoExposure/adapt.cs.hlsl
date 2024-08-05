/// By ProfJack/五脚猫, 2024-2-17 UTC

#include "common.hlsli"

RWTexture2D<float4> RWTexOut : register(u0);

Texture2D<float4> TexColor : register(t0);
StructuredBuffer<float> RWTexAdaptation : register(t1);

// ref:
// https://advances.realtimerendering.com/s2021/jpatry_advances2021/index.html#/167/0/1
// https://github.com/google/filament
// https://www.shadertoy.com/view/ft3Sz7
float4 RGB2LMSR(float3 c)
{
	const float4x3 m = float4x3(
						   0.31670331, 0.70299344, 0.08120592,
						   0.10129085, 0.72118661, 0.12041039,
						   0.01451538, 0.05643031, 0.53416779,
						   0.01724063, 0.60147464, 0.40056206) *
	                   24.303;
	return mul(m, c);
}

float3 LMS2RGB(float3 c)
{
	const float3x3 m = float3x3(
						   4.57829597, -4.48749114, 0.31554848,
						   -0.63342362, 2.03236026, -0.36183302,
						   -0.05749394, -0.09275939, 1.90172089) /
	                   24.303;
	return mul(m, c);
}

// https://www.ncbi.nlm.nih.gov/pmc/articles/PMC2630540/pdf/nihms80286.pdf
float3 PurkinjeShift(float3 c, float nightAdaptation)
{
	const static float3 m = float3(0.63721, 0.39242, 1.6064);
	const static float K = 45.0;
	const static float S = 10.0;
	const static float k3 = 0.6;
	const static float k5 = 0.2;
	const static float k6 = 0.29;
	const static float rw = 0.139;
	const static float p = 0.6189;

	const static float logExposure = 380.0f;

	float4 lmsr = RGB2LMSR(c * logExposure);

	float3 g = 1 / sqrt(1 + (.33 / m) * (lmsr.xyz + float3(k5, k5, k6) * lmsr.w));

	float rc_gr = (K / S) * ((1.0 + rw * k3) * g.y / m.y - (k3 + rw) * g.x / m.x) * k5 * lmsr.w;
	float rc_by = (K / S) * (k6 * g.z / m.z - k3 * (p * k5 * g.x / m.x + (1.0 - p) * k5 * g.y / m.y)) * lmsr.w;
	float rc_lm = K * (p * g.x / m.x + (1.0 - p) * g.y / m.y) * k5 * lmsr.w;

	float3 lms_gain = float3(-0.5 * rc_gr + 0.5 * rc_lm, 0.5 * rc_gr + 0.5 * rc_lm, rc_by + rc_lm) * nightAdaptation;
	float3 rgb_gain = LMS2RGB(lmsr.rgb + lms_gain) / logExposure;

	return rgb_gain;
}

[numthreads(32, 32, 1)] void main(uint2 tid
								  : SV_DispatchThreadID) {
	const static float logEV = -3;  // log2(0.125)

	float3 color = TexColor[tid].rgb;

	// auto exposure
	float avgLuma = RWTexAdaptation[0];
	color *= 0.18 * ExposureCompensation / clamp(avgLuma, AdaptationRange.x, AdaptationRange.y);

	// purkinje shift
	if (PurkinjeStrength > 1e-3) {
		float purkinjeMix = lerp(PurkinjeStrength, 0.f, saturate((log2(avgLuma) - logEV - PurkinjeMaxEV) / (PurkinjeStartEV - PurkinjeMaxEV)));
		if (purkinjeMix > 1e-3)
			color = PurkinjeShift(color, purkinjeMix);
	}

	RWTexOut[tid] = float4(color, 1);
}