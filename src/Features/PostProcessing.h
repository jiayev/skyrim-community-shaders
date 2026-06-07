#pragma once

#include "PostProcessing/PostProcessFeature.h"

#include "PostProcessing/BokehResources.h"
#include "PostProcessing/Border.h"
#include "PostProcessing/CODBloom.h"
#include "PostProcessing/Camera.h"
#include "PostProcessing/ColorGrading.h"
#include "PostProcessing/Composite.h"
#include "PostProcessing/DoF.h"
#include "PostProcessing/HistogramAutoExposure.h"
#include "PostProcessing/LUT.h"
#include "PostProcessing/LensFlare.h"
#include "PostProcessing/LocalExposure.h"
#include "PostProcessing/MotionBlur.h"
#include "PostProcessing/PhysicalGlare.h"
#include "PostProcessing/Vignette.h"

struct PostProcessing : Feature
{
	static PostProcessing* GetSingleton()
	{
		static PostProcessing singleton;
		return &singleton;
	}

	struct alignas(16) Settings
	{
		uint DisableVanillaTonemapping = 1;
		uint pad[3];
	} settings;

	const std::string ppPresetPath = "Data\\SKSE\\Plugins\\CommunityShaders\\PostProcessing";

	virtual inline std::string GetName() override { return "Post Processing"; }
	virtual inline std::string GetDisplayName() override { return T("feature.post_processing.name", "Post Processing"); }
	virtual inline std::string GetShortName() override { return "PostProcessing"; }
	virtual inline std::string_view GetShaderDefineName() override { return "POSTPROCESS"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::ImageSpace;
	};
	virtual std::string_view GetCategory() const override { return FeatureCategories::kPostProcessing; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.post_processing.description", "Post Processing provides advanced image effects and enhancements to improve the visual quality of the game."),
			{ T("feature.post_processing.key_feature_1", "Customizable post-processing effects"),
				T("feature.post_processing.key_feature_2", "Supports various presets for different visual styles"),
				T("feature.post_processing.key_feature_3", "Improves overall image quality and immersion"),
				T("feature.post_processing.key_feature_4", "Includes features like bloom, depth of field, and color grading") }
		};
	}

	virtual bool SupportsVR() { return true; }

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	json pendingSettings = {};

	void ProcessSettings(json& o_json);

	std::vector<std::string> presets = {};
	std::vector<std::string> LoadPresets();
	void SavePresetTo(std::string a_name);
	void LoadPresetFrom(std::string a_name);

	enum class FeaturePipelineIndex : size_t
	{
		DoF,
		Vignette,
		LocalExposure,
		AutoExposure,
		MotionBlur,
		PhysicalGlare,
		CODBloom,
		LensFlare,
		Composite,
		ColorGrading,
		LUT,
		Camera,
		Border,
		COUNT
	};

	std::array<std::unique_ptr<PostProcessFeature>, static_cast<size_t>(FeaturePipelineIndex::COUNT)> pipeline;

	BokehResources bokehResources;

	template <typename T>
	T* GetPipelineFeature(FeaturePipelineIndex idx)
	{
		return static_cast<T*>(pipeline[static_cast<size_t>(idx)].get());
	}

	virtual void ClearShaderCache() override;

	virtual void SetupResources() override;
	virtual void Reset() override;

	virtual void PostPostLoad() override;
	virtual void Prepass() override;

	void PreProcess();
	void DrawBeforeUpscaling();
	void ClearBorderMotionVectorsForFrameGen();
	void DrawFeature(PostProcessFeature& feature, PostProcessFeature::TextureInfo& lastTexColor);

	/// Copy lastTexColor to a render target, performing format conversion via copyCS if needed.
	void CopyToRenderTarget(
		RE::BSGraphics::RenderTargetData& targetRT,
		Texture2D* convertTex,
		ID3D11Texture2D* srcTex,
		ID3D11ShaderResourceView* srcSRV);

	/////////////////////////////////////////////////////////////////////////////////

	bool bypass = false;
	bool isrefraction = false;

	struct ImageSpaceManager
	{
		RE::ImageSpaceData gameISData;
	};

	std::unique_ptr<ImageSpaceManager> imageSpaceManager = std::make_unique<ImageSpaceManager>();

	eastl::unique_ptr<Texture2D> texCopyMain = nullptr;
	eastl::unique_ptr<Texture2D> texCopyMainCopy = nullptr;
	eastl::unique_ptr<Texture2D> texAfterTAA = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> copyCS = nullptr;

	/////////////////////////////////////////////////////////////////////////////////

	struct BSImagespaceShaderHDRTonemapBlendCinematic_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			if (globals::features::postProcessing.loaded)
				globals::features::postProcessing.PreProcess();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderHDRTonemapBlendCinematicFade_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			if (globals::features::postProcessing.loaded)
				globals::features::postProcessing.PreProcess();
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSImagespaceShaderRefraction_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			globals::features::postProcessing.isrefraction = true;
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
