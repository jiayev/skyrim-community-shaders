#pragma once

struct Feature
{
	bool loaded = false;
	std::string version;
	std::string failedLoadedMessage;

	virtual std::string GetName() = 0;
	virtual std::string GetShortName() = 0;
	virtual std::string_view GetShaderDefineName() { return ""; }
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetShaderDefineOptions() { return {}; }

	virtual bool HasShaderDefine(RE::BSShader::Type) { return false; }
	/**
	 * Whether the feature supports VR.
	 * 
	 * \return true if VR supported; else false
	 */
	virtual bool SupportsVR() { return false; }

	virtual void SetupResources() {}
	virtual void Reset() {}

	virtual void DrawSettings() {}
	virtual void Prepass() {}

	virtual void DataLoaded() {}
	virtual void PostPostLoad() {}

	void Load(json& o_json);
	void Save(json& o_json);

	virtual void SaveSettings(json&) {}
	virtual void LoadSettings(json&) {}

	virtual void RestoreDefaultSettings() {}

	virtual bool ValidateCache(CSimpleIniA& a_ini);
	virtual void WriteDiskCacheInfo(CSimpleIniA& a_ini);
	virtual void ClearShaderCache() {}

	static const std::vector<Feature*>& GetFeatureList();

	struct gameSetting
	{
		std::string friendlyName;
		std::string description;
		std::uintptr_t offset;
		std::variant<bool, std::int32_t, std::uint32_t, float> defaultValue;
		std::variant<bool, std::int32_t, std::uint32_t, float> minValue;
		std::variant<bool, std::int32_t, std::uint32_t, float> maxValue;
	};

	/**
	 * @brief Updates boolean settings in the provided map, setting them to true.
	 *        This is intended to be used when features are disabled in Skyrim by default.
	 *        Logs the changes and supports customizing the feature name in the log output.
	 * 
	 * @param settingsMap The map containing settings to update.
	 * @param featureName The name of the feature being enabled, used for logging purposes.
	 */
	void EnableBooleanSettings(const std::map<std::string, Feature::gameSetting>& settingsMap, const std::string& featureName);

	/**
	 * @brief Sets each game setting to its default value if the current value does not match the default.
	 * 
	 * This function iterates through all game settings in the provided map and checks whether each setting's
	 * current value is equal to its default value. If a setting's current value differs from its default,
	 * the function updates the setting to the default value.
	 * 
	 * The function assumes that the `gameSetting` structure contains the following fields:
	 * - `defaultValue`: The default value for the setting.
	 * - `currentValue`: The current value of the setting.
	 * 
	 * The function performs the following steps:
	 * - Iterates over each setting in the `settingsMap`.
	 * - Compares the `currentValue` of the setting to its `defaultValue`.
	 * - If they are not equal, updates `currentValue` to `defaultValue`.
	 * 
	 * This function is useful for resetting settings to their defaults, typically in scenarios where
	 * a reset operation is required, or to ensure consistency of default values across different parts of the application.
	 * 
	 * @param settingsMap A map of setting names to `gameSetting` objects, where each object contains
	 *        the information for a specific game setting. The map is modified in place, with settings
	 *        updated to their default values if necessary.
	 */
	void ResetGameSettingsToDefaults(std::map<std::string, gameSetting>& settingsMap);

	/**
	 * @brief Helper function to render ImGui elements based on variable type inferred from the setting name.
	 *        The first letter of the variable name is used to determine the type (b = bool, f = float, i = int, etc.).
	 * 
	 * @param settingsMap The map of settings to be rendered.
	 * @param tableName A unique identifier for the ImGui tree.
	 */
	void RenderImGuiSettingsTree(const std::map<std::string, gameSetting>& settingsMap, const std::string& tableName);

	/**
	 * @brief Templated function to render an appropriate ImGui element (e.g., checkbox, slider) based on the type.
	 * 
	 * @param settingName The name of the setting.
	 * @param valuePtr Pointer to the value (either from an RE::Setting or direct memory access).
	 */
	template <typename T>
	void RenderImGuiElement(const std::string& settingName, const gameSetting& settingData, T* valuePtr);

	/**
	 * @brief Renders an appropriate ImGui element for RE::Setting data.
	 * 
	 * @param settingName The name of the setting.
	 * @param inputTypeChar Character determining the variable type (b for bool, f for float, i for int).
	 * @param setting Pointer to the RE::Setting.
	 */
	void RenderImGuiElement(const std::string& settingName, const gameSetting& settingData, RE::Setting* setting);

	/**
	 * @brief Saves the provided game settings to the INI file.
	 * 
	 * This function iterates through the settings map, identifies settings that are managed via
	 * the INISettingCollection (i.e., those without an offset), and saves them by calling `WriteSetting`.
	 * 
	 * @param settingsMap The map of game settings.
	 */
	void SaveGameSettings(const std::map<std::string, gameSetting>& settingsMap);

	/**
	 * @brief Loads the provided game settings from the INI file.
	 * 
	 * This function iterates through the settings map, identifies settings that are managed via
	 * the INISettingCollection (i.e., those without an offset), and loads them by calling `ReadSetting`.
	 * 
	 * @param settingsMap The map of game settings.
	 */
	void LoadGameSettings(const std::map<std::string, gameSetting>& settingsMap);
};