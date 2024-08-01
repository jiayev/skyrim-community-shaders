#pragma once

struct TruePBR
{
public:
	static TruePBR* GetSingleton()
	{
		static TruePBR singleton;
		return &singleton;
	}

	inline std::string GetShortName() { return "TruePBR"; }

	void DrawSettings();
	void SetupResources();
	void LoadSettings(json& o_json);
	void SaveSettings(json& o_json);
	void PostPostLoad();

	void SetShaderResouces();
	void GenerateShaderPermutations(RE::BSShader* shader);

	std::unordered_map<uint32_t, std::string> editorIDs;

	float globalPBRDirectLightColorMultiplier = 1.f;
	float globalPBRAmbientLightColorMultiplier = 1.f;

	float weatherPBRDirectionalLightColorMultiplier = 1.f;
	float weatherPBRDirectionalAmbientLightColorMultiplier = 1.f;

	struct alignas(16) Settings
	{
		float directionalLightColorMultiplier = 1.f;
		float pointLightColorMultiplier = 1.f;
		float ambientLightColorMultiplier = 1.f;
		uint32_t useMultipleScattering = true;
		uint32_t useMultiBounceAO = true;
		uint32_t pad[3];
	} settings{};

	struct PBRTextureSetData
	{
		float roughnessScale = 1.f;
		float displacementScale = 1.f;
		float specularLevel = 0.04f;

		RE::NiColor subsurfaceColor;
		float subsurfaceOpacity = 0.f;

		RE::NiColor coatColor = { 1.f, 1.f, 1.f };
		float coatStrength = 1.f;
		float coatRoughness = 1.f;
		float coatSpecularLevel = 0.04f;
		float innerLayerDisplacementOffset = 0.f;

		RE::NiColor fuzzColor;
		float fuzzWeight = 0.f;
	};

	void SetupFrame();

	void SetupTextureSetData();
	PBRTextureSetData* GetPBRTextureSetData(const RE::TESForm* textureSet);
	bool IsPBRTextureSet(const RE::TESForm* textureSet);

	std::unordered_map<std::string, PBRTextureSetData> pbrTextureSets;

	struct PBRMaterialObjectData
	{
		std::array<float, 3> baseColorScale = { 1.f, 1.f, 1.f };
		float roughness = 1.f;
		float specularLevel = 1.f;
	};

	void SetupMaterialObjectData();
	PBRMaterialObjectData* GetPBRMaterialObjectData(const RE::TESForm* materialObject);
	bool IsPBRMaterialObject(const RE::TESForm* materialObject);

	std::unordered_map<std::string, PBRMaterialObjectData> pbrMaterialObjects;

	struct PBRLightingTemplateData
	{
		float directionalLightColorScale = 1.f;
		float directionalAmbientLightColorScale = 1.f;
	};

	void SetupLightingTemplateData();
	PBRLightingTemplateData* GetPBRLightingTemplateData(const RE::TESForm* lightingTemplate);
	bool IsPBRLightingTemplate(const RE::TESForm* lightingTemplate);
	void SavePBRLightingTemplateData(const std::string& editorId);

	std::unordered_map<std::string, PBRLightingTemplateData> pbrLightingTemplates;

	struct PBRWeatherData
	{
		float directionalLightColorScale = 1.f;
		float directionalAmbientLightColorScale = 1.f;
	};

	void SetupWeatherData();
	PBRWeatherData* GetPBRWeatherData(const RE::TESForm* weather);
	bool IsPBRWeather(const RE::TESForm* weather);
	void SavePBRWeatherData(const std::string& editorId);

	std::unordered_map<std::string, PBRWeatherData> pbrWeathers;
};
