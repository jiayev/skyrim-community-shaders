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

	virtual inline std::string GetName() override { return "Water Parallax"; }
	virtual inline std::string GetShortName() override { return "WaterParallax"; }
	virtual inline std::string_view GetShaderDefineName() override { return "WATER_PARALLAX"; }

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	virtual bool SupportsVR() override { return true; };
};
