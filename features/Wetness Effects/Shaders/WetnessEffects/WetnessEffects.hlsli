#ifndef WETNESS_EFFECTS_HLSLI
#define WETNESS_EFFECTS_HLSLI

#include "Common/BRDF.hlsli"
#include "Common/Game.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "WetnessEffects/optimized-ggx.hlsli"

namespace WetnessEffects
{
	Texture2D<float4> TexPrecipOcclusion : register(t70);

	// https://github.com/BelmuTM/Noble/blob/master/LICENSE.txt

	float SmoothstepDeriv(float x)
	{
		return 6.0 * x * (1. - x);
	}

	float RainFade(float normalised_t)
	{
		const float rain_stay = .5;

		if (normalised_t < rain_stay)
			return 1.0;

		float val = lerp(1.0, 0.0, (normalised_t - rain_stay) / (1.0 - rain_stay));
		return val * val;
	}

	// https://blog.selfshadow.com/publications/blending-in-detail/
	// geometric normal s, a base normal t and a secondary (or detail) normal u
	float3 ReorientNormal(float3 u, float3 t, float3 s)
	{
		// Build the shortest-arc quaternion
		float4 q = float4(cross(s, t), dot(s, t) + 1) / sqrt(2 * (dot(s, t) + 1));

		// Rotate the normal
		return u * (q.w * q.w - dot(q.xyz, q.xyz)) + 2 * q.xyz * dot(q.xyz, u) + 2 * q.w * cross(q.xyz, u);
	}

	// for when s = (0,0,1)
	float3 ReorientNormal(float3 n1, float3 n2)
	{
		n1 += float3(0, 0, 1);
		n2 *= float3(-1, -1, 1);

		return n1 * dot(n1, n2) / n1.z - n2;
	}

	// xyz - ripple normal, w - splotches
	float4 GetRainDrops(float3 worldPos, float t, float3 normal, float rippleStrengthModifier = 1.0, float2 flowOffset = float2(0.0, 0.0))
	{
		// Apply flow offset to world position for flow-aware ripple positioning
		worldPos.xy += flowOffset;

		// Precompute constants
		float uintToFloat = rcp(4294967295.0);
		float rippleBreadthRcp = rcp(SharedData::wetnessEffectsSettings.RippleBreadth);
		float intervalRcp = SharedData::wetnessEffectsSettings.RaindropIntervalRcp;
		float lifetimeRcp = SharedData::wetnessEffectsSettings.RippleLifetimeRcp;

		// Calculate grid coordinates
		float2 gridUV = worldPos.xy * SharedData::wetnessEffectsSettings.RaindropGridSizeRcp + normal.xy;
		int2 grid = floor(gridUV);
		gridUV -= grid;

		// Initialize output values
		float3 rippleNormal = float3(0, 0, 1);
		float wetness = 0.0;

		// Early exit if no effects enabled
		bool hasEffects = SharedData::wetnessEffectsSettings.EnableSplashes || SharedData::wetnessEffectsSettings.EnableRipples;
		if (!hasEffects) {
			return float4(rippleNormal, wetness * SharedData::wetnessEffectsSettings.SplashesStrength);
		}

		// Process surrounding grid cells
		for (int i = -1; i <= 1; i++) {
			for (int j = -1; j <= 1; j++) {
				int2 gridCurr = grid + int2(i, j);
				float tOffset = float(Random::iqint3(gridCurr)) * uintToFloat;

				// Calculate splashes
				if (SharedData::wetnessEffectsSettings.EnableSplashes) {
					float residual = t * intervalRcp / SharedData::wetnessEffectsSettings.SplashesLifetime + tOffset + worldPos.z * 0.001;
					uint timestep = uint(residual);
					residual -= timestep;

					uint3 hash = Random::pcg3d(uint3(asuint(gridCurr), timestep));
					float3 floatHash = float3(hash) * uintToFloat;

					if (floatHash.z < SharedData::wetnessEffectsSettings.RaindropChance) {
						float2 vec2Centre = int2(i, j) + floatHash.xy - gridUV;
						float distSqr = dot(vec2Centre, vec2Centre);
						float dropRadius = lerp(SharedData::wetnessEffectsSettings.SplashesMinRadius,
							SharedData::wetnessEffectsSettings.SplashesMaxRadius,
							float(Random::iqint3(hash.yz)) * uintToFloat);
						if (distSqr < dropRadius * dropRadius) {
							wetness = max(wetness, RainFade(residual));
						}
					}
				}

				// Calculate ripples
				if (SharedData::wetnessEffectsSettings.EnableRipples) {
					float residual = t * intervalRcp + tOffset + worldPos.z * 0.001;
					uint timestep = uint(residual);
					residual -= timestep;

					uint3 hash = Random::pcg3d(uint3(asuint(gridCurr), timestep));
					float3 floatHash = float3(hash) * uintToFloat;

					if (floatHash.z < SharedData::wetnessEffectsSettings.RaindropChance) {
						float2 vec2Centre = int2(i, j) + floatHash.xy - gridUV;
						float distSqr = dot(vec2Centre, vec2Centre);
						float rippleT = residual * lifetimeRcp;

						if (rippleT < 1.0) {
							// Vary ripple size using high-quality random hash
							uint sizeHash = Random::iqint3(hash.xy);
							float sizeVariation = lerp(0.7, 1.3, float(sizeHash) * uintToFloat);

							float rippleRadius = SharedData::wetnessEffectsSettings.RippleRadius * sizeVariation;
							float rippleR = lerp(0.0, rippleRadius, rippleT);
							float rippleInnerRadius = rippleR - SharedData::wetnessEffectsSettings.RippleBreadth;

							float bandLerp = (sqrt(distSqr) - rippleInnerRadius) * rippleBreadthRcp;
							if (bandLerp > 0.0 && bandLerp < 1.0) {
								float rippleStrength = SharedData::wetnessEffectsSettings.RippleStrength * rippleStrengthModifier;
								float deriv = (bandLerp < 0.5 ? SmoothstepDeriv(bandLerp * 2.0) : -SmoothstepDeriv(2.0 - bandLerp * 2.0)) *
								              lerp(rippleStrength, 0.0, rippleT * rippleT);

								float3 grad = float3(normalize(vec2Centre), -deriv);
								float3 bitangent = float3(-grad.y, grad.x, 0.0);
								float3 normal = normalize(cross(grad, bitangent));

								rippleNormal = ReorientNormal(normal, rippleNormal);
							}
						}
					}
				}
			}
		}

		return float4(rippleNormal, wetness * SharedData::wetnessEffectsSettings.SplashesStrength);
	}

	float3 GetWetnessSpecular(float3 N, float3 L, float3 V, float3 lightColor, float roughness)
	{
		return LightingFuncGGX_OPT3(N, V, L, roughness, 0.02) * lightColor;
	}

	float4 HashWallRainFlow(float2 cell)
	{
		uint2 seed = asuint(int2(floor(cell)));
		uint3 hash = Random::pcg3d(uint3(seed, 0xA511E9B3u));
		uint hashW = hash.x * 1664525u + hash.y * 1013904223u + hash.z;
		return float4(float(hash.x), float(hash.y), float(hash.z), float(hashW)) * (1.0 / 4294967296.0);
	}

	float3 Hash31(float p)
	{
		float3 p3 = frac(float3(p, p, p) * float3(0.1031, 0.11369, 0.13787));
		p3 += dot(p3, p3.yzx + 19.19);
		return frac(float3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
	}

	float SawTooth(float t)
	{
		return cos(t + cos(t)) + sin(2.0 * t) * 0.2 + sin(4.0 * t) * 0.02;
	}

	float DeltaSawTooth(float t)
	{
		return 0.4 * cos(2.0 * t) + 0.08 * cos(4.0 * t) - (1.0 - sin(t)) * sin(t + cos(t));
	}

	float3 GetWallRainMainDropOffset(float2 uv, float seed, float time)
	{
		uv.y -= time * 0.32;
		uv *= float2(40.0, 6.0);

		float2 id = floor(uv);
		float3 n = Hash31(id.x + (id.y + seed) * 546.3524);
		float2 bd = frac(uv) - 0.5;

		bd.y *= 0.72;
		bd.x += (n.x - 0.5) * 0.28;

		float slidePhase = frac(time * 0.18 + n.z);
		float slide = slidePhase * 2.0 - 1.0;
		bd.y += slide * 1.55;
		bd.y += bd.x * bd.x * slide * 0.85;

		float d = length(bd);
		float mainDrop = smoothstep(0.2, 0.1, d);
		float2 offset = -bd * mainDrop * saturate(1.0 - d);
		return float3(offset, mainDrop);
	}

	float3 GetWallRainFineDropOffset(float2 uv, float time, float layerScale, float layerSpeed)
	{
		float2 p = uv * layerScale + float2(0.0, -time * layerSpeed);
		float2 x = float2(30.0, 30.0);
		float2 xuv = x * p;
		float4 n = HashWallRainFlow(floor(xuv - 0.3 + 0.5));
		float2 offset = (HashWallRainFlow(floor(p * 4.0)).xy - 0.5) * 2.0;
		float2 z = xuv * 6.3 + offset;

		float phase = frac(time * (n.b + 0.1) + n.g);
		float2 wave = sin(z) - phase * 0.5;
		float mask = step(0.5, wave.x + wave.y - n.r * 3.0);
		float2 dir = cos(z);
		return float3(dir * mask * 0.09, mask);
	}

	float3 GetWallRainDropOffset(float horizontalCoord, float downCoord)
	{
		static const float2 wallRainUvPerMeter = float2(0.45, 1.9);
		float rainAmount = saturate(SharedData::wetnessEffectsSettings.Raining);
		float densityScale = lerp(1.35, 2.4, rainAmount) * SharedData::wetnessEffectsSettings.WallRainDensity;
		float sizeScale = max(SharedData::wetnessEffectsSettings.WallRainDropSize, 0.1);
		float secondaryLayerWeight = smoothstep(0.3, 1.0, rainAmount) * 0.7 * SharedData::wetnessEffectsSettings.WallRainLayering;
		float2 uv = float2(horizontalCoord, downCoord) * (wallRainUvPerMeter * GAME_UNIT_TO_M * densityScale / sizeScale);
		float time = SharedData::wetnessEffectsSettings.Time * 0.55 * SharedData::wetnessEffectsSettings.WallRainSpeed;

		float3 mainDrop = GetWallRainMainDropOffset(uv, 1.0, time * 1.25);
		float3 secondaryMainDrop = GetWallRainMainDropOffset(uv * 1.23 + float2(7.13, 3.71), 5.31, time * 1.12) * secondaryLayerWeight;
		float3 fineDrop0 = GetWallRainFineDropOffset(uv, time, 1.0, 0.42) * float3(0.18, 0.10, 0.20);
		float2 rotatedUv = float2(0.8 * uv.x + 0.6 * uv.y, -0.6 * uv.x + 0.8 * uv.y);
		float3 fineDrop1 = GetWallRainFineDropOffset(rotatedUv, time, 2.02, 0.24) * float3(0.10, 0.08, 0.12);
		float3 fineDrop2 = GetWallRainFineDropOffset(rotatedUv * 2.53, time, 1.25, 0.24) * float3(0.06, 0.05, 0.08);

		float2 offset = mainDrop.xy + secondaryMainDrop.xy + fineDrop0.xy + fineDrop1.xy + fineDrop2.xy;
		float mask = saturate(max(mainDrop.z, secondaryMainDrop.z) + max(max(fineDrop0.z, fineDrop1.z), fineDrop2.z) * 0.25);
		return float3(clamp(offset * SharedData::wetnessEffectsSettings.WallRainNormalStrength, -3.0, 3.0), mask);
	}

	float4 GetWallRainFlowNormal(float3 worldPosition, float3 baseNormal)
	{
		float3 N = normalize(baseNormal);
		float3 absN = abs(N);
		float3 worldDown = float3(0, 0, -1);
		float3 horizontalAxis = absN.x > absN.y ? float3(0, 1, 0) : float3(1, 0, 0);

		float3 flowDirection = worldDown - N * dot(worldDown, N);
		float flowLengthSqr = dot(flowDirection, flowDirection);
		flowDirection = flowLengthSqr > 1e-4 ? flowDirection * rsqrt(flowLengthSqr) : worldDown;

		float3 horizontalTangent = horizontalAxis - N * dot(horizontalAxis, N);
		horizontalTangent -= flowDirection * dot(horizontalTangent, flowDirection);
		float horizontalLengthSqr = dot(horizontalTangent, horizontalTangent);
		horizontalTangent = horizontalLengthSqr > 1e-4 ? horizontalTangent * rsqrt(horizontalLengthSqr) : horizontalAxis;

		float horizontalCoord = dot(worldPosition, horizontalAxis);
		float downCoord = -worldPosition.z;
		float3 drop = GetWallRainDropOffset(horizontalCoord, downCoord);
		float2 slope = drop.xy;

		float3 localNormal = normalize(float3(-slope, 1.0));
		float3 rainNormal = normalize(localNormal.x * horizontalTangent + localNormal.y * flowDirection + localNormal.z * N);
		return float4(normalize(lerp(N, rainNormal, drop.z)), drop.z);
	}

	float GetWallRainFlowBlend(float3 vertexNormal, float wetnessOcclusion, float nearFactor)
	{
		if (SharedData::wetnessEffectsSettings.Raining <= 0.0)
			return 0.0;

		float verticalAmount = smoothstep(0.35, 0.95, 1.0 - saturate(abs(vertexNormal.z)));
		float rainAmount = saturate(SharedData::wetnessEffectsSettings.Raining);
		return verticalAmount * rainAmount * saturate(wetnessOcclusion * 2.0) * nearFactor;
	}

	struct SurfaceWetnessState
	{
		float3 normal;
		float roughness;
		float glossinessAlbedo;
		float glossinessSpecular;
	};

	SurfaceWetnessState InitSurfaceWetnessState(float3 normal, float roughness)
	{
		SurfaceWetnessState state;
		state.normal = normal;
		state.roughness = roughness;
		state.glossinessAlbedo = 0.0;
		state.glossinessSpecular = 0.0;
		return state;
	}

	float GetSurfaceWetnessRoughness(float glossinessSpecular)
	{
		static const float wetnessMinPuddleRoughness = 0.05;
		return max(saturate(1.0 - glossinessSpecular), wetnessMinPuddleRoughness);
	}

	SurfaceWetnessState GetSurfaceWetnessState(
		float3 worldPosition,
		float3 ripplePosition,
		float3 puddlePosition,
		float3 shadingNormal,
		float3 vertexNormal,
		float waterHeight,
		float wetnessOcclusion,
		float nearFactor,
		float rainWetnessOverride,
		float rainWetnessAdd,
		bool enablePuddleEffects,
		bool enableRainFlowEffects)
	{
		SurfaceWetnessState state;
		state.normal = vertexNormal;
		state.roughness = 1.0;
		state.glossinessAlbedo = 0.0;
		state.glossinessSpecular = 0.0;

		float wetnessDistToWater = abs(worldPosition.z - waterHeight);
		float shoreFactor = saturate(1.0 - (wetnessDistToWater / SharedData::wetnessEffectsSettings.ShoreRange));
		float shoreFactorAlbedo = (worldPosition.z < waterHeight) ? 1.0 : shoreFactor;

		float minWetnessValue = SharedData::wetnessEffectsSettings.MinRainWetness;
		float minWetnessAngle = saturate(max(minWetnessValue, vertexNormal.z));
		float flatnessAmount = smoothstep(SharedData::wetnessEffectsSettings.PuddleMaxAngle, 1.0, minWetnessAngle);

		float4 raindropInfo = float4(0, 0, 1, 0);
		bool shouldCalculateRaindrops = (shadingNormal.z > 0.0) &&
		                                (SharedData::wetnessEffectsSettings.Raining > 0.0) &&
		                                (SharedData::wetnessEffectsSettings.EnableRaindropFx) &&
		                                (wetnessOcclusion > 0.5);

		if (shouldCalculateRaindrops) {
			raindropInfo = GetRainDrops(ripplePosition, SharedData::wetnessEffectsSettings.Time, state.normal, flatnessAmount);
		}

		float rainWetness = SharedData::wetnessEffectsSettings.Wetness * minWetnessAngle * SharedData::wetnessEffectsSettings.MaxRainWetness;
		rainWetness = max(rainWetness, raindropInfo.w);
		if (rainWetnessOverride >= 0.0) {
			rainWetness = rainWetnessOverride;
		}
		rainWetness += rainWetnessAdd;

		float shoreWetness = shoreFactor * SharedData::wetnessEffectsSettings.MaxShoreWetness;
		float wetness = max(shoreWetness, rainWetness);

		float puddleWetness = enablePuddleEffects ? SharedData::wetnessEffectsSettings.PuddleWetness * minWetnessAngle : 0.0;
		float puddle = wetness;

		if (enablePuddleEffects && (wetness > 0.0 || puddleWetness > 0.0)) {
			float3 puddleCoords = (puddlePosition * 0.5 + 0.5) * 0.01 / SharedData::wetnessEffectsSettings.PuddleRadius;
			puddle = Random::perlinNoise(puddleCoords) * 0.5 + 0.5;
			puddle = puddle * ((minWetnessAngle / SharedData::wetnessEffectsSettings.PuddleMaxAngle) * SharedData::wetnessEffectsSettings.MaxPuddleWetness * 0.25) + 0.5;
			puddle *= lerp(wetness, puddleWetness, saturate(puddle - 0.25));
		}

		puddle *= saturate(wetnessOcclusion * 2.0) * nearFactor;
		state.normal = lerp(shadingNormal, state.normal, saturate(puddle));

		state.glossinessAlbedo = max(puddle, shoreFactorAlbedo * SharedData::wetnessEffectsSettings.MaxShoreWetness);
		state.glossinessAlbedo *= state.glossinessAlbedo;

		state.glossinessSpecular = puddle;
		if (worldPosition.z < waterHeight) {
			state.glossinessSpecular *= shoreFactor;
		}

		flatnessAmount *= smoothstep(SharedData::wetnessEffectsSettings.PuddleMinWetness, 1.0, state.glossinessSpecular);

		float3 rippleNormal = normalize(lerp(float3(0, 0, 1), raindropInfo.xyz, lerp(flatnessAmount, 1.0, 0.5)));
		state.normal = ReorientNormal(rippleNormal, state.normal);

		if (enableRainFlowEffects) {
			float rainFlowBlend = GetWallRainFlowBlend(vertexNormal, wetnessOcclusion, nearFactor);
			if (rainFlowBlend > 0.0) {
				float4 rainFlowNormal = GetWallRainFlowNormal(puddlePosition, state.normal);
				float rainFlowCoverage = saturate(rainFlowBlend * rainFlowNormal.w);
				float rainFlowNormalBlend = saturate(rainFlowBlend * rainFlowNormal.w * 1.5);
				state.normal = normalize(lerp(state.normal, rainFlowNormal.xyz, rainFlowNormalBlend));
				state.glossinessSpecular = max(state.glossinessSpecular, rainFlowCoverage * 0.9 * SharedData::wetnessEffectsSettings.WallRainWetness);
				state.glossinessAlbedo = max(state.glossinessAlbedo, rainFlowCoverage * SharedData::wetnessEffectsSettings.WallRainDarkening);
			}
		}

		state.roughness = GetSurfaceWetnessRoughness(state.glossinessSpecular);

		return state;
	}

	void ApplySurfaceWetnessAlbedo(inout float3 baseColor, SurfaceWetnessState wetnessState, float porosity)
	{
		float wetnessDarkeningAmount = porosity * wetnessState.glossinessAlbedo;
		baseColor = lerp(baseColor, pow(abs(baseColor), 1.0 + wetnessDarkeningAmount), 0.5);
	}

	void ApplySurfaceWetnessDirectLighting(
		SurfaceWetnessState wetnessState,
		float3 viewDirection,
		float3 lightDirection,
		float3 lightColor,
		float specularScale,
		inout float3 diffuse,
		inout float3 specular,
		inout float3 wetnessSpecular)
	{
		if (wetnessState.roughness >= 1.0)
			return;

		const float wetnessStrength = saturate(1.0 - wetnessState.roughness);
		const float wetnessF0 = 0.02;

		const float3 N = wetnessState.normal;
		const float3 V = viewDirection;
		const float3 L = lightDirection;
		const float3 H = normalize(V + L);

		float NdotL = clamp(dot(N, L), EPSILON_DOT_CLAMP, 1);
		float NdotV = saturate(abs(dot(N, V)) + EPSILON_DOT_CLAMP);
		float NdotH = saturate(dot(N, H));
		float VdotH = saturate(dot(V, H));

		float D = BRDF::D_GGX(wetnessState.roughness, NdotH);
		float G = BRDF::Vis_SmithJointApprox(wetnessState.roughness, NdotV, NdotL);
		float3 F = BRDF::F_Schlick(wetnessF0, VdotH);
		float3 wetnessF = F * wetnessStrength;

		float3 wetnessSpecularInput = D * G * wetnessF * NdotL * lightColor * specularScale;

		diffuse *= 1.0 - wetnessF;
		specular *= 1.0 - wetnessF;
		wetnessSpecular += wetnessSpecularInput;
	}

	float3 ApplySurfaceWetnessIndirectLobeWeights(inout float3 diffuseLobeWeight, inout float3 specularLobeWeight, SurfaceWetnessState wetnessState, float3 viewDirection)
	{
#if defined(DYNAMIC_CUBEMAPS)
		if (wetnessState.roughness >= 1.0)
			return 0.0;

		const float wetnessF0 = 0.02;
		const float wetnessStrength = saturate(1.0 - wetnessState.roughness);

		float NdotV = saturate(abs(dot(wetnessState.normal, viewDirection)) + EPSILON_DOT_CLAMP);
		float2 specularBRDF = BRDF::EnvBRDF(wetnessState.roughness, NdotV);
		float3 wetnessLobeWeight = wetnessF0 * specularBRDF.x + specularBRDF.y;
		wetnessLobeWeight *= wetnessStrength;

		diffuseLobeWeight *= 1.0 - wetnessLobeWeight;
		specularLobeWeight *= 1.0 - wetnessLobeWeight;

		return wetnessLobeWeight;
#else
		return 0.0;
#endif
	}

	void ApplySurfaceWetnessOutput(SurfaceWetnessState wetnessState, inout float3 normal, inout float roughness)
	{
		if (wetnessState.roughness >= 1.0)
			return;

		normal = wetnessState.normal;
		roughness = wetnessState.roughness;
	}

// Debug visualization functions for DEBUG_WETNESS_EFFECTS
#ifdef DEBUG_WETNESS_EFFECTS
	/**
	 * Calculates ripple and splash effect intensities from water ripple info
	 *
	 * @param rippleInfo float4 containing scaled ripple normal (xyz) and splash intensity (w)
	 *                   Note: xyz = normalized ripple normal * intensity multiplier
	 * @param rippleMultiplier Multiplier for ripple effect intensity
	 * @param splashMultiplier Multiplier for splash effect intensity
	 * @return float2 where x=ripple effect, y=splash effect
	 */
	float2 GetDebugEffectIntensities(float4 rippleInfo, float rippleMultiplier, float splashMultiplier)
	{
		// rippleInfo.xyz is a scaled normal vector (normalized normal * intensity)
		// length() gives us the intensity/magnitude of the ripple effect
		float rippleEffect = saturate(length(rippleInfo.xyz) * rippleMultiplier);
		float splashEffect = saturate(rippleInfo.w * splashMultiplier);
		return float2(rippleEffect, splashEffect);
	}

	/**
	 * Generates debug color visualization for wetness effects
	 *
	 * @param effectIntensities float2 from GetDebugEffectIntensities()
	 * @param rippleColor Color to use for ripple visualization
	 * @param splashColor Color to use for splash visualization
	 * @param baseColor Base color to start with (default black)
	 * @param brightnessMultiplier Multiplier for effect brightness
	 * @return float3 Debug color, or (0,0,0) if no effects are active
	 */
	float3 GetDebugWetnessColor(float2 effectIntensities, float3 rippleColor, float3 splashColor, float3 baseColor = float3(0, 0, 0), float brightnessMultiplier = 1.0)
	{
		float threshold = 0.01;
		float rippleEffect = effectIntensities.x;
		float splashEffect = effectIntensities.y;

		if (rippleEffect > threshold || splashEffect > threshold) {
			float3 debugColor = baseColor;
			if (rippleEffect > threshold) {
				debugColor += rippleColor * (rippleEffect * brightnessMultiplier);
			}
			if (splashEffect > threshold) {
				debugColor += splashColor * (splashEffect * brightnessMultiplier);
			}
			return saturate(debugColor);
		}
		return float3(0, 0, 0);  // No debug override
	}

	/**
	 * Convenience function for standard water debug colors
	 */
	float3 GetDebugWetnessColorStandard(float4 rippleInfo, float rippleMultiplier, float splashMultiplier)
	{
		float2 effects = GetDebugEffectIntensities(rippleInfo, rippleMultiplier, splashMultiplier);
		float3 rippleColor = float3(1.0, 0.0, 1.0);  // BRIGHT MAGENTA
		float3 splashColor = float3(0.0, 1.0, 0.0);  // BRIGHT GREEN
		return GetDebugWetnessColor(effects, rippleColor, splashColor);
	}

	/**
	 * Convenience function for specular debug colors (extra bright)
	 */
	float3 GetDebugWetnessColorSpecular(float4 rippleInfo, float rippleMultiplier, float splashMultiplier)
	{
		float2 effects = GetDebugEffectIntensities(rippleInfo, rippleMultiplier, splashMultiplier);
		float3 rippleColor = float3(1.0, 0.0, 1.0);                                            // BRIGHT MAGENTA
		float3 splashColor = float3(0.0, 1.0, 0.0);                                            // BRIGHT GREEN
		return GetDebugWetnessColor(effects, rippleColor, splashColor, float3(0, 0, 0), 1.5);  // Extra bright
	}

	/**
	 * Convenience function for underwater debug colors (darker)
	 */
	float3 GetDebugWetnessColorUnderwater(float4 rippleInfo, float rippleMultiplier, float splashMultiplier)
	{
		float2 effects = GetDebugEffectIntensities(rippleInfo, rippleMultiplier, splashMultiplier);
		float3 rippleColor = float3(0.7, 0.0, 0.7);                                         // DARK MAGENTA
		float3 splashColor = float3(0.0, 0.7, 0.0);                                         // DARK GREEN
		return GetDebugWetnessColor(effects, rippleColor, splashColor, float3(0, 0, 0.2));  // Dark blue base
	}
#endif

	/**
	 * Calculates flow-aware ripple positioning with proper timing synchronization
	 *
	 * @param worldFlowVector Flow vector in world coordinate space
	 * @param flowStrength Flow strength (0-1) from flowmap alpha channel
	 * @param reflectionTimingScale Timing scale factor (typically 0.001 * ReflectionColor.w)
	 * @param avgFlowmapMultiplier Average multiplier from flowmap normal calculations
	 * @param uvToWorldScale Scale factor converting UV coordinates to world positioning (typically 1/8)
	 * @return float2 Flow offset to apply to ripple positioning
	 *
	 * @details This function synchronizes ripple movement timing with flowmap normal animations
	 *          by using the same mathematical relationship and dual-phase smoothstep timing.
	 *          The timing creates natural flow-based ripple movement that matches the water surface animation.
	 */
	float2 GetFlowAwareRippleOffset(float2 worldFlowVector, float flowStrength, float reflectionTimingScale, float avgFlowmapMultiplier = 9.26, float uvToWorldScale = 0.125)
	{
		// Calculate flow timing scale matching flowmap normal timing
		// Mathematical relationship: avgMultiplier × uvToWorldScale gives base flow scaling
		// uvToWorldScale (1/8) relates to the 64× texture coordinate scaling: 64 × (1/8) = 8
		float baseFlowMultiplier = avgFlowmapMultiplier * uvToWorldScale;  // ≈ 1.16
		float flowTimeScale = baseFlowMultiplier * reflectionTimingScale;

		// Calculate base flow offset with strength modulation
		float2 flowOffset = worldFlowVector * (flowTimeScale * flowStrength);

		// Apply dual-phase smoothstep timing for natural flow animation
		// This creates the essential dual-phase animation pattern used in flowmap blending
		float smoothTime = smoothstep(0.0, 1.0, frac(flowTimeScale));
		smoothTime = lerp(0.15, 1.0, smoothTime);  // Range: 0.15→1.0→0.15 (avoids complete stops)

		return flowOffset * smoothTime;
	}

}

#endif
