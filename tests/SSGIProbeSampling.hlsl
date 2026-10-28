#include "ScreenSpaceGI/visibility.hlsli"

struct Case
{
	uint mask;
	uint rays;
	float normalAngle;
	float normalLength;
};
StructuredBuffer<Case> Cases : register(t0);
struct Result
{
	float4 integral;
	float4 moment;
};
RWStructuredBuffer<Result> Results : register(u0);

float3 SourceRadiance(float3 direction)
{
	return float3(1, 0.5 + 0.3 * direction.x + 0.2 * direction.z, 0.25 + 0.5 * direction.x * direction.x);
}

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	uint sample = id.x & 1023u;
	Case data = Cases[id.x >> 10u];
	float sn, cn;
	sincos(data.normalAngle, sn, cn);
	float3 normal = float3(sn, 0, cn);
	float3 tangent = float3(cn, 0, -sn);
	OpenBitfieldSamples samples = SampleOpenBitfield(data.mask, normal, tangent, data.normalLength, sn, cn,
		data.rays, (sample + 0.5) / 1024.0);
	float weight = samples.weight;
	Result result = (Result)0;
	[loop] for (uint ray = 0; ray < data.rays; ++ray)
	{
		if (weight <= 0)
			break;
		float3 direction = GetOpenBitfieldDirection(samples, ray);
		float angle = atan2(dot(direction, tangent), dot(direction, normal));
		uint bit = min(uint((Math::HALF_PI - angle) * (32.0 / Math::PI)), 31u);
		if ((data.mask & (1u << bit)) == 0 || !all(isfinite(direction)) || abs(length(direction) - 1) > 1e-5)
			result.moment.w = 1;
		float3 radiance = SourceRadiance(direction);
		result.integral.xyz += radiance * weight;
		result.integral.w += weight;
		result.moment.xyz += direction * radiance.y * weight;
	}
	Results[id.x] = result;
}
