#pragma once

#include <d3d11.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <winrt/base.h>

/**
 * @brief Discovers unified preset packs (Effects 11, CS Post Processing and Scene Manager payloads)
 * plus orphan Effects 11 / CS Post Processing presets.
 */
class UnifiedPresetCatalog
{
public:
	enum class BackendFilter
	{
		All,
		Effects11,
		CSPP,
		Both,
		SceneManager
	};

	enum class SourceKind
	{
		UnifiedPack,
		Effects11Orphan,
		CSPPOrphan
	};

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

	void Discover();
	const std::vector<PackInfo>& GetPacks() const { return packs; }

	const std::string& GetActivePackId() const { return activePackId; }
	void SetActivePackId(const std::string& id);

	/** @brief Folder of the active unified pack; empty when none is active or the active preset is an orphan. */
	std::filesystem::path GetActivePackRoot() const;

	PackInfo* FindPack(const std::string& id);
	const PackInfo* FindPack(const std::string& id) const;

	/** Filter + case-insensitive substring match on name/author/tags. */
	std::vector<size_t> Query(BackendFilter filter, const std::string& search) const;

	/**
	 * Multi-select filter: each enabled flag requires that backend.
	 * All flags false = show everything.
	 */
	std::vector<size_t> Query(bool wantE11, bool wantCSPP, bool wantSM, const std::string& search) const;

	bool EnsureArtwork(PackInfo& pack);
	void ReleaseAllArtwork();

	/** Apply Effects11 and/or CSPP, then swap the Scene Manager overwrite layer to this pack's scene files. */
	bool ApplyPack(const std::string& id, bool saveEffects11Current = true);

	bool OpenPresetsFolder() const;
	bool OpenPackFolder(const std::string& id) const;

	std::filesystem::path GetPresetsRoot() const;
	std::filesystem::path GetPresetsRealPath() const;

	/** @brief A pack's manifest file: `<PackId>.json`, else the legacy `preset.json` when only that exists. */
	static std::filesystem::path GetPackManifestPath(const std::filesystem::path& packRoot);

	/**
	 * @brief Parses a pack's manifest.
	 * @param outError Receives the parse error when the manifest exists but is invalid.
	 * @return The manifest object, or an empty object when absent or invalid.
	 */
	static nlohmann::json ReadPackManifest(const std::filesystem::path& packRoot, std::string* outError = nullptr);

	/**
	 * @brief Folder holding a pack's enbseries.ini + enbseries/: the manifest's `effects11.path`, then `effects11/`,
	 * then the pack root. Empty when none validates or the manifest sets `backends.effects11` to false.
	 */
	static std::filesystem::path ResolveEffects11Root(const std::filesystem::path& packRoot, const nlohmann::json& manifest);

private:
	UnifiedPresetCatalog() { LoadActiveState(); }

	void LoadActiveState();
	void SaveActiveState() const;

	void DiscoverUnifiedPacks();
	void DiscoverEffects11Orphans();
	void DiscoverCSPPOrphans();

	static bool LoadTextureSRV(const std::filesystem::path& path, winrt::com_ptr<ID3D11ShaderResourceView>& outSRV);

	std::vector<PackInfo> packs;
	std::string activePackId;
};
