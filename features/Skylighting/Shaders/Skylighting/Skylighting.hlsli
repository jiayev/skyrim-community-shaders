// Define SL_INCL_STRUCT and SL_INCL_METHODS to include different parts
// Because this file is included by both forward and deferred shaders

#ifdef SL_INCL_STRUCT
struct SkylightingSettings
{
	row_major float4x4 OcclusionViewProj;
	float4 OcclusionDir;

	float4 PosOffset;
	uint4 ArrayOrigin;
	int4 ValidMargin;

	float4 MixParams;  // x: min diffuse visibility, y: diffuse mult, z: min specular visibility, w: specular mult

	uint DirectionalDiffuse;
	float3 _pad1;
};

#endif

#ifdef SL_INCL_METHODS

#	include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

#	ifdef PSHADER
Texture3D<sh2> SkylightingProbeArray : register(t29);
#	endif

const static uint3 SL_ARRAY_DIM = uint3(128, 128, 64);
const static float3 SL_ARRAY_SIZE = float3(10000, 10000, 10000 * 0.5);
const static float3 SL_CELL_SIZE = SL_ARRAY_SIZE / SL_ARRAY_DIM;

float getFadeOutFactor(float3 positionMS)
{
	float3 uvw = saturate(positionMS / SL_ARRAY_SIZE + .5);
	float3 dists = min(uvw, 1 - uvw);
	float edgeDist = min(dists.x, min(dists.y, dists.z));
	return saturate(edgeDist * 20);
}

sh2 sampleSkylighting(SkylightingSettings params, Texture3D<sh2> probeArray, float3 positionMS, float3 normalWS)
{
	const static sh2 unitSH = shEvaluate(float3(0, 0, 1));
	sh2 scaledUnitSH = unitSH / (params.MixParams.y + 1e-10);

	float3 positionMSAdjusted = positionMS - params.PosOffset;
	float3 uvw = positionMSAdjusted / SL_ARRAY_SIZE + .5;

	if (any(uvw < 0) || any(uvw > 1))
		return scaledUnitSH;

	float3 cellVxCoord = uvw * SL_ARRAY_DIM;
	int3 cell000 = floor(cellVxCoord - 0.5);
	float3 trilinearPos = cellVxCoord - 0.5 - cell000;

	sh2 sum = 0;
	float wsum = 0;
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2; j++)
			for (int k = 0; k < 2; k++) {
				int3 offset = int3(i, j, k);
				int3 cellID = cell000 + offset;

				if (any(cellID < 0) || any(cellID >= SL_ARRAY_DIM))
					continue;

				float3 cellCentreMS = cellID + 0.5 - SL_ARRAY_DIM / 2;
				cellCentreMS = cellCentreMS * SL_CELL_SIZE;

				// https://handmade.network/p/75/monter/blog/p/7288-engine_work__global_illumination_with_irradiance_probes
				// basic tangent checks
				if (dot(cellCentreMS - positionMSAdjusted, normalWS) <= 0)
					continue;

				float3 trilinearWeights = 1 - abs(offset - trilinearPos);
				float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z;

				uint3 cellTexID = (cellID + params.ArrayOrigin.xyz) % SL_ARRAY_DIM;
				sh2 probe = shScale(probeArray[cellTexID], w);

				sum = shAdd(sum, probe);
				wsum += w;
			}

	sh2 result = shScale(sum, rcp(wsum + 1e-10));

	// fade out
	result = lerp(scaledUnitSH, result, getFadeOutFactor(positionMS));

	return result;
}

float getVLSkylighting(SkylightingSettings params, Texture3D<sh2> probeArray, float3 startPosWS, float3 endPosWS, float2 screenPosition)
{
	const static uint nSteps = 16;
	const static float step = 1.0 / float(nSteps);

	float3 worldDir = endPosWS - startPosWS;
	float3 worldDirNormalised = normalize(worldDir);

	float noise = InterleavedGradientNoise(screenPosition);

	float vl = 0;

	for (uint i = 0; i < nSteps; ++i) {
		float t = saturate(i * step);

		float shadow = 0;
		{
			float3 samplePositionWS = startPosWS + worldDir * t;

			sh2 skylighting = sampleSkylighting(params, probeArray, samplePositionWS, float3(0, 0, 1));

			shadow += lerp(params.MixParams.x, 1, saturate(shUnproject(skylighting, worldDirNormalised) * params.MixParams.y));
		}
		vl += shadow;
	}
	return vl * step;
}

// http://torust.me/ZH3.pdf
// ZH hallucination that makes skylighting more directional
// skipped luminance because it's single channel
float shHallucinateZH3Irradiance(sh2 sh, float3 normal)
{
	const static float factor = sqrt(5.0f / (16.0f * 3.1415926f));

	float3 zonalAxis = normalize(sh.wyz);
	float ratio = abs(dot(sh.wyz, zonalAxis)) / sh.x;
	float3 zonalL2Coeff = sh.x * (0.08f * ratio + 0.6f * ratio * ratio);

	float fZ = dot(zonalAxis, normal);
	float zhDir = factor * (3.0f * fZ * fZ - 1.0f);

	float result = shFuncProductIntegral(sh, shEvaluateCosineLobe(normal));

	result += 0.25f * zonalL2Coeff * zhDir;

	return saturate(result);
}

sh2 fauxSpecularLobeSH(float3 N, float3 V, float roughness)
{
	// https://www.gdcvault.com/play/1026701/Fast-Denoising-With-Self-Stabilizing
	// get dominant ggx reflection direction
	float f = (1 - roughness) * (sqrt(1 - roughness) + roughness);
	float3 R = reflect(-V, N);
	float3 D = lerp(N, R, f);
	float3 dominantDir = normalize(D);

	sh2 directional = shEvaluate(dominantDir);
	sh2 cosineLobe = shEvaluateCosineLobe(dominantDir);
	sh2 result = shAdd(shScale(directional, f), shScale(cosineLobe, 1 - f));

	return result;
}

#endif