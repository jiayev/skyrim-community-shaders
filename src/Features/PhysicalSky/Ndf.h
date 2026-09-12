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

	static bool IsTextureNdf(ID3D11ShaderResourceView* srv, uint32_t channels);

private:
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
	float ndfAltitudeOffset = 256.f;
	float ndfAltitudeScale = 1792.f;
	float2 ndfScale = { 16.f, 16.f };
	float coverageBottomPower = 0.39f;
	float coverageHeightRange = 0.30f;
	float bottomDensityPower = 6.f;
	float bottomDensityWidth = 6.8f;
	float topExpansion = 1.f;
	float2 windDirection = { 0.f, 0.f };
	float windSpeed = 0.f;
	float shapeShear = 0.f;
	float densityScale = 0.125f;

	float2 GetNdfAltitudeRangeKm() const;
};

struct CirrusSettings
{
	bool enabled = true;
	float altitude = 2048.f;
	float patternScale = 1.f / 0.0002331f;
	float densityScale = 1.f;
	float lightingScale = 0.5f;
	std::string weatherPath;
	std::string patternsPath;
	std::array<NdfNoiseInput, 2> noise = { { { { .type = 1, .frequency = 4 } }, { { .type = 1, .seed = 7331, .frequency = 8 } } } };
	std::array<NdfNoiseLayer, 2> weather = { { { .noise = 0, .range = { -0.7f, 0.7f, 0.f, 0.6f } }, { .noise = 1 } } };
	uint32_t patternSeed = 1337;
	float patternWarp = 0.15f;
	float patternDetail = 0.35f;

	float GetAltitudeKm() const;
};

struct CloudLightingSettings
{
	float lightingScale = 0.36f;
	float sunExtinction = 1.f;
	float phaseForwardG = 0.2f;
	float phaseBackwardG = 0.9f;
	float phaseForwardWeight = 1.f;
	float phaseBackwardWeight = 0.25f;
	float scatterVolumeStrength = 0.5f;
	float scatterVolumeDepth = 0.04f;
	float scatterVolumeHeight = 0.29f;
	float softScatteringStrength = 1.f;
	float powderStrength = 0.5f;
	float ambientStrength = 3.6f;
	float ambientFloor = 0.23f;
	float ambientDensity = 0.5f;
	float ambientBase = 1.f;
};

struct CloudLayer
{
	LowCloudSettings low;
	CirrusSettings cirrus;
	CloudLightingSettings lighting;
};

struct CirrusTextureSet
{
	ID3D11ShaderResourceView* weather = nullptr;
	ID3D11ShaderResourceView* patterns = nullptr;
	explicit operator bool() const { return weather && patterns; }
};

struct CirrusMapManager
{
	void SetupResources();
	void CompileShaders();
	bool ShadersReady(const CirrusSettings& settings) const;
	static void DrawSettings(CirrusSettings& settings, TextureManager& textures);
	bool Update(const CirrusSettings& settings, TextureManager& textures);
	CirrusTextureSet GetTextures() const;
	Texture2D* GetWeatherTexture() { return texWeather.get(); }
	Texture2D* GetPatternsTexture() { return texPatterns.get(); }

private:
	static constexpr uint32_t kDimension = 512;
	struct GenerationParameters
	{
		std::array<NdfNoiseLayer, 2> weather;
		uint32_t seed;
		float warp;
		float detail;
		float padding = 0.f;
	};
	static_assert(sizeof(GenerationParameters) == 112);
	eastl::unique_ptr<Texture2D> texWeather;
	eastl::unique_ptr<Texture2D> texPatterns;
	std::array<eastl::unique_ptr<Texture2D>, 2> noiseTextures;
	eastl::unique_ptr<ConstantBuffer> generationCb;
	eastl::unique_ptr<ConstantBuffer> noiseCb;
	winrt::com_ptr<ID3D11ComputeShader> weatherProgram;
	winrt::com_ptr<ID3D11ComputeShader> patternsProgram;
	winrt::com_ptr<ID3D11ComputeShader> noiseProgram;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	std::array<NdfNoiseParameters, 2> generatedNoise = {};
	std::array<bool, 2> noiseValid = {};
	std::array<std::string, 4> sourcePaths = {};
	GenerationParameters generatedData = {};
	std::array<ID3D11ShaderResourceView*, 4> generatedSources = {};
	uint64_t generatedRevision = 0;
	bool generatedValid = false;
	CirrusTextureSet outputs;
};
