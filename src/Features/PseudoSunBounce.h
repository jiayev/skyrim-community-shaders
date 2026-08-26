#pragma once

struct PseudoSunBounce : Feature
{
	virtual inline std::string GetName() override { return "Pseudo Sun Bounce"; }
	virtual inline std::string GetDisplayName() override { return T("feature.pseudo_sun_bounce.name", "Pseudo Sun Bounce"); }
	virtual inline std::string GetShortName() override { return "PseudoSunBounce"; }

	virtual inline std::string_view GetShaderDefineName() override { return "PSEUDO_SUN_BOUNCE"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.pseudo_sun_bounce.description", "Adds a pseudo sun bounce light to simulate indirect lighting from the sun, enhancing scene realism by providing additional illumination in shadowed areas."),
			{ T("feature.pseudo_sun_bounce.key_feature_1", "Simulates indirect sunlight bounce lighting"),
				T("feature.pseudo_sun_bounce.key_feature_2", "Performance-friendly implementation"),
				T("feature.pseudo_sun_bounce.key_feature_3", "Adjustable albedo, intensity, and window width controls"),
				T("feature.pseudo_sun_bounce.key_feature_4", "Works in conjunction with existing lighting systems") }
		};
	}
	virtual bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	struct Settings
	{
		float3 groundAlbedo = { 0.3f, 0.25f, 0.2f };
		float intensity = 1.0f;
		float3 wallAlbedo = { 0.5f, 0.45f, 0.4f };
		float windowWidth = 2.5f;
	} settings;

	static_assert(sizeof(Settings) % 16 == 0);
};
