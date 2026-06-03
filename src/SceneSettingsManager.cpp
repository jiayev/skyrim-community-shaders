#include "SceneSettingsManager.h"

#include "Feature.h"
#include "Globals.h"
#include "SceneSettingsCatalog.generated.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/Game.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <tuple>

namespace
{
	constexpr auto kOverwriteJsonIndent = 2;
	constexpr auto kMaxSceneOverwriteFileSize = 1024 * 1024;
	constexpr const char* kFeatureKey = "_feature";
	constexpr const char* kMetadataKey = "_metadata";
	constexpr const char* kMetadataDescriptionKey = "description";
	constexpr const char* kMetadataEnabledKey = "enabled";
	constexpr std::string_view kSceneSettingDisplaySeparator = " / ";

	bool IsSceneMetadataKey(std::string_view key)
	{
		return !key.empty() && key.front() == '_';
	}

	// TOD/weather can only interpolate float settings, not integer toggles or enum values.
	bool IsNumericValue(const json& value)
	{
		return value.is_number_float();
	}

	using SceneSettingBlacklistPath = std::vector<std::string_view>;

	const std::vector<SceneSettingBlacklistPath>& GetSceneSettingBlacklist()
	{
		static const std::vector<SceneSettingBlacklistPath> blacklist = {
			{ "PostProcessing", "Border" },
			{ "PostProcessing", "LUT" },
			{ "PostProcessing", "Color Grading and Tone Mapping", "Enable Tonemapping" },
			{ "PostProcessing", "Color Grading and Tone Mapping", "Use OpenDRT" },
			{ "PostProcessing", "Color Grading and Tone Mapping", "Tonemapper" },
			{ "PostProcessing", "Color Grading and Tone Mapping", "Tonemapper Settings" },
			{ "PostProcessing", "Color Grading and Tone Mapping", "ODRT1" },
			{ "PostProcessing", "Color Grading and Tone Mapping", "ODRT2" },
		};
		return blacklist;
	}

	bool IsSceneSettingPathWrapper(std::string_view token)
	{
		return token == "settings" || token == "ppsettings";
	}

	std::string NormalizeSceneSettingAddressToken(std::string_view token)
	{
		auto normalized = token.find(' ') == std::string_view::npos ? Util::PrettifyIdentifier(token) : std::string(token);
		std::erase_if(normalized, [](unsigned char c) { return std::isspace(c); });
		std::transform(normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return normalized;
	}

	bool SceneSettingAddressTokensEqual(std::string_view lhs, std::string_view rhs)
	{
		return NormalizeSceneSettingAddressToken(lhs) == NormalizeSceneSettingAddressToken(rhs);
	}

	bool IsSceneSettingBlacklistPrefix(const std::vector<std::string>& address, const SceneSettingBlacklistPath& prefix)
	{
		if (prefix.size() > address.size())
			return false;

		for (size_t i = 0; i < prefix.size(); ++i)
			if (!SceneSettingAddressTokensEqual(address[i], prefix[i]))
				return false;

		return true;
	}

	std::vector<std::string> GetSceneSettingAddress(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		std::vector<std::string> address{ featureShortName };
		address.reserve(settingPath.size() + 2);
		for (const auto& segment : settingPath)
			if (!IsSceneSettingPathWrapper(segment))
				address.push_back(segment);
		address.push_back(settingKey);
		return address;
	}

	bool IsHiddenSceneSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		auto address = GetSceneSettingAddress(featureShortName, settingPath, settingKey);
		return std::any_of(GetSceneSettingBlacklist().begin(), GetSceneSettingBlacklist().end(),
			[&](const auto& prefix) { return IsSceneSettingBlacklistPrefix(address, prefix); });
	}

	size_t CountSceneOverwriteKeys(const json& data)
	{
		if (!data.is_object())
			return 0;

		size_t count = 0;
		for (const auto& [key, value] : data.items()) {
			if (IsSceneMetadataKey(key))
				continue;
			if (Feature::IsSceneSettingPrimitive(value))
				++count;
			else if (value.is_object())
				count += CountSceneOverwriteKeys(value);
		}
		return count;
	}

	bool IsCompatibleSceneSettingValue(const json& featureValue, const json& value)
	{
		if (featureValue.type() == value.type())
			return true;
		if (featureValue.is_number() && value.is_number())
			return true;
		return false;
	}

	std::string GetDescriptorDisplayName(const SceneSettingDescriptor& descriptor)
	{
		return descriptor.displayName.empty() ? Util::PrettifyIdentifier(descriptor.key) : descriptor.displayName;
	}

	std::string JoinDisplayParts(const std::vector<std::string>& parts, std::string_view leaf)
	{
		std::string displayName;
		for (const auto& part : parts) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += part;
		}
		if (!leaf.empty()) {
			if (!displayName.empty())
				displayName += kSceneSettingDisplaySeparator;
			displayName += leaf;
		}
		return displayName;
	}

	std::vector<std::string> SplitCatalogPath(std::string_view path)
	{
		std::vector<std::string> parts;
		size_t start = 0;
		while (start < path.size()) {
			auto end = path.find('/', start);
			auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
			if (!part.empty())
				parts.emplace_back(part);
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return parts;
	}

	std::string ToCatalogPath(const std::vector<std::string>& path)
	{
		std::string result;
		for (const auto& part : path) {
			if (!result.empty())
				result += '/';
			result += part;
		}
		return result;
	}

	std::vector<std::string> GetCatalogDisplayPath(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return SplitCatalogPath(setting.displayPath.empty() ? setting.settingPath : setting.displayPath);
	}

	bool IsCatalogValueCompatible(const SceneSettingsCatalog::SettingMetadata& setting, const json& value)
	{
		using enum SceneSettingsCatalog::ValueType;
		switch (setting.valueType) {
		case Boolean:
			return value.is_boolean();
		case Integer:
			return value.is_number_integer();
		case Float:
			return value.is_number_float() || value.is_number_integer();
		case String:
			return value.is_string();
		default:
			return false;
		}
	}

	bool IsSameSetting(const SceneSettingsManager::SettingEntry& entry, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return entry.featureShortName == featureShortName &&
		       entry.settingPath == settingPath &&
		       entry.settingKey == settingKey;
	}

	struct SettingIdentity
	{
		std::vector<std::string> path;
		std::string key;

		bool operator<(const SettingIdentity& other) const
		{
			return std::tie(path, key) < std::tie(other.path, other.key);
		}
	};

	SettingIdentity MakeSettingIdentity(const SceneSettingsManager::SettingEntry& entry)
	{
		return { entry.settingPath, entry.settingKey };
	}

	using SettingUpdatesByFeature = std::map<std::string, std::vector<SceneSettingUpdate>>;

	void QueueSettingUpdate(SettingUpdatesByFeature& updatesByFeature, const SceneSettingsManager::SettingEntry& entry)
	{
		updatesByFeature[entry.featureShortName].push_back({ entry.settingPath, entry.settingKey, entry.value });
	}

	void ApplySettingUpdates(const SettingUpdatesByFeature& updatesByFeature)
	{
		for (const auto& [shortName, updates] : updatesByFeature) {
			if (updates.empty())
				continue;
			if (auto* feature = Feature::FindFeatureByShortName(shortName))
				feature->ApplySceneSettings(updates);
		}
	}

	std::string GetSettingLogName(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey)
	{
		return JoinDisplayParts(settingPath, std::format("{}.{}", featureShortName, settingKey));
	}

	json* GetObjectAtPath(json& data, const std::vector<std::string>& path, bool create)
	{
		json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object()) {
				if (!create)
					return nullptr;
				*node = json::object();
			}

			auto it = node->find(segment);
			if (it == node->end()) {
				if (!create)
					return nullptr;
				it = node->emplace(segment, json::object()).first;
			}
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	const json* GetObjectAtPath(const json& data, const std::vector<std::string>& path)
	{
		const json* node = &data;
		for (const auto& segment : path) {
			if (!node->is_object())
				return nullptr;
			auto it = node->find(segment);
			if (it == node->end())
				return nullptr;
			node = &*it;
		}
		return node->is_object() ? node : nullptr;
	}

	void CollectOverwriteEntries(const json& data, const std::vector<std::string>& settingPath,
		const std::function<void(const std::vector<std::string>&, const std::string&, const json&)>& callback)
	{
		if (!data.is_object())
			return;

		for (const auto& [key, value] : data.items()) {
			if (IsSceneMetadataKey(key))
				continue;
			if (Feature::IsSceneSettingPrimitive(value)) {
				callback(settingPath, key, value);
				continue;
			}
			if (!value.is_object())
				continue;

			auto childPath = settingPath;
			childPath.push_back(key);
			CollectOverwriteEntries(value, childPath, callback);
		}
	}

	template <size_t Size>
	bool ContainsFeatureName(const std::array<std::string_view, Size>& featureNames, std::string_view featureShortName)
	{
		return std::find(featureNames.begin(), featureNames.end(), featureShortName) != featureNames.end();
	}

	constexpr std::array<std::string_view, 9> kInteriorOnlyFeatureWhitelist = {
		"ScreenSpaceGI",
		"ScreenSpaceShadows",
		"SubsurfaceScattering",
		"LinearLighting",
		"ImageBasedLighting",
		"PostProcessing",
		"ScreenSpacePointLightShadows",
		"ScreenSpaceRayTracing",
		"VanillaFresnel",
	};

	constexpr std::array<std::string_view, 9> kExteriorFeatureWhitelist = {
		"CloudShadows",
		"ExponentialHeightFog",
		"GrassLighting",
		"ImageBasedLighting",
		"LinearLighting",
		"PostProcessing",
		"Skylighting",
		"SubsurfaceScattering",
		"WetnessEffects",
	};

	bool IsInteriorOnlyFeatureAllowed(std::string_view featureShortName)
	{
		return ContainsFeatureName(kInteriorOnlyFeatureWhitelist, featureShortName);
	}

	bool IsExteriorFeatureAllowed(std::string_view featureShortName)
	{
		return ContainsFeatureName(kExteriorFeatureWhitelist, featureShortName);
	}

	bool IsCatalogSettingHiddenByPolicy(const SceneSettingsCatalog::SettingMetadata& setting)
	{
		return SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Hidden) ||
		       IsHiddenSceneSetting(std::string(setting.featureShortName), SplitCatalogPath(setting.settingPath), std::string(setting.settingKey));
	}

	bool CatalogHasSceneSettings(std::string_view featureShortName, bool transitionableOnly)
	{
		for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
			if (setting.featureShortName != featureShortName || IsCatalogSettingHiddenByPolicy(setting))
				continue;
			if (!transitionableOnly ||
				SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Transitionable))
				return true;
		}
		return false;
	}

	std::vector<std::string> GetLoadedCatalogFeatureNames(bool transitionableOnly)
	{
		auto names = Feature::GetLoadedFeatureNames();
		std::erase_if(names, [&](const auto& name) { return !CatalogHasSceneSettings(name, transitionableOnly); });
		return names;
	}
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type, const SceneSettingsManager::SettingEntry& entry);
static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey);

static bool HasOverwriteEntryForPeriod(const std::vector<SceneSettingsManager::SettingEntry>& entries,
	const SceneSettingsManager::SettingEntry& candidate)
{
	return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
		return entry.source == SceneSettingsManager::EntrySource::Overwrite &&
		       entry.period == candidate.period &&
		       IsSameSetting(entry, candidate.featureShortName, candidate.settingPath, candidate.settingKey);
	});
}

static bool AddOverwriteEntryIfUnique(std::vector<SceneSettingsManager::SettingEntry>& entries,
	SceneSettingsManager::SettingEntry&& entry, std::string_view context)
{
	if (HasOverwriteEntryForPeriod(entries, entry)) {
		logger::warn("[SceneSettings] Duplicate {} overwrite for {}.{} ({}) skipped",
			context, entry.featureShortName, entry.settingKey, entry.sourceFilename);
		return false;
	}

	entries.push_back(std::move(entry));
	return true;
}

// --- Path Resolution ---

std::string SceneSettingsManager::GetSceneTypeName(SceneType type)
{
	switch (type) {
	case SceneType::InteriorOnly:
		return "InteriorOnly";
	case SceneType::TimeOfDay:
		return "TimeOfDay";
	default:
		return "Unknown";
	}
}

std::filesystem::path SceneSettingsManager::GetUserSettingsFilePath()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "SceneManager.json";
}

std::filesystem::path SceneSettingsManager::GetOverwritesPath(SceneType type)
{
	return Util::PathHelpers::GetSceneSettingsPath() / GetSceneTypeName(type);
}

std::filesystem::path SceneSettingsManager::GetWeatherOverwritesDir()
{
	return Util::PathHelpers::GetSceneSettingsPath() / "Weather";
}

// --- Time of Day Period Helpers ---

const char* SceneSettingsManager::GetPeriodName(TimeOfDayPeriod period)
{
	int idx = static_cast<int>(period);
	return (idx >= 0 && idx < kPeriodCount) ? kPeriodNames[idx] : "Unknown";
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetPeriodFromName(const std::string& name)
{
	for (int i = 0; i < kPeriodCount; ++i) {
		if (name == GetPeriodName(static_cast<TimeOfDayPeriod>(i)))
			return static_cast<TimeOfDayPeriod>(i);
	}
	return TimeOfDayPeriod::Count;
}

float SceneSettingsManager::GetCurrentGameHour()
{
	// Prefer calendar (ground truth), which the Weather Editor slider writes to.
	// sky->currentGameHour may lag when timeScale is 0 (time paused).
	auto calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
	float hour = 12.0f;
	if (calendar && calendar->gameHour)
		hour = calendar->gameHour->value;
	else if (auto sky = globals::game::sky)
		hour = sky->currentGameHour;

	// Normalize into [0, 24) so midnight is 0 and never 24.
	hour = std::clamp(hour, 0.0f, 24.0f);
	if (hour >= 24.0f)
		hour = 0.0f;
	return hour;
}

void SceneSettingsManager::GetTimeOfDayFactors(float outFactors[kPeriodCount])
{
	for (int i = 0; i < kPeriodCount; ++i)
		outFactors[i] = 0.0f;

	float hour = GetCurrentGameHour();

	// Normalize to [0, 24) — Night wraps, so also check hour + 24 for pre-dawn hours
	for (int i = 0; i < kPeriodCount; ++i) {
		float start = kPeriodHours[i][0];
		float end = kPeriodHours[i][1];
		float h = (end > 24.0f && hour < start) ? hour + 24.0f : hour;

		if (h >= start && h < end) {
			// Inside this period — check if we're in the blend-out zone near the end.
			float distFromEnd = end - h;

			if (distFromEnd < kTransitionHours) {
				// Blending out to next period
				float t = distFromEnd / kTransitionHours;
				outFactors[i] = t;
				outFactors[(i + 1) % kPeriodCount] = 1.0f - t;
			} else {
				outFactors[i] = 1.0f;
			}
			return;
		}
	}

	// Fallback: noon = Day
	outFactors[static_cast<int>(TimeOfDayPeriod::Day)] = 1.0f;
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetDominantPeriod()
{
	float factors[kPeriodCount];
	GetTimeOfDayFactors(factors);

	int best = 0;
	for (int i = 1; i < kPeriodCount; ++i)
		if (factors[i] > factors[best])
			best = i;
	return static_cast<TimeOfDayPeriod>(best);
}

SceneSettingsManager::TimeOfDayPeriod SceneSettingsManager::GetCurrentPeriod()
{
	float hour = GetCurrentGameHour();
	for (int i = 0; i < kPeriodCount; ++i) {
		float start = kPeriodHours[i][0];
		float end = kPeriodHours[i][1];
		float h = (end > 24.0f && hour < start) ? hour + 24.0f : hour;
		if (h >= start && h < end)
			return static_cast<TimeOfDayPeriod>(i);
	}
	return TimeOfDayPeriod::Day;
}

// --- Feature Metadata (static helpers, zero coupling) ---

bool SceneSettingsManager::IsFeatureAllowedForType(SceneType type, const std::string& featureShortName)
{
	switch (type) {
	case SceneType::InteriorOnly:
		return IsInteriorOnlyFeatureAllowed(featureShortName) &&
		       Feature::FindFeatureByShortName(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, false);
	case SceneType::TimeOfDay:
		return IsExteriorFeatureAllowed(featureShortName) &&
		       Feature::FindFeatureByShortName(featureShortName) &&
		       CatalogHasSceneSettings(featureShortName, true);
	default:
		return Feature::FindFeatureByShortName(featureShortName) != nullptr;
	}
}

std::vector<std::string> SceneSettingsManager::GetInteriorRelevantFeatureNames()
{
	auto names = GetLoadedCatalogFeatureNames(false);
	std::erase_if(names, [](const auto& name) { return !IsInteriorOnlyFeatureAllowed(name); });
	return names;
}

std::vector<std::string> SceneSettingsManager::GetExteriorRelevantFeatureNames()
{
	auto names = GetLoadedCatalogFeatureNames(true);
	std::erase_if(names, [](const auto& name) { return !IsExteriorFeatureAllowed(name); });
	return names;
}

std::string SceneSettingsManager::GetFeatureDisplayName(const std::string& featureShortName)
{
	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	return feature ? feature->GetName() : featureShortName;
}

std::vector<SceneSettingDescriptor> SceneSettingsManager::GetFeatureSceneSettings(const std::string& featureShortName)
{
	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature)
		return {};

	SceneLayerGuard guard(*GetSingleton());
	std::vector<SceneSettingDescriptor> descriptors;
	for (const auto& setting : SceneSettingsCatalog::GetSettings()) {
		if (setting.featureShortName != featureShortName ||
			SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Hidden))
			continue;

		auto settingPath = SplitCatalogPath(setting.settingPath);
		if (setting.settingKey.empty() || IsHiddenSceneSetting(featureShortName, settingPath, std::string(setting.settingKey)))
			continue;

		json value;
		if (!feature->GetSceneSettingValue(settingPath, std::string(setting.settingKey), value) ||
			!Feature::IsSceneSettingPrimitive(value) ||
			!IsCatalogValueCompatible(setting, value))
			continue;

		descriptors.push_back({
			.settingPath = std::move(settingPath),
			.key = std::string(setting.settingKey),
			.displayName = std::string(setting.displayName.empty() ? setting.settingKey : setting.displayName),
			.displayPath = GetCatalogDisplayPath(setting),
			.value = std::move(value),
		});
	}

	std::sort(descriptors.begin(), descriptors.end(), [](const auto& lhs, const auto& rhs) {
		return std::tie(lhs.displayPath, lhs.settingPath, lhs.displayName, lhs.key) <
		       std::tie(rhs.displayPath, rhs.settingPath, rhs.displayName, rhs.key);
	});
	return descriptors;
}

std::vector<SceneSettingDescriptor> SceneSettingsManager::GetTransitionableSceneSettings(const std::string& featureShortName)
{
	auto descriptors = GetFeatureSceneSettings(featureShortName);
	std::erase_if(descriptors, [&](const auto& descriptor) {
		auto* setting = SceneSettingsCatalog::FindSetting(featureShortName, ToCatalogPath(descriptor.settingPath), descriptor.key);
		return !setting ||
		       !SceneSettingsCatalog::HasFlag(setting->flags, SceneSettingsCatalog::SettingFlag::Transitionable) ||
		       !IsNumericValue(descriptor.value);
	});
	return descriptors;
}

std::string SceneSettingsManager::GetSettingDisplayName(const std::string& settingKey)
{
	return Util::PrettifyIdentifier(settingKey);
}

static std::string GetSceneSettingDisplayName(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	for (const auto& descriptor : SceneSettingsManager::GetFeatureSceneSettings(featureShortName)) {
		if (descriptor.settingPath != settingPath || descriptor.key != settingKey)
			continue;
		return JoinDisplayParts(descriptor.displayPath, GetDescriptorDisplayName(descriptor));
	}
	return SceneSettingsManager::GetSettingDisplayName(settingKey);
}

json SceneSettingsManager::GetFeatureSettingValue(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature)
		return {};

	SceneLayerGuard guard(*GetSingleton());
	json value;
	if (feature->GetSceneSettingValue(settingPath, settingKey, value) && Feature::IsSceneSettingPrimitive(value))
		return value;
	return {};
}

SceneSettingsManager::SettingType SceneSettingsManager::DetectSettingType(const json& value)
{
	if (value.is_boolean())
		return SettingType::Boolean;
	if (value.is_number_integer())
		return SettingType::Integer;
	if (value.is_number_float())
		return SettingType::Float;
	if (value.is_string())
		return SettingType::String;
	return SettingType::Unknown;
}

static bool IsSceneSettingValueAllowed(Feature& feature, const SceneSettingsCatalog::SettingMetadata& setting,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value, bool requireNumeric)
{
	json featureValue;
	if (!feature.GetSceneSettingValue(settingPath, settingKey, featureValue) || !Feature::IsSceneSettingPrimitive(featureValue))
		return false;

	if (!IsCatalogValueCompatible(setting, featureValue) || !IsCatalogValueCompatible(setting, value))
		return false;

	if (requireNumeric && (!SceneSettingsCatalog::HasFlag(setting.flags, SceneSettingsCatalog::SettingFlag::Transitionable) ||
		                      !IsNumericValue(featureValue) || !IsNumericValue(value) || !std::isfinite(value.get<float>())))
		return false;
	if (!requireNumeric && !Feature::IsSceneSettingPrimitive(value))
		return false;

	return IsCompatibleSceneSettingValue(featureValue, value);
}

static bool ValidateSceneSettingEntry(std::string_view context, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value, bool requireNumeric)
{
	if (IsHiddenSceneSetting(featureShortName, settingPath, settingKey)) {
		logger::warn("[SceneSettings] {} entry '{}' is blacklisted",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* setting = SceneSettingsCatalog::FindSetting(featureShortName, ToCatalogPath(settingPath), settingKey);
	if (!setting ||
		SceneSettingsCatalog::HasFlag(setting->flags, SceneSettingsCatalog::SettingFlag::Hidden)) {
		logger::warn("[SceneSettings] {} entry {} is not in the compiled scene settings catalog",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}

	auto* feature = Feature::FindFeatureByShortName(featureShortName);
	if (!feature) {
		logger::warn("[SceneSettings] {} entry {} - feature '{}' not found/loaded",
			context, GetSettingLogName(featureShortName, settingPath, settingKey), featureShortName);
		return false;
	}

	if (!IsSceneSettingValueAllowed(*feature, *setting, settingPath, settingKey, value, requireNumeric)) {
		logger::warn("[SceneSettings] {} entry {} is not a supported scene-manager setting",
			context, GetSettingLogName(featureShortName, settingPath, settingKey));
		return false;
	}
	return true;
}

// --- Generic Entry Management ---

std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntriesMut(SceneType type)
{
	return entries[type];
}

const std::vector<SceneSettingsManager::SettingEntry>& SceneSettingsManager::GetEntries(SceneType type) const
{
	static const std::vector<SettingEntry> empty;
	auto it = entries.find(type);
	return (it != entries.end()) ? it->second : empty;
}

bool SceneSettingsManager::IsEntryActive(const SettingEntry& entry) const
{
	return !entry.paused && !IsFeaturePaused(entry.featureShortName);
}

bool SceneSettingsManager::HasActiveEntries(SceneType type) const
{
	for (const auto& entry : GetEntries(type)) {
		if (type == SceneType::TimeOfDay && !IsNumericValue(entry.value))
			continue;
		if (IsEntryActive(entry))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasEntryFromSource(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source) const
{
	for (const auto& entry : GetEntries(type)) {
		if (entry.source == source && IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasEntryForPeriod(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey,
	TimeOfDayPeriod period, EntrySource source) const
{
	for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
		if (entry.source == source && entry.period == period &&
			IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasActiveOverwrite(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey) const
{
	for (const auto& entry : GetEntries(type)) {
		if (entry.source == EntrySource::Overwrite && !entry.paused &&
			IsSameSetting(entry, featureShortName, settingPath, settingKey))
			return true;
	}
	return false;
}

bool SceneSettingsManager::HasDuplicateEntry(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source, TimeOfDayPeriod period) const
{
	if (type == SceneType::TimeOfDay)
		return HasEntryForPeriod(featureShortName, settingPath, settingKey, period, source);
	return HasEntryFromSource(type, featureShortName, settingPath, settingKey, source);
}

bool SceneSettingsManager::AddSetting(SceneType type, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
	TimeOfDayPeriod period, bool deferCommit)
{
	if (!IsFeatureAllowedForType(type, featureShortName))
		return false;

	const bool requireNumeric = type == SceneType::TimeOfDay;
	if (requireNumeric) {
		// Reject invalid period values (Count is the sentinel, not a real period)
		if (period == TimeOfDayPeriod::Count || static_cast<int>(period) < 0 || static_cast<int>(period) >= kPeriodCount) {
			logger::warn("[SceneSettings] Rejecting TOD setting with invalid period: {}", GetSettingLogName(featureShortName, settingPath, settingKey));
			return false;
		}
	}
	if (!ValidateSceneSettingEntry(GetSceneTypeName(type), featureShortName, settingPath, settingKey, value, requireNumeric))
		return false;

	if (HasDuplicateEntry(type, featureShortName, settingPath, settingKey, EntrySource::User, period))
		return false;

	auto& vec = GetEntriesMut(type);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = value;
	entry.originalValue = entry.value;
	entry.source = EntrySource::User;
	entry.period = period;
	vec.push_back(std::move(entry));
	if (!deferCommit)
		CommitSceneSettingChanges();
	return true;
}

void SceneSettingsManager::RemoveSetting(SceneType type, size_t index)
{
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;

	auto& entry = vec[index];
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty() &&
		!RemoveSettingFromOverwriteFile(GetSceneOverwritePath(type, entry), entry.settingPath, entry.settingKey))
		return;

	logger::info("[SceneSettings] Removed {} entry: {} (source={})", GetSceneTypeName(type),
		GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey),
		entry.source == EntrySource::Overwrite ? "overwrite" : "user");

	vec.erase(vec.begin() + static_cast<ptrdiff_t>(index));
	SaveAllUserSettings();
	ReapplyIfActive();
}

void SceneSettingsManager::TogglePauseEntry(SceneType type, size_t index)
{
	auto& vec = GetEntriesMut(type);
	if (index < vec.size()) {
		vec[index].paused = !vec[index].paused;
		if (vec[index].source == EntrySource::User)
			SaveAllUserSettings();
		ReapplyIfActive();
	}
}

void SceneSettingsManager::RevertEntryToDefault(SceneType type, size_t index)
{
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;
	auto& entry = vec[index];
	if (!entry.originalValue.is_null()) {
		entry.value = entry.originalValue;
		SaveAllUserSettings();
		ReapplyIfActive();
	}
}

void SceneSettingsManager::SetAllOverwritesPaused(SceneType type, bool paused)
{
	allOverwritesPausedMap[type] = paused;
	for (auto& entry : GetEntriesMut(type))
		if (entry.source == EntrySource::Overwrite)
			entry.paused = paused;
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllOverwritesPaused(SceneType type) const
{
	auto it = allOverwritesPausedMap.find(type);
	return it != allOverwritesPausedMap.end() && it->second;
}

void SceneSettingsManager::DeleteAllOverwrites(SceneType type)
{
	auto& vec = GetEntriesMut(type);

	std::vector<bool> shouldErase(vec.size(), false);
	std::map<std::filesystem::path, bool> deleteResults;
	for (size_t i = 0; i < vec.size(); ++i) {
		const auto& entry = vec[i];
		if (entry.source != EntrySource::Overwrite)
			continue;
		if (entry.sourceFilename.empty()) {
			shouldErase[i] = true;
			continue;
		}
		auto filepath = GetSceneOverwritePath(type, entry);
		auto [resultIt, inserted] = deleteResults.try_emplace(filepath, false);
		if (inserted) {
			std::error_code ec;
			auto removed = std::filesystem::remove(filepath, ec);
			resultIt->second = removed || !ec;
			if (!resultIt->second)
				logger::error("[SceneSettings] Failed to delete overwrite file: {} ({}) - keeping entry", filepath.string(), ec.message());
		}
		if (resultIt->second)
			shouldErase[i] = true;
	}


	// Erase only entries whose backing files were successfully cleaned up
	// (iterate in reverse to preserve index validity)
	for (size_t i = vec.size(); i-- > 0;) {
		if (shouldErase[i])
			vec.erase(vec.begin() + static_cast<ptrdiff_t>(i));
	}

	allOverwritesPausedMap[type] = false;
	ReapplyIfActive();
}

void SceneSettingsManager::SetAllUserPaused(SceneType type, bool paused)
{
	allUserPausedMap[type] = paused;
	for (auto& entry : GetEntriesMut(type))
		if (entry.source == EntrySource::User)
			entry.paused = paused;
	SaveAllUserSettings();
	ReapplyIfActive();
}

bool SceneSettingsManager::AreAllUserPaused(SceneType type) const
{
	auto it = allUserPausedMap.find(type);
	return it != allUserPausedMap.end() && it->second;
}

void SceneSettingsManager::DeleteAllUserSettings(SceneType type)
{
	auto& vec = GetEntriesMut(type);
	std::erase_if(vec, [](const SettingEntry& e) {
		return e.source == EntrySource::User;
	});

	allUserPausedMap[type] = false;
	SaveAllUserSettings();
	ReapplyIfActive();
}

static std::string GetSceneOverwriteTypeDescription(SceneSettingsManager::SceneType type, SceneSettingsManager::TimeOfDayPeriod period)
{
	if (type == SceneSettingsManager::SceneType::InteriorOnly)
		return "Interior Only";
	if (period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return std::format("Time of Day - {}", SceneSettingsManager::GetPeriodName(period));
	return "Time of Day";
}

static std::string GetWeatherOverwriteTypeDescription(SceneSettingsManager::TimeOfDayPeriod period)
{
	if (period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return std::format("Weather - {}", SceneSettingsManager::GetPeriodName(period));
	return "Weather";
}

static std::filesystem::path GetSceneOverwritePath(SceneSettingsManager::SceneType type, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;

	auto basePath = SceneSettingsManager::GetOverwritesPath(type);
	if (type == SceneSettingsManager::SceneType::TimeOfDay && entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return basePath / SceneSettingsManager::GetPeriodName(entry.period) / entry.sourceFilename;
	return basePath / entry.sourceFilename;
}

static std::filesystem::path GetWeatherOverwritePath(RE::FormID weatherId, const SceneSettingsManager::SettingEntry& entry)
{
	if (!entry.sourcePath.empty())
		return entry.sourcePath;

	auto basePath = SceneSettingsManager::GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		return basePath / SceneSettingsManager::GetPeriodName(entry.period) / entry.sourceFilename;
	return basePath / entry.sourceFilename;
}

static bool WriteGroupedOverwriteFile(const std::filesystem::path& path, const std::string& featureShortName,
	const std::string& overwriteType, const std::vector<const SceneSettingsManager::SettingEntry*>& entries)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	if (ec) {
		logger::error("[SceneSettings] WriteGroupedOverwriteFile: failed to create dirs for '{}': {}", path.string(), ec.message());
		return false;
	}

	json data = json::object();
	if (std::filesystem::exists(path, ec)) {
		std::ifstream existing(path);
		if (existing.is_open()) {
			auto parsed = json::parse(existing, nullptr, false);
			if (parsed.is_object())
				data = std::move(parsed);
		}
	}

	data[kFeatureKey] = featureShortName;
	data[kMetadataKey] = {
		{ kMetadataDescriptionKey, std::format("{} scene settings overwrite ({})", SceneSettingsManager::GetFeatureDisplayName(featureShortName), overwriteType) },
		{ kMetadataEnabledKey, true }
	};
	for (const auto* entry : entries) {
		auto* node = GetObjectAtPath(data, entry->settingPath, true);
		if (node)
			(*node)[entry->settingKey] = entry->value;
	}

	std::ofstream f(path);
	if (!f.is_open()) {
		logger::error("[SceneSettings] WriteGroupedOverwriteFile: could not open '{}' for writing", path.string());
		return false;
	}
	f << data.dump(kOverwriteJsonIndent);
	return true;
}

static bool RemoveSettingFromOverwriteFile(const std::filesystem::path& path,
	const std::vector<std::string>& settingPath, const std::string& settingKey)
{
	if (path.empty())
		return true;

	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return !ec;

	std::ifstream in(path);
	if (!in.is_open()) {
		logger::error("[SceneSettings] Could not open overwrite file '{}' for editing", path.string());
		return false;
	}

	auto data = json::parse(in, nullptr, false);
	if (!data.is_object()) {
		logger::error("[SceneSettings] Could not parse overwrite file '{}' for editing", path.string());
		return false;
	}

	if (auto* node = GetObjectAtPath(data, settingPath, false))
		node->erase(settingKey);
	if (CountSceneOverwriteKeys(data) == 0) {
		auto removed = std::filesystem::remove(path, ec);
		if (removed || !ec)
			return true;
		logger::error("[SceneSettings] Failed to delete overwrite file '{}': {}", path.string(), ec.message());
		return false;
	}

	std::ofstream out(path);
	if (!out.is_open()) {
		logger::error("[SceneSettings] Could not open overwrite file '{}' for writing", path.string());
		return false;
	}
	out << data.dump(kOverwriteJsonIndent);
	return true;
}

void SceneSettingsManager::ExportUserSettingsToOverwrites(SceneType type, const std::vector<size_t>& indices, const std::string& modName)
{
	auto& vec = GetEntriesMut(type);
	auto basePath = GetOverwritesPath(type);
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return;

	std::map<std::pair<std::filesystem::path, std::string>, std::vector<const SettingEntry*>> groupedEntries;
	for (auto idx : indices) {
		if (idx >= vec.size() || vec[idx].source != EntrySource::User)
			continue;
		auto& e = vec[idx];
		auto dir = (type == SceneType::TimeOfDay && e.period != TimeOfDayPeriod::Count) ? basePath / GetPeriodName(e.period) : basePath;
		groupedEntries[{ dir, e.featureShortName }].push_back(&e);
	}

	for (const auto& [group, grouped] : groupedEntries) {
		const auto& [dir, featureShortName] = group;
		auto typeDescription = GetSceneOverwriteTypeDescription(type, grouped.front()->period);
		WriteGroupedOverwriteFile(dir / std::format("{}_{}.json", safeModName, featureShortName), featureShortName, typeDescription, grouped);
	}
}

void SceneSettingsManager::ExportWeatherUserSettingsToOverwrites(RE::FormID weatherId, const std::vector<size_t>& indices, const std::string& modName)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto& vec = GetWeatherConfigMut(weatherId).entries;
	auto baseDir = GetWeatherOverwritesDir() / Util::FormIdToSpid(weatherId);
	auto safeModName = Util::FileHelpers::SanitizeFileName(modName);
	if (safeModName.empty())
		return;

	std::map<std::pair<std::filesystem::path, std::string>, std::vector<const SettingEntry*>> groupedEntries;
	for (auto idx : indices) {
		if (idx >= vec.size() || vec[idx].source != EntrySource::User)
			continue;
		auto& e = vec[idx];
		auto dir = (e.period != TimeOfDayPeriod::Count) ? baseDir / GetPeriodName(e.period) : baseDir;
		groupedEntries[{ dir, e.featureShortName }].push_back(&e);
	}

	for (const auto& [group, grouped] : groupedEntries) {
		const auto& [dir, featureShortName] = group;
		auto typeDescription = GetWeatherOverwriteTypeDescription(grouped.front()->period);
		WriteGroupedOverwriteFile(dir / std::format("{}_{}.json", safeModName, featureShortName), featureShortName, typeDescription, grouped);
	}
}

void SceneSettingsManager::UpdateEntryValue(SceneType type, size_t index, const json& newValue, bool deferSave)
{
	auto& vec = GetEntriesMut(type);
	if (index >= vec.size())
		return;

	if (type == SceneType::TimeOfDay) {
		if (!IsNumericValue(newValue)) {
			logger::warn("[SceneSettings] UpdateEntryValue: rejecting non-float TOD value for {}",
				GetSettingLogName(vec[index].featureShortName, vec[index].settingPath, vec[index].settingKey));
			return;
		}
		float floatVal = newValue.get<float>();
		if (!std::isfinite(floatVal)) {
			logger::warn("[SceneSettings] UpdateEntryValue: rejecting non-finite TOD value ({}) for {}",
				floatVal, GetSettingLogName(vec[index].featureShortName, vec[index].settingPath, vec[index].settingKey));
			return;
		}
		vec[index].value = newValue;
	} else {
		vec[index].value = newValue;
	}

	if (!deferSave && vec[index].source == EntrySource::User)
		SaveAllUserSettings();

	// For TimeOfDay, recompute blended values; for others, apply directly
	if (type == SceneType::TimeOfDay) {
		if (isTimeOfDayActive) {
			// Reset the hour throttle so a user edit (e.g. slider drag) is
			// applied immediately rather than waiting for the game clock to advance.
			lastBlendedHour = -1.0f;
			ApplyTimeOfDayBlended();
			if (isWeatherSceneActive) {
				lastAppliedWeatherFloats.clear();
				UpdateWeatherScene();
			}
		}
	} else if (isCurrentlyApplied && !vec[index].paused && !IsFeaturePaused(vec[index].featureShortName)) {
		if (vec[index].source == EntrySource::Overwrite ||
			!HasActiveOverwrite(type, vec[index].featureShortName, vec[index].settingPath, vec[index].settingKey)) {
			SettingUpdatesByFeature updatesByFeature;
			QueueSettingUpdate(updatesByFeature, vec[index]);
			ApplySettingUpdates(updatesByFeature);
		}
	}
}

void SceneSettingsManager::CommitSceneSettingChanges()
{
	SaveAllUserSettings();
	ReapplyIfActive();
}

// --- Event Handler ---

RE::BSEventNotifyControl SceneSettingsManager::MenuOpenCloseEventHandler::ProcessEvent(
	const RE::MenuOpenCloseEvent* a_event,
	RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME && !a_event->opening) {
		// Defer cell transition to next frame — cell data isn't available yet
		// when this event fires. Same pattern as Skylighting::queuedResetSkylighting.
		GetSingleton()->queuedCellTransition = true;
	}

	return RE::BSEventNotifyControl::kContinue;
}

// --- Scene Application ---

void SceneSettingsManager::Update()
{
	// Revert all overrides on main/loading menu to prevent stale state
	bool isMainOrLoading = globals::state->isMainMenuOpen || globals::state->isLoadingMenuOpen;
	if (isMainOrLoading) {
		if (isCurrentlyApplied) {
			RevertToExteriorSettings();
			isCurrentlyApplied = false;
		}
		if (isTimeOfDayActive)
			DeactivateTimeOfDay();
		if (isWeatherSceneActive)
			DeactivateWeatherScene();
		return;
	}

	if (queuedCellTransition) {
		queuedCellTransition = false;
		OnCellTransition();
	}

	// Continuously update time-of-day blended values when exterior
	if (isTimeOfDayActive)
		UpdateTimeOfDay();

	// Layer per-weather scene overrides on top of global TOD
	if (!isCurrentlyApplied)
		UpdateWeatherScene();
}

void SceneSettingsManager::OnCellTransition()
{
	// Use cell-based check; sky mode is unreliable for mods (DIAL, DWS) that enable kUseSkyLighting in interiors
	bool interior = Util::IsInterior();

	if (interior) {
		// Entering interior: deactivate TOD and weather scene, then apply interior overrides
		if (isTimeOfDayActive)
			DeactivateTimeOfDay();
		if (isWeatherSceneActive)
			DeactivateWeatherScene();
		if (!isCurrentlyApplied) {
			SaveExteriorSettings(SceneType::InteriorOnly);
			ApplyActiveSettings(SceneType::InteriorOnly);
			isCurrentlyApplied = true;
		}
	} else {
		// Entering exterior: revert interior overrides, then activate TOD
		if (isCurrentlyApplied) {
			RevertToExteriorSettings();
			isCurrentlyApplied = false;
		}
		if (!isTimeOfDayActive)
			ActivateTimeOfDay();
	}
}

void SceneSettingsManager::ReapplyIfActive()
{
	if (isCurrentlyApplied) {
		RevertToExteriorSettings();
		SaveExteriorSettings(SceneType::InteriorOnly);
		ApplyActiveSettings(SceneType::InteriorOnly);
	}

	// Use cell-based check consistent with OnCellTransition (DIAL/DWS compatible)
	bool isExterior = !Util::IsInterior();

	bool hasActiveEntries = HasActiveEntries(SceneType::TimeOfDay);

	if (isTimeOfDayActive) {
		if (hasActiveEntries) {
			// Re-blend with updated entries
			RevertTimeOfDayBaseline();
			SaveTimeOfDayBaseline();
			ApplyTimeOfDayBlended();
		} else {
			// All entries removed — deactivate
			DeactivateTimeOfDay();
		}
	} else if (isExterior && hasActiveEntries && !isCurrentlyApplied) {
		// User added first TOD entry while already in an exterior — activate now
		ActivateTimeOfDay();
	}

	if (isWeatherSceneActive) {
		// Revert and force-reapply so pause state changes take effect immediately
		RevertWeatherBaseline();
		if (isTimeOfDayActive)
			UpdateTimeOfDay();
		SaveWeatherBaseline();
		UpdateWeatherScene();
	}
}

bool SceneSettingsManager::HasActiveSettingsForFeature(const std::string& featureShortName) const
{
	if (!isCurrentlyApplied && !isTimeOfDayActive && !isWeatherSceneActive)
		return false;

	for (const auto& [type, vec] : entries) {
		// Only report entries from scene types that are currently active.
		// InteriorOnly entries should not show as active when in an exterior,
		// and TimeOfDay entries should not show as active when in an interior.
		if (type == SceneType::InteriorOnly && !isCurrentlyApplied)
			continue;
		if (type == SceneType::TimeOfDay && !isTimeOfDayActive)
			continue;

		for (const auto& entry : vec) {
			if (!entry.paused && entry.featureShortName == featureShortName)
				return true;
		}
	}

	// Check weather scene settings
	if (isWeatherSceneActive) {
		for (RE::FormID wId : { lastCurrentWeatherId, lastLastWeatherId }) {
			if (wId == 0)
				continue;
			auto wit = weatherSceneConfigs.find(wId);
			if (wit == weatherSceneConfigs.end())
				continue;
			for (const auto& e : wit->second.entries)
				if (!e.paused && e.featureShortName == featureShortName)
					return true;
		}
	}

	return false;
}

bool SceneSettingsManager::IsActiveSceneSetting(std::string_view featureShortName,
	std::string_view settingPath, std::string_view settingKey) const
{
	return IsActiveSceneSetting(std::string(featureShortName), SplitCatalogPath(settingPath), std::string(settingKey));
}

bool SceneSettingsManager::IsActiveSceneSetting(const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey) const
{
	if (!isCurrentlyApplied && !isTimeOfDayActive && !isWeatherSceneActive)
		return false;

	auto hasActiveEntry = [&](SceneType type) {
		for (const auto& entry : GetEntries(type)) {
			if (IsEntryActive(entry) && IsSameSetting(entry, featureShortName, settingPath, settingKey))
				return true;
		}
		return false;
	};

	if (isCurrentlyApplied && hasActiveEntry(SceneType::InteriorOnly))
		return true;
	if (isTimeOfDayActive && hasActiveEntry(SceneType::TimeOfDay))
		return true;

	if (!isWeatherSceneActive)
		return false;

	for (RE::FormID weatherId : { lastCurrentWeatherId, lastLastWeatherId }) {
		if (weatherId == 0)
			continue;

		auto it = weatherSceneConfigs.find(weatherId);
		if (it == weatherSceneConfigs.end())
			continue;

		for (const auto& entry : it->second.entries)
			if (IsEntryActive(entry) && IsSameSetting(entry, featureShortName, settingPath, settingKey))
				return true;
	}

	return false;
}

SceneSettingsManager::SceneLayerGuard::SceneLayerGuard(SceneSettingsManager& manager) :
	manager(manager)
{
	manager.SuspendSceneLayer();
}

SceneSettingsManager::SceneLayerGuard::~SceneLayerGuard()
{
	manager.ResumeSceneLayer();
}

bool SceneSettingsManager::IsFeaturePaused(const std::string& featureShortName) const
{
	auto it = featurePauseStates.find(featureShortName);
	return it != featurePauseStates.end() && it->second;
}

void SceneSettingsManager::SetFeaturePaused(const std::string& featureShortName, bool paused)
{
	featurePauseStates[featureShortName] = paused;
	ReapplyIfActive();
}

void SceneSettingsManager::SuspendSceneLayer()
{
	if (++sceneLayerSuspendDepth > 1)
		return;

	suspendedInteriorLayer = isCurrentlyApplied;
	suspendedTimeOfDayLayer = isTimeOfDayActive;
	suspendedWeatherLayer = isWeatherSceneActive;

	if (suspendedWeatherLayer) {
		RevertWeatherBaseline();
		isWeatherSceneActive = false;
	}
	if (suspendedTimeOfDayLayer) {
		RevertTimeOfDayBaseline();
		isTimeOfDayActive = false;
		lastDominantPeriod = TimeOfDayPeriod::Count;
	}
	if (suspendedInteriorLayer) {
		RevertToExteriorSettings();
		isCurrentlyApplied = false;
	}
}

void SceneSettingsManager::ResumeSceneLayer()
{
	if (sceneLayerSuspendDepth <= 0) {
		logger::warn("[SceneSettings] ResumeSceneLayer called without a matching suspend");
		sceneLayerSuspendDepth = 0;
		return;
	}
	if (--sceneLayerSuspendDepth > 0)
		return;

	if (suspendedInteriorLayer && HasActiveEntries(SceneType::InteriorOnly)) {
		SaveExteriorSettings(SceneType::InteriorOnly);
		ApplyActiveSettings(SceneType::InteriorOnly);
		isCurrentlyApplied = true;
	}
	if (suspendedTimeOfDayLayer && HasActiveEntries(SceneType::TimeOfDay)) {
		SaveTimeOfDayBaseline();
		isTimeOfDayActive = true;
		lastDominantPeriod = GetDominantPeriod();
		lastBlendedHour = -1.0f;
		ApplyTimeOfDayBlended();
	}
	if (suspendedWeatherLayer) {
		SaveWeatherBaseline();
		isWeatherSceneActive = true;
		lastAppliedWeatherFloats.clear();
		lastCurrentWeatherId = 0;
		lastLastWeatherId = 0;
		lastWeatherLerp = -1.0f;
		lastBlendedWeatherHour = -1.0f;
		UpdateWeatherScene();
	}

	suspendedInteriorLayer = false;
	suspendedTimeOfDayLayer = false;
	suspendedWeatherLayer = false;
}

// --- Apply / Revert ---

void SceneSettingsManager::SaveBaselineForEntries(const std::vector<SettingEntry>& sourceEntries,
	std::map<std::string, json>& outBaseline)
{
	for (const auto& entry : sourceEntries) {
		if (!IsEntryActive(entry))
			continue;

		auto* feature = Feature::FindFeatureByShortName(entry.featureShortName);
		if (!feature)
			continue;

		json& partial = outBaseline[entry.featureShortName];
		if (!partial.is_object())
			partial = json::object();

		auto* node = GetObjectAtPath(partial, entry.settingPath, true);
		if (!node || node->contains(entry.settingKey))
			continue;

		json value;
		if (feature->GetSceneSettingValue(entry.settingPath, entry.settingKey, value))
			(*node)[entry.settingKey] = std::move(value);
	}
}

void SceneSettingsManager::SavePartialBaseline(SceneType type, std::map<std::string, json>& outBaseline)
{
	SaveBaselineForEntries(GetEntries(type), outBaseline);
}

void SceneSettingsManager::SaveExteriorSettings(SceneType type)
{
	SavePartialBaseline(type, savedExteriorSettings);
}

void SceneSettingsManager::ApplyActiveSettings(SceneType type)
{
	SettingUpdatesByFeature updatesByFeature;
	const auto queueSource = [&](EntrySource source) {
		for (const auto& entry : GetEntries(type)) {
			if (entry.source == source && IsEntryActive(entry))
				QueueSettingUpdate(updatesByFeature, entry);
		}
	};

	queueSource(EntrySource::User);
	queueSource(EntrySource::Overwrite);
	ApplySettingUpdates(updatesByFeature);
}

void SceneSettingsManager::RevertFromBaseline(std::map<std::string, json>& baseline)
{
	for (const auto& [shortName, savedKeys] : baseline) {
		auto* feature = Feature::FindFeatureByShortName(shortName);
		if (!feature)
			continue;

		std::vector<SceneSettingUpdate> updates;
		CollectOverwriteEntries(savedKeys, {}, [&](const auto& settingPath, const auto& key, const auto& value) {
			updates.push_back({ settingPath, key, value });
		});
		feature->ApplySceneSettings(updates);
	}
	baseline.clear();
}

void SceneSettingsManager::RevertToExteriorSettings()
{
	RevertFromBaseline(savedExteriorSettings);
}

// --- Time of Day ---

void SceneSettingsManager::ActivateTimeOfDay()
{
	if (isTimeOfDayActive || !HasActiveEntries(SceneType::TimeOfDay))
		return;
	// TOD and InteriorOnly are mutually exclusive — don't activate TOD while
	// interior overrides are applied, as they write to the same feature values.
	if (isCurrentlyApplied) {
		logger::debug("[SceneSettings] Skipping TOD activation — interior overrides are active");
		return;
	}
	SaveTimeOfDayBaseline();
	isTimeOfDayActive = true;
	lastDominantPeriod = GetDominantPeriod();
	ApplyTimeOfDayBlended();
	logger::info("[SceneSettings] Time of Day activated");
}

void SceneSettingsManager::DeactivateTimeOfDay()
{
	if (!isTimeOfDayActive)
		return;
	RevertTimeOfDayBaseline();
	isTimeOfDayActive = false;
	lastDominantPeriod = TimeOfDayPeriod::Count;
	logger::info("[SceneSettings] Time of Day deactivated");
}

void SceneSettingsManager::SaveTimeOfDayBaseline()
{
	SavePartialBaseline(SceneType::TimeOfDay, savedTimeOfDayBaseline);
}

void SceneSettingsManager::RevertTimeOfDayBaseline()
{
	RevertFromBaseline(savedTimeOfDayBaseline);
	lastAppliedTODFloats.clear();
	lastBlendedHour = -1.0f;
}

void SceneSettingsManager::UpdateTimeOfDay()
{
	if (!HasActiveEntries(SceneType::TimeOfDay)) {
		if (isTimeOfDayActive)
			DeactivateTimeOfDay();
		return;
	}
	// Safety: if interior overrides are somehow active while TOD is running,
	// deactivate TOD to prevent conflicting writes to the same feature values.
	if (isCurrentlyApplied) {
		logger::warn("[SceneSettings] TOD was active while interior overrides applied — deactivating TOD");
		DeactivateTimeOfDay();
		return;
	}
	ApplyTimeOfDayBlended();
}

void SceneSettingsManager::ApplyTimeOfDayBlended()
{
	// Throttle: skip the expensive map rebuild + blend when the game hour
	// hasn't moved enough to produce a visible change.  On a hot per-frame
	// path this avoids thousands of string-keyed map operations per second.
	float currentHour = GetCurrentGameHour();
	if (lastBlendedHour >= 0.0f && std::abs(currentHour - lastBlendedHour) < kHourUpdateThreshold)
		return;
	lastBlendedHour = currentHour;

	float factors[kPeriodCount];
	GetTimeOfDayFactors(factors);

	// Inline dominant period computation to avoid a second GetTimeOfDayFactors call
	int bestIdx = 0;
	for (int i = 1; i < kPeriodCount; ++i)
		if (factors[i] > factors[bestIdx])
			bestIdx = i;
	auto dominant = static_cast<TimeOfDayPeriod>(bestIdx);

	// Group active entries by feature, using pointers to avoid JSON copies.
	struct PeriodSlot
	{
		const json* value = nullptr;
		EntrySource source = EntrySource::User;
	};
	// featureShortName -> settingKey -> periodIdx -> resolved slot
	std::map<std::string, std::map<SettingIdentity, std::map<int, PeriodSlot>>> collapsedSettings;
	for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
		if (!IsEntryActive(entry) || entry.period == TimeOfDayPeriod::Count || !IsNumericValue(entry.value))
			continue;
		int pIdx = static_cast<int>(entry.period);
		auto& slot = collapsedSettings[entry.featureShortName][MakeSettingIdentity(entry)][pIdx];
		// First write always wins; Overwrite always supersedes User.
		if (!slot.value || (entry.source == EntrySource::Overwrite && slot.source != EntrySource::Overwrite)) {
			slot.value = &entry.value;
			slot.source = entry.source;
		}
	}

	// Build the final PeriodRef vectors from the collapsed map
	std::map<std::string, std::map<SettingIdentity, std::vector<PeriodRef>>> featureSettings;
	for (auto& [shortName, keyMap] : collapsedSettings) {
		for (auto& [setting, periodMap] : keyMap) {
			auto& refs = featureSettings[shortName][setting];
			refs.reserve(periodMap.size());
			for (auto& [pIdx, slot] : periodMap)
				refs.push_back({ pIdx, slot.value });
		}
	}

	for (auto& [shortName, settingsMap] : featureSettings) {
		std::vector<SceneSettingUpdate> dirtyKeys;

		for (auto& [setting, periodRefs] : settingsMap) {
			const json* baseline = FindTODBaseline(shortName, setting.path, setting.key);
			if (!baseline || !IsNumericValue(*baseline))
				continue;

			float baseVal = baseline->get<float>();
			if (!std::isfinite(baseVal))
				baseVal = 0.0f;

			float result = BlendFloatForPeriods(baseVal, periodRefs, factors, shortName, setting.path, setting.key);

			auto cacheKey = JoinDisplayParts(setting.path, setting.key);
			auto& featureFloats = lastAppliedTODFloats[shortName];
			auto floatIt = featureFloats.find(cacheKey);
			if (floatIt != featureFloats.end() && std::abs(floatIt->second - result) < kBlendEpsilon)
				continue;
			featureFloats[cacheKey] = result;
			if (IsActiveWeatherSetting(shortName, setting.path, setting.key))
				continue;
			dirtyKeys.push_back({ setting.path, setting.key, result });
		}

		if (dirtyKeys.empty())
			continue;

		auto* feature = Feature::FindFeatureByShortName(shortName);
		if (!feature)
			continue;

		feature->ApplySceneSettings(dirtyKeys);
	}

	lastDominantPeriod = dominant;
}

const json* SceneSettingsManager::FindTODBaseline(const std::string& shortName,
	const std::vector<std::string>& settingPath, const std::string& key) const
{
	auto baseIt = savedTimeOfDayBaseline.find(shortName);
	if (baseIt == savedTimeOfDayBaseline.end())
		return nullptr;

	auto* node = GetObjectAtPath(baseIt->second, settingPath);
	if (node && node->contains(key))
		return &(*node)[key];
	return nullptr;
}

float SceneSettingsManager::BlendFloatForPeriods(float baseVal, const std::vector<PeriodRef>& periodRefs,
	const float* factors, const std::string& shortName, const std::vector<std::string>& settingPath, const std::string& key) const
{
	float result = 0.0f;
	float coveredFactor = 0.0f;

	for (auto& pr : periodRefs) {
		float f = factors[pr.periodIdx];
		if (f > 0.0f) {
			if (!IsNumericValue(*pr.value)) {
				logger::warn("SceneSettingsManager: TOD period value for '{}' is not a float - falling back to baseline for this period",
					GetSettingLogName(shortName, settingPath, key));
				continue;  // Don't add to coveredFactor — baseline fills in via (1 - coveredFactor) * baseVal
			}
			float periodVal = pr.value->get<float>();
			if (!std::isfinite(periodVal))
				periodVal = 0.0f;
			result += f * periodVal;
			coveredFactor += f;
		}
	}

	return result + (1.0f - coveredFactor) * baseVal;
}

float SceneSettingsManager::BlendFloatForWeatherPeriods(float baseVal, const std::vector<PeriodRef>& periodRefs,
	const float* factors, const std::string& shortName, const std::vector<std::string>& settingPath, const std::string& key) const
{
	float result = 0.0f;

	for (int i = 0; i < kPeriodCount; ++i) {
		float f = factors[i];
		if (f <= 0.0f)
			continue;

		const json* periodValue = nullptr;
		for (auto& pr : periodRefs) {
			if (pr.periodIdx == i) {
				periodValue = pr.value;
				break;
			}
		}

		float resolved = GetTimeOfDayPeriodFallbackFloat(baseVal, shortName, settingPath, key, i);
		if (periodValue) {
			if (IsNumericValue(*periodValue)) {
				float value = periodValue->get<float>();
				resolved = std::isfinite(value) ? value : 0.0f;
			} else {
				logger::warn("SceneSettingsManager: weather TOD period value for '{}' is not a float",
					GetSettingLogName(shortName, settingPath, key));
			}
		}
		result += f * resolved;
	}

	return result;
}

// --- Unified Persistence ---

static json EntryToJson(const SceneSettingsManager::SettingEntry& entry)
{
	json item;
	item["feature"] = entry.featureShortName;
	if (!entry.settingPath.empty())
		item["path"] = entry.settingPath;
	item["setting"] = entry.settingKey;
	item["value"] = entry.value;
	item["originalValue"] = entry.originalValue;
	item["paused"] = entry.paused;
	if (entry.period != SceneSettingsManager::TimeOfDayPeriod::Count)
		item["period"] = SceneSettingsManager::GetPeriodName(entry.period);
	return item;
}

static json UserEntriesToArray(const std::vector<SceneSettingsManager::SettingEntry>& entries, bool transitionOnly = false)
{
	json arr = json::array();
	for (const auto& entry : entries)
		if (entry.source == SceneSettingsManager::EntrySource::User &&
			(!transitionOnly || IsNumericValue(entry.value)))
			arr.push_back(EntryToJson(entry));
	return arr;
}

static void AppendRawEntries(json& arr, const std::vector<json>& rawEntries)
{
	if (!arr.is_array())
		arr = json::array();
	for (const auto& raw : rawEntries)
		arr.push_back(raw);
}

static void MergePreservedWeatherSettings(json& weatherObj, const json& preserved)
{
	if (!preserved.is_object())
		return;

	for (const auto& [spid, rawWeather] : preserved.items()) {
		if (!rawWeather.is_object())
			continue;
		auto& target = weatherObj[spid];
		if (!target.is_object()) {
			target = rawWeather;
			continue;
		}
		if (rawWeather.contains("entries") && rawWeather["entries"].is_array())
			AppendRawEntries(target["entries"], rawWeather["entries"].get<std::vector<json>>());
	}
}

void SceneSettingsManager::SaveAllUserSettings()
{
	const bool weatherLoaded = TryEnsureWeatherDataLoaded();

	auto path = GetUserSettingsFilePath();
	Util::FileHelpers::EnsureDirectoryExists(path.parent_path());

	json data = json::object();
	if (!weatherLoaded) {
		std::ifstream existingFile(path);
		if (existingFile.is_open()) {
			auto existingData = json::parse(existingFile, nullptr, false);
			if (existingData.is_object() && existingData.contains("weather"))
				data["weather"] = existingData["weather"];
		}
	}

	data["interiorOnly"] = UserEntriesToArray(GetEntries(SceneType::InteriorOnly));
	AppendRawEntries(data["interiorOnly"], unresolvedUserEntries[SceneType::InteriorOnly]);
	data["timeOfDay"] = UserEntriesToArray(GetEntries(SceneType::TimeOfDay), true);
	AppendRawEntries(data["timeOfDay"], unresolvedUserEntries[SceneType::TimeOfDay]);

	// Weather entries (keyed by SPID)
	if (weatherLoaded) {
		json weatherObj = json::object();
		for (const auto& [weatherId, config] : weatherSceneConfigs) {
			bool hasUserEntries = std::any_of(config.entries.begin(), config.entries.end(),
				[](const SettingEntry& e) { return e.source == EntrySource::User && IsNumericValue(e.value); });

			auto showIt = weatherShowTimeOfDay_.find(weatherId);
			bool hasShowPref = showIt != weatherShowTimeOfDay_.end() && showIt->second;

			if (!hasUserEntries && !hasShowPref)
				continue;

			auto spid = Util::FormIdToSpid(weatherId);
			json weatherEntry = json::object();
			if (hasShowPref)
				weatherEntry["showTimeOfDay"] = true;
			weatherEntry["entries"] = UserEntriesToArray(config.entries, true);
			weatherObj[spid] = std::move(weatherEntry);
		}
		MergePreservedWeatherSettings(weatherObj, unresolvedWeatherUserSettings);
		data["weather"] = std::move(weatherObj);
	} else if (!data.contains("weather")) {
		data["weather"] = json::object();
	}

	try {
		std::ofstream file(path);
		if (file.is_open()) {
			file << data.dump(2);
			if (file.fail())
				logger::error("[SceneSettings] Write error saving SceneManager.json (disk full or permissions issue)");
			else
				logger::info("[SceneSettings] Saved SceneManager.json");
		}
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to save SceneManager.json: {}", e.what());
	}
}

static bool LoadEntryFromJson(const nlohmann::json& item, SceneSettingsManager::SettingEntry& entry,
	bool requirePeriod, const char* typeName)
{
	using SSM = SceneSettingsManager;

	if (!item.contains("feature") || !item.contains("setting") || !item.contains("value")) {
		logger::warn("[SceneSettings] {} entry missing feature/setting/value fields", typeName);
		return false;
	}
	if (!item["feature"].is_string() || !item["setting"].is_string()) {
		logger::warn("[SceneSettings] {} entry feature/setting not strings", typeName);
		return false;
	}

	entry.featureShortName = item["feature"].get<std::string>();
	entry.settingPath.clear();
	if (auto it = item.find("path"); it != item.end() && it->is_array()) {
		for (const auto& part : *it) {
			if (!part.is_string())
				return false;
			entry.settingPath.push_back(part.get<std::string>());
		}
	}
	entry.settingKey = item["setting"].get<std::string>();
	entry.value = item["value"];
	entry.originalValue = item.value("originalValue", entry.value);
	entry.paused = (item.contains("paused") && item["paused"].is_boolean()) ? item["paused"].get<bool>() : false;
	entry.source = SSM::EntrySource::User;

	auto sceneType = requirePeriod ? SSM::SceneType::TimeOfDay : SSM::SceneType::InteriorOnly;
	if (!SSM::IsFeatureAllowedForType(sceneType, entry.featureShortName)) {
		logger::warn("[SceneSettings] {} entry feature '{}' is not allowed for this scene type", typeName, entry.featureShortName);
		return false;
	}

	if (requirePeriod) {
		if (!item.contains("period") || !item["period"].is_string()) {
			logger::warn("[SceneSettings] {} entry {}.{} missing period — skipping", typeName, entry.featureShortName, entry.settingKey);
			return false;
		}
		entry.period = SSM::GetPeriodFromName(item["period"].get<std::string>());
		if (entry.period == SSM::TimeOfDayPeriod::Count) {
			logger::warn("[SceneSettings] {} entry {}.{} has invalid period '{}' — skipping", typeName, entry.featureShortName, entry.settingKey, item["period"].get<std::string>());
			return false;
		}
		if (!IsNumericValue(entry.value) || !IsNumericValue(entry.originalValue)) {
			logger::warn("[SceneSettings] {} entry {} is not a float setting - skipping",
				typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
		if (!std::isfinite(entry.value.get<float>())) {
			logger::warn("[SceneSettings] {} entry {} has non-finite value - skipping",
				typeName, GetSettingLogName(entry.featureShortName, entry.settingPath, entry.settingKey));
			return false;
		}
	}

	if (!ValidateSceneSettingEntry(typeName, entry.featureShortName, entry.settingPath, entry.settingKey, entry.value, requirePeriod))
		return false;

	entry.displayName = GetSceneSettingDisplayName(entry.featureShortName, entry.settingPath, entry.settingKey);
	return true;
}

void SceneSettingsManager::LoadAllUserSettings()
{
	auto path = GetUserSettingsFilePath();
	logger::info("[SceneSettings] Loading user settings from: {}", path.string());
	unresolvedUserEntries[SceneType::InteriorOnly].clear();
	unresolvedUserEntries[SceneType::TimeOfDay].clear();
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) {
		logger::info("[SceneSettings] SceneManager.json not found at {}", path.string());
		return;
	}

	try {
		std::ifstream file(path);
		if (!file.is_open())
			return;

		json data = json::parse(file);

		// Interior Only
		if (data.contains("interiorOnly") && data["interiorOnly"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::InteriorOnly);
			int loaded = 0;
			for (const auto& item : data["interiorOnly"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, false, "InteriorOnly")) {
					unresolvedUserEntries[SceneType::InteriorOnly].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::InteriorOnly, entry.featureShortName, entry.settingPath, entry.settingKey, EntrySource::User, entry.period))
					continue;
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} InteriorOnly user settings", loaded);
		}

		// Time of Day
		if (data.contains("timeOfDay") && data["timeOfDay"].is_array()) {
			auto& vec = GetEntriesMut(SceneType::TimeOfDay);
			int loaded = 0;
			for (const auto& item : data["timeOfDay"]) {
				SettingEntry entry;
				if (!LoadEntryFromJson(item, entry, true, "TimeOfDay")) {
					unresolvedUserEntries[SceneType::TimeOfDay].push_back(item);
					continue;
				}
				if (HasDuplicateEntry(SceneType::TimeOfDay, entry.featureShortName, entry.settingPath, entry.settingKey, EntrySource::User, entry.period))
					continue;
				vec.push_back(std::move(entry));
				loaded++;
			}
			if (loaded > 0)
				logger::info("[SceneSettings] Loaded {} TimeOfDay user settings", loaded);
		}

		// Weather is loaded lazily once game data is available for SPID resolution.

		logger::info("[SceneSettings] Loaded SceneManager.json (non-weather)");
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load SceneManager.json: {}", e.what());
	}
}

void SceneSettingsManager::LoadWeatherUserSettings()
{
	auto path = GetUserSettingsFilePath();
	unresolvedWeatherUserSettings = json::object();
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return;

	try {
		std::ifstream file(path);
		if (!file.is_open())
			return;

		json data = json::parse(file);

		// Weather
		if (data.contains("weather") && data["weather"].is_object()) {
			logger::info("[SceneSettings] Weather section found with {} entries", data["weather"].size());
			for (auto& [spidKey, weatherData] : data["weather"].items()) {
				logger::info("[SceneSettings] Processing weather SPID '{}'", spidKey);
				RE::FormID weatherId = Util::SpidToFormId(spidKey);
				if (weatherId == 0) {
					unresolvedWeatherUserSettings[spidKey] = weatherData;
					logger::warn("[SceneSettings] Weather SPID '{}' could not be resolved — skipping", spidKey);
					continue;
				}
				logger::info("[SceneSettings] Resolved SPID '{}' to FormID 0x{:X}", spidKey, weatherId);

				// Load showTimeOfDay preference
				if (weatherData.contains("showTimeOfDay") && weatherData["showTimeOfDay"].is_boolean())
					weatherShowTimeOfDay_[weatherId] = weatherData["showTimeOfDay"].get<bool>();

				if (!weatherData.contains("entries") || !weatherData["entries"].is_array())
					continue;

				auto& config = GetWeatherConfigMut(weatherId);
				int loaded = 0;
				for (const auto& item : weatherData["entries"]) {
					SettingEntry entry;
					if (!LoadEntryFromJson(item, entry, true, "Weather")) {
						unresolvedWeatherUserSettings[spidKey]["entries"].push_back(item);
						continue;
					}
					if (HasWeatherEntryForPeriod(weatherId, entry.featureShortName, entry.settingPath, entry.settingKey, entry.period, EntrySource::User))
						continue;
					config.entries.push_back(std::move(entry));
					loaded++;
				}
				if (loaded > 0)
					logger::info("[SceneSettings] Loaded {} weather entries for {}", loaded, spidKey);
			}
		}

		logger::info("[SceneSettings] Loaded weather user settings");
	} catch (const std::exception& e) {
		logger::error("[SceneSettings] Failed to load weather user settings: {}", e.what());
	}
}

void SceneSettingsManager::DiscoverOverwrites(SceneType type)
{
	// TimeOfDay has period subfolders; delegate to a shared loader
	if (type == SceneType::TimeOfDay) {
		auto basePath = GetOverwritesPath(type);
		for (int i = 0; i < kPeriodCount; ++i) {
			auto period = static_cast<TimeOfDayPeriod>(i);
			auto periodPath = basePath / GetPeriodName(period);
			DiscoverOverwritesInDir(type, periodPath, period);
		}
		return;
	}

	DiscoverOverwritesInDir(type, GetOverwritesPath(type));
}

static bool ParseOverwriteFileEntries(const std::filesystem::path& filePath,
	SceneSettingsManager::SceneType allowedType, bool requireNumeric,
	std::vector<SceneSettingsManager::SettingEntry>& outEntries)
{
	using SSM = SceneSettingsManager;

	if (std::filesystem::file_size(filePath) > kMaxSceneOverwriteFileSize)
		return false;

	std::ifstream file(filePath);
	if (!file.is_open())
		return false;

	json data = json::parse(file);
	if (!data.is_object())
		return false;

	std::string featureShortName = data.value(kFeatureKey, "");
	if (featureShortName.empty()) {
		auto stem = filePath.stem().string();
		auto lastUnderscore = stem.rfind('_');
		if (lastUnderscore != std::string::npos)
			featureShortName = stem.substr(lastUnderscore + 1);
	}

	auto* featurePtr = Feature::FindFeatureByShortName(featureShortName);
	if (!featurePtr || !SSM::IsFeatureAllowedForType(allowedType, featureShortName))
		return false;

	bool foundAny = false;
	CollectOverwriteEntries(data, {}, [&](const auto& settingPath, const auto& key, const auto& value) {
		if (!ValidateSceneSettingEntry("Overwrite", featureShortName, settingPath, key, value, requireNumeric))
			return;

		SSM::SettingEntry entry;
		entry.featureShortName = featureShortName;
		entry.settingPath = settingPath;
		entry.settingKey = key;
		entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, key);
		entry.value = value;
		entry.originalValue = entry.value;
		entry.source = SSM::EntrySource::Overwrite;
		entry.sourceFilename = filePath.filename().string();
		entry.sourcePath = filePath;
		outEntries.push_back(std::move(entry));
		foundAny = true;
	});
	return foundAny;
}

void SceneSettingsManager::DiscoverOverwritesInDir(SceneType type, const std::filesystem::path& dir, TimeOfDayPeriod period)
{
	auto typeName = GetSceneTypeName(type);

	std::error_code ec;
	if (!std::filesystem::exists(dir, ec))
		return;

	logger::info("[SceneSettings] Discovering {} overwrites in: {}", typeName, dir.string());

	bool requireNumeric = (type == SceneType::TimeOfDay);
	auto& vec = GetEntriesMut(type);
	int filesFound = 0, overwritesLoaded = 0;

	for (const auto& dirEntry : std::filesystem::directory_iterator(dir, ec)) {
		if (ec) {
			logger::error("[SceneSettings] Error iterating {} overwrites: {}", typeName, ec.message());
			break;
		}
		if (!dirEntry.is_regular_file() || dirEntry.path().extension() != ".json")
			continue;

		filesFound++;
		try {
			std::vector<SettingEntry> parsedEntries;
			if (!ParseOverwriteFileEntries(dirEntry.path(), type, requireNumeric, parsedEntries))
				continue;
			for (auto& entry : parsedEntries) {
				entry.period = period;
				if (AddOverwriteEntryIfUnique(vec, std::move(entry), typeName))
					overwritesLoaded++;
			}
		} catch (const std::exception& e) {
			logger::error("[SceneSettings] Failed to load {} overwrite '{}': {}", typeName, dirEntry.path().filename().string(), e.what());
		}
	}

	if (filesFound > 0)
		logger::info("[SceneSettings] {} overwrite scan: {} files, {} loaded", typeName, filesFound, overwritesLoaded);
}

void SceneSettingsManager::LoadAll()
{
	DiscoverOverwrites(SceneType::InteriorOnly);
	DiscoverOverwrites(SceneType::TimeOfDay);
	LoadAllUserSettings();
}

bool SceneSettingsManager::TryEnsureWeatherDataLoaded()
{
	if (weatherDataLoaded)
		return true;
	if (!globals::game::sky || !RE::TESDataHandler::GetSingleton())
		return false;

	weatherDataLoaded = true;
	LoadWeatherData();
	return true;
}

void SceneSettingsManager::LoadWeatherData()
{
	DiscoverWeatherOverwrites();
	LoadWeatherUserSettings();
}

// --- Per-Weather Scene Settings ---

const SceneSettingsManager::WeatherSceneConfig SceneSettingsManager::kEmptyWeatherConfig{};

const SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return kEmptyWeatherConfig;

	auto it = weatherSceneConfigs.find(weatherId);
	return (it != weatherSceneConfigs.end()) ? it->second : kEmptyWeatherConfig;
}

SceneSettingsManager::WeatherSceneConfig& SceneSettingsManager::GetWeatherConfigMut(RE::FormID weatherId)
{
	return weatherSceneConfigs[weatherId];
}

bool SceneSettingsManager::HasWeatherConfig(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	return it != weatherSceneConfigs.end() && std::any_of(it->second.entries.begin(), it->second.entries.end(),
		[](const auto& entry) { return IsNumericValue(entry.value); });
}

bool SceneSettingsManager::AddWeatherSetting(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value, TimeOfDayPeriod period,
	bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;
	if (!IsFeatureAllowedForType(SceneType::TimeOfDay, featureShortName))
		return false;

	// All weather entries are per-period
	if (period == TimeOfDayPeriod::Count || static_cast<int>(period) < 0 || static_cast<int>(period) >= kPeriodCount)
		return false;
	if (!ValidateSceneSettingEntry("Weather", featureShortName, settingPath, settingKey, value, true))
		return false;
	if (HasWeatherEntryForPeriod(weatherId, featureShortName, settingPath, settingKey, period, EntrySource::User))
		return false;

	auto& config = GetWeatherConfigMut(weatherId);

	SettingEntry entry;
	entry.featureShortName = featureShortName;
	entry.settingPath = settingPath;
	entry.settingKey = settingKey;
	entry.displayName = GetSceneSettingDisplayName(featureShortName, settingPath, settingKey);
	entry.value = value;
	entry.originalValue = value;
	entry.source = EntrySource::User;
	entry.period = period;
	config.entries.push_back(std::move(entry));
	if (!deferSave)
		SaveAllUserSettings();
	return true;
}

void SceneSettingsManager::RemoveWeatherSetting(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	if (entry.source == EntrySource::Overwrite && !entry.sourceFilename.empty() &&
		!RemoveSettingFromOverwriteFile(GetWeatherOverwritePath(weatherId, entry), entry.settingPath, entry.settingKey))
		return;

	it->second.entries.erase(it->second.entries.begin() + static_cast<ptrdiff_t>(index));
	SaveAllUserSettings();
}

void SceneSettingsManager::TogglePauseWeatherEntry(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	it->second.entries[index].paused = !it->second.entries[index].paused;
	SaveAllUserSettings();
}

void SceneSettingsManager::UpdateWeatherEntryValue(RE::FormID weatherId, size_t index, const json& newValue, bool deferSave)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	if (!IsNumericValue(newValue) || !std::isfinite(newValue.get<float>()))
		return;
	it->second.entries[index].value = newValue;
	if (!deferSave)
		SaveAllUserSettings();
}

void SceneSettingsManager::RevertWeatherEntryToDefault(RE::FormID weatherId, size_t index)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end() || index >= it->second.entries.size())
		return;
	auto& entry = it->second.entries[index];
	entry.value = entry.originalValue;
	SaveAllUserSettings();
}

void SceneSettingsManager::DeleteAllWeatherSettings(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it != weatherSceneConfigs.end()) {
		it->second.entries.clear();
		SaveAllUserSettings();
	}
}

bool SceneSettingsManager::HasWeatherEntryForPeriod(RE::FormID weatherId, const std::string& featureShortName,
	const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period, std::optional<EntrySource> source)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherSceneConfigs.find(weatherId);
	if (it == weatherSceneConfigs.end())
		return false;
	for (const auto& e : it->second.entries)
		if (IsSameSetting(e, featureShortName, settingPath, settingKey) && e.period == period &&
			(!source || e.source == *source))
			return true;
	return false;
}

// --- Per-Weather Persistence ---

bool SceneSettingsManager::IsWeatherShowTimeOfDay(RE::FormID weatherId)
{
	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto it = weatherShowTimeOfDay_.find(weatherId);
	return it != weatherShowTimeOfDay_.end() && it->second;
}

void SceneSettingsManager::SetWeatherShowTimeOfDay(RE::FormID weatherId, bool show)
{
	if (!TryEnsureWeatherDataLoaded())
		return;

	weatherShowTimeOfDay_[weatherId] = show;
	SaveAllUserSettings();
}

void SceneSettingsManager::DiscoverWeatherOverwrites()
{
	auto baseDir = GetWeatherOverwritesDir();
	std::error_code ec;
	if (!std::filesystem::exists(baseDir, ec))
		return;

	logger::info("[SceneSettings] Discovering weather overwrites in: {}", baseDir.string());

	for (const auto& dirEntry : std::filesystem::directory_iterator(baseDir, ec)) {
		if (!dirEntry.is_directory())
			continue;

		auto folderName = dirEntry.path().filename().string();
		RE::FormID weatherId = Util::SpidToFormId(folderName);
		if (weatherId == 0) {
			logger::warn("[SceneSettings] Weather overwrite folder '{}' could not be resolved — skipping", folderName);
			continue;
		}

		DiscoverWeatherOverwritesForSpid(weatherId, dirEntry.path());
	}
}

void SceneSettingsManager::DiscoverWeatherOverwritesForSpid(RE::FormID weatherId, const std::filesystem::path& weatherDir)
{
	auto& config = GetWeatherConfigMut(weatherId);

	// Scan period subfolders (TOD entries)
	for (int i = 0; i < kPeriodCount; ++i) {
		auto period = static_cast<TimeOfDayPeriod>(i);
		auto periodDir = weatherDir / GetPeriodName(period);
		std::error_code ec;
		if (!std::filesystem::exists(periodDir, ec))
			continue;

		for (const auto& fileEntry : std::filesystem::directory_iterator(periodDir, ec)) {
			if (!fileEntry.is_regular_file() || fileEntry.path().extension() != ".json")
				continue;

			try {
				std::vector<SettingEntry> parsedEntries;
				if (!ParseOverwriteFileEntries(fileEntry.path(), SceneType::TimeOfDay, true, parsedEntries))
					continue;
				for (auto& entry : parsedEntries) {
					entry.period = period;
					AddOverwriteEntryIfUnique(config.entries, std::move(entry), "weather");
				}
			} catch (const std::exception& e) {
				logger::error("[SceneSettings] Failed to load weather overwrite '{}': {}", fileEntry.path().filename().string(), e.what());
			}
		}
	}

	// Scan files directly in the weather folder (flat → copy to all periods)
	{
		std::error_code ec;
		for (const auto& fileEntry : std::filesystem::directory_iterator(weatherDir, ec)) {
			if (!fileEntry.is_regular_file() || fileEntry.path().extension() != ".json")
				continue;

			try {
				std::vector<SettingEntry> parsedEntries;
				if (!ParseOverwriteFileEntries(fileEntry.path(), SceneType::TimeOfDay, true, parsedEntries))
					continue;
				for (auto& parsed : parsedEntries) {
					for (int p = 0; p < kPeriodCount; ++p) {
						SettingEntry entry = parsed;
						entry.period = static_cast<TimeOfDayPeriod>(p);
						AddOverwriteEntryIfUnique(config.entries, std::move(entry), "weather");
					}
				}
			} catch (const std::exception& e) {
				logger::error("[SceneSettings] Failed to load weather overwrite '{}': {}", fileEntry.path().filename().string(), e.what());
			}
		}
	}
}

// --- Per-Weather Application ---

void SceneSettingsManager::SaveWeatherBaseline()
{
	std::vector<SettingEntry> entriesToSave;
	for (const auto& configPair : weatherSceneConfigs)
		for (const auto& e : configPair.second.entries)
			if (!e.paused && IsNumericValue(e.value))
				entriesToSave.push_back(e);

	savedWeatherBaseline.clear();
	SaveBaselineForEntries(entriesToSave, savedWeatherBaseline);
}

void SceneSettingsManager::RevertWeatherBaseline()
{
	// Invalidate TOD cache for weather-managed features: weather revert may restore a stale value
	// that predates TOD modifications, so TOD must force-reapply to correct it.
	for (const auto& [name, _] : savedWeatherBaseline) {
		lastAppliedTODFloats.erase(name);
	}
	lastBlendedHour = -1.0f;

	RevertFromBaseline(savedWeatherBaseline);
	lastAppliedWeatherFloats.clear();
	lastCurrentWeatherId = 0;
	lastLastWeatherId = 0;
	lastWeatherLerp = -1.0f;
	lastBlendedWeatherHour = -1.0f;
}

void SceneSettingsManager::ActivateWeatherScene()
{
	if (isWeatherSceneActive)
		return;
	// Don't activate while interior overrides are active
	if (isCurrentlyApplied)
		return;
	SaveWeatherBaseline();
	isWeatherSceneActive = true;
	logger::info("[SceneSettings] Weather scene activated");
}

void SceneSettingsManager::DeactivateWeatherScene()
{
	if (!isWeatherSceneActive)
		return;
	RevertWeatherBaseline();
	isWeatherSceneActive = false;
	// Immediately re-run TOD so it corrects keys in this frame, not the next
	if (isTimeOfDayActive)
		UpdateTimeOfDay();
	logger::info("[SceneSettings] Weather scene deactivated");
}

bool SceneSettingsManager::IsActiveWeatherSetting(const std::string& shortName,
	const std::vector<std::string>& settingPath, const std::string& key)
{
	auto sky = globals::game::sky;
	if (!sky || !sky->currentWeather)
		return false;

	if (!TryEnsureWeatherDataLoaded())
		return false;

	auto hasEntry = [&](RE::FormID weatherId) {
		auto it = weatherSceneConfigs.find(weatherId);
		if (it == weatherSceneConfigs.end())
			return false;
		return std::any_of(it->second.entries.begin(), it->second.entries.end(), [&](const auto& entry) {
			return !entry.paused && IsSameSetting(entry, shortName, settingPath, key);
		});
	};

	return hasEntry(sky->currentWeather->GetFormID()) ||
	       (sky->lastWeather && hasEntry(sky->lastWeather->GetFormID()));
}

float SceneSettingsManager::GetTimeOfDayPeriodFallbackFloat(float baseVal, const std::string& shortName,
	const std::vector<std::string>& settingPath, const std::string& key, int periodIdx) const
{
	const json* value = nullptr;
	EntrySource source = EntrySource::User;
	auto period = static_cast<TimeOfDayPeriod>(periodIdx);

	for (const auto& entry : GetEntries(SceneType::TimeOfDay)) {
		if (!IsEntryActive(entry) || entry.period != period ||
			!IsSameSetting(entry, shortName, settingPath, key))
			continue;
		if (!value || (entry.source == EntrySource::Overwrite && source != EntrySource::Overwrite)) {
			value = &entry.value;
			source = entry.source;
		}
	}

	if (!value)
		return baseVal;
	if (!IsNumericValue(*value)) {
		logger::warn("SceneSettingsManager: TOD fallback value for '{}' is not a float",
			GetSettingLogName(shortName, settingPath, key));
		return baseVal;
	}

	float result = value->get<float>();
	return std::isfinite(result) ? result : baseVal;
}

bool SceneSettingsManager::ComputeWeatherBlendedFloat(const std::string& shortName,
	const std::vector<std::string>& settingPath, const std::string& key,
	RE::FormID currentId, RE::FormID lastId, float weatherLerp, float& outValue)
{
	float factors[kPeriodCount];
	GetTimeOfDayFactors(factors);

	auto getBaseValue = [&]() {
		float baseVal = 0.0f;
		auto baseIt = savedWeatherBaseline.find(shortName);
		if (baseIt != savedWeatherBaseline.end()) {
			auto* node = GetObjectAtPath(baseIt->second, settingPath);
			if (node && node->contains(key) && IsNumericValue((*node)[key]))
				baseVal = (*node)[key].get<float>();
		}
		return baseVal;
	};

	// Helper: resolve a weather's value for a setting (always per-period / TOD blend)
	auto resolveWeatherValue = [&](RE::FormID wId, float& result) -> bool {
		auto wit = weatherSceneConfigs.find(wId);
		if (wit == weatherSceneConfigs.end())
			return false;

		struct PeriodSlot
		{
			const json* value = nullptr;
			EntrySource source = EntrySource::User;
		};
		std::array<PeriodSlot, kPeriodCount> slots{};

		for (const auto& e : wit->second.entries) {
			if (e.paused || !IsSameSetting(e, shortName, settingPath, key))
				continue;
			if (e.period == TimeOfDayPeriod::Count || !IsNumericValue(e.value))
				continue;
			auto& slot = slots[static_cast<int>(e.period)];
			if (!slot.value || (e.source == EntrySource::Overwrite && slot.source != EntrySource::Overwrite)) {
				slot.value = &e.value;
				slot.source = e.source;
			}
		}

		std::vector<PeriodRef> refs;
		for (int i = 0; i < kPeriodCount; ++i)
			if (slots[i].value)
				refs.push_back({ i, slots[i].value });
		if (refs.empty())
			return false;

		result = BlendFloatForWeatherPeriods(getBaseValue(), refs, factors, shortName, settingPath, key);
		return true;
	};

	float currentVal = 0.0f, lastVal = 0.0f;
	bool hasCurrent = (currentId != 0) && resolveWeatherValue(currentId, currentVal);
	bool hasLast = (lastId != 0) && resolveWeatherValue(lastId, lastVal);

	if (!hasCurrent && !hasLast)
		return false;

	if (hasCurrent && hasLast) {
		outValue = lastVal + (currentVal - lastVal) * weatherLerp;
	} else {
		float baseVal = getBaseValue();
		if (hasCurrent)
			outValue = baseVal + (currentVal - baseVal) * weatherLerp;
		else
			outValue = lastVal + (baseVal - lastVal) * weatherLerp;
	}
	return true;
}

void SceneSettingsManager::UpdateWeatherScene()
{
	auto sky = globals::game::sky;
	if (!sky || !sky->currentWeather)
		return;

	if (!TryEnsureWeatherDataLoaded())
		return;

	RE::FormID currentId = sky->currentWeather->GetFormID();
	RE::FormID lastId = sky->lastWeather ? sky->lastWeather->GetFormID() : 0;
	float weatherLerp = sky->currentWeatherPct;

	// Check if either weather has scene settings
	bool anyWeatherConfig = HasWeatherConfig(currentId) || (lastId != 0 && HasWeatherConfig(lastId));

	if (!anyWeatherConfig) {
		if (isWeatherSceneActive)
			DeactivateWeatherScene();
		return;
	}

	if (!isWeatherSceneActive) {
		ActivateWeatherScene();
		if (!isWeatherSceneActive)
			return;
	}

	float gameHour = GetCurrentGameHour();

	// Skip if nothing changed (same weathers, same lerp, same game hour within epsilon).
	// The game hour check is critical: weather TOD blending depends on GetTimeOfDayFactors(),
	// so we must re-blend when the hour advances even if the weather transition is stable.
	if (currentId == lastCurrentWeatherId && lastId == lastLastWeatherId &&
		std::abs(weatherLerp - lastWeatherLerp) < kBlendEpsilon &&
		std::abs(gameHour - lastBlendedWeatherHour) < kHourUpdateThreshold)
		return;
	lastCurrentWeatherId = currentId;
	lastLastWeatherId = lastId;
	lastWeatherLerp = weatherLerp;
	lastBlendedWeatherHour = gameHour;

	// Collect all feature+key pairs from both weathers
	std::map<std::string, std::set<SettingIdentity>> affectedKeys;
	auto collectKeys = [&](RE::FormID wId) {
		auto wit = weatherSceneConfigs.find(wId);
		if (wit == weatherSceneConfigs.end())
			return;
		for (const auto& e : wit->second.entries)
			if (!e.paused && IsNumericValue(e.value))
				affectedKeys[e.featureShortName].insert(MakeSettingIdentity(e));
	};
	collectKeys(currentId);
	if (lastId != 0)
		collectKeys(lastId);

	// Blend and apply
	for (auto& [shortName, keys] : affectedKeys) {
		if (IsFeaturePaused(shortName)) {
			lastAppliedWeatherFloats.erase(shortName);  // evict so unpause forces immediate reapply
			continue;
		}

		auto* feature = Feature::FindFeatureByShortName(shortName);
		if (!feature)
			continue;

		std::vector<SceneSettingUpdate> dirtyValues;

		for (auto& setting : keys) {
			float blended = 0.0f;
			if (!ComputeWeatherBlendedFloat(shortName, setting.path, setting.key, currentId, lastId, weatherLerp, blended))
				continue;

			auto cacheKey = JoinDisplayParts(setting.path, setting.key);
			auto& cache = lastAppliedWeatherFloats[shortName];
			auto cacheIt = cache.find(cacheKey);
			if (cacheIt != cache.end() && std::abs(cacheIt->second - blended) < kBlendEpsilon)
				continue;
			cache[cacheKey] = blended;
			dirtyValues.push_back({ setting.path, setting.key, blended });
		}

		if (!dirtyValues.empty())
			feature->ApplySceneSettings(dirtyValues);
	}
}
