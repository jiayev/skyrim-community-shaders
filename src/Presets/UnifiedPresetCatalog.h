#pragma once

#include <d3d11.h>
#include <filesystem>
#include <string>
#include <vector>
#include <winrt/base.h>

/**
 * @brief Discovers unified preset packs, orphan Effects 11 / CS Post Processing presets,
 * and Scene Manager exports under SceneSettings/ (presetMetadata + overwrite files).
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
		CSPPOrphan,
		SceneManager
	};

	struct PackInfo
	{
		std::string id;
		std::string name;
		std::string author;
		std::string version;
		std::string description;
		std::vector<std::string> tags;

		bool hasEffects11 = false;
		bool hasCSPP = false;
		bool hasSceneManager = false;
		bool valid = true;
		std::string invalidReason;

		SourceKind source = SourceKind::UnifiedPack;
		std::filesystem::path rootPath;           ///< Pack folder, library root, PP parent, or SceneSettings root
		std::filesystem::path effects11Root;      ///< Folder with enbseries.ini + enbseries/
		std::filesystem::path csppFile;           ///< Absolute path to CSPP json
		std::filesystem::path sceneMetadataPath;  ///< SceneSettings/<Name>.json when SourceKind::SceneManager

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

	/** Apply Effects11 and/or CSPP; Scene Manager packs reload live overwrites. */
	bool ApplyPack(const std::string& id, bool saveEffects11Current = true);

	bool OpenPresetsFolder() const;
	bool OpenPackFolder(const std::string& id) const;

	std::filesystem::path GetPresetsRoot() const;
	std::filesystem::path GetPresetsRealPath() const;

private:
	void LoadActiveState();
	void SaveActiveState() const;

	void DiscoverUnifiedPacks();
	void DiscoverEffects11Orphans();
	void DiscoverCSPPOrphans();
	void DiscoverSceneManagerPresets();

	static bool LoadTextureSRV(const std::filesystem::path& path, winrt::com_ptr<ID3D11ShaderResourceView>& outSRV);

	std::vector<PackInfo> packs;
	std::string activePackId;
};
