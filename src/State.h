#pragma once

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

	void Draw();
	void Reset();
	void Setup();

	void Load();
	void Save();

	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);

	void SetLogLevel(spdlog::level::level_enum a_level = spdlog::level::info);
	spdlog::level::level_enum GetLogLevel();

};

