#pragma once

#include "Buffer.h"

struct TextureManager
{
	std::string name;
	uint64_t revision = 0;
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
	std::string heightPath;
	std::string modelingPath;
};

struct NdfNoiseParameters
{
	uint32_t type = 1;
	uint32_t seed = 1337;
	uint32_t frequency = 16;
	uint32_t octaves = 4;
	float persistence = 0.5f;
	uint32_t lacunarity = 2;
	float contrast = 1.f;
	float bias = 0.f;
	uint32_t repetitions = 1;
	float responseExponent = 1.f;
	float2 padding = {};
};
static_assert(sizeof(NdfNoiseParameters) == 48);

struct NdfNoiseInput
{
	NdfNoiseParameters parameters;
	std::string texturePath;
};

struct NdfNoiseLayer
{
	uint32_t noise = 2;
	float frequency = 1.f;
	float exponent = 1.f;
	float padding0 = 0.f;
	float2 offset = {};
	float2 padding1 = {};
	float4 range = { -1.f, 1.f, 0.f, 1.f };
};
static_assert(sizeof(NdfNoiseLayer) == 48);

struct NdfGenerationParameters
{
	NdfNoiseLayer primary = { .noise = 3, .exponent = 1.4f, .range = { -0.4f, 0.65f, 0.f, 0.65f } };
	NdfNoiseLayer secondary = { .noise = 1, .range = { -0.2f, 0.8f, 0.f, 0.4f } };
	NdfNoiseLayer coverageGain = { .range = { -1.f, 1.f, 1.f, 1.f } };
	NdfNoiseLayer modeling = { .range = { -0.5f, 0.6f, 0.05f, 0.85f } };
	NdfNoiseLayer modelingGain = { .range = { -1.f, 1.f, 1.f, 1.f } };
	NdfNoiseLayer heightVariation = { .range = { 0.f, 1.f, 0.f, 0.02f } };
	float4 bottomTypeRange = { -0.4f, 0.5f, 0.f, 0.25f };
	float2 baseHeight = { 0.f, 0.95f };
	float bottomTypeExponent = 1.f;
	uint32_t heightFromCoverage = 1;
	uint32_t localBlendMode = 0;
	float localModelingWeight = 1.f;
	float localHeightWeight = 1.f;
	float localWindScale = 1.f;
	float2 windOffset = {};
	uint32_t hasLocalMask = 0;
	float padding = 0.f;
};
static_assert(sizeof(NdfGenerationParameters) == 352);

struct ProceduralNdfSettings
{
	NdfGenerationParameters parameters;
	std::array<NdfNoiseInput, 4> noise = { { { { .type = 0, .frequency = 12 } },
		{ { .type = 1, .frequency = 4, .octaves = 6, .persistence = 0.6f, .contrast = 1.6f, .bias = -0.02f, .repetitions = 2, .responseExponent = 1.8f } },
		{ { .type = 1, .frequency = 32 } },
		{ { .type = 2, .frequency = 12, .contrast = 1.1f, .bias = -0.05f, .responseExponent = 2.f } } } };
	TexNdfSettings local;
	std::string localMaskPath;
};

enum class NdfType : uint32_t
{
	Texture = 0,
	Procedural = 1
};

struct NdfSettings
{
	NdfType type = NdfType::Procedural;
	TexNdfSettings texture;
	ProceduralNdfSettings procedural;
};

struct NdfTextureSet
{
	ID3D11ShaderResourceView* height = nullptr;
	ID3D11ShaderResourceView* modeling = nullptr;
	explicit operator bool() const { return height && modeling; }
};

struct NdfManager
{
	constexpr static uint16_t kNdfDim = 512;
	eastl::unique_ptr<Texture2D> texHeight = nullptr;
	eastl::unique_ptr<Texture2D> texModeling = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> generatorProgram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> noiseProgram = nullptr;
	eastl::unique_ptr<Texture2D> texOccupancy = nullptr;
	eastl::unique_ptr<Texture2D> texDistance = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> occupancyProgram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> distanceProgram = nullptr;
	bool accelerationValid = false;

	void SetupResources();
	void CompileShaders();
	static const char* GetSettingsTypeName(const NdfSettings& settings);
	static const char* GetSettingsHint(const NdfSettings& settings);
	static void DrawNdfSettings(NdfSettings& settings, TextureManager& textures);
	bool UpdateNdf(const NdfSettings& settings, TextureManager& textures);
	void UpdateAcceleration(const NdfSettings& settings, TextureManager& textures);
	NdfTextureSet GetNdf(const NdfSettings& settings, TextureManager& textures);

private:
	static bool IsTextureNdf(ID3D11ShaderResourceView* srv, uint32_t channels);
	static NdfTextureSet QueryTextures(const TexNdfSettings& settings, TextureManager& textures);
	eastl::unique_ptr<ConstantBuffer> generatorCb;
	eastl::unique_ptr<ConstantBuffer> noiseCb;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	std::array<eastl::unique_ptr<Texture2D>, 4> noiseTextures;
	std::array<NdfNoiseParameters, 4> generatedNoise = {};
	std::array<bool, 4> noiseValid = {};
	NdfGenerationParameters generatedData = {};
	std::array<ID3D11ShaderResourceView*, 7> generatedSources = {};
	std::array<std::string, 9> sourcePaths = {};
	uint64_t generatedRevision = 0;
	uint64_t importedRevision = 0;
	bool generatedValid = false;
	ID3D11ShaderResourceView* acceleratedNdf = nullptr;
};

struct LowCloudSettings
{
	// Absolute altitude of the shared low-cloud condensation base, in kilometres.
	float baseAltitude = 1.0f;
	float thickness = 0.3f;
	float2 ndfScale = { 16.f, 16.f };
	// Physical repeat length of the authored 128^3 RGBA Nubis noise composite.
	float noiseCompositeScale = 0.2f;
	float noiseRoundness = 0.5f;
	float3 noiseOffset = { 0.f, 0.f, 0.f };
	float2 windDirection = { 1.f, 0.2f };
	float windSpeed = 12.f;
	float shapeShear = 0.f;
	// Optical scale applied to normalized reconstructed density after NDF shaping.
	float extinctionCoefficient = 0.09f;
};

struct HighCloudSettings
{
	bool enabled = true;
	bool thinLayer = false;
	float thinLayerStart = 15.f;
	float thinLayerEnd = 25.f;
	uint32_t viewSteps = 194;
	uint32_t lightSteps = 6;
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

struct CloudLightingSettings
{
	bool useLightCache = true;
	bool crossLayerShadows = true;
	uint32_t cacheSteps = 16;
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
	// Legacy integration only: artistic edge shaping tied to the march step OD.
	float scatterSourceODScale = 0.08f;
	float scatterSourceCurvePow = 1.f;
	float powderIntensity = 0.35f;
	uint32_t lightSteps = 6;
	// 0 = normalized equal-weight dual-lobe HG (uses the eccentricity sliders)
	// 1 = approximate Mie (HG + Draine fit, 10 um diameter water droplets); the
	//     eccentricity sliders have no effect on it.
	uint32_t phaseModel = 0;
	// 0 = legacy scalar integral with step-dependent edge shaping
	// 1 = analytical per-channel albedo * (1 - transmittance), without edge gating
	uint32_t scatterIntegration = 1;
	// Fades the secondary (light) march step budget down to one step with view
	// distance. 0 disables the LOD, 1 applies it fully.
	float lightStepDistanceLod = 1.f;
};

struct CloudForwardScatteringSettings
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
	LowCloudSettings low;
	HighCloudSettings high;
	CloudLightingSettings lighting;
	CloudForwardScatteringSettings phiFwd;
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
	HighCloudTextureSet GetTextures(const HighCloudSettings& settings);

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

	bool EnsureResources(const HighCloudSettings& settings);
	void GenerateTextures(const HighCloudSettings& settings);
};
