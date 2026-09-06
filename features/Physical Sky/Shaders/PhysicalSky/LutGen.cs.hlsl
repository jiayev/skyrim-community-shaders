#ifndef COMPUTESHADER
#	define COMPUTESHADER
#endif
#ifndef LUTGEN
#	define LUTGEN 0
#endif

#define PS_PREPASS_SAMPLERS
#define PS_PREPASS_RSRCS
#define OMIT_PS_NAMESPACE
#include "PhysicalSky/Common.hlsli"

#if LUTGEN == 3
RWTexture3D<float4> RWTexOutput : register(u0);
RWTexture3D<float4> RWTexSunOutput : register(u1);
#else
RWTexture2D<float4> RWTexOutput : register(u0);
#endif

float TransmittanceRayDistance(float u, float distance, float closest)
{
	// Concentrate quadrature around the ray's lowest altitude, where density
	// varies fastest. A grazing ray needs samples on both sides of that point.
	if (closest <= 0.0)
		return distance * u * u;
	if (u < 0.5) {
		const float v = 1.0 - 2.0 * u;
		return closest * (1.0 - v * v);
	}
	const float v = 2.0 * u - 1.0;
	return closest + (distance - closest) * v * v;
}

void rayMarch(
	float3 pos, float3 rayDir,
#if LUTGEN == 0
	float3 sunDir,
	inout float3 tr
#elif LUTGEN == 1
	float3 sunDir,
	inout float3 tr,
	inout float3 lum, inout float3 lumFactor
#elif LUTGEN == 2
	inout float3 tr,
	inout float3 lum
#elif LUTGEN == 3
	uint2 tid, uint depth,
	inout float3 tr,
	inout float3 lum, inout float3 lumSun
#endif
)
{
	const SharedData::PhysSkyData data = SharedData::physSkyData;

#if LUTGEN == 0
	const uint nsteps = 40;
#elif LUTGEN == 1
	const uint nsteps = 20;
#elif LUTGEN == 2
	const uint nsteps = 30;
#else
	const uint nsteps = depth - 1;
#endif

#if LUTGEN > 1
	const float3 sunDir = data.sunDir;
#endif

	float tGround = RayIntersectSphere(pos, rayDir, 0, data.rPlanet);

	float tAtmos = RayIntersectSphere(pos, rayDir, 0, data.rAtmosphere);
#if LUTGEN == 0
	// All transmittance texels describe unoccluded paths to the outer boundary.
	// Planet visibility is evaluated analytically when sampling the LUT.
	const float originRadius = length(pos);
	const float projectedOrigin = dot(pos, rayDir);
	float tMax = max(0.0, -projectedOrigin + sqrt(max(0.0, projectedOrigin * projectedOrigin +
															   (data.rAtmosphere - originRadius) * (data.rAtmosphere + originRadius))));
	const float tClosest = clamp(-projectedOrigin, 0.0, tMax);
#elif LUTGEN != 3
	float tMax = tGround > 0 ? tGround : tAtmos;
#endif
#if LUTGEN != 3 && LUTGEN != 0
	float dt = tMax / float(nsteps);
	float3 stride = dt * rayDir;
#endif

#if LUTGEN != 0
	float uSun = dot(rayDir, sunDir);
#	if LUTGEN == 1
	// The higher-order scattering approximation is isotropic.  Using the
	// sharply peaked aerosol phase here makes the small spherical quadrature
	// strongly dependent on whether a sample happens to land near the sun.
	float phaseAerosolSun = 0.25 * RCP_PI;
	float phaseRayleighSun = 0.25 * RCP_PI;
#	else
	float phaseAerosolSun = Phase::CornetteShanks(uSun, data.aerosolPhaseG);
	float phaseRayleighSun = Phase::Rayleigh(uSun);

	float uMasser = dot(rayDir, data.masserDir);
	float phaseAerosolMasser = Phase::CornetteShanks(uMasser, data.aerosolPhaseG);
	float phaseRayleighMasser = Phase::Rayleigh(uMasser);

	float uSecunda = dot(rayDir, data.secundaDir);
	float phaseAerosolSecunda = Phase::CornetteShanks(uSecunda, data.aerosolPhaseG);
	float phaseRayleighSecunda = Phase::Rayleigh(uSecunda);
#	endif
#endif

	float3 curr_pos = pos;
	[loop] for (uint i = 0; i < nsteps; ++i)
	{
#if LUTGEN == 3
		const float tNear = ApSliceDistance(i, depth);
		const float tFar = ApSliceDistance(i + 1, depth);
		const float dt = tFar - tNear;
		// Integrate each nonuniform interval at its midpoint; store at its far boundary.
		curr_pos = pos + (0.5 * (tNear + tFar)) * rayDir;
#elif LUTGEN == 0
		const float tNear = TransmittanceRayDistance(float(i) / nsteps, tMax, tClosest);
		const float tFar = TransmittanceRayDistance(float(i + 1) / nsteps, tMax, tClosest);
		const float dt = tFar - tNear;
		curr_pos = pos + (0.5 * (tNear + tFar)) * rayDir;
#else
		curr_pos += stride;
#endif

		float rouRayleigh, rouAerosol, rouOzone;
		SampleAtmosphere(
			max(0.f, (length(curr_pos) - data.rPlanet)),
			rouRayleigh, rouAerosol, rouOzone);
		float3 muSRayleigh = rouRayleigh * data.rayleighScatter;
		float3 muSAerosol = rouAerosol * data.aerosolScatter;
		float3 extinction = muSRayleigh + muSAerosol +
		                    rouAerosol * data.aerosolAbsorption +
		                    rouOzone * data.ozoneAbsorption;

		float3 trSample = exp(-dt * extinction);

#if LUTGEN != 0
		float3 scatterFactor = (1 - trSample) / extinction;

		float3 scatterNoPhase = muSRayleigh + muSAerosol;
#	if LUTGEN == 1  // multiscatter
		float3 fScatter = scatterNoPhase * scatterFactor;
		lumFactor += tr * fScatter;
#	endif

		float3 trSun = SampleAtmosphereLightTr(TexTrLut, SampTr, curr_pos, sunDir);
#	if LUTGEN != 1
		float3 trMasser = SampleAtmosphereLightTr(TexTrLut, SampTr, curr_pos, data.masserDir);

		float3 trSecunda = SampleAtmosphereLightTr(TexTrLut, SampTr, curr_pos, data.secundaDir);
		const float rSample = length(curr_pos);
		const float3 upSample = curr_pos / max(rSample, 1.0);
		const float2 lutUvSun = MsLutUv(rSample, dot(upSample, sunDir));
		const float2 lutUvMasser = MsLutUv(rSample, dot(upSample, data.masserDir));
		const float2 lutUvSecunda = MsLutUv(rSample, dot(upSample, data.secundaDir));

		float3 psiMs = TexMsLut.SampleLevel(SampTr, lutUvSun, 0).rgb * data.sunlightColor;
		psiMs += TexMsLut.SampleLevel(SampTr, lutUvMasser, 0).rgb * data.masserColor;
		psiMs += TexMsLut.SampleLevel(SampTr, lutUvSecunda, 0).rgb * data.secundaColor;
#	endif

		float3 inscatter = (muSRayleigh * phaseRayleighSun + muSAerosol * phaseAerosolSun) * trSun;
#	if LUTGEN != 1
		inscatter *= data.sunlightColor;
#		if LUTGEN == 3
		const float3 inscatterSun = inscatter;
		inscatter = 0.0;
#		endif
		inscatter += (muSRayleigh * phaseRayleighMasser + muSAerosol * phaseAerosolMasser) * trMasser * data.masserColor;
		inscatter += (muSRayleigh * phaseRayleighSecunda + muSAerosol * phaseAerosolSecunda) * trSecunda * data.secundaColor;
		inscatter += scatterNoPhase * psiMs;
#	endif

		float3 scatterIntegeral = inscatter * scatterFactor;

		lum += scatterIntegeral * tr;
#	if LUTGEN == 3
		lumSun += inscatterSun * scatterFactor * tr;
#	endif
#endif
		tr *= trSample;

#if LUTGEN == 3
		RWTexOutput[uint3(tid.xy, i + 1)] = float4(lum, dot(tr, float3(0.2126, 0.7152, 0.0722)));
		RWTexSunOutput[uint3(tid.xy, i + 1)] = float4(lumSun, 1.0);
#endif
	}

#if LUTGEN == 1  // multiscatter
	if (tGround > 0) {
		float3 hit_pos = pos + tGround * rayDir;
		if (dot(pos, sunDir) > 0) {
			hit_pos = normalize(hit_pos) * data.rPlanet;
			lum += tr * data.groundAlbedo * SampleAtmosphereLightTr(TexTrLut, SampTr, hit_pos, sunDir);
		}
	}
#endif
}

[numthreads(8, 8, 1)] void main(uint3 tid : SV_DispatchThreadID) {
	const SharedData::PhysSkyData data = SharedData::physSkyData;

#if LUTGEN == 3
	RWTexOutput[uint3(tid.xy, 0)] = float4(0, 0, 0, 1);
	RWTexSunOutput[uint3(tid.xy, 0)] = float4(0, 0, 0, 1);
#endif

	uint3 outDims;
#if LUTGEN == 3
	RWTexOutput.GetDimensions(outDims.x, outDims.y, outDims.z);
#else
	RWTexOutput.GetDimensions(outDims.x, outDims.y);
#endif
	float2 uv = (tid.xy + 0.5) / outDims.xy;

#if LUTGEN == 0
	float altitude, zenithCos;
	TrLutParameters(uv, altitude, zenithCos);
	float3 pos = float3(0, 0, altitude);
	float3 sunDir = float3(0, sqrt(max(0.0, 1.0 - zenithCos * zenithCos)), zenithCos);
#elif LUTGEN == 1
	float altitude = lerp(data.rPlanet, data.rAtmosphere, uv.y);
	float3 pos = float3(0, 0, altitude);

	// float horZenithCos = HorizonZenithCos(altitude);
	float horZenithCos = -0.414;
	float zenithCos = lerp(horZenithCos, 1, uv.x);
	float3 sunDir = float3(0, sqrt(1 - zenithCos * zenithCos), zenithCos);
#else
	float3 rayDir = InvSkyViewLutUv(uv);
	float3 sunDir = data.sunDir;
	float3 pos = float3(0, 0, data.zCameraPlanet);
#endif

	float3 tr = 1.0;
#if LUTGEN == 0
	rayMarch(pos, sunDir, sunDir, tr);
	RWTexOutput[tid.xy] = float4(tr, 1.0);

#elif LUTGEN == 1
	const uint sqrtSamples = 4;
	const float rcpSqrtSamples = rcp(sqrtSamples);
	const float rcpSamples = rcpSqrtSamples * rcpSqrtSamples;

	float3 lumTotal = 0;
	float3 fMs = 0;
	for (uint i = 0; i < sqrtSamples; ++i)
		for (uint j = 0; j < sqrtSamples; ++j) {
			const float theta = (i + 0.5) * (2.0 * Math::PI) * rcpSqrtSamples;
			const float phi = acos(1.0 - 2.0 * (j + 0.5) * rcpSqrtSamples);
			const float3 rayDir = SphericalDir(theta, phi);

			tr = 1;
			float3 lum = 0;
			float3 lumFactor = 0;
			rayMarch(pos, rayDir, sunDir, tr, lum, lumFactor);

			fMs += lumFactor;
			lumTotal += lum;
		}
	RWTexOutput[tid.xy] = float4(lumTotal * rcpSamples / (1 - fMs * rcpSamples), 1.0);

#elif LUTGEN == 2
	float3 lum = 0;
	rayMarch(pos, rayDir, tr, lum);
	RWTexOutput[tid.xy] = float4(lum, 1.0);

#elif LUTGEN == 3
	float3 lum = 0;
	float3 lumSun = 0;
	rayMarch(pos, rayDir, tid.xy, outDims.z, tr, lum, lumSun);
#endif
}
