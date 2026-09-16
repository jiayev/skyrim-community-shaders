#pragma once

#include "CreationEngineRaytracing.h"
#include "Feature.h"
#include "FeatureCategories.h"

/**
 * @brief Regular feature providing hardware-accelerated Path Tracing.
 */
struct PathTracing : Feature
{
public:
	// Metadata
	virtual std::string GetName() override { return "Path Tracing"; }
	virtual std::string GetDisplayName() override { return T("feature.path_tracing.name", "Path Tracing"); }
	virtual std::string GetShortName() override { return "PathTracing"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool IsCore() const override { return false; }
	virtual bool IsInMenu() const override { return true; }
	virtual ReleaseStage GetReleaseStage() const override { return ReleaseStage::Alpha; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return std::make_pair(
			"Path Tracing simulates physically accurate light transport including global illumination, soft shadows, and reflections.",
			std::vector<std::string>{
				"Full path-traced lighting and reflections",
				"Configurable bounces and samples per pixel",
				"Russian Roulette path termination"
			});
	}

	struct Settings
	{
		bool Enabled = true;
		CreationEngineRaytracing::RaytracingSettings RaytracingSettings;
		CreationEngineRaytracing::GeneralSettings GeneralSettings{
			.Denoiser = CreationEngineRaytracing::Denoiser::None,
			.Mode = CreationEngineRaytracing::Mode::PathTracing
		};
		bool StablePlanes = false;
		CreationEngineRaytracing::NRDSettings NRDSettings;
		CreationEngineRaytracing::NRDReblurSettings NRDReblurSettings;
		CreationEngineRaytracing::NRDRelaxSettings NRDRelaxSettings;
		CreationEngineRaytracing::SSSSettings SSSSettings;
		CreationEngineRaytracing::MaterialSettings MaterialSettings;
		CreationEngineRaytracing::LightingSettings LightingSettings;
		CreationEngineRaytracing::WaterSettings WaterSettings;

		bool operator==(const Settings&) const = default;
	} settings;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	void DrawGeneralSettings();
	void DrawAdvancedSettings();
	void DrawNRDSettings();
	void DrawReblurSettings();
	void DrawRelaxSettings();
	void DrawSSSSettings();
	void DrawMaterialSettings();
	void DrawLightingSettings();
	void DrawWaterSettings();

	void UpdateSettings();
	bool Available() const;
};

