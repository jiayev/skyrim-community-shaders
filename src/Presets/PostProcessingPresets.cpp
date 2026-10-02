#include "PostProcessingPresets.h"

#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

#include "CSEditor/SceneManager/SceneSettingsManager.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SettingsOverrideManager.h"
#include "Utils/FileSystem.h"

namespace
{
	/// Where Post Processing kept its own presets before they became Baseline packs.
	constexpr const char* kLegacyPresetDir = "Data/SKSE/Plugins/CommunityShaders/PostProcessing";
	/// The shipped defaults file, now baked into the sub-features' Settings initializers. Older
	/// installs may still have it; it is the stock look, so it is not converted into a preset.
	constexpr const char* kLegacyDefaultName = "default";
	constexpr const char* kMigratedSuffix = ".migrated";
	constexpr int kBaselineIndent = 4;

	std::filesystem::path BaselinePath(const std::filesystem::path& packRoot)
	{
		return packRoot / UnifiedPresetCatalog::kBaselineSubdir /
		       std::format("{}.json", PostProcessingPresets::kFeatureShortName);
	}

	/// A Baseline file carries settings only: no legacy preset name and no metadata keys.
	json ToBaselineDocument(json settings)
	{
		if (!settings.is_object())
			return json::object();
		settings.erase("preset_name");
		for (auto it = settings.begin(); it != settings.end();) {
			if (it.key().starts_with('_'))
				it = settings.erase(it);
			else
				++it;
		}
		return settings;
	}

	/// The same checks a Baseline file meets when the catalog loads it, so nothing is written that would be rejected.
	bool IsValidBaseline(const json& document, const std::filesystem::path& path)
	{
		auto* overrides = SettingsOverrideManager::GetSingleton();
		return overrides && overrides->IsValidOverrideDocument(document, path);
	}

	std::optional<json> ReadJsonFile(const std::filesystem::path& path)
	{
		try {
			std::ifstream file(path);
			if (!file.is_open())
				return std::nullopt;
			return json::parse(file);
		} catch (const std::exception& e) {
			logger::warn("[PostProcessingPresets] Could not read {}: {}", path.string(), e.what());
			return std::nullopt;
		}
	}

	bool WriteDocument(const std::filesystem::path& packRoot, const json& document)
	{
		const auto path = BaselinePath(packRoot);
		if (!IsValidBaseline(document, path))
			return false;
		Util::FileHelpers::EnsureDirectoryExists(path.parent_path());
		return Util::FileHelpers::WriteJsonAtomically(path, document, kBaselineIndent, "post processing baseline");
	}
}

void PostProcessingPresets::MigrateLegacyPresets()
{
	static bool migrated = false;
	if (std::exchange(migrated, true))
		return;

	const std::filesystem::path legacyDir = kLegacyPresetDir;
	std::error_code ec;
	if (!std::filesystem::is_directory(legacyDir, ec))
		return;

	const auto presetsRoot = UnifiedPresetCatalog::GetSingleton().GetPresetsRealPath();
	for (const auto& entry : std::filesystem::directory_iterator(legacyDir, ec)) {
		const auto& source = entry.path();
		if (!entry.is_regular_file(ec) || source.extension() != ".json" ||
			_stricmp(source.stem().string().c_str(), kLegacyDefaultName) == 0)
			continue;

		const auto name = source.stem().string();
		const auto packId = Util::FileHelpers::SanitizeFileName(name);
		if (packId.empty())
			continue;
		const auto packRoot = presetsRoot / packId;
		if (std::filesystem::exists(packRoot, ec)) {
			logger::info("[PostProcessingPresets] '{}' left in PostProcessing/: a preset named '{}' already exists", name, packId);
			continue;
		}

		auto document = ReadJsonFile(source);
		if (!document)
			continue;
		if (!WriteDocument(packRoot, ToBaselineDocument(std::move(*document))) ||
			!UnifiedPresetCatalog::EnsureBaselineManifest(packRoot, name)) {
			logger::warn("[PostProcessingPresets] Could not convert '{}' into a preset; it stays in PostProcessing/", name);
			continue;
		}

		auto migratedPath = source;
		migratedPath += kMigratedSuffix;
		std::filesystem::rename(source, migratedPath, ec);
		if (ec)
			logger::warn("[PostProcessingPresets] Converted '{}' but could not rename the original: {}", name, ec.message());
		logger::info("[PostProcessingPresets] Moved Post Processing preset '{}' to the Presets pack '{}'", name, packId);
	}
}

bool PostProcessingPresets::WriteBaseline(const std::filesystem::path& packRoot)
{
	auto& postProcessing = globals::features::postProcessing;
	if (!postProcessing.loaded)
		return false;

	json settings;
	{
		// The base settings, not whatever a scene layer has applied over them right now.
		SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
		postProcessing.SaveSettings(settings);
	}
	if (!WriteDocument(packRoot, ToBaselineDocument(std::move(settings))))
		return false;

	SettingsOverrideManager::GetSingleton()->RefreshOverrides();
	return true;
}

bool PostProcessingPresets::HasBaseline(const std::filesystem::path& packRoot)
{
	std::error_code ec;
	return std::filesystem::is_regular_file(BaselinePath(packRoot), ec);
}
