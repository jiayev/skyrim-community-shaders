#ifndef SSGI_VISIBILITY_HLSLI
#define SSGI_VISIBILITY_HLSLI

#include "Common/Math.hlsli"

static const uint SSGI_MAX_RAY = 32;
static const uint SSGI_MAX_PROBE_RAYS = 4;

// sin/cos of the positive bit-center offsets from the projected receiver normal.
// The negative half is mirrored, reducing the table and avoiding per-bit sincos.
static const float2 SSGI_BIT_SIN_COS[SSGI_MAX_RAY / 2] = {
	float2(0.9987954562, 0.0490676743),
	float2(0.9891765100, 0.1467304745),
	float2(0.9700312532, 0.2429801799),
	float2(0.9415440652, 0.3368898534),
	float2(0.9039892931, 0.4275550934),
	float2(0.8577286100, 0.5141027442),
	float2(0.8032075315, 0.5956993045),
	float2(0.7409511254, 0.6715589548),
	float2(0.6715589548, 0.7409511254),
	float2(0.5956993045, 0.8032075315),
	float2(0.5141027442, 0.8577286100),
	float2(0.4275550934, 0.9039892931),
	float2(0.3368898534, 0.9415440652),
	float2(0.2429801799, 0.9700312532),
	float2(0.1467304745, 0.9891765100),
	float2(0.0490676743, 0.9987954562)
};

uint ComputeOccludedBitfield(float minHorizon, float maxHorizon, inout uint globalOccludedBitfield)
{
	uint startHorizonInt = min((uint)(saturate(minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY);
	uint angleHorizonInt = min((uint)ceil(saturate(maxHorizon - minHorizon) * SSGI_MAX_RAY), SSGI_MAX_RAY - startHorizonInt);

	if (angleHorizonInt == 0)
		return 0;

	uint angleHorizonBitfield = 0xFFFFFFFFu;
	if (angleHorizonInt < SSGI_MAX_RAY)
		angleHorizonBitfield = (1u << angleHorizonInt) - 1u;
	uint currentOccludedBitfield = angleHorizonBitfield << startHorizonInt;
	currentOccludedBitfield &= ~globalOccludedBitfield;
	globalOccludedBitfield |= currentOccludedBitfield;
	return currentOccludedBitfield;
}

// Integrate the newly covered angular zones using the spherical Jacobian for
// a view-axis slice. RotationCount uniformly samples plane azimuth over PI;
// Lambert's 1/PI therefore cancels the azimuthal Monte Carlo factor. The
// returned value is diffuse illumination divided by PI, ready for one albedo
// multiplication at composite time.
void IntegrateBitfield(
	uint bitfield,
	float3 projectedNormal, float3 projectedNormalTangent,
	float projectedNormalLength, float projectedNormalSin, float projectedNormalCos,
	float3 sourceNormal, bool requireSourceFacing,
	out float scalarWeight, out float3 directionWeight)
{
	scalarWeight = 0;
	directionWeight = 0;

	const float bitAngle = Math::PI / float(SSGI_MAX_RAY);
	// Midpoint integration of cos(theta) * |sin(theta)| over all bins yields
	// bitAngle / sin(bitAngle). Using sin(bitAngle) is the corresponding analytic
	// finite-bin normalization, so a constant-radiance white furnace returns 1.
	const float normalizedBitMeasure = sin(bitAngle);
	// Newly covered hit bits never overlap, so sparse iteration caps all hit
	// integration work across the ray march at 32 iterations per slice.
	[loop] while (bitfield != 0)
	{
		uint bit = (uint)firstbitlow(bitfield);
		bitfield &= bitfield - 1u;

		uint mirroredBit = bit < SSGI_MAX_RAY / 2 ? bit : SSGI_MAX_RAY - 1 - bit;
		float2 bitSinCos = SSGI_BIT_SIN_COS[mirroredBit];
		float bitSin = bit < SSGI_MAX_RAY / 2 ? bitSinCos.x : -bitSinCos.x;
		float bitCos = bitSinCos.y;

		float3 direction = projectedNormal * bitCos + projectedNormalTangent * bitSin;
		// Outgoing Lambertian radiance is angle-independent over the source's
		// front hemisphere. Gate the back hemisphere, but do not multiply by a
		// second source cosine.
		if (requireSourceFacing && dot(sourceNormal, -direction) <= 0.0)
			continue;

		float sineFromView = bitSin * projectedNormalCos + bitCos * projectedNormalSin;
		float weight = projectedNormalLength * bitCos * abs(sineFromView) * normalizedBitMeasure;

		scalarWeight += weight;
		directionWeight += direction * weight;
	}
}

// Stratified inverse-CDF sampling of the same finite-bin integral used above.
// Each selected direction belongs to an open bit. Its probability is w_i / W,
// so W * L_i / rayCount estimates the uncovered integral without counting any
// screen-covered bin again. Changing rayCount changes variance, not energy.
struct OpenBitfieldSamples
{
	float3 direction0, direction1, direction2, direction3;
	float weight;
};

float3 GetOpenBitfieldDirection(OpenBitfieldSamples samples, uint ray)
{
	return ray == 0 ? samples.direction0 : ray == 1 ? samples.direction1 :
	                                   ray == 2     ? samples.direction2 :
	                                                  samples.direction3;
}

OpenBitfieldSamples SampleOpenBitfield(uint bitfield, float3 projectedNormal, float3 projectedNormalTangent,
	float projectedNormalLength, float projectedNormalSin, float projectedNormalCos,
	uint rayCount, float noise)
{
	OpenBitfieldSamples samples = (OpenBitfieldSamples)0;
	float totalWeight;
	float3 unusedMoment;
	IntegrateBitfield(bitfield, projectedNormal, projectedNormalTangent, projectedNormalLength,
		projectedNormalSin, projectedNormalCos, 0, false, totalWeight, unusedMoment);
	if (totalWeight <= 0)
		return samples;
	rayCount = clamp(rayCount, 1u, SSGI_MAX_PROBE_RAYS);
	float interval = totalWeight / rayCount;
	samples.weight = interval;
	float target = clamp(noise, 1e-6, 1.0 - 1e-6) * interval;
	float cumulative = 0;
	uint selected = 0;
	[loop] while (bitfield != 0 && selected < rayCount)
	{
		uint bit = (uint)firstbitlow(bitfield);
		bitfield &= bitfield - 1u;
		uint mirroredBit = bit < SSGI_MAX_RAY / 2 ? bit : SSGI_MAX_RAY - 1 - bit;
		float2 bitSinCos = SSGI_BIT_SIN_COS[mirroredBit];
		float bitSin = bit < SSGI_MAX_RAY / 2 ? bitSinCos.x : -bitSinCos.x;
		float bitCos = bitSinCos.y;
		float sineFromView = bitSin * projectedNormalCos + bitCos * projectedNormalSin;
		float weight = projectedNormalLength * bitCos * abs(sineFromView) * sin(Math::PI / SSGI_MAX_RAY);
		cumulative += weight;
		if (weight <= 0)
			continue;
		float3 direction = projectedNormal * bitCos + projectedNormalTangent * bitSin;
		[loop] while (selected < rayCount && target <= cumulative)
		{
			// Explicit slots keep DXBC free of indexable temporary arrays, including
			// at the lowest ray budget. Dynamic register-array indexing can spill.
			if (selected == 0)
				samples.direction0 = direction;
			else if (selected == 1)
				samples.direction1 = direction;
			else if (selected == 2)
				samples.direction2 = direction;
			else
				samples.direction3 = direction;
			++selected;
			target += interval;
		}
	}
	return samples;
}
#endif
