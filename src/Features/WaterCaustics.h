#pragma once

#include "Feature.h"
#include "State.h"
struct WaterCaustics : Feature
{
public:
	static WaterCaustics* GetSingleton()
	{
		static WaterCaustics singleton;
		return &singleton;
	}

	struct Settings
	{
		uint32_t EnableWaterCaustics = 1;
	};

	struct PerPass
	{
		Settings settings;
	};

	Settings settings;

	std::unique_ptr<Buffer> perPass = nullptr;

	ID3D11ShaderResourceView* causticsView;

	virtual inline std::string GetName() { return "Water Caustics"; }
	virtual inline std::string GetShortName() { return "WaterCaustics"; }
	inline std::string_view GetShaderDefineName() override { return "WATER_CAUSTICS"; }

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	virtual void SetupResources();
	virtual inline void Reset() {}

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();

	bool SupportsVR() override { return true; };
};
