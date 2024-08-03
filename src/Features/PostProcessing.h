#pragma once

#include "Feature.h"

#include "PostProcessing/PostProcessModule.h"

struct PostProcessing : Feature
{
	static PostProcessing* GetSingleton()
	{
		static PostProcessing singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Post Processing"; }
	virtual inline std::string GetShortName() override { return "PostProcessing"; }

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual void ClearShaderCache() override;

	/////////////////////////////////////////////////////////////////////////////////

	std::vector<std::unique_ptr<PostProcessModule>> modules = {};
};