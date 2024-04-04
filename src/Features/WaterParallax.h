#pragma once
#include "Buffer.h"
#include "Feature.h"

struct WaterParallax : Feature
{
public:
	static WaterParallax* GetSingleton()
	{
		static WaterParallax singleton;
		return &singleton;
	}

	struct Settings
	{
		uint32_t EnableWaterParallax = 1;
	};

	struct PerPass
	{
		Settings settings;
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	virtual inline std::string GetName() { return "Water Parallax"; }
	virtual inline std::string GetShortName() { return "WaterParallax"; }
	inline std::string_view GetShaderDefineName() override { return "WATER_PARALLAX"; }

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	virtual void SetupResources();
	virtual inline void Reset() {}

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();
};
