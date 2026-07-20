#ifndef __SKYLIGHTING_DEPENDENCY_HLSL__
#define __SKYLIGHTING_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

namespace Skylighting
{
#if defined(SKYLIGHTING_PROBE_REGISTER)
	Texture3D<sh2> SkylightingProbeArray : register(SKYLIGHTING_PROBE_REGISTER);
#elif defined(PSHADER)
	Texture3D<sh2> SkylightingProbeArray : register(t50);
#endif

	// SH projections of constant full-sphere visibility and the two z hemispheres.
	// Probes themselves contain only the sampled upper sky cap; FULL_VISIBILITY_SH
	// remains the no-op sentinel for interiors and positions outside the probe grid.
	const static sh2 FULL_VISIBILITY_SH = float4(sqrt(4.0 * Math::PI), 0, 0, 0);
	const static sh2 UPPER_HEMISPHERE_SH = float4(sqrt(Math::PI), 0, 0.5 * sqrt(3.0 * Math::PI), 0);
	const static sh2 LOWER_HEMISPHERE_SH = float4(sqrt(Math::PI), 0, -0.5 * sqrt(3.0 * Math::PI), 0);
	const static sh2 UNIT_SH = FULL_VISIBILITY_SH;  // Compatibility alias for non-world call sites.

	const static uint3 ARRAY_DIM = uint3(256, 256, 128);
	const static float3 ARRAY_SIZE = 10000.f * float3(1, 1, 0.5);
	const static float3 CELL_SIZE = ARRAY_SIZE / ARRAY_DIM;

	float GetFadeOutFactor(float3 positionMS)
	{
		float3 uvw = saturate(positionMS / ARRAY_SIZE + .5);
		float3 dists = min(uvw, 1 - uvw);
		float edgeDist = min(dists.x, min(dists.y, dists.z));
		return saturate(edgeDist * 20);
	}

	float MixDiffuse(float visibility)
	{
		return lerp(SharedData::skylightingSettings.MinDiffuseVisibility, 1.0, visibility);
	}

	float MixSpecular(float visibility)
	{
		return lerp(SharedData::skylightingSettings.MinSpecularVisibility, 1.0, visibility);
	}

	sh2 GetMacroEnvironmentVisibilitySH(sh2 skyVisibilitySH)
	{
		// Infer the lowest-frequency bounded full-sphere visibility consistent
		// with the measured sky cap instead of assuming every unmeasured direction
		// is visible. A fully open cap still reconstructs exact full visibility.
		return SphericalHarmonics::ExtrapolateZonalCapVisibility(
			skyVisibilitySH, SharedData::skylightingSettings.OpenSkySH);
	}

	float EvaluateSkyVisibility(sh2 skyVisibilitySH, sh2 responseLobe, float fadeOutFactor = 1.0)
	{
		float openResponse = SphericalHarmonics::FuncProductIntegral(SharedData::skylightingSettings.OpenSkySH, responseLobe);
		float visibleResponse = max(0.0, SphericalHarmonics::FuncProductIntegral(skyVisibilitySH, responseLobe));
		float visibility = openResponse > EPSILON_WEIGHT_SUM ? saturate(visibleResponse / openResponse) : 1.0;
		return lerp(1.0, visibility, fadeOutFactor);
	}

	float EvaluateSkyDirectionalVisibility(sh2 skyVisibilitySH, float3 direction, float fadeOutFactor = 1.0)
	{
		return EvaluateSkyVisibility(skyVisibilitySH, SphericalHarmonics::Evaluate(normalize(direction)), fadeOutFactor);
	}

	float EvaluateSkyDiffuse(sh2 skyVisibilitySH, float3 normal, float fadeOutFactor = 1.0)
	{
		return MixDiffuse(EvaluateSkyVisibility(skyVisibilitySH, SphericalHarmonics::EvaluateCosineLobe(normal), fadeOutFactor));
	}

	float EvaluateFullSphereVisibility(sh2 visibilitySH, sh2 responseLobe, float fadeOutFactor = 1.0)
	{
		float openResponse = SphericalHarmonics::FuncProductIntegral(FULL_VISIBILITY_SH, responseLobe);
		float visibleResponse = max(0.0, SphericalHarmonics::FuncProductIntegral(visibilitySH, responseLobe));
		float visibility = openResponse > EPSILON_WEIGHT_SUM ? saturate(visibleResponse / openResponse) : 1.0;
		visibility = lerp(1.0, visibility, fadeOutFactor);
		return visibility;
	}

	float EvaluateEnvironmentVisibility(sh2 skyVisibilitySH, sh2 responseLobe, float fadeOutFactor = 1.0)
	{
		return EvaluateFullSphereVisibility(GetMacroEnvironmentVisibilitySH(skyVisibilitySH), responseLobe, fadeOutFactor);
	}

	float EvaluateEnvironmentDiffuse(sh2 skyVisibilitySH, float3 normal, float fadeOutFactor = 1.0)
	{
		return MixDiffuse(EvaluateEnvironmentVisibility(
			skyVisibilitySH, SphericalHarmonics::EvaluateCosineLobe(normal), fadeOutFactor));
	}

	float EvaluateSkySpecular(sh2 skyVisibilitySH, sh2 specularLobe, float fadeOutFactor = 1.0)
	{
		return MixSpecular(EvaluateSkyVisibility(skyVisibilitySH, specularLobe, fadeOutFactor));
	}

	float EvaluateEnvironmentSpecular(sh2 skyVisibilitySH, sh2 specularLobe, float fadeOutFactor = 1.0)
	{
		return MixSpecular(EvaluateEnvironmentVisibility(skyVisibilitySH, specularLobe, fadeOutFactor));
	}

	// Legacy ambient consumers represent full-environment/DALC illumination.
	float EvaluateDiffuse(sh2 skyVisibilitySH, float3 normal, float fadeOutFactor = 1.0)
	{
		return EvaluateEnvironmentDiffuse(skyVisibilitySH, normal, fadeOutFactor);
	}

	float EvaluateSpecular(sh2 skyVisibilitySH, sh2 specularLobe, float fadeOutFactor = 1.0)
	{
		return EvaluateEnvironmentSpecular(skyVisibilitySH, specularLobe, fadeOutFactor);
	}

#if defined(PSHADER)
	void ApplySkylighting(inout float3 diffuseColor, inout float3 directionalAmbientColor, float3 albedo, float skylightingDiffuse)
	{
		float maxScale = 1.0;
		if (directionalAmbientColor.x > 0.0)
			maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
		if (directionalAmbientColor.y > 0.0)
			maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
		if (directionalAmbientColor.z > 0.0)
			maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
		directionalAmbientColor *= maxScale;

		diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);

		float3 linAmbient = Color::IrradianceToLinear(directionalAmbientColor);
		float3 multiBounceSkylighting = MultiBounceAO(albedo, skylightingDiffuse);
		directionalAmbientColor = Color::IrradianceToGamma(linAmbient * multiBounceSkylighting);

		diffuseColor += directionalAmbientColor;
	}
#endif

#if defined(PSHADER) || defined(SKYLIGHTING_PROBE_REGISTER)
	sh2 Sample(float3 positionMS, float3 normalWS)
	{
		sh2 scaledUnitSH = FULL_VISIBILITY_SH / 1e-10;

		if (SharedData::InInterior)
			return scaledUnitSH;

		positionMS.xyz += normalWS * CELL_SIZE * 0.5;  // Receiver normal bias

		float3 positionMSAdjusted = positionMS - SharedData::skylightingSettings.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return scaledUnitSH;

		float3 cellVxCoord = uvw * ARRAY_DIM;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		sh2 sum = 0;
		float wsum = 0;
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 2; k++) {
					int3 offset = int3(i, j, k);
					int3 cellID = cell000 + offset;

					if (any(cellID < 0) || any((uint3)cellID >= ARRAY_DIM))
						continue;

					float3 cellCentreMS = cellID + 0.5 - ARRAY_DIM / 2;
					cellCentreMS = cellCentreMS * CELL_SIZE;

					// https://handmade.network/p/75/monter/blog/p/7288-engine_work__global_illumination_with_irradiance_probes
					// basic tangent checks
					float tangentWeight = dot(normalize(cellCentreMS - positionMSAdjusted), normalWS) * 0.5 + 0.5;

					float3 trilinearWeights = 1 - abs(offset - trilinearPos);
					float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z * tangentWeight;

					uint3 cellTexID = (cellID + SharedData::skylightingSettings.ArrayOrigin.xyz) % ARRAY_DIM;
					sh2 probe = SphericalHarmonics::Scale(SkylightingProbeArray[cellTexID], w);

					sum = SphericalHarmonics::Add(sum, probe);
					wsum += w;
				}

		return SphericalHarmonics::Scale(sum, rcp(wsum + EPSILON_WEIGHT_SUM));
	}

	// Compute skylighting diffuse for a receiver biased to face upward (grass/foliage).
	// The result is pre-divided by vertexAO so that a subsequent multiply by vertexAO
	// yields min(skylightingDiffuse, vertexAO). Pass vertexAO = 1 to skip this compensation.
	float GetVertexSkylightingDiffuse(float3 positionMS, float3 normalWS, float vertexAO)
	{
		if (SharedData::InInterior)
			return 1.0;

		float fadeOutFactor = GetFadeOutFactor(positionMS);

		float3 biasedNormal = normalWS;
		biasedNormal.z = max(0.0, biasedNormal.z);
		biasedNormal = normalize(biasedNormal);

		sh2 skylightingSH = Sample(positionMS, normalWS);
		float skylightingDiffuse = EvaluateEnvironmentDiffuse(skylightingSH, biasedNormal, fadeOutFactor);

		return saturate(skylightingDiffuse / max(vertexAO, 1e-5));
	}

	sh2 SampleNoBias(float3 positionMS)
	{
		sh2 scaledUnitSH = FULL_VISIBILITY_SH / 1e-10;

		if (SharedData::InInterior)
			return scaledUnitSH;

		float3 positionMSAdjusted = positionMS - SharedData::skylightingSettings.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return scaledUnitSH;

		float3 cellVxCoord = uvw * ARRAY_DIM;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		sh2 sum = 0;
		float wsum = 0;
		[unroll] for (int i = 0; i < 2; i++)
			[unroll] for (int j = 0; j < 2; j++)
				[unroll] for (int k = 0; k < 2; k++)
		{
			int3 offset = int3(i, j, k);
			int3 cellID = cell000 + offset;

			if (any(cellID < 0) || any((uint3)cellID >= ARRAY_DIM))
				continue;

			float3 cellCentreMS = cellID + 0.5 - ARRAY_DIM / 2;
			cellCentreMS = cellCentreMS * CELL_SIZE;

			float3 trilinearWeights = 1 - abs(offset - trilinearPos);
			float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z;

			uint3 cellTexID = (cellID + SharedData::skylightingSettings.ArrayOrigin.xyz) % ARRAY_DIM;
			sh2 probe = SphericalHarmonics::Scale(SkylightingProbeArray[cellTexID], w);

			sum = SphericalHarmonics::Add(sum, probe);
			wsum += w;
		}

		return SphericalHarmonics::Scale(sum, rcp(wsum + EPSILON_WEIGHT_SUM));
	}
#endif
}

#endif
