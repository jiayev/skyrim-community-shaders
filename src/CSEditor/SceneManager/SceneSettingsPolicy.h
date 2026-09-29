#pragma once

#include <string_view>
#include <vector>

namespace SceneSettingsPolicy
{
	/// Catalog address prefix: feature short name, then setting path segments, then the key.
	/// A bare feature name covers everything the feature exposes.
	using SettingPolicyPath = std::vector<std::string_view>;

	/// Settings that must never be scene-overridden, matched by catalog address prefix.
	inline const std::vector<SettingPolicyPath> kSettingBlacklist = {
		{ "ExponentialHeightFog", "volumetricGridPixelSize" },
		{ "ExponentialHeightFog", "volumetricGridSizeZ" },
		{ "ExponentialHeightFog", "volumetricFarGridPixelSize" },
		{ "ExponentialHeightFog", "volumetricFarGridSizeZ" },
		{ "ExponentialHeightFog", "volumetricShadowBias" },
		{ "ExponentialHeightFog", "volumetricDepthDistributionScale" },
		{ "ExponentialHeightFog", "volumetricHistoryWeight" },
		{ "ExponentialHeightFog", "volumetricHistoryMissSampleCount" },
		{ "ExponentialHeightFog", "volumetricSampleJitterMultiplier" },
		{ "ExponentialHeightFog", "volumetricUpsampleJitterMultiplier" },
		{ "GrassOptimizations" },
		{ "TerrainVariation" },
		{ "ImageBasedLighting", "DisableInWorldMap" },
		{ "ImageBasedLighting", "DisableInLoadingScreen" },
		{ "PhysicalSky", "enableAllExteriorCells" },
		{ "PhysicalSky", "forceEnableAllInteriorCells" },
		{ "PhysicalSky", "fallbackZBottom" },
		{ "PhysicalSky", "planetRadius" },
		{ "PhysicalSky", "atmosphereRadius" },
		{ "PhysicalSky", "halfResApShadow" },
		{ "PhysicalSky", "rayMarchRange" },
		{ "PhysicalSky", "shadowVolumeRange" },
		{ "PhysicalSky", "marchStepScale" },
		{ "PhysicalSky", "cloudNoise" },
		{ "PhysicalSky", "cloudRelightMix" },
		{ "PhysicalSky", "cloudOriginalMix" },
		{ "PhysicalSky", "silverLiningMix" },
		{ "PhysicalSky", "silverLiningSpread" },
		{ "PhysicalSky", "cloudShadowRemapRange" },
		{ "PhysicalSky", "cloudLayer", "lighting" },
		{ "PhysicalSky", "cloudMap", "type" },
		{ "PhysicalSky", "cloudMap", "texture" },
		{ "PhysicalSky", "cloudMap", "procedural", "noise" },
		{ "PhysicalSky", "cloudMap", "procedural", "local" },
		{ "PhysicalSky", "cloudMap", "procedural", "localMaskPath" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "heightFromCoverage" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "localBlendMode" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "primary", "noise" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "secondary", "noise" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "coverageGain", "noise" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "coverageGain", "exponent" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "modeling", "noise" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "modelingGain", "noise" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "modelingGain", "exponent" },
		{ "PhysicalSky", "cloudMap", "procedural", "parameters", "heightVariation", "noise" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "lightingScale" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "weatherPath" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "patternsPath" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "weather", "0", "noise" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "weather", "1", "noise" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "weather", "0", "exponent" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "weather", "1", "exponent" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "patternSeed" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "patternWarp" },
		{ "PhysicalSky", "cloudLayer", "cirrus", "patternDetail" },
		{ "PostProcessing", "Border" },
		{ "PostProcessing", "Depth of Field", "HighlightShape" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "enableTonemap" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "useOpenDrt" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "currentTonemapper" },
		{ "PostProcessing", "Color Grading and Tone Mapping", "tonemapParams" },
		{ "PostProcessing", "Motion Blur", "VelocityScale" },
		{ "VolumetricLighting" },
	};

	inline const std::vector<SettingPolicyPath> kLocationFeatureWhitelist = {
		{ "ExponentialHeightFog" },
		{ "ImageBasedLighting" },
		{ "PhysicalSky" },
		{ "PostProcessing" },
		{ "ScreenSpaceGI" },
		{ "ScreenSpaceShadows" },
		{ "SubsurfaceScattering" },
	};

	inline const std::vector<SettingPolicyPath> kTimeOfDayFeatureWhitelist = {
		{ "CloudShadows" },
		{ "ExponentialHeightFog" },
		{ "GrassLighting" },
		{ "ImageBasedLighting" },
		{ "PhysicalSky" },
		{ "PostProcessing" },
		{ "ScreenSpaceGI" },
		{ "ScreenSpaceShadows" },
		{ "Skylighting" },
		{ "SubsurfaceScattering" },
		{ "WetnessEffects" },
	};
}
