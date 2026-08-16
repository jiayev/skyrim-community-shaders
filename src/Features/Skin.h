#pragma once

#include "I18n/I18n.h"

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

	/** @brief Reloads the skin detail normal map texture from disk. */
	void ReloadSkinDetail();
	/** @brief Loads the skin detail normal map DDS texture and creates its shader resource view. */
	void LoadSkinDetailTexture();

	/** @brief Appearance settings that can be overridden per race (and, later, per actor). */
	struct SkinProfile
	{
		float SkinMainRoughness = 0.7f;
		float SkinSecondRoughness = 0.35f;
		float SkinSpecularTexMultiplier = 1.0f;
		float SecondarySpecularStrength = 0.15f;
		float F0 = 0.0278f;
		float BaseColorMultiplier = 1.0f;
		float PhysicalMainRoughnessMultiplier = 1.3f;
		float PhysicalSecondRoughnessMultiplier = 0.75f;
		float PhysicalSpecularStrength = 1.0f;
		float ExtraEdgeRoughness = 0.25f;
		bool EnableSkinDetail = true;
		float SkinDetailStrength = 0.25f;
		float SkinDetailTiling = 10.0f;
		float BodyTilingMultiplier = 2.0f;
		float Translucency = 0.1f;
		float sssWidth = 0.2f;
		bool UseSSS = true;
		float FuzzStrength = 1.0f;
		float FuzzRoughness = 0.35f;
		float FuzzF0 = 0.045f;
	};

	struct Settings
	{
		bool EnableSkin = true;
		float ExtraSkinWetness = 0.0f;
		float WetFadeTime = 10.0f;
		float StartSweat = 0.75f;
		float FullSweat = 0.15f;
		float4 WetParams = { 512.0f, 0.7f, 10.0f, 4.0f };
		bool UseDynamicWetness = false;

		SkinProfile DefaultProfile;
		/** @brief Named profile pool, shared by any number of bindings. */
		std::map<std::string, SkinProfile> Profiles;
		/** @brief Race editor ID -> profile name. */
		std::map<std::string, std::string> RaceProfiles;
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
		SkinData profile;
	};

	eastl::unique_ptr<ConstantBuffer> PerGeometryCB;
	float playerStamina = 0.0f;
	float playerStaminaMax = 0.0f;

	/** @brief GPU-ready profile data; index 0 is always the default profile. */
	std::vector<SkinData> profileData;
	/** @brief CPU-side profiles parallel to profileData (index-aligned), kept as the partial-merge base. */
	std::vector<SkinProfile> profileBaseData;
	std::unordered_map<std::string, uint32_t> profileNameToIndex;
	std::unordered_map<RE::FormID, uint32_t> raceProfileIndex;
	uint32_t profileDataRevision = 0;
	bool profileBindingsDirty = true;

	/** @brief Override key scope: per-NIF mesh vs per-NPC base form ID. */
	enum class OverrideKind
	{
		Nif,
		BaseId
	};

	/** @brief Per-NIF / Per-BaseID JSON override store (Data\Shaders\Skin\Overrides). */
	struct OverrideStore
	{
		/** @brief A resolved override entry in the effective (merged) view. */
		struct Entry
		{
			std::string source;  // "User" or the mod .json file name
			bool isUser = false;
			json partial;  // partial SkinProfile JSON
		};

		/** @brief Effective merged NIF overrides (user wins); normalized key -> entry. */
		std::unordered_map<std::string, Entry> nifOverrides;
		/** @brief Effective merged BaseID overrides (user wins); normalized key -> entry. */
		std::unordered_map<std::string, Entry> baseIdOverrides;
		/** @brief User-authored NIF partials, persisted to User\SkinOverrides.user.json. */
		std::unordered_map<std::string, json> userNifOverrides;
		/** @brief User-authored BaseID partials, persisted to User\SkinOverrides.user.json. */
		std::unordered_map<std::string, json> userBaseIdOverrides;
		/** @brief Last scanned override file set: file path -> last write time. */
		std::unordered_map<std::string, std::filesystem::file_time_type> fileTimes;
		/** @brief Bumped whenever the override set changes; invalidates per-geometry caches. */
		uint32_t revision = 0;

		/** @brief Scans the overrides directory (and User subfolder) and reloads when the file set changes. */
		void Refresh();
		/** @brief Returns the effective partial override for a kind + normalized key, or nullptr. */
		const json* Lookup(OverrideKind a_kind, const std::string& a_key) const;
		/** @brief Adds/overwrites a user override and persists it. */
		void AddOverride(OverrideKind a_kind, const std::string& a_key, const json& a_partial);
		/** @brief Removes a user override (revealing any mod override underneath) and persists. */
		void RemoveOverride(OverrideKind a_kind, const std::string& a_key);
		/** @brief Writes the user override file to disk. */
		void SaveUserOverrides();
		/** @brief True when no overrides of either kind are loaded. */
		bool Empty() const { return nifOverrides.empty() && baseIdOverrides.empty(); }
	};

	/** @brief Per-geometry override resolution cache, keyed by geometry pointer. */
	struct GeometryOverrideCacheEntry
	{
		SkinData merged{};
		bool initialized = false;
		std::string nifKey;
		std::string baseIdKey;
		uint32_t profileIndex = UINT32_MAX;
		uint32_t baseProfileRevision = UINT32_MAX;
		uint32_t overrideRevision = UINT32_MAX;
	};

	OverrideStore overrideStore;
	std::unordered_map<RE::BSGeometry*, GeometryOverrideCacheEntry> geometryOverrideCache;
	uint32_t lastOverrideScanFrame = 0;

	/** @brief Packs a profile plus the global (non-per-race) settings into GPU data. */
	SkinData MakeProfileData(const SkinProfile& a_profile) const;
	/** @brief Merges a partial JSON override onto a base profile and packs the result for the GPU. */
	SkinData ApplyOverride(const SkinProfile& a_base, const json& a_override) const;
	/** @brief Rebuilds the GPU profile array, bumping the revision when the contents change. */
	void RebuildProfileData();
	/** @brief Drops cached race->profile resolutions, forcing them to be resolved again. */
	void InvalidateProfileBindings();
	/**
	 * @brief Resolves the profile index used for a race, caching the result by race form ID.
	 * @param a_race The race to resolve, may be null.
	 * @return Index into profileData; 0 when no override applies.
	 */
	uint32_t GetProfileIndexForRace(const RE::TESRace* a_race);

	/** @brief Draws the global (non-per-race) settings block. */
	void DrawGlobalSettings();
	/** @brief Draws the profile selector plus add/duplicate/rename/delete controls. */
	void DrawProfileManager();
	/** @brief Draws every per-race-able setting of the given profile.
	 *  @param a_id Unique ImGui ID scope. DrawProfileSettings is called from several
	 *  sections at the same time (main editor and override editor), so every call
	 *  site must supply a distinct ID prefix to avoid widget ID collisions. */
	void DrawProfileSettings(SkinProfile& a_profile, const char* a_id);
	/** @brief Draws the race to profile binding table. */
	void DrawRaceBindings();
	/** @brief Collects all races with an editor ID for the binding UI. */
	void RefreshRaceList();
	/** @brief Draws the per-NIF JSON override manager UI. */
	void DrawNifOverrides();
	/** @brief Resolves a reference into the pending pick state (key, label, skin flag). */
	void ResolveUiPick(RE::TESObjectREFR* a_ref);
	/** @brief Opens the override editor for a key, seeded from the given base profile. */
	void BeginOverrideEdit(OverrideKind a_kind, const std::string& a_key, const SkinProfile& a_base, const std::string& a_baseLabel, bool a_isNew);
	/** @brief Derives all unique normalized NIF override keys for a reference.
	 *  @return The target's MODL key (when present) followed by every unique
	 *  per-geometry key, in scenegraph traversal order. Geometry keys use the
	 *  same logic as the runtime override lookup, so the pick UI and the
	 *  rendered result stay aligned. */
	std::vector<std::string> DeriveNifKeysForRef(RE::TESObjectREFR* a_ref) const;
	/** @brief True if any geometry of the reference uses a skin shader. */
	bool ReferenceHasSkin(RE::TESObjectREFR* a_ref) const;
	/** @brief Returns the race for an actor reference, or null. */
	RE::TESRace* GetRaceForRef(RE::TESObjectREFR* a_ref) const;
	/** @brief Returns a partial profile JSON containing only fields that differ from the base. */
	json DiffProfile(const SkinProfile& a_base, const SkinProfile& a_full) const;

	std::string uiSelectedProfile;  // empty = default profile
	std::string uiPendingRace;
	std::string uiProfileNameBuffer;
	std::vector<std::pair<std::string, std::string>> raceList;  // editor ID, display name

	// Per-NIF / Per-BaseID override UI state
	std::vector<std::string> uiPickNifKeys;  // all unique NIF keys derived from the last pick
	std::string uiPickKey;                   // currently selected NIF key from uiPickNifKeys
	std::string uiPickBaseIdKey;             // normalized BaseID key (empty for non-actors)
	std::string uiPickRefLabel;              // human-readable target label
	std::string uiPickMessage;               // non-empty = info/error message to show
	bool uiPickValid = false;
	bool uiPickHasSkin = false;
	SkinProfile uiPickBase;       // base profile for a new override (race profile or default)
	std::string uiPickBaseLabel;  // "Default" or the race editor ID

	bool uiOverrideEditorOpen = false;
	bool uiOverrideEditorIsNew = false;
	OverrideKind uiOverrideEditorKind = OverrideKind::Nif;
	std::string uiOverrideEditorKey;
	std::string uiOverrideEditorBaseLabel;
	SkinProfile uiOverrideEditorBase;
	SkinProfile uiOverrideEditorProfile;

	struct ExtraTextures
	{
		RE::NiSourceTexturePtr rfaosTexture;
		RE::NiSourceTexturePtr wetnessTexture;
		std::string extraTexturePath;
		std::string wetnessTexturePath;
		bool hasExtraTexture = false;
		bool hasWetnessTexture = false;
	};

	struct ActorWetnessCacheEntry
	{
		float4 wetness = { 0.0f, 0.0f, 0.0f, 0.0f };
		uint frameCount = 0;
		uint32_t profileIndex = 0;
	};

	eastl::unique_ptr<Texture2D> texSkinDetail = nullptr;
	std::unordered_map<uint32_t, ExtraTextures> skinExtraTextures;
	std::unordered_map<uint32_t, ActorWetnessCacheEntry> actorWetnessMap;  // keyed by actor formID

	/** @brief Packs current skin settings into a GPU constant buffer data structure. */
	SkinData GetCommonBufferData();

	/**
	 * @brief Queries the water height at the given actor's position.
	 * @param a_ref The object reference (typically an actor) to query water height for.
	 * @param a_pos The world position to check for nearby water objects.
	 * @return The water surface height, or -NI_INFINITY if no water is found.
	 */
	float GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos);

	/**
	 * @brief Computes per-geometry wetness data (sweat, water submersion, fade) for an actor.
	 * @param geometry The geometry to retrieve wetness data for.
	 * @param a_profileIndex Receives the profile index resolved for the geometry's actor.
	 * @return A float4 containing (sweat, waterWetness, positionZ, waterDepth).
	 */
	float4 GetWetness(RE::BSGeometry* geometry, uint32_t& a_profileIndex);

	/**
	 * @brief Discovers and loads extra skin textures (RFAOS, wetness) based on the material's texture set.
	 * @param material The lighting shader material to derive texture paths from.
	 * @param inTextureSet The texture set to search for extra texture slots.
	 * @param i_hashKey The material hash key used for caching extra textures.
	 */
	void SetupExtraTexture(RE::BSLightingShaderMaterialBase const* material, RE::BSTextureSet* inTextureSet, uint32_t i_hashKey);

	/** @brief Handles material setup for face/face-gen materials, loading extra skin textures as needed. */
	void BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material);

	/** @brief Updates per-geometry wetness constant buffer during geometry setup. */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);

	/** @brief Binds extra skin textures (RFAOS, wetness) to pixel shader resource slots. */
	void SetShaderResources(ID3D11DeviceContext* a_context);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[Advanced Skin] Installed hooks");
			return;
		}
	};

	bool isDynamicWetnessAvailable = false;
};
