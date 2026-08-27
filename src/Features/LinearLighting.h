#pragma once

struct LinearLighting : Feature
{
	enum class ColorEncoding : uint
	{
		SRGB,
		Linear,
		GameGamma
	};

	static LinearLighting* GetSingleton()
	{
		static LinearLighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Linear Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.linear_lighting.name", "Linear Lighting"); }
	virtual inline std::string GetShortName() override { return "LinearLighting"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.linear_lighting.description", "Linear Lighting does internal color space conversion to improve lighting calculation accuracy."),
			{ T("feature.linear_lighting.key_feature_1", "Managed color input encoding"),
				T("feature.linear_lighting.key_feature_2", "Corrects lighting calculations"),
				T("feature.linear_lighting.key_feature_3", "Makes PBR really work") } };
	};

	virtual bool IsCore() const override { return true; };

	struct Settings
	{
		uint enableLinearLighting = false;
		uint enableACEScg = false;
		uint colorEncoding = static_cast<uint>(ColorEncoding::SRGB);

		// Lighting multipliers
		float vanillaDiffuseColorMult = 1.0f;
		float directionalLightMult = 1.0f;
		float pointLightMult = 1.0f;
		float ambientMult = 1.0f;
		float emitColorMult = 1.0f;
		float glowmapMult = 0.66f;

		// Effect multipliers
		float effectLightingMult = 0.32f;
		float membraneEffectMult = 1.0f;
		float bloodEffectMult = 1.0f;
		float projectedEffectMult = 1.0f;
		float deferredEffectMult = 1.0f;
		float otherEffectMult = 1.0f;
	} settings;

	struct alignas(16) PerFrameData
	{
		uint enableLinearLighting;
		uint enableACEScg;
		float vanillaDiffuseColorMult;
		float directionalLightMult;
		float pointLightMult;
		float ambientMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
		float pad0[3];
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrameData);

	float currentEmissiveMult = 1.0f;

	/** @brief Draws the ImGui settings UI for color management and lighting multiplier configuration. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	/** @brief Populates and returns the per-frame constant buffer data with color-management and multiplier settings. */
	PerFrameData GetCommonBufferData();

	bool IsColorManagementEnabled() const;
	ColorEncoding GetColorEncoding() const;
	RE::NiColor DecodeColor(RE::NiColor color) const;
	void DecodeColor(float* color) const;
	void TrackMappedColorBuffer(ID3D11Resource* resource, D3D11_MAPPED_SUBRESOURCE* mappedResource);
	void ConvertMappedColorBuffer(ID3D11Resource* resource);
	void BeginPassColorManagement(RE::BSRenderPass* pass, RE::BSShader::Type shaderType);
	void EndPassColorManagement();

	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
};
