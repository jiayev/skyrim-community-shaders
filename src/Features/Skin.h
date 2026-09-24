#pragma once

#include "I18n/I18n.h"
#include "Skin/SkinMaterial.h"

#include <mutex>

/** @brief Advanced skin rendering feature with dual specular lobes, detail textures, and wetness effects. */
struct Skin : Feature
{
	static Skin* GetSingleton()
	{
		static Skin singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Advanced Skin"; }
	virtual inline std::string GetDisplayName() override { return T("feature.skin.name", "Advanced Skin"); }
	virtual inline std::string GetShortName() override { return "Skin"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CS_SKIN"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }
	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.skin.description", "Advanced Skin enhances character skin rendering with multiple techniques."),
			{ T("feature.skin.key_feature_1", "Physically-based dual specular lobes for realistic skin highlights"),
				T("feature.skin.key_feature_2", "Tiled skin detail textures for enhanced realism"),
				T("feature.skin.key_feature_3", "Extra texture support for roughness, translucency, and wetness"),
				T("feature.skin.key_feature_4", "Reworked wetness system for dynamic skin effects") }
		};
	}
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting;
	};

	virtual void RestoreDefaultSettings() override;
	/** @brief Draws the ImGui settings panel for Advanced Skin configuration. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief Binds the skin detail texture to the pixel shader during the prepass stage. */
	virtual void Prepass() override;
	/** @brief Installs rendering hooks for material and geometry setup after plugin load. */
	virtual void PostPostLoad() override;

	/** @brief Creates GPU resources including the skin detail texture and per-geometry constant buffer. */
	virtual void SetupResources() override;
	virtual void DataLoaded() override;

	/** @brief Reloads the skin detail normal map texture from disk. */
	void ReloadSkinDetail();
	/** @brief Loads the skin detail normal map DDS texture and creates its shader resource view. */
	void LoadSkinDetailTexture();

	using SkinProfile = SkinMaterials::Parameters;
	struct Settings
	{
		bool EnableSkin = true;
		float ExtraSkinWetness = 0.0f;
		float WetFadeTime = 10.0f;
		float StartSweat = 0.75f;
		float FullSweat = 0.15f;
		float4 WetParams = { 512.0f, 0.7f, 10.0f, 4.0f };
		SkinProfile DefaultProfile;
	} settings;

	struct alignas(16) SkinData
	{
		float4 skinParams;
		float4 skinParams2;
		float4 skinDetailParams;
		float4 sssParams;
		float4 fuzzParams;
		float4 physicalParams;
		float4 wetParams;
	};

	struct alignas(16) PerGeometryData
	{
		float4 skinPerGeometry;
		float4 materialFlags;
		SkinData profile;
	};
	static_assert(sizeof(SkinData) == 112);
	static_assert(sizeof(PerGeometryData) == 144);

	struct ExtraTextures
	{
		RE::NiSourceTexturePtr rfaosTexture;
		RE::NiSourceTexturePtr wetnessTexture;
		bool hasExtraTexture = false;
		bool hasWetnessTexture = false;
	};

	SkinData GetCommonBufferData();
	SkinData MakeProfileData(const SkinProfile& a_profile) const;
	void SetShaderResources(ID3D11DeviceContext* a_context);
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
	void DrawMaterialEditor();

private:
	struct GeometryEntry
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiPointer<RE::BSShaderProperty> property;
		RE::NiPointer<RE::BSTextureSet> textureSet;
		RE::NiPointer<RE::NiTexture> normal;
		RE::NiPointer<RE::NiTexture> diffuse;
		const RE::BSShaderMaterial* material = nullptr;
		SkinMaterials::NifMaterial nif;
		SkinMaterials::Target surface;
		SkinMaterials::Resolution resolved;
		ExtraTextures textures;
		RE::FormID txst = 0;
		RE::FormID npc = 0;
		RE::FormID race = 0;
		uint32_t materialHash = 0;
		uint32_t lastFrame = 0;
		uint64_t revision = 0;
		uint64_t previewRevision = 0;
		bool head = false;
	};

	struct ActorWetnessCacheEntry
	{
		float4 wetness = { 0.0f, 0.0f, 0.0f, 0.0f };
		uint32_t frameCount = 0;
	};

	struct Editor
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		std::vector<RE::NiPointer<RE::BSGeometry>> surfaces;
		int scope = 0;
		SkinMaterials::UserEdit edit;
		SkinMaterials::Material draft;
		SkinProfile adjusted;
		bool replacing = false;
		bool preview = false;
		bool valid = true;
		std::string baseline;
		std::string error;
		std::string exportName = "SkinMaterial";
		uint64_t revision = 1;
	} editor;

	std::recursive_mutex mutex;
	SkinMaterials::Store materials;
	std::unordered_map<RE::BSGeometry*, GeometryEntry> geometries;
	std::unordered_map<std::string, ExtraTextures> textureCache;
	std::unordered_map<uint32_t, ActorWetnessCacheEntry> actorWetnessMap;
	ExtraTextures pendingTextures;
	bool texturesDirty = false;
	uint32_t lastScanFrame = 0;
	eastl::unique_ptr<ConstantBuffer> PerGeometryCB;
	eastl::unique_ptr<Texture2D> texSkinDetail;

	GeometryEntry& ResolveGeometry(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property);
	bool PreviewMatches(const GeometryEntry& a_entry) const;
	void Invalidate();
	ExtraTextures LoadConfiguredExtraTextures(const std::string& a_rfaos, const std::string& a_wetness);
	float GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos);
	float4 GetWetness(RE::BSGeometry* a_geometry);
	void DrawGlobalSettings();
	SkinMaterials::UserEdit DraftEdit() const;
	bool HasUnsavedEdit() const;
	SkinMaterials::Target EditTarget(const GeometryEntry& a_entry) const;
	void SelectReference(RE::TESObjectREFR* a_ref);
	void SelectSurface(RE::BSGeometry* a_geometry);
	void BeginEdit();
	static void DrawParameters(SkinProfile& a_profile, SkinMaterials::Patch* a_patch = nullptr);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags);
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};
};
