#pragma once

#include <d3d11.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <winrt/base.h>

/** @brief Discovers unified preset packs (Effects 11 and CS Presets payloads) plus orphan Effects 11 presets. */
class UnifiedPresetCatalog
{
public:
	/** @brief Where a pack was found: a Presets pack folder, a standalone Effects 11 preset, or the game root / Data install. */
	enum class SourceKind
	{
		UnifiedPack,
		Effects11Orphan,
		Effects11Legacy
	};

	/** @brief Pipeline a pack targets, declared by the manifest's "type" field. */
	enum class PresetType
	{
		CS,
		E11
	};

	static constexpr const char* kPresetTypeKey = "type";

	/** @brief The manifest spelling of a preset type. */
	static const char* GetPresetTypeName(PresetType type);

	/** @brief A discovered pack's manifest data, backend payloads and lazily loaded artwork. */
	struct PackInfo
	{
		std::string id;
		std::string name;
		std::string author;
		std::string version;
		std::string description;
		std::string nexusUrl;  ///< Optional "nexusUrl" manifest field; shown as a link when non-empty.
		std::vector<std::string> tags;

		bool hasEffects11 = false;
		bool hasCSPresets = false;
		/// Declared type; groups the pack, while the has* payload flags still decide what Apply loads.
		std::optional<PresetType> type;
		bool valid = true;
		std::string invalidReason;

		SourceKind source = SourceKind::UnifiedPack;
		std::filesystem::path rootPath;       ///< Pack folder or library root
		std::filesystem::path effects11Root;  ///< Folder with enbseries.ini + enbseries/

		std::filesystem::path logoPath;
		std::filesystem::path coverPath;
		std::vector<std::filesystem::path> screenshotPaths;

		winrt::com_ptr<ID3D11ShaderResourceView> logoSRV;
		winrt::com_ptr<ID3D11ShaderResourceView> coverSRV;
		std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> screenshotSRVs;
		bool artworkLoaded = false;

		/** @brief Grouped as E11: the declared type, else whether an Effects 11 payload was found. */
		bool IsE11() const { return type ? *type == PresetType::E11 : hasEffects11; }
		/** @brief Grouped as CS: the declared type, else whether scene files were found. */
		bool IsCS() const { return type ? *type == PresetType::CS : hasCSPresets; }
		/** @brief Grouped under the given type. */
		bool IsType(PresetType presetType) const { return presetType == PresetType::E11 ? IsE11() : IsCS(); }
	};

	static UnifiedPresetCatalog& GetSingleton();

	/** @brief Rescans every pack and orphan, sorted by name, releasing loaded artwork and reloading the active id. */
	void Discover();
	/** @brief Packs from the last Discover. */
	const std::vector<PackInfo>& GetPacks() const { return packs; }

	/** @brief The applied pack's id, empty when none. */
	const std::string& GetActivePackId() const { return activePackId; }
	/** @brief Sets and persists the active pack id. */
	void SetActivePackId(const std::string& id);

	/** @brief Folder of the active unified pack; empty when none is active or the active preset is an orphan. */
	std::filesystem::path GetActivePackRoot() const;

	/** @brief Resolves a relative path against the game directory first, then the active pack root.
	 *  @return The first existing candidate, or the path unchanged when neither exists. */
	std::filesystem::path ResolveActivePackPath(const std::filesystem::path& path) const;

	/** @brief The pack with this id, or null. */
	PackInfo* FindPack(const std::string& id);
	const PackInfo* FindPack(const std::string& id) const;

	/** @brief Indices of packs of the given type (any when null) matching a case-insensitive name/author/description/tag search. */
	std::vector<size_t> Query(std::optional<PresetType> typeFilter, const std::string& search) const;

	/** @brief Loads a pack's logo, cover and screenshots once; the cover stands in for a missing logo. */
	bool EnsureArtwork(PackInfo& pack);
	/** @brief Drops every pack's artwork textures. */
	void ReleaseAllArtwork();

	/** @brief Applies Effects11, then swaps the Scene Manager overwrite layer to this pack's CS Presets files. */
	bool ApplyPack(const std::string& id, bool saveEffects11Current = true);

	/** @brief Creates the presets root if needed and opens its real path in Explorer. */
	bool OpenPresetsFolder() const;
	/** @brief Opens a pack's folder in Explorer, preferring its physical copy over the MO2 VFS path. */
	bool OpenPackFolder(const std::string& id) const;

	/** @brief In-game / VFS path to the unified presets root. */
	std::filesystem::path GetPresetsRoot() const;
	/** @brief On-disk presets root under the CS mod root, else GetPresetsRoot. */
	std::filesystem::path GetPresetsRealPath() const;

	/** @brief A pack's manifest file: `<PackId>.json`, else the legacy `preset.json` when only that exists. */
	static std::filesystem::path GetPackManifestPath(const std::filesystem::path& packRoot);

	/** @brief Parses a pack's manifest.
	 *  @param outError Receives the parse error when the manifest exists but is invalid.
	 *  @return The manifest object, or an empty object when absent or invalid. */
	static nlohmann::json ReadPackManifest(const std::filesystem::path& packRoot, std::string* outError = nullptr);

	/** @brief Folder holding a pack's enbseries.ini + enbseries/: the manifest's `effects11.path`, then `effects11/`,
	 *  then the pack root. Empty when none validates or the manifest sets `backends.effects11` to false. */
	static std::filesystem::path ResolveEffects11Root(const std::filesystem::path& packRoot, const nlohmann::json& manifest);

private:
	UnifiedPresetCatalog() { LoadActiveState(); }

	/** @brief Reads the persisted active pack id, clearing it when the state file is missing or bad. */
	void LoadActiveState();
	/** @brief Writes the active pack id next to the presets. */
	void SaveActiveState() const;

	/** @brief Adds each folder under Presets as a pack, read from its manifest. */
	void DiscoverUnifiedPacks();
	/** @brief Adds Effects 11 library presets that no unified pack of the same name already covers. */
	void DiscoverEffects11Orphans();
	/** @brief Adds the Effects 11 install at the game root or Data folder, when present. */
	void DiscoverEffects11Legacy();
	/** @brief With no pack applied, marks the Legacy or orphan pack Effects 11 is already running as active. */
	void AdoptEffects11ActivePack();

	/** @brief Decodes an image file into an SRV; false when missing or undecodable. */
	static bool LoadTextureSRV(const std::filesystem::path& path, winrt::com_ptr<ID3D11ShaderResourceView>& outSRV);

	std::vector<PackInfo> packs;
	std::string activePackId;
};
