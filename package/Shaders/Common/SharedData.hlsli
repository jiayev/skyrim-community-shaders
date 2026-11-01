#ifndef __SHARED_DATA_DEPENDENCY_HLSL__
#define __SHARED_DATA_DEPENDENCY_HLSL__

#include "Common/FrameBuffer.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

#include "Common/NamespacedCBuffer.hlsli"

namespace SharedData
{
	static const uint TonemapOwnerVanilla = 0;
	static const uint TonemapOwnerPostProcessing = 1;
	static const uint TonemapOwnerEffects11 = 2;

	cbuffer SharedData : register(b5)
	{
		float4 NSCB(SharedData, WaterData)[25];
		float4 NSCB(SharedData, DirLightDirection);
		float4 NSCB(SharedData, DirLightColor);
		float4 NSCB(SharedData, SunDirection);
		float4 NSCB(SharedData, SunColor);
		float4 NSCB(SharedData, MasserDirection);
		float4 NSCB(SharedData, MasserColor);
		float4 NSCB(SharedData, SecundaDirection);
		float4 NSCB(SharedData, SecundaColor);
		float4 NSCB(SharedData, CameraData);
		float4 NSCB(SharedData, BufferDim);
		float NSCB(SharedData, Timer);
		uint NSCB(SharedData, FrameCount);
		uint NSCB(SharedData, FrameCountAlwaysActive);
		bool NSCB(SharedData, InInterior);  // If the current cell is an interior
		bool NSCB(SharedData, HasDirectionalShadows);
		bool NSCB(SharedData, InMapMenu);           // If the world/local map is open (note that the renderer is still deferred here)
		bool NSCB(SharedData, HideSky);             // HideSky flag in WorldSpace, e.g. Blackreach
		float NSCB(SharedData, MipBias);            // Offset to mip level for TAA sharpness
		float NSCB(SharedData, WaterSystemHeight);  // TES::GetWaterHeight in camera-relative Z; -FLT_MAX when no water body found
		uint NSCB(SharedData, PostWaterComposite);
		uint NSCB(SharedData, ResetHistory);
		uint NSCB(SharedData, TonemapOwner);
		float4 NSCB(SharedData, AmbientSHR);
		float4 NSCB(SharedData, AmbientSHG);
		float4 NSCB(SharedData, AmbientSHB);
		float4 NSCB(SharedData, HDRData);
	};
	NSCB_ARRAY_ALIAS(SharedData, float4, WaterData, 25)
	NSCB_ALIAS(SharedData, float4, DirLightDirection)
	NSCB_ALIAS(SharedData, float4, DirLightColor)
	NSCB_ALIAS(SharedData, float4, SunDirection)
	NSCB_ALIAS(SharedData, float4, SunColor)
	NSCB_ALIAS(SharedData, float4, MasserDirection)
	NSCB_ALIAS(SharedData, float4, MasserColor)
	NSCB_ALIAS(SharedData, float4, SecundaDirection)
	NSCB_ALIAS(SharedData, float4, SecundaColor)
	NSCB_ALIAS(SharedData, float4, CameraData)
	NSCB_ALIAS(SharedData, float4, BufferDim)
	NSCB_ALIAS(SharedData, float, Timer)
	NSCB_ALIAS(SharedData, uint, FrameCount)
	NSCB_ALIAS(SharedData, uint, FrameCountAlwaysActive)
	NSCB_ALIAS(SharedData, bool, InInterior)
	NSCB_ALIAS(SharedData, bool, HasDirectionalShadows)
	NSCB_ALIAS(SharedData, bool, InMapMenu)
	NSCB_ALIAS(SharedData, bool, HideSky)
	NSCB_ALIAS(SharedData, float, MipBias)
	NSCB_ALIAS(SharedData, float, WaterSystemHeight)
	NSCB_ALIAS(SharedData, uint, PostWaterComposite)
	NSCB_ALIAS(SharedData, uint, ResetHistory)
	NSCB_ALIAS(SharedData, uint, TonemapOwner)
	NSCB_ALIAS(SharedData, float4, AmbientSHR)
	NSCB_ALIAS(SharedData, float4, AmbientSHG)
	NSCB_ALIAS(SharedData, float4, AmbientSHB)
	NSCB_ALIAS(SharedData, float4, HDRData)

	struct GrassLightingSettings
	{
		float Glossiness;
		float SpecularStrength;
		float SubsurfaceScatteringAmount;
		bool OverrideComplexGrassSettings;

		float BasicGrassBrightness;
		float ComplexGrassThreshold;
		// Only read by the GRASS_OPTIMIZATIONS permutation, for grass drawn with an LOD mesh.
		float MidLODBrightness;
		float FarLODBrightness;
		float PBRWrappedLightingAmount;
		float SubsurfaceBalance;
		float CanopyOcclusion;
		float ClumpShading;
	};

	struct CPMSettings
	{
		bool EnableComplexMaterial;
		bool EnableParallax;
		bool EnableTerrainParallax;
		bool EnableHeightBlending;
		bool EnableShadows;
		bool EnableParallaxWarpingFix;
		uint2 pad0;
	};

	struct CubemapCreatorSettings
	{
		uint Enabled;
		float3 pad0;

		float4 CubemapColor;
	};

	struct TerraOccSettings
	{
		bool EnableTerrainShadow;
		float3 Scale;
		float2 ZRange;
		float2 Offset;
		float ZBlur;
		float3 pad0;
	};

	struct LightLimitFixSettings
	{
		uint EnableLightsVisualisation;
		uint LightsVisualisationMode;
		float2 pad0;
		uint4 ClusterSize;
		uint EnableLocalShadows;
		uint LocalShadowSamples;
		float LocalShadowFilterRadius;
		float LocalShadowTexelSize;
	};

	struct WetnessEffectsSettings
	{
		row_major float4x4 OcclusionViewProj;

		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;

		bool EnableWetnessEffects;
		float MaxRainWetness;
		float MaxPuddleWetness;
		float MaxShoreWetness;

		uint ShoreRange;
		float PuddleRadius;
		float PuddleMaxAngle;
		float PuddleMinWetness;

		float MinRainWetness;
		float SkinWetness;
		float WeatherTransitionSpeed;
		bool EnableRaindropFx;

		bool EnableSplashes;
		bool EnableRipples;
		uint EnableVanillaRipples;
		float RaindropFxRange;

		float RaindropGridSizeRcp;
		float RaindropIntervalRcp;
		float RaindropChance;
		float SplashesLifetime;

		float SplashesStrength;
		float SplashesMinRadius;
		float SplashesMaxRadius;
		float RippleStrength;

		float RippleRadius;
		float RippleBreadth;
		float RippleLifetimeRcp;
		float pad0;
	};

	struct SkylightingSettings
	{
		row_major float4x4 OcclusionViewProj;
		float4 OcclusionSHBasis4Pi;  // SH basis of the occlusion direction, times 4 pi (Monte Carlo weight)

		float4 PosOffset;   // xyz: cell origin in camera model space
		uint4 ArrayOrigin;  // xyz: array origin
		int4 ValidMargin;

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint2 pad0;
	};

	struct CloudShadowsSettings
	{
		float Opacity;
		float3 pad0;
	};

	struct LODBlendingSettings
	{
		float LODTerrainBrightness;
		float LODObjectBrightness;
		float LODObjectSnowBrightness;
		bool DisableTerrainVertexColors;
		float LODTerrainGamma;
		float LODObjectGamma;
		float LODObjectSnowGamma;
		float pad0;
	};

	struct HairSpecularSettings
	{
		uint Enabled;
		float HairGlossiness;
		float SpecularMult;
		float DiffuseMult;
		uint EnableTangentShift;
		float PrimaryTangentShift;
		float SecondaryTangentShift;
		float HairSaturation;
		float SpecularIndirectMult;
		float DiffuseIndirectMult;
		float BaseColorMult;
		float Transmission;
		uint EnableSelfShadow;
		float SelfShadowStrength;
		float SelfShadowExponent;
		float SelfShadowScale;
		uint HairMode;  // 0: Kajiya-Kay, 1: Marschner
		uint3 pad;
	};

	/** @brief Terrain Variation feature settings. */
	struct TerrainVariationSettings
	{
		uint enableLODTerrainTilingFix;  ///< 1 = apply variation to LOD terrain.
		uint enableMeshSupport;          ///< 1 = apply variation to landscape-textured meshes.
		uint2 pad;
	};

	struct IBLSettings
	{
		uint EnableIBL;
		uint PreserveFogLuminance;
		uint UseStaticIBL;
		float DALCAmount;
		float EnvIBLScale;
		float SkyIBLScale;
		float EnvIBLSaturation;
		float SkyIBLSaturation;
		float FogAmount;
		uint DALCMode;       // 0: Luminance Ratio, 1: Color Ratio, 2: DALC + Sky, 3: DALC + Sky (Directional)
		uint ENBSkyFalloff;  // Effects 11 owns IBL: darken sky irradiance from below
		float pad0;
	};

	struct ExtendedTranslucencySettings
	{
		uint MaterialModel;  // [0,1,2,3] The MaterialModel
		float Reduction;     // [0, 1.0] The factor to reduce the transparency to matain the average transparency [0,1]
		float Softness;      // [0, 2.0] The soft remap upper limit [0,2]
		float Strength;      // [0, 1.0] The inverse blend weight of the effect
	};

	struct LinearLightingSettings
	{
		uint isMainOrLoadingMenu;
		float diffuseGamma;
		float directionalLightMult;
		float pointLightMult;
		float ambientMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
		float particleEffectMult;
		float lightSpriteEffectMult;
		float fireEffectMult;
		float fireEffectCurve;
		float diffuseCurve;
		float diffuseWhiteReflectance;
		float2 pad;
	};

	struct ENBSettings
	{
		uint Enable;
		float ColorPow;
		float LightSpriteIntensity;
		float FireIntensity;

		float FireCurve;
		float3 pad0;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsEdgeIntensity;
		float CloudsEdgeMoonMultiplier;

		uint EnableProceduralSun;
		float ProceduralSunDiskCos;
		float ProceduralSunDiskRadiance;
		float ProceduralSunEdgeSoftness;

		float ProceduralSunHaloCos;
		float ProceduralSunHaloIntensity;
		uint UseProceduralGradientWeights;
		float ProceduralGradientWeightCurve;

		float LightSpriteCurve;
		float ProceduralSunHaloFalloff;
		float ProceduralSunRadianceLimit;
		float pad1;

		float ParticleIntensity;
		float ParticleLightingInfluence;
		float ParticleAmbientInfluence;
		float ParticlePointLightingInfluence;

		uint EnableVolumetricRays;
		float VolumetricRaysIntensity;
		float VolumetricRaysDensity;
		float VolumetricRaysSkyColorAmount;

		float VolumetricRaysDesaturation;
		float3 VolumetricRaysColorFilter;

		uint EnableCloudsScattering;
		float SkyScatteringIntensity;
		float SkyScatteringShadowAmount;
		float SkyScatteringAmount;

		float3 SkyScatteringColor;
		float SkyScatteringDustDarkening;

		float3 SkyScatteringDustTint;
		float SkyScatteringDustVolume;

		float3 SkyScatteringSunDirection;
		float SkyScatteringSunVisibility;

		float SkyScatteringHorizonRange;
		float SkyScatteringAtmosphereThickness;
		float SkyScatteringAirGlowIntensity;
		float SkyScatteringAirGlowRange;

		float SkyScatteringSunGlowIntensity;
		float SkyScatteringSunGlowRange;
		float SkyScatteringMoonGlowAmount;
		float SkyScatteringMoonGlowRange;

		float SkyScatteringSunIntensity;
		float CloudsLightingSunIntensity;
		float CloudsLightingMoonIntensity;
		uint EnableCloudsLightingFromMoon;

		uint CalculateCloudsEdgeFromScattering;
		float CloudsLightingDesaturation;
		float CloudsLightingForwardScattering;
		float CloudsLightingDensity;

		float3 CloudsColorFilter;
		float CloudsIntensity;

		float CloudsVertexAlphaBoost;
		float CloudsEdgeClamp;
		float CloudsEdgeFadePower;
		float SunBillboardTan;

		float MasserBillboardTan;
		float SecundaBillboardTan;
		float SkyScaleMultiplier;
		float SkyScatteringPad0;

		uint EnableWater;
		float WaterWavesAmplitude;
		float WaterMuddiness;
		float WaterSunLightingMultiplier;

		float WaterSunSpecularMultiplier;
		float WaterFresnelMin;
		float WaterFresnelMax;
		float WaterFresnelMultiplier;

		float WaterReflectionAmount;
		float WaterPad0;
		float WaterPad1;
		float WaterPad2;

		float StarsCurve;
		float StarsIntensity;
		float MoonCurve;
		uint EnableAnimatedStars;

		float StarsAnimationTime;
		float StarsAnimationDensity;
		float StarsAnimationIntensity;
		float AuroraIntensity;

		float AuroraCurve;
		uint FixBlackCrush;
		float2 NightSkyPad0;

		float3 VolumetricFogColorFilter;
		float VolumetricFogIntensity;

		float VolumetricFogCurve;
		float VolumetricFogOpacity;
		float VolumetricFogShadowAmount;
		uint VolumetricFogEnableLighting;

		float3 VolumetricRaysSkyColor;
		uint VolumetricFogEnableShadows;

		uint VolumetricRaysSampleCount;
		uint GameVolumetricRaysDisableDithering;
		float2 GameVolumetricRaysPad0;

		float3 GameVolumetricRaysLightScale;
		float GameVolumetricRaysCustomContribution;

		float3 GameVolumetricRaysCustomColor;
		float GameVolumetricRaysPad1;
	};
	struct TerrainBlendingSettings
	{
		uint Enabled;
		uint3 _padding;
	};

	struct ExponentialHeightFogSettings
	{
		uint enabled;
		uint useDynamicCubemaps;
		float startDistance;
		float fogHeight;
		float fogHeightFalloff;
		float fogDensity;
		float fogHeight2;
		float fogHeightFalloff2;
		float fogDensity2;
		float directionalInscatteringMultiplier;
		float directionalInscatteringAnisotropy;
		uint useSkyIBL;
		float4 inscatteringTint;
		float cubemapMipLevel;
		float sunlightAttenuationAmount;
		uint respectVanillaFogFade;
		uint disableVanillaFog;
		float4 fogInscatteringColor;
		float originalFogColorAmount;
		uint volumetricFogEnabled;
		uint volumetricGridPixelSize;
		uint volumetricGridSizeZ;
		float volumetricFogDistance;
		float volumetricFogStartDistance;
		float volumetricFogNearFadeInDistance;
		float volumetricFogExtinctionScale;
		float4 volumetricFogAlbedo;
		float4 volumetricFogEmissive;
		float volumetricDirectionalScatteringIntensity;
		float volumetricShadowBias;
		float volumetricDepthDistributionScale;
		float volumetricSkyLightingIntensity;
		float volumetricFogScatteringDistribution;
		float volumetricHistoryWeight;
		uint volumetricHistoryMissSampleCount;
		float volumetricSampleJitterMultiplier;
		float volumetricUpsampleJitterMultiplier;
		float volumetricNearGridDistance;
		uint volumetricFarGridPixelSize;
		uint volumetricFarGridSizeZ;
		float volumetricLocalLightScatteringIntensity;
		float volumetricFogNoiseScale;
		float volumetricFogNoiseThreshold;
		float pad3;
		float3 volumetricFogNoiseVelocity;
		float pad0;
		float4 originalFogColorOverride;
	};

	struct TruePBRSettings
	{
		float VertexAOStrength;
		uint EnableMicroShadows;
		float MicroShadowStrength;
		uint pad;
	};

	struct SkinData
	{
		float4 skinParams;
		float4 skinParams2;
		float4 skinDetailParams;
		float4 sssParams;
		float4 fuzzParams;
		float4 physicalParams;
		float4 wetParams;
	};

	struct HorizonFixSettings
	{
		float farWaterDistance;
		float3 pad;
	};

	struct RainRefractionSettings
	{
		uint Enable;
		float MotionStretch;
		float MotionTransparency;
		float pad0;
		float4 TextureBounds;
		float2 TextureRadiusScale;
		float2 pad1;
	};

	cbuffer FeatureData : register(b6)
	{
		GrassLightingSettings NSCB(SharedData, grassLightingSettings);
		CPMSettings NSCB(SharedData, extendedMaterialSettings);
		CubemapCreatorSettings NSCB(SharedData, cubemapCreatorSettings);
		TerraOccSettings NSCB(SharedData, terraOccSettings);
		LightLimitFixSettings NSCB(SharedData, lightLimitFixSettings);
		WetnessEffectsSettings NSCB(SharedData, wetnessEffectsSettings);
		SkylightingSettings NSCB(SharedData, skylightingSettings);
		CloudShadowsSettings NSCB(SharedData, cloudShadowsSettings);
		LODBlendingSettings NSCB(SharedData, lodBlendingSettings);
		HairSpecularSettings NSCB(SharedData, hairSpecularSettings);
		TerrainVariationSettings NSCB(SharedData, terrainVariationSettings);
		IBLSettings NSCB(SharedData, iblSettings);
		ExtendedTranslucencySettings NSCB(SharedData, extendedTranslucencySettings);
		LinearLightingSettings NSCB(SharedData, linearLightingSettings);
		ENBSettings NSCB(SharedData, enbSettings);
		TerrainBlendingSettings NSCB(SharedData, terrainBlendingSettings);
		ExponentialHeightFogSettings NSCB(SharedData, exponentialHeightFogSettings);
		TruePBRSettings NSCB(SharedData, truePBRSettings);
		SkinData NSCB(SharedData, skinData);
		HorizonFixSettings NSCB(SharedData, horizonFixSettings);
		RainRefractionSettings NSCB(SharedData, rainRefractionSettings);
	};
	NSCB_ALIAS(SharedData, GrassLightingSettings, grassLightingSettings)
	NSCB_ALIAS(SharedData, CPMSettings, extendedMaterialSettings)
	NSCB_ALIAS(SharedData, CubemapCreatorSettings, cubemapCreatorSettings)
	NSCB_ALIAS(SharedData, TerraOccSettings, terraOccSettings)
	NSCB_ALIAS(SharedData, LightLimitFixSettings, lightLimitFixSettings)
	NSCB_ALIAS(SharedData, WetnessEffectsSettings, wetnessEffectsSettings)
	NSCB_ALIAS(SharedData, SkylightingSettings, skylightingSettings)
	NSCB_ALIAS(SharedData, CloudShadowsSettings, cloudShadowsSettings)
	NSCB_ALIAS(SharedData, LODBlendingSettings, lodBlendingSettings)
	NSCB_ALIAS(SharedData, HairSpecularSettings, hairSpecularSettings)
	NSCB_ALIAS(SharedData, TerrainVariationSettings, terrainVariationSettings)
	NSCB_ALIAS(SharedData, IBLSettings, iblSettings)
	NSCB_ALIAS(SharedData, ExtendedTranslucencySettings, extendedTranslucencySettings)
	NSCB_ALIAS(SharedData, LinearLightingSettings, linearLightingSettings)
	NSCB_ALIAS(SharedData, ENBSettings, enbSettings)
	NSCB_ALIAS(SharedData, TerrainBlendingSettings, terrainBlendingSettings)
	NSCB_ALIAS(SharedData, ExponentialHeightFogSettings, exponentialHeightFogSettings)
	NSCB_ALIAS(SharedData, TruePBRSettings, truePBRSettings)
	NSCB_ALIAS(SharedData, SkinData, skinData)
	NSCB_ALIAS(SharedData, HorizonFixSettings, horizonFixSettings)
	NSCB_ALIAS(SharedData, RainRefractionSettings, rainRefractionSettings)

	Texture2D<float4> DepthTexture : register(t17);

	// Get a int3 to be used as texture sample coord. [0,1] in uv space
	int3 ConvertUVToSampleCoord(float2 uv)
	{
		uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
		return int3(uv * BufferDim.xy, 0);
	}

	// Get a raw depth from the depth buffer. [0,1] in uv space
	float GetDepth(float2 uv)
	{
		return DepthTexture.Load(ConvertUVToSampleCoord(uv)).x;
	}

	float GetScreenDepth(float depth)
	{
		return (CameraData.w / (-depth * CameraData.z + CameraData.x));
	}

	float4 GetScreenDepths(float4 depths)
	{
		return (CameraData.w / (-depths * CameraData.z + CameraData.x));
	}

	float GetScreenDepth(float2 uv)
	{
		float depth = GetDepth(uv);
		return GetScreenDepth(depth);
	}

	// Returns water data for the tile containing worldPosition (camera-relative XY).
	float4 GetWaterData(float3 worldPosition)
	{
		float2 cellF = (worldPosition.xy + FrameBuffer::CameraPosAdjust.xy) / 4096.0;
		float2 cellFrac = cellF - floor(cellF);  // floor keeps this in [0, 1] for negative coordinates too
		int2 cellInt;

		cellF = worldPosition.xy / float2(4096.0, 4096.0);  // remap to cell scale
		cellF += 2.5;                                       // 5x5 cell grid
		cellF -= cellFrac;                                  // align to cell borders
		cellInt = round(cellF);

		uint waterTile = (uint)clamp(cellInt.x + (cellInt.y * 5), 0, 24);  // remap xy to 0-24

		float4 waterData = float4(1.0, 1.0, 1.0, -2147483648);

		[flatten] if (cellInt.x < 5 && cellInt.x >= 0 && cellInt.y < 5 && cellInt.y >= 0)
			waterData = WaterData[waterTile];

		return waterData;
	}

	float3 GetAmbient(float3 normal)
	{
		return SphericalHarmonics::Unproject(AmbientSHR, AmbientSHG, AmbientSHB, normal);
	}
}
#endif  // __SHARED_DATA_DEPENDENCY_HLSL__
