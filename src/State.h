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

	RE::BSShader* currentShader = nullptr;
	uint32_t currentVertexDescriptor = 0;
	uint32_t currentPixelDescriptor = 0;
	spdlog::level::level_enum logLevel = spdlog::level::info;
	std::string shaderDefinesString = "";
	std::vector<std::pair<std::string, std::string>> shaderDefines{};  // data structure to parse string into; needed to avoid dangling pointers
	const std::string userConfigPath = "Data\\SKSE\\Plugins\\CommunityShadersUSER.json";
	const std::string defaultConfigPath = "Data\\SKSE\\Plugins\\CommunityShaders.json";

	bool upscalerLoaded = false;

	void Draw();
	void Reset();
	void Setup();

	void Load();
	void Save();

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

	void SetupResources();
	void ModifyShaderLookup(const RE::BSShader& a_shader, uint& a_vertexDescriptor, uint& a_pixelDescriptor);

	struct PerShader
	{
		uint VertexShaderDescriptor;
		uint PixelShaderDescriptor;
	};

	uint lastVertexDescriptor = 0;
	uint lastPixelDescriptor = 0;

	std::unique_ptr<Buffer> perShader = nullptr;
};
