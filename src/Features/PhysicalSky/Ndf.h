#pragma once

#include "Buffer.h"

struct TextureManager
{
	std::string name;
	ankerl::unordered_dense::map<std::string, winrt::com_ptr<ID3D11ShaderResourceView>> texList;

	bool LoadTexture(std::filesystem::path path);

	inline ID3D11ShaderResourceView* Query(const std::string& path) const
	{
		if (texList.contains(path))
			return texList.at(path).get();
		return nullptr;
	}

	inline std::vector<std::string> ListPaths()
	{
		std::vector<std::string> retval;
		std::ranges::transform(texList, std::back_inserter(retval), [](auto& pair) { return pair.first; });
		return retval;
	}

	std::string uiPath;
	void DrawUI();
};

namespace nlohmann
{
	void to_json(json&, const TextureManager&);
	void from_json(const json&, TextureManager&);
}

struct HpTextureOverrideSettings
{
	std::string lowWeatherPath;
	std::string highWeatherPath;
	std::string profilePath;
	std::string scCellPath;
	std::string highCellPath;
	std::string highWarpPath;
	std::string highWispPath;
};

struct HpGeneratedCloudMapSettings
{
	uint32_t generationVersion = 0;
	uint32_t weatherDim = 512;
	uint32_t profileWidth = 256;
	uint32_t profileHeight = 64;
	// Horizontal weather-map extent in kilometres. It is independent of the
	// vertical shared cloud layer, but their default feature scales are paired.
	float worldSize = 64.f;
	float2 center = { 0.f, 0.f };
	float lowCoverage = 0.62f;
	float lowContrast = 1.25f;
	// Fraction of the weather field occupied by full-strength stratocumulus mask
	// regions. Runtime stratocumulus intensity is controlled separately.
	float stratocumulus = 0.35f;
	float highCoverage = 0.28f;
	float highContrast = 1.1f;
	HpTextureOverrideSettings overrides;
};

using NdfSettings = HpGeneratedCloudMapSettings;

struct HpLowCloudSettings
{
	// Inverse metres. The base field is intentionally kilometre-scale; the profile
	// texture, rather than high-frequency 3D noise, defines the vertical silhouette.
	float3 noiseScale = { 0.000045f, 0.00007f, 0.000045f };
	float3 noiseOffset = { 0.f, 0.f, 0.f };
	float detailNoiseScale = 0.00019f;
	float2 windDirection = { 1.f, 0.2f };
	float windSpeed = 12.f;
	float baseNoiseWindSpeed = 1.f;
	float detailNoiseWindSpeed = 1.5f;
	float detailNoiseVerticalWindSpeed = 0.08f;
	float billowyLow = 0.55f;
	float billowyHigh = 0.45f;
	float wispyLow = 0.45f;
	float wispyHigh = 0.55f;
	float detailStrengthCu = 0.35f;
	float detailStrengthTcu = 0.55f;
	float detailStrengthCb = 0.75f;
	float densityThreshold = 0.05f;
	float densityMultiplier = 0.09f;
	float densityMultiplierCu = 0.85f;
	float densityMultiplierTcu = 1.1f;
	float densityMultiplierCb = 1.35f;
	float bottomSmoothHeight = 0.08f;
	float bottomSmoothPow = 2.f;
	float wispyEdgeWidth = 0.25f;
	float wispyReach = 0.18f;
	float wispyTopHeight = 0.72f;
	float wispyTopHardness = 0.25f;
	float coverageCoverIntensity = 1.f;
	float coverageCoverContrast = 1.f;
	float coverageHeightIntensity = 1.f;
	float coverageHeightContrast = 1.f;
	float coverTopStrength = 0.65f;
	float coverTopMax = 2.f;
	float coverTopCurvePow = 1.f;
};

struct HpStratocumulusSettings
{
	float2 cellScale = { 6.f, 6.f };
	float worleyStrength = 0.65f;
	// Fraction of the shared physical shell occupied by stratocumulus profiles.
	float heightScale = 0.28f;
	float detailStrength = 0.32f;
	float cellThickPow = 1.7f;
	float cellThickStrength = 0.85f;
	float cellNoiseStrength = 1.25f;
	float coverageIntensity = 1.15f;
	float coverageContrast = 1.25f;
};

struct HpHighCloudSettings
{
	bool enabled = true;
	float2 cellScale = { 4.f, 4.f };
	float cellWindSpeed = 1.35f;
	float2 cellWarpScale = { 1.5f, 1.5f };
	float cellWarpStrength = 0.12f;
	float cellThickStrength = 0.75f;
	float asCellThickStrength = 0.25f;
	float cellThickPow = 1.6f;
	// Normalized positions inside the shared physical shell, not kilometres.
	float bottom = 0.58f;
	float top = 0.92f;
	float bottomCoverageScale = 0.35f;
	float heightCurvePow = 0.85f;
	float densityThreshold = 0.08f;
	float densitySoftness = 0.22f;
	float softness = 0.04f;
	float2 wispScale = { 7.f, 7.f };
	float wispStrength = 0.18f;
	float horizonDistanceStart = 18000.f;
	float horizonDistanceEnd = 65000.f;
	float densityMultiplier = 0.35f;
	float densitySoftAIntensity = 0.3f;
	float densitySoftAContrast = 1.5f;
	float densityModAIntensity = 0.25f;
	float densityModAContrast = 1.5f;
	float forwardEccentricity = 0.78f;
	float backwardEccentricity = 0.22f;
	float ambientTopMultiplier = 1.8f;
	float ambientBottomMultiplier = 0.65f;
	float skyBlendStrength = 0.35f;
	float msAttenuation = 0.55f;
	float msContribution = 0.5f;
	float msEccentricity = 0.55f;
	float lightAbsorption = 0.65f;
	float viewAbsorption = 0.5f;
	float coverAbsorptionStrength = 0.6f;
};

struct HpLightingSettings
{
	float3 scatterTint = { 1.f, 1.f, 1.f };
	float forwardEccentricity = 0.85f;
	float backwardEccentricity = 0.3f;
	float ambientTopMultiplier = 1.6f;
	float ambientBottomMultiplier = 0.75f;
	float aoUpwardScale = 1.f;
	float msAttenuation = 0.5f;
	float msContribution = 0.5f;
	float msEccentricity = 0.5f;
	float scatterSourceODScale = 0.08f;
	float scatterSourceCurvePow = 1.f;
	float powderIntensity = 0.35f;
	uint32_t lightSteps = 6;
};

struct HpPhiFwdSettings
{
	float intensity = 0.65f;
	float depthPow = 1.f;
	float depthBias = 0.05f;
	float boundaryConfidence = 0.55f;
	float msBuildScale = 1.4f;
	float compress = 0.35f;
};

struct CloudLayer
{
	// Kilometres above the local ground reference. These are the shared physical
	// shell boundaries; individual cloud types distribute themselves inside it.
	float lowestAltitude = 0.5f;
	float highestAltitude = 3.0f;
	HpLowCloudSettings low;
	HpStratocumulusSettings stratocumulus;
	HpHighCloudSettings high;
	HpLightingSettings lighting;
	HpPhiFwdSettings phiFwd;
};

struct HpCloudTextureSet
{
	ID3D11ShaderResourceView* lowWeather = nullptr;
	ID3D11ShaderResourceView* highWeather = nullptr;
	ID3D11ShaderResourceView* profile = nullptr;
	ID3D11ShaderResourceView* scCell = nullptr;
	ID3D11ShaderResourceView* highCell = nullptr;
	ID3D11ShaderResourceView* highWarp = nullptr;
	ID3D11ShaderResourceView* highWisp = nullptr;
};

struct NdfManager
{
	constexpr static uint16_t kNdfDim = 256;

	eastl::unique_ptr<Texture2D> texLowWeather = nullptr;
	eastl::unique_ptr<Texture2D> texHighWeather = nullptr;
	eastl::unique_ptr<Texture2D> texProfile = nullptr;
	eastl::unique_ptr<Texture2D> texScCell = nullptr;
	eastl::unique_ptr<Texture2D> texHighCell = nullptr;
	eastl::unique_ptr<Texture2D> texHighWarp = nullptr;
	eastl::unique_ptr<Texture2D> texHighWisp = nullptr;

	void SetupResources();
	void CompileShaders();

	static const char* GetSettingsTypeName(const NdfSettings& ndfSettings);
	static const char* GetSettingsHint(const NdfSettings& ndfSettings);
	void DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager);
	void UpdateNdf(const NdfSettings& ndfSettings);
	ID3D11ShaderResourceView* GetNdf(const NdfSettings& ndfSettings, TextureManager& texManager);
	HpCloudTextureSet GetHpTextures(const NdfSettings& ndfSettings, TextureManager& texManager);

private:
	uint32_t generatedWeatherDim = 0;
	uint32_t generatedProfileWidth = 0;
	uint32_t generatedProfileHeight = 0;
	float generatedLowCoverage = -1.0f;
	float generatedLowContrast = -1.0f;
	float generatedStratocumulus = -1.0f;
	float generatedHighCoverage = -1.0f;
	float generatedHighContrast = -1.0f;
	int deferredGenerationFrame = -1;
	std::vector<float> cachedCoverageField;
	std::vector<float> cachedCloudType;
	std::vector<float> cachedScRegion;
	std::vector<float> cachedHighField;
	std::vector<float> cachedHighType;
	std::vector<float> cachedHighScatterField;
	void GenerateDefaultTextures(const NdfSettings& ndfSettings);
};
