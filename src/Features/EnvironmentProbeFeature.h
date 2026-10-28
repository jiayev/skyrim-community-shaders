#pragma once
#include "Feature.h"
#include "Utils/TexturePreview.h"

struct EnvironmentProbeFeature : Feature, RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
	std::string GetName() override { return "Environment Probe"; }
	std::string GetShortName() override { return "EnvironmentProbe"; }
	std::string GetDisplayName() override { return T("feature.environment_probe.name", "Environment Probe"); }
	std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.environment_probe.summary", "Captures nearby surface radiance and depth for reflections and indirect lighting."), {} };
	}
	void LoadSettings(json& value) override;
	void SaveSettings(json& value) override;
	void RestoreDefaultSettings() override;
	void DrawSettings() override;
	void DataLoaded() override;
	RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
	/** Called after world rendering; independent of menu visibility. */
	void Render();
	struct Settings
	{
		bool Enabled = true;
		bool SupplementalSunShadows = true;
		int UpdateMode = 1;
		float AnchorDistance = 64.0f;
	} settings;

private:
	TexturePreview preview;
};
