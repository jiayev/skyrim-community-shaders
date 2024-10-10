#include "Feature.h"

#include "FeatureVersions.h"
#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ExtendedMaterials.h"
#include "Features/GrassCollision.h"
#include "Features/GrassLighting.h"
#include "Features/LightLimitFix.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/VolumetricLighting.h"
#include "Features/WaterLighting.h"
#include "Features/WetnessEffects.h"

#include "State.h"

void Feature::Load(json& o_json)
{
	if (o_json[GetName()].is_structured())
		LoadSettings(o_json[GetName()]);

	// Convert string to wstring
	auto ini_filename = std::format("{}.ini", GetShortName());
	std::wstring ini_filename_w;
	std::ranges::copy(ini_filename, std::back_inserter(ini_filename_w));
	auto ini_path = L"Data\\Shaders\\Features\\" + ini_filename_w;

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.LoadFile(ini_path.c_str());
	if (auto value = ini.GetValue("Info", "Version")) {
		REL::Version featureVersion(std::regex_replace(value, std::regex("-"), "."));

		auto& minimalFeatureVersion = FeatureVersions::FEATURE_MINIMAL_VERSIONS.at(GetShortName());

		bool oldFeature = featureVersion.compare(minimalFeatureVersion) == std::strong_ordering::less;
		bool majorVersionMismatch = minimalFeatureVersion.major() < featureVersion.major();

		if (!oldFeature && !majorVersionMismatch) {
			loaded = true;
			logger::info("{} {} successfully loaded", ini_filename, value);
		} else {
			loaded = false;

			std::string minimalVersionString = minimalFeatureVersion.string();
			minimalVersionString = minimalVersionString.substr(0, minimalVersionString.size() - 2);

			if (majorVersionMismatch) {
				failedLoadedMessage = std::format("{} {} requires a newer version of community shaders, the feature version should be {}", GetShortName(), value, minimalVersionString);
			} else {
				failedLoadedMessage = std::format("{} {} is an old feature version, required: {}", GetShortName(), value, minimalVersionString);
			}
			logger::warn("{}", failedLoadedMessage);
		}

		version = value;
	} else {
		loaded = false;
		failedLoadedMessage = std::format("{} missing version info; not successfully loaded", ini_filename);
		logger::warn("{}", failedLoadedMessage);
	}
}

void Feature::Save(json& o_json)
{
	SaveSettings(o_json[GetName()]);
}

bool Feature::ValidateCache(CSimpleIniA& a_ini)
{
	auto name = GetName();
	auto ini_name = GetShortName();

	logger::info("Validating {}", name);

	auto enabledInCache = a_ini.GetBoolValue(ini_name.c_str(), "Enabled", false);
	if (enabledInCache && !loaded) {
		logger::info("Feature was uninstalled");
		return false;
	}
	if (!enabledInCache && loaded) {
		logger::info("Feature was installed");
		return false;
	}

	if (loaded) {
		auto versionInCache = a_ini.GetValue(ini_name.c_str(), "Version");
		if (strcmp(versionInCache, version.c_str()) != 0) {
			logger::info("Change in version detected. Installed {} but {} in Disk Cache", version, versionInCache);
			return false;
		} else {
			logger::info("Installed version and cached version match.");
		}
	}

	logger::info("Cached feature is valid");
	return true;
}

void Feature::WriteDiskCacheInfo(CSimpleIniA& a_ini)
{
	auto ini_name = GetShortName();
	a_ini.SetBoolValue(ini_name.c_str(), "Enabled", loaded);
	a_ini.SetValue(ini_name.c_str(), "Version", version.c_str());
}

const std::vector<Feature*>& Feature::GetFeatureList()
{
	static std::vector<Feature*> features = {
		GrassLighting::GetSingleton(),
		GrassCollision::GetSingleton(),
		ScreenSpaceShadows::GetSingleton(),
		ExtendedMaterials::GetSingleton(),
		WetnessEffects::GetSingleton(),
		LightLimitFix::GetSingleton(),
		DynamicCubemaps::GetSingleton(),
		CloudShadows::GetSingleton(),
		WaterLighting::GetSingleton(),
		SubsurfaceScattering::GetSingleton(),
		TerrainShadows::GetSingleton(),
		ScreenSpaceGI::GetSingleton(),
		Skylighting::GetSingleton(),
		TerrainBlending::GetSingleton(),
		VolumetricLighting::GetSingleton()
	};

	static std::vector<Feature*> featuresVR(features);
	std::erase_if(featuresVR, [](Feature* a) {
		return !a->SupportsVR();
	});
	return (REL::Module::IsVR() && !State::GetSingleton()->IsDeveloperMode()) ? featuresVR : features;
}

void Feature::SetBooleanSettings(const std::map<std::string, Feature::gameSetting>& settingsMap, const std::string& featureName, bool a_value)
{
	// Extract first letter from each word in featureName
	std::string logTag;
	bool capitalizeNext = true;
	for (char ch : featureName) {
		if (std::isalpha(ch) && capitalizeNext) {
			logTag += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
			capitalizeNext = false;
		}
		if (std::isspace(ch)) {
			capitalizeNext = true;
		}
	}

	// Initialize collections
	std::vector<std::pair<RE::INISettingCollection*, std::string>> iniCollections = {
		{ RE::INISettingCollection::GetSingleton(), "INISettingCollection" },
		{ RE::INIPrefSettingCollection::GetSingleton(), "INIPrefSettingCollection" }
	};
	auto gameSettingCollection = RE::GameSettingCollection::GetSingleton();

	// Handle INI settings
	for (const auto& [settingName, settingData] : settingsMap) {
		if (settingData.offset == 0) {  // INI-based settings
			bool processed = false;
			for (const auto& [collection, collectionName] : iniCollections) {
				if (auto setting = collection->GetSetting(settingName); setting) {
					if (setting->data.b != a_value) {
						logger::info("[{}] Changing {}:{} from {} to {} to support {}", logTag, collectionName, settingName, setting->data.b, a_value, featureName);
						setting->data.b = a_value;
					}
					processed = true;
					break;  // Exit once the setting is found and processed
				}
			}

			// Handle game settings if not processed by INI collections
			if (!processed) {
				if (auto setting = gameSettingCollection->GetSetting(settingName.data()); setting) {
					if (setting->data.b != a_value) {
						logger::info("[{}] Changing {} from {} to {} to support {}", logTag, settingName, setting->data.b, a_value, featureName);
						setting->data.b = a_value;
					}
				}
			}
		} else {
			// Handle settings with memory offsets
			auto address = REL::Offset{ settingData.offset }.address();
			bool* setting = reinterpret_cast<bool*>(address);
			if (*setting != a_value) {
				logger::info("[{}] Changing {} from {} to {} to support {}", logTag, settingName, *setting, a_value, featureName);
				*setting = a_value;
			}
		}
	}
}

void Feature::EnableBooleanSettings(const std::map<std::string, Feature::gameSetting>& settingsMap, const std::string& featureName)
{
	SetBooleanSettings(settingsMap, featureName, true);
}

void Feature::DisableBooleanSettings(const std::map<std::string, Feature::gameSetting>& settingsMap, const std::string& featureName)
{
	SetBooleanSettings(settingsMap, featureName, false);
}

void Feature::ResetGameSettingsToDefaults(std::map<std::string, gameSetting>& settingsMap)
{
	std::vector<std::pair<RE::INISettingCollection*, std::string>> iniCollections = {
		{ RE::INISettingCollection::GetSingleton(), "INISettingCollection" },
		{ RE::INIPrefSettingCollection::GetSingleton(), "INIPrefSettingCollection" }
	};
	auto gameSettingCollection = RE::GameSettingCollection::GetSingleton();

	for (auto& [settingName, settingData] : settingsMap) {
		char inputTypeChar = settingName[0];

		if (settingData.offset == 0) {  // INI-based settings
			bool processed = false;
			for (const auto& [collection, collectionName] : iniCollections) {
				if (auto setting = collection->GetSetting(settingName); setting) {
					switch (inputTypeChar) {
					case 'b':
						{
							bool currentValue = setting->data.b;
							bool defaultValue = std::get<bool>(settingData.defaultValue);
							if (currentValue != defaultValue) {
								setting->data.b = defaultValue;
								logger::debug("{} Setting {}: changed from {} to default boolean value {}.", collectionName, settingName, currentValue, defaultValue);
							}
						}
						break;
					case 'f':
						{
							float currentValue = setting->data.f;
							float defaultValue = std::get<float>(settingData.defaultValue);
							if (currentValue != defaultValue) {
								setting->data.f = defaultValue;
								logger::debug("{} Setting {}: changed from {} to default float value {}.", collectionName, settingName, currentValue, defaultValue);
							}
						}
						break;
					case 'i':
					case 'u':
						{
							int32_t currentValue = setting->data.i;
							int32_t defaultValue = std::get<int32_t>(settingData.defaultValue);
							if (currentValue != defaultValue) {
								setting->data.i = defaultValue;
								logger::debug("{} Setting {}: changed from {} to default integer value {}.", collectionName, settingName, currentValue, defaultValue);
							}
						}
						break;
					default:
						logger::debug("Unknown type for {} setting {}.", collectionName, settingName);
						break;
					}
					processed = true;
					break;  // Exit once the setting is found and processed
				}
			}

			// Handle game settings if not processed by INI collections
			if (!processed) {
				if (auto setting = gameSettingCollection->GetSetting(settingName.data()); setting) {
					bool currentValue = setting->data.b;
					bool defaultValue = std::get<bool>(settingData.defaultValue);
					if (currentValue != defaultValue) {
						setting->data.b = defaultValue;
						logger::debug("GameSetting {}: changed from {} to default boolean value {}.", settingName, currentValue, defaultValue);
					}
				}
			}
		} else {
			// Handle settings with memory offsets
			auto address = REL::Offset{ settingData.offset }.address();
			switch (inputTypeChar) {
			case 'b':
				{
					bool* ptr = reinterpret_cast<bool*>(address);
					bool currentValue = *ptr;
					bool defaultValue = std::get<bool>(settingData.defaultValue);
					if (currentValue != defaultValue) {
						*ptr = defaultValue;
						logger::debug("Setting {}: changed from {} to default boolean value {}.", settingName, currentValue, defaultValue);
					}
				}
				break;
			case 'f':
				{
					float* ptr = reinterpret_cast<float*>(address);
					float currentValue = *ptr;
					float defaultValue = std::get<float>(settingData.defaultValue);
					if (currentValue != defaultValue) {
						*ptr = defaultValue;
						logger::debug("Setting {}: changed from {} to default float value {}.", settingName, currentValue, defaultValue);
					}
				}
				break;
			case 'i':
			case 'u':
				{
					int32_t* ptr = reinterpret_cast<int32_t*>(address);
					int32_t currentValue = *ptr;
					int32_t defaultValue = std::get<int32_t>(settingData.defaultValue);
					if (currentValue != defaultValue) {
						*ptr = defaultValue;
						logger::debug("Setting {}: changed from {} to default integer value {}.", settingName, currentValue, defaultValue);
					}
				}
				break;
			default:
				logger::debug("Unknown type for setting {}.", settingName);
				break;
			}
		}
	}
}

void Feature::RenderImGuiSettingsTree(const std::map<std::string, gameSetting>& settingsMap, const std::string& treeName)
{
	if (ImGui::TreeNode(treeName.c_str())) {  // Create a tree node
		std::vector<std::pair<RE::INISettingCollection*, std::string>> iniCollections = {
			{ RE::INISettingCollection::GetSingleton(), "INISettingCollection" },
			{ RE::INIPrefSettingCollection::GetSingleton(), "INIPrefSettingCollection" }
		};
		auto gameSettingCollection = RE::GameSettingCollection::GetSingleton();

		for (const auto& [settingName, settingData] : settingsMap) {
			// Handle INI and INIPref settings
			if (settingData.offset == 0) {
				bool found = false;
				for (const auto& [collection, collectionName] : iniCollections) {
					if (auto setting = collection->GetSetting(settingName); setting) {
						RenderImGuiElement(settingName, settingData, setting, collectionName);
						found = true;
						break;  // Exit once the setting is found and processed
					}
				}
				// Handle game settings if not found in INI collections
				if (!found) {
					if (auto gameSetting = gameSettingCollection->GetSetting(settingName.data()); gameSetting) {
						RenderImGuiElement(settingName, settingData, gameSetting, "GameSetting");
						continue;
					}
				}
			} else {
				// Handle settings with offsets (raw memory)
				std::visit([&](auto&& defaultValue) {
					using ValueType = std::decay_t<decltype(defaultValue)>;
					if constexpr (std::is_same_v<ValueType, bool>) {
						bool* ptr = reinterpret_cast<bool*>(REL::Offset{ settingData.offset }.address());
						RenderImGuiElement(settingName, settingData, ptr);
					} else if constexpr (std::is_same_v<ValueType, float>) {
						float* ptr = reinterpret_cast<float*>(REL::Offset{ settingData.offset }.address());
						RenderImGuiElement(settingName, settingData, ptr);
					} else if constexpr (std::is_same_v<ValueType, int32_t>) {
						int32_t* ptr = reinterpret_cast<int32_t*>(REL::Offset{ settingData.offset }.address());
						RenderImGuiElement(settingName, settingData, ptr);
					} else if constexpr (std::is_same_v<ValueType, uint32_t>) {
						uint32_t* ptr = reinterpret_cast<uint32_t*>(REL::Offset{ settingData.offset }.address());
						RenderImGuiElement(settingName, settingData, ptr);
					} else {
						logger::warn("Unsupported type for setting {}", settingName);
					}
				},
					settingData.defaultValue);
			}
		}

		ImGui::TreePop();  // Close the tree node
	}
}

template <typename T>
void Feature::RenderImGuiElement(const std::string& settingName, const gameSetting& settingData, T* valuePtr, const std::string& collectionName)
{
	// Unique ID for each element
	ImGui::PushID(settingName.c_str());  // Use settingName as unique ID
	bool success = false;                // Flag to track if element creation is successful

	if constexpr (std::is_same_v<T, bool>) {
		ImGui::Checkbox(settingData.friendlyName.c_str(), valuePtr);
		success = true;  // Mark as successful
	} else if constexpr (std::is_same_v<T, float>) {
		try {
			auto minFloat = std::get<float>(settingData.minValue);
			auto maxFloat = std::get<float>(settingData.maxValue);
			ImGui::SliderFloat(settingData.friendlyName.c_str(), valuePtr, minFloat, maxFloat);
			success = true;  // Mark as successful
		} catch (const std::bad_variant_access&) {
			logger::warn("Type mismatch for {} {}: expected float for minValue or maxValue but received other type", collectionName, settingName);
		}
	} else if constexpr (std::is_same_v<T, int>) {
		try {
			auto minInt = std::get<int32_t>(settingData.minValue);
			auto maxInt = std::get<int32_t>(settingData.maxValue);
			ImGui::SliderInt(settingData.friendlyName.c_str(), reinterpret_cast<int*>(valuePtr), minInt, maxInt);
			success = true;  // Mark as successful
		} catch (const std::bad_variant_access&) {
			logger::warn("Type mismatch for {} {}: expected int for minValue or maxValue but received other type", collectionName, settingName);
		}
	} else if constexpr (std::is_same_v<T, unsigned int>) {
		try {
			auto minUInt = std::get<uint32_t>(settingData.minValue);  // Use uint32_t for unsigned
			auto maxUInt = std::get<uint32_t>(settingData.maxValue);
			ImGui::SliderScalar(settingData.friendlyName.c_str(), ImGuiDataType_U32, reinterpret_cast<unsigned int*>(valuePtr), &minUInt, &maxUInt);
			success = true;  // Mark as successful
		} catch (const std::bad_variant_access&) {
			logger::warn("Type mismatch for {} {}: expected unsigned int for minValue or maxValue but received other type", collectionName, settingName);
		}
	} else {
		logger::warn("Unsupported type for {} {}: {}", collectionName, settingName, typeid(T).name());
	}

	// Log if element creation failed
	if (!success) {
		logger::debug("Failed to create element for {} {} of type {}", collectionName, settingName, typeid(T).name());
	}

	// Tooltip handling
	if (auto _tt = Util::HoverTooltipWrapper()) {
		std::string hover = "";
		if (settingData.offset != 0)
			hover = std::format("{}\n\nNOTE: CS cannot save this game setting directly. Setting {} '{}' might be able to be saved manually in the ini. Use the Copy button to export to clipboard.", settingData.description, collectionName, settingName);
		else
			hover = settingData.description;
		ImGui::Text(hover.c_str());
	}
	if (settingData.offset != 0) {
		ImGui::SameLine();
		if (ImGui::Button("Copy")) {
			ImGui::SetClipboardText(settingName.c_str());
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(std::format("Copy {} '{}' to clipboard.", collectionName, settingName).c_str());
		}
	}
	ImGui::PopID();  // End unique ID scope
}

void Feature::RenderImGuiElement(const std::string& settingName, const gameSetting& settingData, RE::Setting* setting, const std::string& collectionName)
{
	// Determine the type of the setting
	switch (setting->GetType()) {
	case RE::Setting::Type::kBool:
		RenderImGuiElement(settingName, settingData, &setting->data.b, collectionName);
		break;
	case RE::Setting::Type::kFloat:
		RenderImGuiElement(settingName, settingData, &setting->data.f, collectionName);
		break;
	case RE::Setting::Type::kSignedInteger:
		RenderImGuiElement(settingName, settingData, &setting->data.i, collectionName);
		break;
	case RE::Setting::Type::kUnsignedInteger:
		RenderImGuiElement(settingName, settingData, &setting->data.u, collectionName);
		break;
	case RE::Setting::Type::kColor:
	case RE::Setting::Type::kString:
	case RE::Setting::Type::kUnknown:
	default:
		logger::warn("Unsupported type for {} setting '{}'", collectionName, settingName);
		break;
	}
}

void Feature::SaveGameSettings(const std::map<std::string, gameSetting>& settingsMap)
{
	// Initialize collections
	std::vector<std::pair<RE::INISettingCollection*, std::string>> iniCollections = {
		{ RE::INISettingCollection::GetSingleton(), "INISettingCollection" },
		{ RE::INIPrefSettingCollection::GetSingleton(), "INIPrefSettingCollection" }
	};
	auto gameSettingCollection = RE::GameSettingCollection::GetSingleton();

	// Single iteration for settings
	for (const auto& [settingName, settingData] : settingsMap) {
		// Only process settings without an offset (INI-based settings)
		if (settingData.offset == 0) {  // INI-based settings
			bool processed = false;
			for (const auto& [collection, collectionName] : iniCollections) {
				if (auto setting = collection->GetSetting(settingName); setting) {
					if (collection->WriteSetting(setting)) {
						logger::debug("Saved {} setting {}", collectionName, settingName);
					} else {
						logger::warn("Failed to save {} setting {}", collectionName, settingName);
					}
					processed = true;
					break;  // Exit once the setting is found and processed
				}
			}

			// Handle game settings if not processed by INI collections
			if (!processed) {
				if (auto setting = gameSettingCollection->GetSetting(settingName.data()); setting) {
					if (gameSettingCollection->WriteSetting(setting)) {
						logger::debug("Saved Game setting '{}'", settingName);
					} else {
						logger::warn("Failed to save Game setting {}", settingName);
					}
				} else {
					logger::warn("Setting '{}' not found.", settingName);
				}
			}
		}
	}
}

void Feature::LoadGameSettings(const std::map<std::string, gameSetting>& settingsMap)
{
	// Handle INI and Game settings in a single loop
	std::vector<std::pair<RE::INISettingCollection*, std::string>> iniCollections = {
		{ RE::INISettingCollection::GetSingleton(), "INISettingCollection" },
		{ RE::INIPrefSettingCollection::GetSingleton(), "INIPrefSettingCollection" }
	};

	auto gameSettingCollection = RE::GameSettingCollection::GetSingleton();

	for (const auto& [settingName, settingData] : settingsMap) {
		if (settingData.offset == 0) {  // INI-based or Game settings
			bool found = false;

			// First, check the INI and INIPref collections
			for (const auto& [collection, collectionName] : iniCollections) {
				if (auto setting = collection->GetSetting(settingName); setting) {
					if (collection->ReadSetting(setting)) {
						logger::debug("Loaded {} setting {}", collectionName, settingName);
					} else {
						logger::warn("Failed to load {} setting {}", collectionName, settingName);
					}
					found = true;
					break;  // Exit once setting is found and processed
				}
			}

			// If not found in INI collections, check the game settings collection
			if (!found) {
				if (auto setting = gameSettingCollection->GetSetting(settingName.data()); setting) {
					if (gameSettingCollection->ReadSetting(setting)) {
						logger::debug("Loaded Game setting '{}'", settingName);
					} else {
						logger::warn("Failed to load Game setting {}", settingName);
					}
				} else {
					logger::warn("Setting '{}' not found.", settingName);
				}
			}
		}
	}
}
