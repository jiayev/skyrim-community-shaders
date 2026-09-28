#pragma once

#include <d3d11.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <winrt/base.h>

/** @brief Discovers unified preset packs (Effects 11, CS Post Processing and Scene Manager payloads)
 *  plus orphan Effects 11 / CS Post Processing presets. */
class UnifiedPresetCatalog
{
public:
	/** @brief Single-choice backend filter for Query; Both requires Effects 11 and CS Post Processing. */
	enum class BackendFilter
	{
		All,
		Effects11,
		CSPP,
		Both,
		SceneManager
	};

	/** @brief Where a pack was found: a Presets pack folder, or a standalone Effects 11 / CSPP preset. */
	enum class SourceKind
	{
		UnifiedPack,
		Effects11Orphan,
		CSPPOrphan
	};

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
		bool hasCSPP = false;
		bool hasSceneManager = false;
		bool valid = true;
		std::string invalidReason;

		SourceKind source = SourceKind::UnifiedPack;
		std::filesystem::path rootPath;       ///< Pack folder, library root, or PP parent
		std::filesystem::path effects11Root;  ///< Folder with enbseries.ini + enbseries/
		std::filesystem::path csppFile;       ///< Absolute path to CSPP json

		std::filesystem::path logoPath;
		std::filesystem::path coverPath;
		std::vector<std::filesystem::path> screenshotPaths;

		winrt::com_ptr<ID3D11ShaderResourceView> logoSRV;
		winrt::com_ptr<ID3D11ShaderResourceView> coverSRV;
		std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> screenshotSRVs;
		bool artworkLoaded = false;
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

	/** @brief Indices of packs passing a backend filter and a case-insensitive name/author/description/tag search. */
	std::vector<size_t> Query(BackendFilter filter, const std::string& search) const;

	/** @brief Multi-select form: each enabled flag requires that backend; all false shows everything. */
	std::vector<size_t> Query(bool wantE11, bool wantCSPP, bool wantSM, const std::string& search) const;

	/** @brief Loads a pack's logo, cover and screenshots once; the cover stands in for a missing logo. */
	bool EnsureArtwork(PackInfo& pack);
	/** @brief Drops every pack's artwork textures. */
	void ReleaseAllArtwork();

	/** @brief Applies Effects11 and/or CSPP, then swaps the Scene Manager overwrite layer to this pack's scene files. */
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
	/** @brief Adds each PostProcessing .json as a CSPP-only pack. */
	void DiscoverCSPPOrphans();

	/** @brief Decodes an image file into an SRV; false when missing or undecodable. */
	static bool LoadTextureSRV(const std::filesystem::path& path, winrt::com_ptr<ID3D11ShaderResourceView>& outSRV);

	std::vector<PackInfo> packs;
	std::string activePackId;
};
