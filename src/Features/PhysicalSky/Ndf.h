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
	uint32_t seed = 1337;

	// --- Primary controls -------------------------------------------------
	// Each of these maps onto one solved quantity in the generator rather than
	// onto an opaque noise threshold.

	// Fraction of the sky carrying low cloud. The generator solves the field
	// threshold by histogram quantile, so this is the actual covered area.
	float skyCoverage = 0.38f;
	// Horizontal diameter of a single convective cloud body, in kilometres.
	float cloudSize = 2.4f;
	// Atmospheric instability: drives the Cu -> TCu -> Cb species mix and the
	// vertical development of each species.
	float instability = 0.45f;
	// 0 = discrete convective cells, 1 = continuous stratiform sheets.
	float character = 0.35f;
	// Separation between convective cloud bodies. Independent of coverage: the
	// same cloud area is redistributed into fewer, denser bodies.
	float breakup = 0.45f;
	// Fraction of the sky carrying high cloud, solved the same way.
	float highCoverage = 0.28f;

	// --- Advanced ---------------------------------------------------------
	// Edge softness of the coverage ramp. Unlike the former contrast control,
	// this cannot change how much sky is covered.
	float coverageEdgeWidth = 0.45f;
	float highCoverageEdgeWidth = 0.5f;
	// Frontal band contribution to coverage, and the band bearing in degrees.
	float frontStrength = 0.35f;
	float frontBearing = 45.f;
	// Radial dome falloff applied to the profile LUT. 0 = flat slabs.
	float domeStrength = 0.85f;
	// Shares are measured over generated low-cloud coverage. Cu/Tcu/Cb weights
	// are normalized after the independent stratocumulus share is assigned.
	float stratocumulus = 0.20f;
	float cumulusWeight = 0.62f;
	float toweringCumulusWeight = 0.28f;
	float cumulonimbusWeight = 0.10f;
	// Physical vertical development for each profile family in kilometres. The
	// shared cloud shell remains only a safety bound, so raising its ceiling does
	// not stretch shallow clouds into full-height columns.
	float cumulusDepth = 1.2f;
	float toweringCumulusDepth = 3.0f;
	float cumulonimbusDepth = 10.0f;
	float altostratusWeight = 0.68f;
	float altocumulusWeight = 0.32f;
	HpTextureOverrideSettings overrides;
};

using NdfSettings = HpGeneratedCloudMapSettings;

struct HpLowCloudSettings
{
	float3 noiseScale = { 0.0001f, 0.000145f, 0.0001f };
	float3 noiseOffset = { 0.f, 0.f, 0.f };
	float detailNoiseScale = 0.00042f;
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
	// Physical vertical development in kilometres; converted to a fraction of
	// the shared shell when populating the shader buffer.
	float verticalDepth = 0.8f;
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
	eastl::unique_ptr<Texture2D> texLowWeather = nullptr;
	eastl::unique_ptr<Texture2D> texHighWeather = nullptr;
	eastl::unique_ptr<Texture2D> texProfile = nullptr;
	eastl::unique_ptr<Texture2D> texScCell = nullptr;
	eastl::unique_ptr<Texture2D> texHighCell = nullptr;
	eastl::unique_ptr<Texture2D> texHighWarp = nullptr;
	eastl::unique_ptr<Texture2D> texHighWisp = nullptr;

	void SetupResources();
	void CompileShaders();
	bool ShadersReady() const;

	static const char* GetSettingsTypeName(const NdfSettings& ndfSettings);
	static const char* GetSettingsHint(const NdfSettings& ndfSettings);
	void DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager);
	/** @brief Regenerates the cloud maps if any generation input changed. */
	void UpdateNdf(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer);
	ID3D11ShaderResourceView* GetNdf(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer, TextureManager& texManager);
	HpCloudTextureSet GetHpTextures(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer, TextureManager& texManager);

private:
	// Generation is GPU-side, so it is cheap enough to re-run whenever an input
	// changes without deferring or throttling. The hash exists only to skip
	// redundant dispatches on unchanged frames.
	size_t generatedHash = 0;
	uint32_t generatedWeatherDim = 0;
	uint32_t generatedProfileWidth = 0;
	uint32_t generatedProfileHeight = 0;

	// Intermediate morphology fields consumed by the histogram and compose passes.
	eastl::unique_ptr<Texture2D> texFieldLow = nullptr;
	eastl::unique_ptr<Texture2D> texFieldHigh = nullptr;
	eastl::unique_ptr<Buffer> bufHistogram = nullptr;
	eastl::unique_ptr<Buffer> bufThresholds = nullptr;
	eastl::unique_ptr<ConstantBuffer> cbGen = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> csFields = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csHistogram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csSolve = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csCompose = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csProfile = nullptr;

	struct GenCB
	{
		uint32_t weatherDim[2];
		uint32_t profileDim[2];

		uint32_t cellPeriod;
		uint32_t seed;
		uint32_t solveRound;
		float skyCoverage;

		float highCoverage;
		float instability;
		float character;
		float breakup;

		float coverageEdgeWidth;
		float highCoverageEdgeWidth;
		float frontStrength;
		float domeStrength;

		float frontNormal[2];
		float frontTangent[2];

		float scShare;
		float cuShare;
		float tcuShare;
		float asShare;

		float cumulusDepth;
		float toweringCumulusDepth;
		float cumulonimbusDepth;
		float layerDepth;
	};
	STATIC_ASSERT_ALIGNAS_16(GenCB);

	bool EnsureResources(const NdfSettings& ndfSettings);
	void GenerateTextures(const NdfSettings& ndfSettings, const CloudLayer& cloudLayer);
};
