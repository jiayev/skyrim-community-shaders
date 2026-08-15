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

struct TexNdfSettings
{
	std::string texPath;
};

struct CumuliformNdfSettings
{
	DirectX::XMUINT2 scale0 = { 10, 10 };
	float2 offset0 = { 3.f, 3.f };
	DirectX::XMUINT2 scale1 = { 20, 20 };
	float2 offset1 = { 6.f, 6.f };
	DirectX::XMUINT2 scale2 = { 40, 40 };
	float2 offset2 = { 24.f, 24.f };
	float2 clipRange = { 0.4f, 1.f };
	float power = 0.7f;
	float wispiness = 0.1f;
	float rot0 = 1.f;
	float rot1 = 2.f;
	float rot2 = 3.f;
	float _pad = 0.f;
};

using NdfSettings = std::variant<TexNdfSettings, CumuliformNdfSettings>;

struct NdfManager
{
	constexpr static uint16_t kNdfDim = 256;

	eastl::unique_ptr<Texture2D> texNdfOutput = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> cumuliformProgram = nullptr;
	eastl::unique_ptr<ConstantBuffer> cumuliformCb = {};

	void SetupResources();
	void CompileShaders();

	static const char* GetSettingsTypeName(const NdfSettings& ndfSettings);
	static const char* GetSettingsHint(const NdfSettings& ndfSettings);
	static void DrawNdfSettings(NdfSettings& ndfSettings, TextureManager& texManager);
	void UpdateNdf(const NdfSettings& ndfSettings);
	ID3D11ShaderResourceView* GetNdf(const NdfSettings& ndfSettings, TextureManager& texManager);
};

struct HpLowCloudSettings
{
	// Absolute altitude of the shared low-cloud condensation base, in kilometres.
	float baseAltitude = 1.0f;
	float thickness = 0.3f;
	float2 ndfScale = { 16.f, 16.f };
	// Physical repeat length of the authored 128^3 RGBA Nubis noise composite.
	float noiseCompositeScale = 0.2f;
	float3 noiseOffset = { 0.f, 0.f, 0.f };
	float2 windDirection = { 1.f, 0.2f };
	float windSpeed = 12.f;
	// Optical scale applied to normalized reconstructed density after NDF shaping.
	float extinctionCoefficient = 0.09f;
};

struct HpHighCloudSettings
{
	bool enabled = true;
	uint32_t weatherDim = 512;
	float weatherWorldSize = 64.f;
	float2 weatherCenter = { 0.f, 0.f };
	uint32_t weatherSeed = 1337;
	float coverage = 0.28f;
	float coverageEdgeWidth = 0.5f;
	float frontStrength = 0.35f;
	float frontBearing = 45.f;
	float altostratusWeight = 0.68f;
	float altocumulusWeight = 0.32f;
	float2 cellScale = { 4.f, 4.f };
	float cellWindSpeed = 1.35f;
	float2 cellWarpScale = { 1.5f, 1.5f };
	float cellWarpStrength = 0.12f;
	float cellThickStrength = 0.75f;
	float asCellThickStrength = 0.25f;
	float cellThickPow = 1.6f;
	// Absolute altitude band in kilometres. High clouds do not share the low-cloud
	// NDF coordinate frame.
	float bottomAltitude = 6.0f;
	float topAltitude = 12.0f;
	float bottomCoverageScale = 0.35f;
	float heightCurvePow = 0.85f;
	float densityThreshold = 0.08f;
	float densitySoftness = 0.22f;
	float softness = 0.04f;
	float2 wispScale = { 7.f, 7.f };
	float wispStrength = 0.18f;
	float densityMultiplier = 0.35f;
	float densitySoftAIntensity = 0.3f;
	float densitySoftAContrast = 1.5f;
	float densityModAIntensity = 0.25f;
	float densityModAContrast = 1.5f;
	float forwardEccentricity = 0.78f;
	float backwardEccentricity = 0.22f;
	// Unit multipliers preserve the radiance reconstructed from the sky probe.
	float ambientTopMultiplier = 1.f;
	float ambientBottomMultiplier = 1.f;
	// 1 preserves integrated cloud radiance; lower values artistically blend the
	// high-cloud top toward the view-direction environment probe.
	float skyBlendStrength = 1.f;
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
	// Unit multipliers preserve the radiance reconstructed from the sky probe.
	float ambientTopMultiplier = 1.f;
	float ambientBottomMultiplier = 1.f;
	float aoUpwardScale = 1.f;
	float msAttenuation = 0.5f;
	float msContribution = 0.5f;
	float msEccentricity = 0.5f;
	float scatterSourceODScale = 0.08f;
	float scatterSourceCurvePow = 1.f;
	float powderIntensity = 0.35f;
	uint32_t lightSteps = 6;
	// 0 = dual-lobe Henyey-Greenstein (default, uses the eccentricity sliders)
	// 1 = approximate Mie (HG + Draine fit); physically parameterised, so the
	//     eccentricity sliders have no effect on it.
	uint32_t phaseModel = 0;
	// 0 = legacy scalar in-scattering integral
	// 1 = Frostbite energy-conserving per-channel albedo * (1 - transmittance)
	uint32_t scatterIntegration = 0;
	// Fades the secondary (light) march step budget down to one step with view
	// distance. 0 disables the LOD, 1 applies it fully.
	float lightStepDistanceLod = 1.f;
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
	HpLowCloudSettings low;
	HpHighCloudSettings high;
	HpLightingSettings lighting;
	HpPhiFwdSettings phiFwd;
};

struct HighCloudTextureSet
{
	ID3D11ShaderResourceView* highWeather = nullptr;
	ID3D11ShaderResourceView* highCell = nullptr;
	ID3D11ShaderResourceView* highWarp = nullptr;
	ID3D11ShaderResourceView* highWisp = nullptr;
};

struct HighCloudMapManager
{
	eastl::unique_ptr<Texture2D> texHighWeather = nullptr;
	eastl::unique_ptr<Texture2D> texHighCell = nullptr;
	eastl::unique_ptr<Texture2D> texHighWarp = nullptr;
	eastl::unique_ptr<Texture2D> texHighWisp = nullptr;

	void SetupResources();
	void CompileShaders();
	bool ShadersReady() const;
	HighCloudTextureSet GetTextures(const HpHighCloudSettings& settings);

private:
	// Generation is GPU-side, so it is cheap enough to re-run whenever an input
	// changes without deferring or throttling. The hash exists only to skip
	// redundant dispatches on unchanged frames.
	size_t generatedHash = 0;
	uint32_t generatedWeatherDim = 0;

	// Intermediate morphology fields consumed by the histogram and compose passes.
	eastl::unique_ptr<Texture2D> texFieldHigh = nullptr;
	eastl::unique_ptr<Buffer> bufHistogram = nullptr;
	eastl::unique_ptr<Buffer> bufThresholds = nullptr;
	eastl::unique_ptr<ConstantBuffer> cbGen = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> csFields = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csHistogram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csSolve = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csCompose = nullptr;

	struct GenCB
	{
		uint32_t weatherDim[2];
		uint32_t seed;
		uint32_t solveRound;

		float coverage;
		float highCoverageEdgeWidth;
		float frontStrength;
		float asShare;

		float frontNormal[2];
		float frontTangent[2];
		float padding[4];
	};
	STATIC_ASSERT_ALIGNAS_16(GenCB);

	bool EnsureResources(const HpHighCloudSettings& settings);
	void GenerateTextures(const HpHighCloudSettings& settings);
};
