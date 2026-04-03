#pragma once

struct CloudRelight final : Feature
{
	static CloudRelight* GetSingleton()
	{
		static CloudRelight singleton;
		return &singleton;
	}

	// Metadata
	inline std::string GetName() override { return "Cloud Relight"; }
	inline std::string GetShortName() override { return "CloudRelight"; }
	inline std::string_view GetCategory() const override { return FeatureCategories::kSky; }
	inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Physically-based relighting of vanilla cloud textures using the directional sun light "
			"and the Cloud Shadows cubemap, adding silver-lining and forward-scattering effects.",
			{
				"Silver-lining and forward-scattering on vanilla clouds",
				"Cloud self-shadowing via Cloud Shadows cubemap",
				"Adjustable vanilla vs. relit blend",
			}
		};
	}

	// Functionality
	bool inline SupportsVR() override { return true; }
	inline std::string_view GetShaderDefineName() override { return "CLOUD_RELIGHT"; }
	inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	// Settings & UI
	void DataLoaded() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;
	void DrawSettings() override;

	// alignas(16) so the struct can be uploaded directly as a cbuffer entry
	struct alignas(16) Settings
	{
		uint32_t enabled = 1;
		float cloudRelightMix = 1.f;
		float cloudOriginalMix = 0.5f;
		float silverLiningMix = 1.f;
		//
		float silverLiningSpread = 0.f;
		float pad[3] = {};
	} settings;
};
