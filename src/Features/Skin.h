#pragma once

#include "I18n/I18n.h"
#include "Skin/Editor/Files.h"
#include "Skin/Editor/NifDocument.h"
#include "Skin/Editor/TextureTools.h"
#include "Skin/Runtime/ActorTargets.h"
#include "Skin/Runtime/Textures.h"

#include <mutex>

struct Skin : Feature
{
	static Skin* GetSingleton()
	{
		static Skin singleton;
		return &singleton;
	}
	std::string GetName() override { return "Advanced Skin"; }
	std::string GetDisplayName() override { return T("feature.skin.name", "Advanced Skin"); }
	std::string GetShortName() override { return "Skin"; }
	std::string_view GetShaderDefineName() override { return "CS_SKIN"; }
	std::string_view GetCategory() const override { return FeatureCategories::kCharacters; }
	std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.skin.skin_materials_with_character_customization", "Skin materials authored in NIF assets, with optional player and NPC customization."),
			{ T("feature.skin.roughness_fuzz_and_reflectance_controls", "Roughness, fuzz and reflectance controls"), T("feature.skin.detail_normals_with_a_main_uv_region_mask", "Detail normals with a main-UV region mask"),
				T("feature.skin.independent_transmission_and_post_process_sss_strength", "Independent transmission and post-process SSS strength"), T("feature.skin.nif_editing_preview_and_character_persistence", "NIF editing, preview and character persistence") } };
	}
	bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }
	void RestoreDefaultSettings() override;
	void DrawSettings() override;
	void LoadSettings(json& a_json) override;
	void SaveSettings(json& a_json) override;
	void Prepass() override;
	void PostPostLoad() override;
	void SetupResources() override;
	void DataLoaded() override;
	void GameReset() override;
	void DrawMaterialEditor();
	void SetShaderResources(ID3D11DeviceContext* a_context);
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
	void RestoreSampler();

	struct Settings
	{
		bool EnableSkin = true;
		SkinMaterials::Parameters DefaultProfile;
		std::string DetailTexture = "Data/Shaders/Skin/skin_detail_n.dds";
		float ExtraSkinWetness = 0.0f;
		float WetFadeTime = 10.0f;
		float StartSweat = 0.75f;
		float FullSweat = 0.15f;
		float4 WetParams{ 512.0f, 0.7f, 10.0f, 4.0f };
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
	SkinData GetCommonBufferData();

private:
	struct TextureEntry
	{
		SkinTextures::Resource resource;
		bool prepared = false;
	};
	struct GeometryEntry
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiPointer<RE::BSShaderProperty> property;
		RE::NiPointer<RE::BSTextureSet> textureSet;
		RE::NiSourceTexturePtr normal;
		RE::NiSourceTexturePtr diffuse;
		const RE::BSShaderMaterial* materialPointer = nullptr;
		RE::NiAVObject* parent = nullptr;
		RE::BSGraphics::TriShape* buffer = nullptr;
		RE::NiSkinPartition* partition = nullptr;
		std::array<float, 4> uvTransform{};
		bool modelSpace = false;
		SkinSources::Snapshot source;
		SkinMaterials::Material base;
		SkinMaterials::Material effective;
		SkinActors::Target target;
		RE::ActorHandle actor;
		std::array<std::shared_ptr<TextureEntry>, 3> textures;
		std::string diagnostic;
		uint32_t frame = 0;
		RE::FormID race = 0;
		uint32_t sex = 0;
		uint64_t revision = 0;
		uint64_t actorRevision = 0;
		bool ready = false;
		bool inspect = false;
		bool transferPreview = false;
		bool head = false;
		bool overridden = false;
		bool thickness = false;
	};
	struct ActorWetnessCacheEntry
	{
		float4 wetness{ 0, 0, 0, 0 };
		uint32_t frameCount = 0;
	};
	struct Editor
	{
		enum class Workspace
		{
			Asset,
			Character,
			Textures,
			Storage
		};
		Workspace workspace = Workspace::Asset;
		int step = 0;
		int materialSection = 0;
		struct BatchItem
		{
			SkinEditor::NifDocument document;
			uint32_t block = UINT32_MAX;
			std::filesystem::path output;
		};
		struct TransferRow
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			SkinActors::Part part = SkinActors::Part::Face;
			SkinActors::Guard guard;
			int sample = -1;
			bool include = true;
			bool confirmed = false;
			std::string error;
		};
		struct Transfer
		{
			SkinActors::Scheme scheme;
			RE::ActorHandle actor;
			std::string key;
			uint64_t revision = 0, session = 0;
			std::vector<TransferRow> rows;
			bool preview = false;
		};
		std::optional<Transfer> transfer;
		std::optional<SkinActors::Scheme> clipboard;
		RE::NiPointer<RE::BSGeometry> geometry;
		std::vector<RE::NiPointer<RE::BSGeometry>> surfaces;
		std::vector<RE::NiPointer<RE::BSGeometry>> targets;
		std::optional<SkinEditor::NifDocument> document;
		std::vector<SkinEditor::NifDocument::Surface> materials;
		uint32_t selectedBlock = UINT32_MAX;
		SkinMaterials::Material draft;
		SkinMaterials::Material baseline;
		SkinMaterials::Changes changes;
		SkinMaterials::Changes changesBaseline;
		std::vector<SkinMaterials::Material> undo;
		std::vector<SkinMaterials::Material> redo;
		std::vector<SkinMaterials::Changes> characterUndo;
		std::vector<SkinMaterials::Changes> characterRedo;
		std::vector<BatchItem> batch;
		std::vector<SkinEditor::Output> package;
		std::optional<SkinEditor::NifDocument> packageDocument;
		SkinEditor::TextureTools tools;
		SkinTextures::Resource thumbnail;
		std::string thumbnailKey;
		std::string generatedDDS;
		bool generatedDetail = false;
		std::optional<SkinMaterials::Material> imported;
		std::vector<std::pair<std::string, SkinMaterials::Material>> legacy;
		std::array<bool, SkinMaterials::ParameterCount> importParameters{};
		std::array<bool, 3> importTextures{};
		std::string sourceInput;
		std::string message;
		std::string error;
		bool character = false;
		bool preview = false;
		bool discardConfirmed = false;
		std::function<void()> pendingAction;
		std::function<void()> pendingSave;
		std::optional<SkinActors::SaveConflict> saveConflict;
		bool confirmSave = false;
		bool editGesture = false;
		float previewWetness = -1.0f;
		int channel = 0;
		int texturePreview = 0;
	};
	std::recursive_mutex mutex;
	std::unordered_map<RE::BSGeometry*, GeometryEntry> geometries;
	std::unordered_map<std::string, std::shared_ptr<TextureEntry>> textures;
	std::unordered_map<std::string, uint64_t> resourceRevisions;
	std::unordered_map<uint32_t, ActorWetnessCacheEntry> actorWetnessMap;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 3> pendingViews;
	winrt::com_ptr<ID3D11SamplerState> detailSampler;
	winrt::com_ptr<ID3D11SamplerState> savedSampler;
	bool samplerBound = false;
	eastl::unique_ptr<ConstantBuffer> PerGeometryCB;
	Editor editor;
	uint64_t revision = 1;
	uint64_t session = 0;
	bool texturesDirty = false;
	uint32_t lastPrune = 0;

	void Invalidate(bool a_resources = false);
	std::shared_ptr<TextureEntry> RequestTexture(const std::string& a_path);
	void Prepare(GeometryEntry& a_entry, const SkinActors::Snapshot& a_state);
	SkinData MakeData(const SkinMaterials::Material& a_material, bool a_head) const;
	GeometryEntry& Observe(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property);
	void UpdateEditorPreview();
	void DrawCharacterStorageStatus();
	bool RunCharacterSave(std::function<void()> a_action);
	void SaveCharacterDraft();
	bool PreviewMatches(const GeometryEntry& a_entry) const;
	void SelectReference(RE::TESObjectREFR* a_reference, bool a_firstPerson = false, bool a_previewOnly = false);
	void SelectSurface(RE::BSGeometry* a_geometry);
	void OpenDocument(const std::string& a_path, uint32_t a_block = UINT32_MAX);
	void SelectMaterial(uint32_t a_block);
	void DrawMaterialControls();
	void DrawEditorTarget();
	void DrawEditorPreview();
	void DrawEditorSurfacePicker(bool a_previewOnly);
	void DrawEditorSave();
	void DrawEditorHistory();
	void DrawEditorDialogs();
	void DrawCharacterTargets();
	void RefreshCharacterDraft();
	void DrawAssetActions();
	void DrawCharacterActions();
	void DrawCharacterStorage();
	void DrawCharacterSchemeActions();
	void DrawCharacterTransfer();
	std::vector<RE::NiPointer<RE::BSGeometry>> DiscoverActor(RE::Actor* a_actor);
	SkinActors::Scheme CaptureScheme(RE::Actor* a_actor);
	void BeginTransfer(SkinActors::Scheme a_scheme, RE::Actor* a_actor, bool a_apply);
	void CommitTransfer();
	const SkinActors::Sample* TransferSample(const GeometryEntry& a_entry) const;
	void DrawTextureTools();
	void DrawBatch();
	void DrawPresets();
	void DrawGlobalSettings();
	bool EditorDirty() const;
	void ChangeEditor(std::function<void()> a_action);
	float GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos);
	float4 GetWetness(RE::BSGeometry* a_geometry);
	struct Hooks
	{
		struct SetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags);
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct RestoreGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags);
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};
};
