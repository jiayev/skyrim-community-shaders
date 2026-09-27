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
	constexpr const char* kEffects11PackSubdir = "effects11";
	constexpr const char* kDefaultCSPPFileName = "cspp.json";

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

std::filesystem::path UnifiedPresetCatalog::GetPackManifestPath(const std::filesystem::path& packRoot)
{
	auto manifestPath = packRoot / packRoot.filename();
	manifestPath += ".json";
	std::error_code ec;
	if (!std::filesystem::exists(manifestPath, ec) && std::filesystem::exists(packRoot / kLegacyMetaFileName, ec))
		return packRoot / kLegacyMetaFileName;
	return manifestPath;
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
	for (const auto& packRoot : Util::PathHelpers::ListCommunityShaderEntries(Util::PathHelpers::kUnifiedPresetsSubdir, true)) {
		const auto packId = packRoot.filename().string();

		PackInfo pack;
		pack.id = packId;
		pack.name = packId;
		pack.source = SourceKind::UnifiedPack;
		pack.rootPath = packRoot;

		std::string csppRel = kDefaultCSPPFileName;
		const auto meta = ReadPackManifest(packRoot, &pack.invalidReason);
		pack.valid = pack.invalidReason.empty();
		try {
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

			if (meta.contains("cspp") && meta["cspp"].is_object())
				csppRel = meta["cspp"].value("file", csppRel);

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

		auto csppPath = packRoot / csppRel;
		if (!std::filesystem::exists(csppPath) && csppRel != kDefaultCSPPFileName)
			csppPath = packRoot / kDefaultCSPPFileName;
		if (std::filesystem::exists(csppPath)) {
			pack.hasCSPP = true;
			pack.csppFile = csppPath;
		}

		if (meta.contains("backends") && meta["backends"].is_object()) {
			const auto& backendsOverride = meta["backends"];
			if (backendsOverride.contains("cspp") && backendsOverride["cspp"].is_boolean() &&
				!backendsOverride["cspp"].get<bool>()) {
				pack.hasCSPP = false;
				pack.csppFile.clear();
			}
		}

		pack.hasSceneManager = SceneSettingsManager::HasScenePayload(packRoot);

		if (pack.description.empty())
			pack.description = InferDescriptionFromReadme(packRoot);
		InferMissingArtwork(pack);

		if (!pack.hasEffects11 && !pack.hasCSPP && !pack.hasSceneManager) {
			pack.valid = false;
			if (pack.invalidReason.empty())
				pack.invalidReason = "No Effects11, CSPP or Scene Manager payload found";
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
		pack.id = std::string("e11:") + name;
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

void UnifiedPresetCatalog::DiscoverCSPPOrphans()
{
	for (const auto& file : Util::PathHelpers::ListCommunityShaderEntries("PostProcessing", false)) {
		if (file.extension() != ".json")
			continue;

		const auto stem = file.stem().string();
		PackInfo pack;
		pack.id = std::string("cspp:") + stem;
		pack.name = stem;
		pack.source = SourceKind::CSPPOrphan;
		pack.rootPath = file.parent_path();
		pack.csppFile = file;
		pack.hasCSPP = true;
		pack.description = "Post Processing preset";
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

	if (pack->hasSceneManager)
		appliedAny = true;

	if (appliedAny) {
		SetActivePackId(id);
		// The scene layer always follows the active pack, so a pack without scene files clears it.
		globals::features::sceneManager.ReloadOverwrites();
	}

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

	std::filesystem::path path = pack->source == SourceKind::CSPPOrphan ? pack->csppFile.parent_path() : pack->rootPath;
	// Explorer runs outside MO2's VFS, so open the physical copy when this mod folder has one.
	if (const auto dataRelative = path.lexically_relative(Util::PathHelpers::GetDataPath()); !dataRelative.empty() && *dataRelative.begin() != "..") {
		std::error_code ec;
		if (auto realPath = Util::PathHelpers::GetRealPathFromDataRelative(dataRelative); !realPath.empty() && std::filesystem::exists(realPath, ec))
			path = std::move(realPath);
	}

	const auto result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<intptr_t>(result) > kShellExecuteSuccessThreshold;
}
