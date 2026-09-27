#include "UnifiedPresetCatalog.h"
#include "PCH.h"

#include "Features/Effects11.h"
#include "Features/Effects11/PresetManager.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "CSEditor/SceneManager/SceneManager.h"
#include "Utils/FileSystem.h"
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
	constexpr const char* kLegacyMetaFileName = "preset.json";

	std::string ToLower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

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

	bool IsImageExtension(const std::filesystem::path& path)
	{
		const auto ext = ToLower(path.extension().string());
		return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp" || ext == ".dds";
	}

	/** effects11/ nested pack, or classic ENB drop with enbseries at the pack root (NAT.ENB-style). */
	bool TryResolveEffects11Root(const std::filesystem::path& packRoot, const std::string& preferredRel,
		std::filesystem::path& outRoot)
	{
		std::string reason;
		const auto tryCandidate = [&](const std::filesystem::path& candidate) {
			if (candidate.empty())
				return false;
			if (!PresetManager::ValidateLibraryPreset(candidate, reason))
				return false;
			outRoot = candidate;
			return true;
		};

		if (!preferredRel.empty() && tryCandidate(packRoot / preferredRel))
			return true;
		if (preferredRel != "effects11" && tryCandidate(packRoot / "effects11"))
			return true;
		if (tryCandidate(packRoot))
			return true;
		return false;
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

void UnifiedPresetCatalog::LoadActiveState()
{
	activePackId.clear();
	const auto path = GetPresetsRealPath() / kActiveStateFileName;
	std::ifstream in(path);
	if (!in)
		return;
	try {
		json j;
		in >> j;
		activePackId = j.value("activePackId", "");
	} catch (...) {
		activePackId.clear();
	}
}

void UnifiedPresetCatalog::SaveActiveState() const
{
	Util::FileHelpers::EnsureDirectoryExists(GetPresetsRealPath());
	const auto path = GetPresetsRealPath() / kActiveStateFileName;
	try {
		json j;
		j["activePackId"] = activePackId;
		std::ofstream out(path);
		out << std::setw(4) << j;
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
	std::error_code ec;
	const auto root = GetPresetsRealPath();
	if (!std::filesystem::is_directory(root, ec))
		return;

	for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
		if (ec || !entry.is_directory())
			continue;

		const auto packId = entry.path().filename().string();
		if (packId.empty() || packId.starts_with('_') || packId.starts_with('.'))
			continue;

		PackInfo pack;
		pack.id = packId;
		pack.name = packId;
		pack.source = SourceKind::UnifiedPack;
		pack.rootPath = entry.path();

		std::string effects11Rel = "effects11";
		std::string csppRel = "cspp.json";
		json backendsOverride = json::object();
		bool haveBackendsOverride = false;

		// Prefer <PackId>.json, fall back to legacy preset.json.
		std::filesystem::path metaPath = entry.path() / (packId + ".json");
		if (!std::filesystem::exists(metaPath))
			metaPath = entry.path() / kLegacyMetaFileName;

		if (std::filesystem::exists(metaPath)) {
			try {
				std::ifstream in(metaPath);
				json meta;
				in >> meta;
				pack.name = meta.value("name", packId);
				pack.author = meta.value("author", "");
				pack.version = meta.value("version", "");
				pack.description = meta.value("description", "");
				pack.nexusUrl = meta.value("nexusUrl", "");
				if (meta.contains("tags") && meta["tags"].is_array()) {
					for (const auto& tag : meta["tags"]) {
						if (tag.is_string())
							pack.tags.push_back(tag.get<std::string>());
					}
				}

				if (meta.contains("effects11") && meta["effects11"].is_object())
					effects11Rel = meta["effects11"].value("path", effects11Rel);
				if (meta.contains("cspp") && meta["cspp"].is_object())
					csppRel = meta["cspp"].value("file", csppRel);

				if (meta.contains("logo") && meta["logo"].is_string())
					pack.logoPath = ResolveRelative(entry.path(), meta["logo"].get<std::string>());
				if (meta.contains("cover") && meta["cover"].is_string())
					pack.coverPath = ResolveRelative(entry.path(), meta["cover"].get<std::string>());
				if (meta.contains("screenshots") && meta["screenshots"].is_array()) {
					for (const auto& shot : meta["screenshots"]) {
						if (!shot.is_string())
							continue;
						auto path = ResolveRelative(entry.path(), shot.get<std::string>());
						if (!path.empty())
							pack.screenshotPaths.push_back(std::move(path));
					}
				}

				if (meta.contains("backends") && meta["backends"].is_object()) {
					backendsOverride = meta["backends"];
					haveBackendsOverride = true;
				}
			} catch (const std::exception& e) {
				pack.valid = false;
				pack.invalidReason = std::format("Invalid {}: {}", metaPath.filename().string(), e.what());
			}
		}

		// Filesystem inference — classic ENB drops (NAT.ENB) put enbseries at the pack root.
		std::filesystem::path e11Root;
		if (TryResolveEffects11Root(entry.path(), effects11Rel, e11Root)) {
			pack.hasEffects11 = true;
			pack.effects11Root = std::move(e11Root);
		}

		auto csppPath = entry.path() / csppRel;
		if (!std::filesystem::exists(csppPath) && csppRel != "cspp.json")
			csppPath = entry.path() / "cspp.json";
		if (std::filesystem::exists(csppPath)) {
			pack.hasCSPP = true;
			pack.csppFile = csppPath;
		}

		if (haveBackendsOverride) {
			if (backendsOverride.contains("effects11") && backendsOverride["effects11"].is_boolean() &&
				!backendsOverride["effects11"].get<bool>()) {
				pack.hasEffects11 = false;
				pack.effects11Root.clear();
			}
			if (backendsOverride.contains("cspp") && backendsOverride["cspp"].is_boolean() &&
				!backendsOverride["cspp"].get<bool>()) {
				pack.hasCSPP = false;
				pack.csppFile.clear();
			}
		}

		if (pack.description.empty())
			pack.description = InferDescriptionFromReadme(entry.path());
		InferMissingArtwork(pack);

		// Scene Manager exports live under SceneSettings/ (see DiscoverSceneManagerPresets), not
		// as InteriorOnly/ folders inside a Presets pack.
		if (!pack.hasEffects11 && !pack.hasCSPP) {
			pack.valid = false;
			if (pack.invalidReason.empty())
				pack.invalidReason = "No Effects11 or CSPP payload found";
		}

		packs.push_back(std::move(pack));
	}
}

void UnifiedPresetCatalog::DiscoverEffects11Orphans()
{
	std::error_code ec;
	const auto root = Util::PathHelpers::GetEffects11PresetsRealPath();
	if (!std::filesystem::is_directory(root, ec))
		return;

	for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
		if (ec || !entry.is_directory())
			continue;

		const auto name = entry.path().filename().string();
		if (name.empty() || name.starts_with('_') || name.starts_with('.'))
			continue;

		// Skip if already covered by a unified pack with the same folder name
		if (FindPack(name))
			continue;

		PackInfo pack;
		pack.id = std::string("e11:") + name;
		pack.name = name;
		pack.source = SourceKind::Effects11Orphan;
		pack.rootPath = entry.path();
		pack.effects11Root = entry.path();
		pack.hasEffects11 = std::filesystem::exists(entry.path() / PresetManager::kEnbSeriesIniName) &&
							std::filesystem::is_directory(entry.path() / PresetManager::kEnbSeriesDirName);
		if (!pack.hasEffects11) {
			pack.valid = false;
			pack.invalidReason = "Missing enbseries.ini / enbseries";
		}
		pack.description = "Effects11 library preset";
		packs.push_back(std::move(pack));
	}
}

void UnifiedPresetCatalog::DiscoverCSPPOrphans()
{
	std::error_code ec;
	const auto root = std::filesystem::path("Data") / "SKSE" / "Plugins" / "CommunityShaders" / "PostProcessing";
	std::filesystem::path scanRoot = root;
	const auto realRoot = Util::PathHelpers::GetRootRealPath() / "SKSE" / "Plugins" / "CommunityShaders" / "PostProcessing";
	if (std::filesystem::is_directory(realRoot, ec))
		scanRoot = realRoot;
	else if (!std::filesystem::is_directory(scanRoot, ec))
		return;

	for (const auto& entry : std::filesystem::directory_iterator(scanRoot, ec)) {
		if (ec || !entry.is_regular_file() || entry.path().extension() != ".json")
			continue;

		const auto stem = entry.path().stem().string();
		if (stem.empty() || stem.starts_with('_'))
			continue;

		PackInfo pack;
		pack.id = std::string("cspp:") + stem;
		pack.name = stem;
		pack.source = SourceKind::CSPPOrphan;
		pack.rootPath = entry.path().parent_path();
		pack.csppFile = entry.path();
		pack.hasCSPP = true;
		pack.description = "Post Processing preset";
		packs.push_back(std::move(pack));
	}
}

void UnifiedPresetCatalog::DiscoverSceneManagerPresets()
{
	// Canonical Scene Manager export layout (source of truth for SM packs):
	//   SceneSettings/<Name>.json          — presetMetadata { name, version, author?, description?, tags?, logo?, cover?, screenshots? }
	//   SceneSettings/<Name>/              — optional artwork (logo.png, cover.png, gallery/)
	//   SceneSettings/InteriorOnly/<Name>_*.json
	//   SceneSettings/TimeOfDay/...        — same naming prefix
	//   SceneSettings/Weather/... / Locations/...
	auto& sceneManager = globals::features::sceneManager;
	sceneManager.RefreshPresetMetadata();

	const auto sceneRoot = Util::PathHelpers::GetSceneSettingsPath();

	for (const auto& meta : sceneManager.GetPresetMetadata()) {
		if (meta.name.empty())
			continue;

		const auto files = sceneManager.FindPresetFiles(meta.name);
		const auto overwriteCount = files.empty() ? 0 : files.size() - (std::filesystem::exists(meta.path) ? 1 : 0);
		const auto fallbackDescription = std::format(
			"Scene Manager export — {} overwrite file(s) under SceneSettings. Apply reloads the live overwrite layer.",
			overwriteCount);

		const auto applyMetaToPack = [&](PackInfo& pack) {
			pack.hasSceneManager = true;
			pack.sceneMetadataPath = meta.path;
			if (pack.version.empty())
				pack.version = meta.version;
			if (pack.author.empty())
				pack.author = meta.author;
			if (pack.description.empty())
				pack.description = !meta.description.empty() ? meta.description : fallbackDescription;
			if (pack.tags.empty())
				pack.tags = meta.tags;
			if (pack.tags.empty())
				pack.tags = { "scene-manager", "exported" };

			const auto assetsRoot = sceneRoot / meta.name;
			if (pack.logoPath.empty() && !meta.logo.empty())
				pack.logoPath = ResolveRelative(sceneRoot, meta.logo);
			if (pack.coverPath.empty() && !meta.cover.empty())
				pack.coverPath = ResolveRelative(sceneRoot, meta.cover);
			if (pack.screenshotPaths.empty()) {
				for (const auto& shot : meta.screenshots) {
					auto path = ResolveRelative(sceneRoot, shot);
					if (!path.empty())
						pack.screenshotPaths.push_back(std::move(path));
				}
			}

			// Convention: SceneSettings/<Name>/{logo,cover,gallery} without listing paths in JSON.
			PackInfo artProbe;
			artProbe.rootPath = assetsRoot;
			InferMissingArtwork(artProbe);
			if (pack.logoPath.empty())
				pack.logoPath = std::move(artProbe.logoPath);
			if (pack.coverPath.empty())
				pack.coverPath = std::move(artProbe.coverPath);
			if (pack.screenshotPaths.empty())
				pack.screenshotPaths = std::move(artProbe.screenshotPaths);
		};

		// Same display name as a unified / orphan pack: attach SM badge rather than duplicating.
		if (auto* existing = FindPack(meta.name)) {
			applyMetaToPack(*existing);
			continue;
		}
		if (FindPack(std::string("sm:") + meta.name))
			continue;

		PackInfo pack;
		pack.id = std::string("sm:") + meta.name;
		pack.name = meta.name;
		pack.version = meta.version;
		pack.source = SourceKind::SceneManager;
		pack.rootPath = sceneRoot / meta.name;
		applyMetaToPack(pack);
		packs.push_back(std::move(pack));
	}
}

void UnifiedPresetCatalog::Discover()
{
	ReleaseAllArtwork();
	packs.clear();
	Util::FileHelpers::EnsureDirectoryExists(GetPresetsRealPath());
	LoadActiveState();

	DiscoverUnifiedPacks();
	DiscoverEffects11Orphans();
	DiscoverCSPPOrphans();
	DiscoverSceneManagerPresets();

	std::sort(packs.begin(), packs.end(), [](const PackInfo& a, const PackInfo& b) {
		return ToLower(a.name) < ToLower(b.name);
	});

	logger::info("[Presets] Discovered {} pack(s); active='{}'", packs.size(), activePackId.empty() ? "(none)" : activePackId);
}

std::vector<size_t> UnifiedPresetCatalog::Query(BackendFilter filter, const std::string& search) const
{
	switch (filter) {
	case BackendFilter::Effects11:
		return Query(true, false, false, search);
	case BackendFilter::CSPP:
		return Query(false, true, false, search);
	case BackendFilter::Both:
		return Query(true, true, false, search);
	case BackendFilter::SceneManager:
		return Query(false, false, true, search);
	case BackendFilter::All:
	default:
		return Query(false, false, false, search);
	}
}

std::vector<size_t> UnifiedPresetCatalog::Query(bool wantE11, bool wantCSPP, bool wantSM, const std::string& search) const
{
	const auto needle = ToLower(search);
	std::vector<size_t> indices;
	indices.reserve(packs.size());

	for (size_t i = 0; i < packs.size(); ++i) {
		const auto& pack = packs[i];
		if (wantE11 && !pack.hasEffects11)
			continue;
		if (wantCSPP && !pack.hasCSPP)
			continue;
		if (wantSM && !pack.hasSceneManager)
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
	// List thumbs need something — fall back to cover when logo is missing/unloadable.
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

	if (pack->hasEffects11) {
		auto& presetManager = PresetManager::GetSingleton();
		presetManager.DiscoverPresets();

		std::string e11Id;
		if (pack->source == SourceKind::UnifiedPack)
			e11Id = PresetManager::MakeUnifiedPackPresetId(pack->id);
		else if (pack->source == SourceKind::Effects11Orphan && pack->id.starts_with("e11:"))
			e11Id = pack->id.substr(4);
		else
			e11Id = pack->id;

		if (presetManager.SwitchPreset(e11Id, saveEffects11Current)) {
			globals::features::effects11.PersistActivePreset();
			appliedAny = true;
		} else {
			logger::warn("[Presets] Failed to switch Effects11 preset '{}'", e11Id);
		}
	}

	if (pack->hasCSPP && !pack->csppFile.empty()) {
		auto& pp = globals::features::postProcessing;
		if (pp.loaded) {
			pp.LoadPresetFromFile(pack->csppFile);
			appliedAny = true;
		} else {
			logger::warn("[Presets] Post Processing feature is not loaded; skipping CSPP apply");
		}
	}

	if (pack->hasSceneManager) {
		// Scene Manager exports install overwrite files into SceneSettings immediately; refresh
		// so newly exported/replaced files are the live mod layer. This is not an exclusive
		// switch — every SceneSettings overwrite file remains active after reload.
		auto& sceneManager = globals::features::sceneManager;
		const auto fileCount = sceneManager.FindPresetFiles(pack->name).size();
		sceneManager.ReloadOverwrites();
		appliedAny = true;
		logger::info("[Presets] Reloaded Scene Manager overwrites for preset '{}' ({} file(s))", pack->name, fileCount);
	}

	if (appliedAny)
		SetActivePackId(id);

	return appliedAny;
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
	if (pack->source == SourceKind::CSPPOrphan)
		path = pack->csppFile.parent_path();
	else if (pack->source == SourceKind::SceneManager && !pack->sceneMetadataPath.empty())
		path = pack->sceneMetadataPath.parent_path();

	const auto result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<intptr_t>(result) > kShellExecuteSuccessThreshold;
}
