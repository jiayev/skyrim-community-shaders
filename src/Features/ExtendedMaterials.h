#pragma once

#include "Feature.h"

struct ExtendedMaterials : Feature
{
	static ExtendedMaterials* GetSingleton()
	{
		static ExtendedMaterials singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Extended Materials"; }
	virtual inline std::string GetShortName() { return "ExtendedMaterials"; }
	inline std::string_view GetShaderDefineName() override { return "EXTENDED_MATERIALS"; }

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct alignas(16) Settings
	{
		uint EnableComplexMaterial = 1;

		uint EnableParallax = 1;
		uint EnableTerrain = 0;

		uint EnableShadows = 1;
	};

	Settings settings;

	virtual void SetupResources(){};
	virtual inline void Reset() {}

	virtual void DataLoaded() override;

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader*, const uint32_t){};

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();

	bool SupportsVR() override { return true; };
};
