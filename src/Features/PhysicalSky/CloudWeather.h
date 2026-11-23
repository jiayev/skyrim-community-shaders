#pragma once

#include "Buffer.h"
#include "Utils/SceneBlend.h"
#include "Utils/Serialize.h"

namespace Util
{
	class AsyncShaderSet;
}

struct TextureManager
{
	std::string name;
	uint64_t revision = 0;
	ankerl::unordered_dense::map<std::string, winrt::com_ptr<ID3D11ShaderResourceView>> texList;
	ankerl::unordered_dense::map<std::string, HRESULT> loadResults;

	bool LoadTexture(std::filesystem::path path);
	void EnsureLoaded(const std::string& path);
	void Reload();
	static std::string Key(const std::filesystem::path& path);

	inline ID3D11ShaderResourceView* Query(const std::string& path) const
	{
		const auto key = Key(std::filesystem::u8path(path));
		if (texList.contains(key))
			return texList.at(key).get();
		return nullptr;
	}

	inline std::vector<std::string> ListPaths()
	{
		std::vector<std::string> retval;
		std::ranges::transform(texList, std::back_inserter(retval), [](auto& pair) { return pair.first; });
		std::ranges::sort(retval);
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

struct WeatherNoiseParameters
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
	uint32_t frequencyY = 0;
	float padding = 0.f;
};
static_assert(sizeof(WeatherNoiseParameters) == 48);

struct WeatherNoiseInput
{
	WeatherNoiseParameters parameters;
	std::string texturePath;
};

struct WeatherNoiseLayer
{
	uint32_t noise = 2;
	float frequency = 1.f;
	float exponent = 1.f;
	float padding0 = 0.f;
	float2 offset = {};
	float2 padding1 = {};
	float4 range = { -1.f, 1.f, 0.f, 1.f };
};
static_assert(sizeof(WeatherNoiseLayer) == 48);

struct CloudWeatherLayout
{
	uint32_t baseSeed = 1763;
	uint32_t stormSeed = 15373;
	std::array<int, 2> baseCells = { 6, 6 };
	std::array<int, 2> stormCells = { 4, 4 };
	float baseEdge = .18f;
	float stormEdge = .24f;
	float coreFraction = .35f;
	float cellular = 0.f;
	float updraftOrganization = .65f;
	float stormOutflow = .65f;
	std::array<float, 3> coverageWeights = { .45f, .35f, .20f };
	float secondaryCoverage = 0.f;
	float regionalCoverageStrength = 0.f;
	int regionalCells = 3;
	std::string paintedMap;
	float paintedWeight = 1.f;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudWeatherLayout,
	baseSeed, stormSeed, baseCells, stormCells, baseEdge, stormEdge, coreFraction, cellular, stormOutflow,
	coverageWeights, secondaryCoverage, regionalCoverageStrength, regionalCells, updraftOrganization, paintedMap, paintedWeight)

struct CloudWeatherSettings
{
	CloudWeatherSettings();
	float coverage = 0.58f;
	float cloudType = 0.24f;
	float typeVariation = 0.30f;
	float convection = 0.f;
	SceneBlend layout;
	std::array<WeatherNoiseInput, 4> noise = { { { { .type = 1, .seed = 1763, .frequency = 3, .octaves = 4 } },
		{ { .type = 1, .seed = 2137, .frequency = 7, .octaves = 4, .persistence = 0.6f } },
		{ { .type = 1, .frequency = 32 } },
		{ { .type = 2, .seed = 15373, .frequency = 10, .octaves = 4 } } } };
};

struct NdfTextureSet
{
	ID3D11ShaderResourceView* height = nullptr;
	ID3D11ShaderResourceView* modeling = nullptr;
	explicit operator bool() const { return height && modeling; }
};

struct CloudWeatherManager
{
	static constexpr uint32_t kMapSize = 512;
	winrt::com_ptr<ID3D11ComputeShader> generatorProgram, noiseProgram;

	void SetupResources();
	void CompileShaders(Util::AsyncShaderSet& shaders);
	bool Update(const CloudWeatherSettings& settings, TextureManager& textures, double phase, float2 flow);
	static void DrawNoise(CloudWeatherSettings& settings, TextureManager& textures);
	static void DrawLayout(CloudWeatherSettings& settings, TextureManager& textures);
	bool Ready() const { return generatorProgram && noiseProgram && maps[0] && maps[1]; }
	ID3D11ShaderResourceView* Weather() const { return generatedValid ? maps[outputIndex]->srv.get() : nullptr; }
	ID3D11ShaderResourceView* FiniteWeather() const { return generatedValid && hasFiniteWeather ? finiteMaps[outputIndex]->srv.get() : nullptr; }
	bool HasConvection() const { return generatedValid && hasConvection; }
	bool HasProceduralConvection() const { return generatedValid && hasProceduralLayout && generatedWeather.w > 0.f; }
	float GetProceduralWeight() const { return generatedValid ? proceduralWeight : 0.f; }

	const std::array<ID3D11ShaderResourceView*, 4>& GetNoiseInputs() const { return noiseInputs; }
	uint64_t GetNoiseRevision() const { return noiseRevision; }
	uint64_t GetWeatherNoiseRevision() const { return weatherNoiseRevision; }

	static bool IsLinearTexture(ID3D11ShaderResourceView* srv, uint32_t channels);
	static bool IsLinearFormat(DXGI_FORMAT format, uint32_t channels);
	static bool IsTexturePair(NdfTextureSet maps);

private:
	struct GenerationParameters
	{
		float4 weather;
		float4 shape;  // base edge, storm edge, core fraction, painted map weight
		std::array<uint32_t, 4> base;
		std::array<uint32_t, 4> storm;
		float phase;
		float weight;
		uint32_t hasPrevious;
		uint32_t resolve;
		float4 organization;  // upper flow in map space, cellular organization, outflow reach
		float4 coverageMix;
		float updraftOrganization;
		float regionalCoverageStrength;
		uint32_t regionalCells;
		uint32_t writeFiniteWeather;
	};
	static_assert(sizeof(GenerationParameters) == 128);
	struct NoiseField
	{
		eastl::unique_ptr<Texture2D> texture;
		WeatherNoiseParameters parameters;
		bool valid = false;
	};
	static constexpr size_t kWeatherNoiseFields = 4;
	bool UpdateNoise(NoiseField& field, const WeatherNoiseParameters& parameters);

	std::array<eastl::unique_ptr<Texture2D>, 2> maps;
	std::array<eastl::unique_ptr<Texture2D>, 2> finiteMaps;
	eastl::unique_ptr<ConstantBuffer> generatorCb, noiseCb;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	std::array<NoiseField, 4> sharedNoise;
	std::vector<std::array<NoiseField, kWeatherNoiseFields>> layoutNoise;
	float4 generatedWeather = {};
	nlohmann::json generatedLayout;
	float generatedPhase = 0.f;
	float2 generatedFlow = {};
	uint32_t outputIndex = 0;
	bool hasConvection = false;
	bool hasFiniteWeather = false;
	bool hasProceduralLayout = true;
	float proceduralWeight = 0.f;
	uint64_t noiseRevision = 0;
	uint64_t weatherNoiseRevision = 0;
	uint64_t generatedNoiseRevision = 0;
	uint64_t textureRevision = 0;
	std::array<ID3D11ShaderResourceView*, 4> noiseInputs = {};
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 4> noiseInputResources;
	bool generatedValid = false;
};

struct LowCloudSettings
{
	static constexpr float defaultScale = .4f;
	static constexpr float sourceStratus = .0015f, sourceCumulus = .012f, sourceCore = .002f;
	float baseAltitude = 700.f;
	float worldScale = defaultScale;
	float curvatureRadius = 1000.f;
	float2 weatherSize = { 24.f, 24.f };
	float baseDepthScale = 1.f;
	float stratusExtinction = sourceStratus / defaultScale;
	float cumulusExtinction = sourceCumulus / defaultScale;
	float stormExtinction = sourceCore / defaultScale;
	float baseFadeMeters = 160.f;
	float baseHeightVariation = 0.f;
	float nvdfDetail = 0.65f;

	float GetWorldScale() const;
	float2 GetAltitudeRangeKm(bool convection) const;
	float GetCurvatureRadiusKm() const;
	float GetShadowBottomKm(float bottomKm, float rangeKm, uint32_t resolution, float2 cameraXY) const;
};

struct CirrusNoiseLayer
{
	uint32_t noise = 2;
	float frequency = 1.f;
	float2 offset = {};
	float4 range = { -1.f, 1.f, 0.f, 1.f };
};
static_assert(sizeof(CirrusNoiseLayer) == 32);

struct CirrusWeatherState
{
	std::array<CirrusNoiseLayer, 2> layers = { { { .noise = 1, .range = { -0.7f, 0.7f, 0.f, 0.6f } }, {} } };
	std::string localMap;
	float localWeight = 0.f;
	float localWindScale = 1.f;
	uint32_t localBlendMode = 0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CirrusNoiseLayer,
	noise, frequency, offset, range)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CirrusWeatherState,
	layers, localMap, localWeight, localWindScale, localBlendMode)

struct MidCloudWeatherState
{
	// Retain the old coverage/type keys. Missing new fields are neutral so
	// existing scene endpoints and RGB maps keep their authored appearance.
	std::array<CirrusNoiseLayer, 2> layers = { { { .noise = 1, .range = { -0.7f, 0.7f, 0.f, 0.6f } },
		{ .range = { -1.f, 1.f, 0.65f, 0.65f } } } };
	CirrusNoiseLayer secondaryCoverage{ .noise = 3, .range = { -1.f, 1.f, 0.f, 0.f } };
	CirrusNoiseLayer coverageGain{ .range = { -1.f, 1.f, 1.f, 1.f } };
	CirrusNoiseLayer thicknessVariation{ .noise = 0, .range = { -1.f, 1.f, 0.f, 0.f } };
	std::string localMap;
	float localWeight = 0.f;
	float localWindScale = 1.f;
	uint32_t localBlendMode = 0;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	MidCloudWeatherState,
	layers, secondaryCoverage, coverageGain, thicknessVariation, localMap, localWeight, localWindScale, localBlendMode)

struct CirrusSettings
{
	CirrusSettings();
	bool enabled = true;
	float altitude = 8000.f;
	float uvSize = 1.f;
	float densityScale = 1.f;
	SceneBlend weather;

	float GetAltitudeKm() const;
	float GetUvSize() const;
};

struct CloudLightingSettings
{
	float brightness = 1.f;
	float sunExtinction = 1.f;
	float viewExtinction = 1.f;
	float dropletDiameter = 20.f;
	float phaseForwardG = 0.85f;
	float phaseBackwardG = -0.3f;
	float msStrength = 1.f;
	float msAlbedo = 0.999f;
	float msBoundaryWidth = 100.f;
	float ambientStrength = 1.f;
	float scatterDensityThreshold = 0.001f;
	float scatterDensityPower = 1.f;
	float cirrusLightThickness = 200.f;
};

struct CloudWindSettings
{
	float2 lowVelocity = { 5.f, 0.f };
	float2 highVelocity = { 10.f, 0.f };
	float development = 0.25f;
	float disturbance = 0.2f;

	float2 UpperFlowDirection() const;
	static CloudWindSettings Interpolate(const CloudWindSettings& from, const CloudWindSettings& to, float weight);
};

struct MidCloudBands
{
	float direction = 0.f;
	float wavelength = 2000.f;
	float warp = 0.25f;
	float coverage = 0.f;
	float thickness = 0.f;
	float displacement = 0.f;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MidCloudBands, direction, wavelength, warp, coverage, thickness, displacement)

struct MidCloudSettings
{
	MidCloudSettings();
	bool enabled = false;
	float altitude = 3500.f;
	float thickness = 800.f;
	float densityScale = 0.02f;
	float uvSize = 1.f;
	float edgeSoftness = 0.1f;
	float cellFrequency = 2.f;
	float cellStrength = 0.9f;
	float stratusCellStrength = 0.1f;
	float warpStrength = 0.1f;
	float detailScale = 256.f;
	uint32_t detailMode = 1;
	float heightPower = 1.f;
	MidCloudBands bands;
	SceneBlend weather;
	/// Conservative curved-layer support, in metres above the atmosphere base.
	float2 GetAltitudeRange() const;
};

struct MidCloudMapManager
{
	/// Allocate the weather-map accumulation pair and generation constants.
	void SetupResources();
	/// Queue the generation shader and invalidate the cached output.
	void CompileShaders(Util::AsyncShaderSet& a_shaders);
	/// Regenerate changed scene/noise inputs; returns true when the map changes.
	bool Update(const MidCloudSettings& settings, TextureManager& textures, const CloudWeatherManager& weatherMap);
	/// Draw middle-layer and scene weather controls.
	static void DrawSettings(MidCloudSettings& settings, TextureManager& textures);
	/// Return the generated map, or null when generation is unavailable.
	ID3D11ShaderResourceView* Weather() const { return valid ? output.get() : nullptr; }

private:
	struct Parameters
	{
		std::array<CirrusNoiseLayer, 5> weather;
		float localWeight;
		uint32_t localBlendMode;
		float weight;
		uint32_t hasPrevious;
		float4 shape;
	};
	static_assert(sizeof(Parameters) == 192);
	std::array<eastl::unique_ptr<Texture2D>, 2> maps;
	eastl::unique_ptr<ConstantBuffer> parameters;
	winrt::com_ptr<ID3D11ComputeShader> shader;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	winrt::com_ptr<ID3D11ShaderResourceView> output;
	std::string key;
	uint64_t textureRevision = 0, noiseRevision = 0;
	bool valid = false;
};

struct CloudLayer
{
	LowCloudSettings low;
	MidCloudSettings middle;
	CirrusSettings cirrus;
	CloudLightingSettings lighting;
	CloudWindSettings wind;
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
	void CompileShaders(Util::AsyncShaderSet& a_shaders);
	bool ShadersReady(const CirrusSettings& settings) const;
	static void DrawSettings(CirrusSettings& settings, CloudLightingSettings& lighting, TextureManager& textures);
	bool Update(const CirrusSettings& settings, TextureManager& textures, const CloudWeatherManager& weatherMap);
	CirrusTextureSet GetTextures() const;

private:
	static constexpr uint32_t kDimension = 512;
	struct GenerationParameters
	{
		std::array<CirrusNoiseLayer, 2> weather;
		float2 windOffset = {};
		float localWeight = 0.f;
		float localWindScale = 1.f;
		uint32_t localBlendMode = 0;
		float weight = 1.f;
		uint32_t hasPrevious = 0;
		float padding = 0.f;
	};
	static_assert(sizeof(GenerationParameters) == 96);
	std::array<eastl::unique_ptr<Texture2D>, 2> texWeather;
	eastl::unique_ptr<ConstantBuffer> generationCb;
	winrt::com_ptr<ID3D11ComputeShader> weatherProgram;
	winrt::com_ptr<ID3D11SamplerState> sampler;
	std::string generatedKey;
	uint64_t generatedRevision = 0;
	uint64_t noiseRevision = 0;
	bool generatedValid = false;
	CirrusTextureSet outputs;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> outputResources;
};
