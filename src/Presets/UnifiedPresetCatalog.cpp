#include "UnifiedPresetCatalog.h"
#include "PCH.h"

#include "Feature.h"
#include "Features/Effects11.h"
#include "Features/Effects11/PresetManager.h"
#include "Globals.h"
#include "CSEditor/SceneManager/SceneManager.h"
#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "I18n/I18n.h"
#include "PostProcessingMode.h"
#include "Presets/PresetCompatibility.h"
#include "SettingsOverrideManager.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/UI.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <vector>
#include <Windows.h>

using json = nlohmann::json;

namespace
{
	constexpr intptr_t kShellExecuteSuccessThreshold = 32;
	constexpr const char* kActiveStateFileName = "_active.json";
	constexpr const char* kBaselinePackIdsKey = "baselinePackIds";
	constexpr const char* kLegacyMetaFileName = "preset.json";
	constexpr const char* kEffects11PackSubdir = UnifiedPresetCatalog::kEffects11PackSubdir;
	// The colon keeps it out of the pack folder namespace, like orphan "e11:" ids.
	constexpr const char* kEffects11LegacyPackId = "legacy:effects11";
	constexpr std::string_view kEffects11OrphanPrefix = "e11:";

	using Util::ToLower;

	/** @brief The Effects 11 preset id a pack switches to. */
	std::string GetEffects11PresetId(const UnifiedPresetCatalog::PackInfo& pack)
	{
		using SourceKind = UnifiedPresetCatalog::SourceKind;
		if (pack.source == SourceKind::UnifiedPack)
			return PresetManager::MakeUnifiedPackPresetId(pack.id);
		if (pack.source == SourceKind::Effects11Orphan && pack.id.starts_with(kEffects11OrphanPrefix))
			return pack.id.substr(kEffects11OrphanPrefix.size());
		if (pack.source == SourceKind::Effects11Legacy)
			return PresetManager::kLegacyPresetId;
		return pack.id;
	}

	/** @brief Case-insensitive substring match; the needle must already be lowercase. */
	bool ContainsCI(const std::string& haystack, const std::string& needleLower)
	{
		if (needleLower.empty())
			return true;
		return ToLower(haystack).find(needleLower) != std::string::npos;
	}

	std::filesystem::path ResolveRelative(const std::filesystem::path& root, const std::string& rel)
	{
		if (rel.empty())
			return {};
		auto path = root / rel;
		if (std::filesystem::exists(path))
			return path;
		return {};
	}

	/** @brief The manifest's declared preset type, matched case-insensitively; null when absent or unrecognized. */
	std::optional<UnifiedPresetCatalog::PresetType> ReadPresetType(const json& manifest, const std::string& packId)
	{
		using PresetType = UnifiedPresetCatalog::PresetType;
		const auto it = manifest.find(UnifiedPresetCatalog::kPresetTypeKey);
		if (it == manifest.end())
			return std::nullopt;
		if (it->is_string()) {
			for (auto type : { PresetType::CS, PresetType::E11, PresetType::Baseline })
				if (Util::IEquals(it->get_ref<const std::string&>(), UnifiedPresetCatalog::GetPresetTypeName(type)))
					return type;
		}
		logger::warn("[Presets] Pack '{}' has unknown type '{}'; grouping by detected payloads", packId, it->dump());
		return std::nullopt;
	}

	/** @brief Whether an id from persisted state is a plain pack folder name, never a path or an orphan id. */
	bool IsPackFolderId(const std::string& id)
	{
		return !id.empty() && id != "." && id != ".." && id.find_first_of("/\\:") == std::string::npos;
	}

	/** @brief Feature short names set by a pack's Baseline folder, sorted and de-duplicated. */
	std::vector<std::string> ListBaselineFeatures(const std::filesystem::path& packRoot)
	{
		std::vector<std::string> features;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(packRoot / UnifiedPresetCatalog::kBaselineSubdir, ec)) {
			std::error_code typeEc;
			if (!entry.is_regular_file(typeEc))
				continue;
			auto name = SettingsOverrideManager::ParseBaselineFeatureName(entry.path());
			if (!name.empty())
				features.push_back(std::move(name));
		}
		std::ranges::sort(features);
		features.erase(std::unique(features.begin(), features.end()), features.end());
		return features;
	}

	bool IsImageExtension(const std::filesystem::path& path)
	{
		const auto ext = ToLower(path.extension().string());
		return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp" || ext == ".dds";
	}

	std::string InferDescriptionFromReadme(const std::filesystem::path& packRoot)
	{
		static constexpr const char* kReadmeNames[] = {
			"ReadMe.txt", "README.txt", "readme.txt", "ReadMe.md", "README.md", "readme.md"
		};
		for (const char* name : kReadmeNames) {
			const auto path = packRoot / name;
			std::ifstream in(path);
			if (!in)
				continue;
			std::string line;
			std::string result;
			while (std::getline(in, line)) {
				// Trim trailing CR from Windows text files.
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty()) {
					if (!result.empty())
						break;
					continue;
				}
				if (!result.empty())
					result += ' ';
				result += line;
				if (result.size() > 400)
					break;
			}
			if (!result.empty()) {
				if (result.size() > 400)
					result.resize(397), result += "...";
				return result;
			}
		}
		return {};
	}

	/** @brief Fill unset logo, cover and screenshots from conventional file and folder names in the pack root. */
	void InferMissingArtwork(UnifiedPresetCatalog::PackInfo& pack)
	{
		static constexpr const char* kLogoCandidates[] = {
			"logo.png", "logo.jpg", "logo.jpeg", "logo.webp", "logo.dds",
			"preview.png", "preview.jpg", "thumbnail.png", "thumbnail.jpg", "icon.png"
		};
		static constexpr const char* kCoverCandidates[] = {
			"cover.png", "cover.jpg", "cover.jpeg", "cover.webp", "cover.dds",
			"banner.png", "banner.jpg", "header.png", "header.jpg"
		};
		static constexpr const char* kShotDirs[] = {
			"gallery", "screenshots", "Screenshots", "images", "Images", "previews", "Previews"
		};

		if (pack.logoPath.empty()) {
			for (const char* name : kLogoCandidates) {
				pack.logoPath = ResolveRelative(pack.rootPath, name);
				if (!pack.logoPath.empty())
					break;
			}
		}
		if (pack.coverPath.empty()) {
			for (const char* name : kCoverCandidates) {
				pack.coverPath = ResolveRelative(pack.rootPath, name);
				if (!pack.coverPath.empty())
					break;
			}
		}

		if (!pack.screenshotPaths.empty())
			return;

		std::error_code ec;
		for (const char* dirName : kShotDirs) {
			const auto dir = pack.rootPath / dirName;
			if (!std::filesystem::is_directory(dir, ec))
				continue;
			for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
				if (ec || !file.is_regular_file() || !IsImageExtension(file.path()))
					continue;
				pack.screenshotPaths.push_back(file.path());
			}
		}

		// Loose images at pack root (skip logo/cover already chosen).
		for (const auto& file : std::filesystem::directory_iterator(pack.rootPath, ec)) {
			if (ec || !file.is_regular_file() || !IsImageExtension(file.path()))
				continue;
			const auto path = file.path();
			if ((!pack.logoPath.empty() && path == pack.logoPath) ||
				(!pack.coverPath.empty() && path == pack.coverPath))
				continue;
			pack.screenshotPaths.push_back(path);
		}

		std::sort(pack.screenshotPaths.begin(), pack.screenshotPaths.end());
		constexpr size_t kMaxShots = 12;
		if (pack.screenshotPaths.size() > kMaxShots)
			pack.screenshotPaths.resize(kMaxShots);
	}
}

UnifiedPresetCatalog& UnifiedPresetCatalog::GetSingleton()
{
	static UnifiedPresetCatalog instance;
	return instance;
}

std::filesystem::path UnifiedPresetCatalog::GetPresetsRoot() const
{
	return Util::PathHelpers::GetUnifiedPresetsPath();
}

std::filesystem::path UnifiedPresetCatalog::GetPresetsRealPath() const
{
	if (!Util::PathHelpers::GetRootRealPath().empty())
		return Util::PathHelpers::GetUnifiedPresetsRealPath();
	return GetPresetsRoot();
}

std::filesystem::path UnifiedPresetCatalog::GetPackManifestPath(const std::filesystem::path& packRoot)
{
	auto manifestPath = packRoot / packRoot.filename();
	manifestPath += ".json";
	std::error_code ec;
	if (!std::filesystem::exists(manifestPath, ec) && std::filesystem::exists(packRoot / kLegacyMetaFileName, ec))
		return packRoot / kLegacyMetaFileName;
	return manifestPath;
}

const char* UnifiedPresetCatalog::GetPresetTypeName(PresetType type)
{
	switch (type) {
	case PresetType::E11:
		return "E11";
	case PresetType::Baseline:
		return "Baseline";
	default:
		return "CS";
	}
}

json UnifiedPresetCatalog::ReadPackManifest(const std::filesystem::path& packRoot, std::string* outError)
{
	const auto manifestPath = GetPackManifestPath(packRoot);
	std::ifstream in(manifestPath);
	if (!in)
		return json::object();

	std::string error;
	try {
		auto manifest = json::parse(in);
		if (manifest.is_object())
			return manifest;
		error = "not a JSON object";
	} catch (const json::exception& e) {
		error = e.what();
	}
	if (outError)
		*outError = std::format("Invalid {}: {}", manifestPath.filename().string(), error);
	return json::object();
}

std::filesystem::path UnifiedPresetCatalog::ResolveEffects11Root(const std::filesystem::path& packRoot, const json& manifest)
{
	assert(manifest.is_object());
	if (const auto backends = manifest.find("backends"); backends != manifest.end() && backends->is_object()) {
		if (const auto enabled = backends->find("effects11");
			enabled != backends->end() && enabled->is_boolean() && !enabled->get<bool>())
			return {};
	}

	std::filesystem::path preferredRoot;
	if (const auto effects11 = manifest.find("effects11"); effects11 != manifest.end() && effects11->is_object()) {
		if (const auto path = effects11->find("path"); path != effects11->end() && path->is_string())
			preferredRoot = packRoot / path->get<std::string>();
	}

	// Classic ENB drops (NAT.ENB-style) put enbseries at the pack root instead of effects11/.
	for (const auto& candidate : { preferredRoot, packRoot / kEffects11PackSubdir, packRoot }) {
		std::string reason;
		if (!candidate.empty() && PresetManager::ValidateLibraryPreset(candidate, reason))
			return candidate;
	}
	return {};
}

std::filesystem::path UnifiedPresetCatalog::GetActivePackRoot() const
{
	// Orphan ids carry a "source:" prefix, which no pack folder name can contain.
	if (activePackId.empty() || activePackId.contains(':'))
		return {};
	return Util::PathHelpers::GetUnifiedPackPath(activePackId);
}

std::filesystem::path UnifiedPresetCatalog::ResolveActivePackPath(const std::filesystem::path& path) const
{
	std::error_code ec;
	if (path.empty() || path.is_absolute() || std::filesystem::exists(path, ec))
		return path;
	const auto packRoot = GetActivePackRoot();
	if (auto packPath = packRoot / path; !packRoot.empty() && std::filesystem::exists(packPath, ec))
		return packPath;
	return path;
}

void UnifiedPresetCatalog::LoadActiveState()
{
	activePackId.clear();
	baselinePackIds.clear();
	const auto path = GetPresetsRealPath() / kActiveStateFileName;
	std::ifstream in(path);
	if (!in)
		return;
	try {
		json j;
		in >> j;
		activePackId = j.value("activePackId", "");
		if (const auto ids = j.find(kBaselinePackIdsKey); ids != j.end() && ids->is_array()) {
			for (const auto& id : *ids) {
				if (id.is_string() && IsPackFolderId(id.get_ref<const std::string&>()) && !IsBaselineEnabled(id.get<std::string>()))
					baselinePackIds.push_back(id.get<std::string>());
			}
		}
	} catch (...) {
		activePackId.clear();
		baselinePackIds.clear();
	}
}

void UnifiedPresetCatalog::SaveActiveState() const
{
	Util::FileHelpers::EnsureDirectoryExists(GetPresetsRealPath());
	const auto path = GetPresetsRealPath() / kActiveStateFileName;
	try {
		json j;
		j["activePackId"] = activePackId;
		j[kBaselinePackIdsKey] = baselinePackIds;
		if (!Util::FileHelpers::WriteJsonAtomically(path, j, 4, "active pack state"))
			logger::warn("[Presets] Failed to save active pack state to {}", path.string());
	} catch (const std::exception& e) {
		logger::warn("[Presets] Failed to save active pack state: {}", e.what());
	}
}

void UnifiedPresetCatalog::SetActivePackId(const std::string& id)
{
	activePackId = id;
	SaveActiveState();
}

UnifiedPresetCatalog::PackInfo* UnifiedPresetCatalog::FindPack(const std::string& id)
{
	for (auto& pack : packs) {
		if (pack.id == id)
			return &pack;
	}
	return nullptr;
}

const UnifiedPresetCatalog::PackInfo* UnifiedPresetCatalog::FindPack(const std::string& id) const
{
	for (const auto& pack : packs) {
		if (pack.id == id)
			return &pack;
	}
	return nullptr;
}

void UnifiedPresetCatalog::DiscoverUnifiedPacks()
{
	for (const auto& packRoot : Util::PathHelpers::ListCommunityShaderEntries(Util::PathHelpers::kUnifiedPresetsSubdir, true)) {
		const auto packId = packRoot.filename().string();

		PackInfo pack;
		pack.id = packId;
		pack.name = packId;
		pack.source = SourceKind::UnifiedPack;
		pack.rootPath = packRoot;

		const auto meta = ReadPackManifest(packRoot, &pack.invalidReason);
		pack.valid = pack.invalidReason.empty();
		try {
			pack.name = meta.value("name", packId);
			pack.author = meta.value("author", "");
			pack.version = meta.value("version", "");
			pack.description = meta.value("description", "");
			pack.nexusUrl = meta.value("nexusUrl", "");
			pack.csVersion = meta.value("csVersion", "");
			pack.type = ReadPresetType(meta, packId);
			if (meta.contains("tags") && meta["tags"].is_array()) {
				for (const auto& tag : meta["tags"]) {
					if (tag.is_string())
						pack.tags.push_back(tag.get<std::string>());
				}
			}
			if (meta.contains("requiredFeatures") && meta["requiredFeatures"].is_array()) {
				for (const auto& feature : meta["requiredFeatures"]) {
					if (feature.is_string())
						pack.requiredFeatures.push_back(feature.get<std::string>());
				}
			}

			if (meta.contains("logo") && meta["logo"].is_string())
				pack.logoPath = ResolveRelative(packRoot, meta["logo"].get<std::string>());
			if (meta.contains("cover") && meta["cover"].is_string())
				pack.coverPath = ResolveRelative(packRoot, meta["cover"].get<std::string>());
			if (meta.contains("screenshots") && meta["screenshots"].is_array()) {
				for (const auto& shot : meta["screenshots"]) {
					if (!shot.is_string())
						continue;
					auto path = ResolveRelative(packRoot, shot.get<std::string>());
					if (!path.empty())
						pack.screenshotPaths.push_back(std::move(path));
				}
			}
		} catch (const std::exception& e) {
			pack.valid = false;
			pack.invalidReason = std::format("Invalid {}: {}", GetPackManifestPath(packRoot).filename().string(), e.what());
		}

		pack.effects11Root = ResolveEffects11Root(packRoot, meta);
		pack.hasEffects11 = !pack.effects11Root.empty();
		pack.hasCSPresets = SceneSettingsManager::HasScenePayload(packRoot);
		pack.baselineFeatures = ListBaselineFeatures(packRoot);
		pack.hasBaseline = !pack.baselineFeatures.empty();

		if (pack.description.empty())
			pack.description = InferDescriptionFromReadme(packRoot);
		InferMissingArtwork(pack);

		if (!pack.hasEffects11 && !pack.hasCSPresets && !pack.hasBaseline) {
			pack.valid = false;
			if (pack.invalidReason.empty())
				pack.invalidReason = "No Effects11, CS Presets or Baseline payload found";
		}

		packs.push_back(std::move(pack));
	}
}

void UnifiedPresetCatalog::DiscoverEffects11Orphans()
{
	for (const auto& presetRoot : Util::PathHelpers::ListCommunityShaderEntries(Util::PathHelpers::kEffects11PresetsSubdir, true)) {
		const auto name = presetRoot.filename().string();

		// Skip if already covered by a unified pack with the same folder name
		if (FindPack(name))
			continue;

		PackInfo pack;
		pack.id = std::string(kEffects11OrphanPrefix) + name;
		pack.name = name;
		pack.source = SourceKind::Effects11Orphan;
		pack.rootPath = presetRoot;
		pack.effects11Root = presetRoot;
		pack.hasEffects11 = std::filesystem::exists(presetRoot / PresetManager::kEnbSeriesIniName) &&
							std::filesystem::is_directory(presetRoot / PresetManager::kEnbSeriesDirName);
		if (!pack.hasEffects11) {
			pack.valid = false;
			pack.invalidReason = "Missing enbseries.ini / enbseries";
		}
		pack.description = "Effects11 library preset";
		packs.push_back(std::move(pack));
	}
}

void UnifiedPresetCatalog::DiscoverEffects11Legacy()
{
	const auto& presetManager = PresetManager::GetSingleton();
	if (!presetManager.HasLegacyInstall())
		return;

	PackInfo pack;
	pack.id = kEffects11LegacyPackId;
	pack.name = T("menu.presets.legacy_name", "Effects 11 Preset (Legacy)");
	pack.source = SourceKind::Effects11Legacy;
	pack.rootPath = presetManager.GetLegacyENBSeriesIniPath().parent_path();
	pack.effects11Root = pack.rootPath;
	pack.hasEffects11 = true;
	pack.description = T("menu.presets.legacy_description", "Effects 11 preset installed in the game root or Data folder.");
	packs.push_back(std::move(pack));
}

void UnifiedPresetCatalog::AdoptEffects11ActivePack()
{
	if (!activePackId.empty() || !globals::features::effects11.loaded)
		return;
	const auto& e11Id = PresetManager::GetSingleton().GetActivePresetId();
	const auto match = std::ranges::find_if(packs, [&](const PackInfo& pack) {
		return pack.source != SourceKind::UnifiedPack && pack.valid && GetEffects11PresetId(pack) == e11Id;
	});
	if (match != packs.end())
		activePackId = match->id;
}

void UnifiedPresetCatalog::Discover()
{
	ReleaseAllArtwork();
	packs.clear();
	Util::FileHelpers::EnsureDirectoryExists(GetPresetsRealPath());
	LoadActiveState();

	DiscoverUnifiedPacks();
	DiscoverEffects11Orphans();
	DiscoverEffects11Legacy();
	AdoptEffects11ActivePack();

	std::sort(packs.begin(), packs.end(), [](const PackInfo& a, const PackInfo& b) {
		return ToLower(a.name) < ToLower(b.name);
	});

	logger::info("[Presets] Discovered {} pack(s); active='{}'", packs.size(), activePackId.empty() ? "(none)" : activePackId);
}

std::vector<size_t> UnifiedPresetCatalog::Query(std::optional<PresetType> typeFilter, const std::string& search) const
{
	const auto needle = ToLower(search);
	std::vector<size_t> indices;
	indices.reserve(packs.size());

	for (size_t i = 0; i < packs.size(); ++i) {
		const auto& pack = packs[i];
		if (typeFilter && !pack.IsType(*typeFilter))
			continue;

		if (!needle.empty()) {
			bool match = ContainsCI(pack.name, needle) || ContainsCI(pack.author, needle) || ContainsCI(pack.description, needle);
			if (!match) {
				for (const auto& tag : pack.tags) {
					if (ContainsCI(tag, needle)) {
						match = true;
						break;
					}
				}
			}
			if (!match)
				continue;
		}

		indices.push_back(i);
	}
	return indices;
}

bool UnifiedPresetCatalog::LoadTextureSRV(const std::filesystem::path& path, winrt::com_ptr<ID3D11ShaderResourceView>& outSRV)
{
	outSRV = nullptr;
	if (path.empty() || !std::filesystem::exists(path) || !globals::d3d::device)
		return false;

	// Read via filesystem::path so MO2 / Unicode paths work; stbi_load(char*) uses fopen and can fail.
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		logger::warn("[Presets] Failed to open artwork '{}'", path.string());
		return false;
	}
	const auto fileSize = file.tellg();
	if (fileSize <= 0) {
		logger::warn("[Presets] Empty artwork '{}'", path.string());
		return false;
	}
	file.seekg(0, std::ios::beg);
	std::vector<unsigned char> bytes(static_cast<size_t>(fileSize));
	if (!file.read(reinterpret_cast<char*>(bytes.data()), fileSize)) {
		logger::warn("[Presets] Failed to read artwork '{}'", path.string());
		return false;
	}

	ID3D11ShaderResourceView* srv = nullptr;
	ImVec2 size{};
	const auto ext = ToLower(path.extension().string());
	bool ok = false;
	if (ext == ".dds") {
		ok = Util::LoadDDSTextureFromFile(globals::d3d::device, path.string().c_str(), &srv, size);
	} else {
		int width = 0, height = 0;
		unsigned char* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, nullptr, 4);
		if (!pixels) {
			logger::warn("[Presets] stbi failed for '{}': {}", path.string(), stbi_failure_reason() ? stbi_failure_reason() : "unknown");
			return false;
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = static_cast<UINT>(width);
		desc.Height = static_cast<UINT>(height);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = pixels;
		init.SysMemPitch = static_cast<UINT>(width * 4);

		winrt::com_ptr<ID3D11Texture2D> texture;
		const HRESULT hrTex = globals::d3d::device->CreateTexture2D(&desc, &init, texture.put());
		stbi_image_free(pixels);
		if (FAILED(hrTex) || !texture) {
			logger::warn("[Presets] CreateTexture2D failed for '{}' ({:x})", path.string(), static_cast<unsigned>(hrTex));
			return false;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		const HRESULT hrSrv = globals::d3d::device->CreateShaderResourceView(texture.get(), &srvDesc, &srv);
		if (FAILED(hrSrv) || !srv) {
			logger::warn("[Presets] CreateSRV failed for '{}' ({:x})", path.string(), static_cast<unsigned>(hrSrv));
			return false;
		}
		size = ImVec2(static_cast<float>(width), static_cast<float>(height));
		ok = true;
	}
	if (!ok || !srv)
		return false;

	outSRV.attach(srv);
	return true;
}

bool UnifiedPresetCatalog::EnsureArtwork(PackInfo& pack)
{
	if (pack.artworkLoaded)
		return true;

	LoadTextureSRV(pack.logoPath, pack.logoSRV);
	LoadTextureSRV(pack.coverPath, pack.coverSRV);
	// List thumbs need something: fall back to cover when logo is missing/unloadable.
	if (!pack.logoSRV && pack.coverSRV)
		pack.logoSRV = pack.coverSRV;

	pack.screenshotSRVs.clear();
	pack.screenshotSRVs.reserve(pack.screenshotPaths.size());
	for (const auto& shot : pack.screenshotPaths) {
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		if (LoadTextureSRV(shot, srv))
			pack.screenshotSRVs.push_back(std::move(srv));
	}
	pack.artworkLoaded = true;
	return true;
}

void UnifiedPresetCatalog::ReleaseAllArtwork()
{
	for (auto& pack : packs) {
		pack.logoSRV = nullptr;
		pack.coverSRV = nullptr;
		pack.screenshotSRVs.clear();
		pack.artworkLoaded = false;
	}
}

bool UnifiedPresetCatalog::ApplyPack(const std::string& id, bool saveEffects11Current)
{
	auto* pack = FindPack(id);
	if (!pack || !pack->valid)
		return false;

	bool appliedAny = false;
	bool effects11Applied = false;

	if (pack->hasEffects11) {
		auto& presetManager = PresetManager::GetSingleton();
		presetManager.DiscoverPresets();

		const auto e11Id = GetEffects11PresetId(*pack);
		if (presetManager.SwitchPreset(e11Id, saveEffects11Current)) {
			globals::features::effects11.PersistActivePreset();
			appliedAny = effects11Applied = true;
		} else {
			logger::warn("[Presets] Failed to switch Effects11 preset '{}'", e11Id);
		}
	}

	if (pack->hasCSPresets)
		appliedAny = true;

	// A Baseline-only pack is its own layer, so it must not displace the active Effects 11 / CS pack.
	if (appliedAny) {
		SetActivePackId(id);
		// The scene layer always follows the active pack, so a pack without scene files clears it.
		globals::features::sceneManager.ReloadOverwrites();
		// Applying a pack, even again, re-selects the pipeline it was authored for.
		using PostProcessingMode::Mode;
		PostProcessingMode::Set(pack->IsE11() && effects11Applied ? Mode::Effects11 : Mode::PostProcessing);
	}

	if (pack->hasBaseline) {
		EnableBaseline(*pack);
		appliedAny = true;
	}

	return appliedAny;
}

bool UnifiedPresetCatalog::IsBaselineEnabled(const std::string& id) const
{
	return std::ranges::find(baselinePackIds, id) != baselinePackIds.end();
}

void UnifiedPresetCatalog::EnableBaseline(const PackInfo& pack)
{
	// Re-enabling moves the pack to the end so it wins conflicts against the other Baseline packs.
	std::erase(baselinePackIds, pack.id);
	baselinePackIds.push_back(pack.id);
	SaveActiveState();

	SettingsOverrideManager::GetSingleton()->RefreshOverrides();

	// The baseline is the layer beneath the scene, so reapply it without the scene's values in play.
	SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
	for (const auto& featureName : pack.baselineFeatures) {
		auto* feature = Feature::FindFeatureByShortName(featureName);
		if (!feature || !feature->loaded) {
			logger::info("[Presets] Baseline '{}': feature '{}' is not loaded; it applies once the feature loads", pack.id, featureName);
			continue;
		}
		if (!feature->ReapplyOverrideSettings())
			logger::warn("[Presets] Baseline '{}': nothing applied to '{}' (see Feature Issues for rejected files)", pack.id, featureName);
	}
}

bool UnifiedPresetCatalog::RemoveBaseline(const std::string& id)
{
	if (std::erase(baselinePackIds, id) == 0)
		return false;
	SaveActiveState();

	auto* overrides = SettingsOverrideManager::GetSingleton();
	overrides->RefreshOverrides();
	// Drops the .user deltas of features that no longer have any override behind them.
	overrides->CleanupStaleUserOverrides();
	logger::info("[Presets] Removed Baseline pack '{}'", id);
	return true;
}

bool UnifiedPresetCatalog::EnsureBaselineManifest(const std::filesystem::path& packRoot, const std::string& displayName)
{
	const auto manifestPath = GetPackManifestPath(packRoot);
	std::error_code ec;
	if (std::filesystem::exists(manifestPath, ec))
		return true;

	json manifest;
	manifest["name"] = displayName;
	manifest["author"] = "";
	manifest["version"] = "1.0.0";
	manifest["description"] = "";
	manifest[kPresetTypeKey] = GetPresetTypeName(PresetType::Baseline);
	manifest["csVersion"] = PresetCompatibility::CurrentCsVersionString();
	return Util::FileHelpers::WriteJsonAtomically(manifestPath, manifest, 4, "preset pack manifest");
}

bool UnifiedPresetCatalog::OpenPresetsFolder() const
{
	Util::FileHelpers::EnsureDirectoryExists(GetPresetsRealPath());
	const auto result = ShellExecuteW(nullptr, L"open", GetPresetsRealPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<intptr_t>(result) > kShellExecuteSuccessThreshold;
}

bool UnifiedPresetCatalog::OpenPackFolder(const std::string& id) const
{
	const auto* pack = FindPack(id);
	if (!pack)
		return false;

	std::filesystem::path path = pack->rootPath;
	// Explorer runs outside MO2's VFS, so open the physical copy when this mod folder has one.
	// A Legacy install belongs to another mod (or the game root), never the CS mod folder.
	if (const auto dataRelative = path.lexically_relative(Util::PathHelpers::GetDataPath());
		pack->source != SourceKind::Effects11Legacy && !dataRelative.empty() && *dataRelative.begin() != "..") {
		std::error_code ec;
		if (auto realPath = Util::PathHelpers::GetRealPathFromDataRelative(dataRelative); !realPath.empty() && std::filesystem::exists(realPath, ec))
			path = std::move(realPath);
	}

	const auto result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<intptr_t>(result) > kShellExecuteSuccessThreshold;
}
