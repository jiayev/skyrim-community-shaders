#pragma once

#include "Buffer.h"
#include "LightLimitFix.h"

struct PhysicalLighting : Feature
{
public:
	virtual inline std::string GetName() override { return "Physical Lighting"; }
	virtual std::string GetDisplayName() override { return T("feature.physical_lighting.name", "Physical Lighting"); }
	virtual inline std::string GetShortName() override { return "PhysicalLighting"; }
	virtual inline std::string_view GetShaderDefineName() override { return "PHYSICAL_LIGHTING"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool HasShaderDefine(RE::BSShader::Type) override { return true; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.physical_lighting.description", "Physical Lighting adds optional physical units, color temperature, and basic area light metadata for dynamic lights."),
			{ T("feature.physical_lighting.key_feature_1", "Physical light units"),
				T("feature.physical_lighting.key_feature_2", "Kelvin color temperature"),
				T("feature.physical_lighting.key_feature_3", "Basic area light support") } };
	}

	enum class UnitType : std::uint32_t
	{
		Lumen = 0,
		Candela = 1,
		Lux = 2,
	};

	enum class AreaType : std::uint32_t
	{
		Point = 0,
		Sphere = 1,
		Disc = 2,
		Tube = 3,
		Rect = 4,
	};

	struct alignas(16) PhysicalLightExt
	{
		float luminousIntensity = 0.0f;
		float luminousPower = 0.0f;
		std::uint32_t unitType = static_cast<std::uint32_t>(UnitType::Lumen);
		float unitScale = 1.0f;

		float colorTemperature = 6500.0f;
		float tint = 0.0f;
		std::uint32_t pad0 = 0;
		std::uint32_t pad1 = 0;

		std::uint32_t areaType = static_cast<std::uint32_t>(AreaType::Point);
		float areaWidth = 0.0f;
		float areaHeight = 0.0f;
		float areaNormalize = 1.0f;

		std::uint32_t pad2 = 0;
		std::uint32_t pad3 = 0;
		std::uint32_t pad4 = 0;
		std::uint32_t pad5 = 0;
	};
	STATIC_ASSERT_ALIGNAS_16(PhysicalLightExt);

	struct alignas(16) PerFrame
	{
		float globalIntensityScale = 1.0f;
		std::uint32_t enableAreaLights = 1;
		float pad0[2]{};
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	struct PhysicalLightConfig
	{
		UnitType unitType = UnitType::Lumen;
		float intensity = 800.0f;
		float unitScale = 1.0f;
		float colorTemperature = 6500.0f;
		float tint = 0.0f;
		bool useColorTemp = false;
		AreaType areaType = AreaType::Point;
		float areaWidth = 0.0f;
		float areaHeight = 0.0f;
	};

	struct Settings
	{
		float globalIntensityScale = 1.0f;
		bool enableAreaLights = true;
	} settings;

	PerFrame GetCommonBufferData() const;

	virtual void SetupResources() override;
	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	void ProcessPhysicalLight(LightLimitFix::LightData& a_light, RE::NiLight* a_niLight, std::uint32_t a_lightIndex);
	void SetPhysicalLightData(RE::FormID a_formId, const PhysicalLightConfig& a_config);
	void SetPhysicalLightData(RE::NiLight* a_niLight, const PhysicalLightConfig& a_config);
	void ClearPhysicalLightData(RE::FormID a_formId);
	void ClearPhysicalLightData(RE::NiLight* a_niLight);
	void ClearAllPhysicalLightData();

	static float LumensToCandela(float a_lumens);
	static float LuxToCandela(float a_lux, float a_distance);
	static float3 KelvinToLinearRGB(float a_kelvin, float a_tint);

private:
	static constexpr std::uint32_t MAX_PHYSICAL_LIGHTS = 1024;

	static float ComputeAreaNormalization(AreaType a_type, float a_width, float a_height);
	const PhysicalLightConfig* FindConfig(RE::NiLight* a_niLight) const;

	eastl::unique_ptr<Buffer> physicalLightBuffer;
	eastl::array<PhysicalLightExt, MAX_PHYSICAL_LIGHTS> physicalLightExtData{};
	eastl::hash_map<RE::FormID, PhysicalLightConfig> configByFormId;
	eastl::hash_map<RE::NiLight*, PhysicalLightConfig> configByPointer;
};
