#pragma once

#include <Buffer.h>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

class State
{
public:
	static State* GetSingleton()
	{
		static State singleton;
		return &singleton;
	}

	bool enabledClasses[RE::BSShader::Type::Total - 1];

	bool updateShader = true;
	RE::BSShader* currentShader = nullptr;

	uint32_t currentVertexDescriptor = 0;
	uint32_t currentPixelDescriptor = 0;
	spdlog::level::level_enum logLevel = spdlog::level::info;
	std::string shaderDefinesString = "";
	std::vector<std::pair<std::string, std::string>> shaderDefines{};  // data structure to parse string into; needed to avoid dangling pointers
	const std::string testConfigPath = "Data\\SKSE\\Plugins\\CommunityShadersTEST.json";
	const std::string userConfigPath = "Data\\SKSE\\Plugins\\CommunityShadersUSER.json";
	const std::string defaultConfigPath = "Data\\SKSE\\Plugins\\CommunityShaders.json";

	bool upscalerLoaded = false;

	float timer = 0;

	void Draw();
	void DrawDeferred();
	void DrawPreProcess();
	void Reset();
	void Setup();

	void Load(bool a_test = false);
	void Save(bool a_test = false);
	void PostPostLoad();

	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);

	void SetLogLevel(spdlog::level::level_enum a_level = spdlog::level::info);
	spdlog::level::level_enum GetLogLevel();

	void SetDefines(std::string defines);
	std::vector<std::pair<std::string, std::string>>* GetDefines();

	/*
     * Whether a_type is currently enabled in Community Shaders
     *
     * @param a_type The type of shader to check
     * @return Whether the shader has been enabled.
     */
	bool ShaderEnabled(const RE::BSShader::Type a_type);

	/*
     * Whether a_shader is currently enabled in Community Shaders
     *
     * @param a_shader The shader to check
     * @return Whether the shader has been enabled.
     */
	bool IsShaderEnabled(const RE::BSShader& a_shader);

	/*
     * Whether developer mode is enabled allowing advanced options.
	 * Use at your own risk! No support provided.
     *
	 * <p>
	 * Developer mode is active when the log level is trace or debug.
	 * </p>
	 * 
     * @return Whether in developer mode.
     */
	bool IsDeveloperMode();

	void ModifyRenderTarget(RE::RENDER_TARGETS::RENDER_TARGET a_targetIndex, RE::BSGraphics::RenderTargetProperties* a_properties);

	void SetupResources();
	void ModifyShaderLookup(const RE::BSShader& a_shader, uint& a_vertexDescriptor, uint& a_pixelDescriptor);

	struct PerShader
	{
		uint VertexShaderDescriptor;
		uint PixelShaderDescriptor;
	};

	uint lastVertexDescriptor = 0;
	uint lastPixelDescriptor = 0;

	std::unique_ptr<Buffer> shaderDataBuffer = nullptr;

	void UpdateSharedData(const RE::BSShader* shader, const uint32_t descriptor);

	bool lightingDataRequiresUpdate = false;

	struct LightingData
	{
		float WaterHeight[25];
		uint Reflections;
		float4 CameraData;
		float2 BufferDim;
		float Timer;
	};

	LightingData lightingData{};

	std::unique_ptr<Buffer> lightingDataBuffer = nullptr;

	// Skyrim constants
	bool isVR = false;
	float screenWidth = 0;
	float screenHeight = 0;
	ID3D11DeviceContext* context = nullptr;
	ID3D11Device* device = nullptr;
	RE::BSGraphics::RendererShadowState* shadowState = nullptr;
};
