#pragma once

#include "Feature.h"

#include "PostProcessing/PostProcessFeature.h"

struct PostProcessing : Feature
{
	static PostProcessing* GetSingleton()
	{
		static PostProcessing singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Post Processing"; }
	virtual inline std::string GetShortName() override { return "PostProcessing"; }

	virtual bool SupportsVR() { return true; }

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual void ClearShaderCache() override;

	virtual void SetupResources() override;
	virtual void Reset() override;

	/////////////////////////////////////////////////////////////////////////////////

	std::vector<std::unique_ptr<PostProcessFeature>> feats = {};
};