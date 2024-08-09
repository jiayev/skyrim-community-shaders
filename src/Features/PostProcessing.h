#pragma once

#include "Buffer.h"
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

	virtual void PostPostLoad() override;

	void PreProcess();

	/////////////////////////////////////////////////////////////////////////////////

	bool bypass = false;

	std::vector<std::unique_ptr<PostProcessFeature>> feats = {};

	eastl::unique_ptr<Texture2D> texCopy = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> copyCS = nullptr;

	/////////////////////////////////////////////////////////////////////////////////

	struct BSImagespaceShaderHDRTonemapBlendCinematic_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			if (GetSingleton()->loaded)
				GetSingleton()->PreProcess();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderHDRTonemapBlendCinematicFade_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			if (GetSingleton()->loaded)
				GetSingleton()->PreProcess();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
};