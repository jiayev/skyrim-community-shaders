#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using json = nlohmann::json;

#include "Feature.h"
#include "Globals.h"
#include "Utils/Form.h"

/**
 * @brief Manages scene-specific setting overrides (Interior Only, TimeOfDay).
 *
 * Applies overrides via Feature::SaveSettings/LoadSettings JSON round-trips with
 * epsilon-cached blending to minimise redundant updates during time-of-day transitions.
 * Event-driven: cell transitions detected via MenuOpenCloseEvent, mutations applied immediately.
 */
class SceneSettingsManager
{
public:
	/** @brief Gets the singleton instance. */
	static SceneSettingsManager* GetSingleton()
	{
		static SceneSettingsManager singleton;
		return &singleton;
	}

	// --- Scene Types ---

	enum class SceneType
	{
		InteriorOnly,
		TimeOfDay
	};

	// --- Time of Day Periods ---

	enum class TimeOfDayPeriod
	{
		Dawn = 0,
		Sunrise,
		Day,
		Sunset,
		Dusk,
		Night,
		Count
	};

	/// Number of time-of-day periods (avoids repeated static_cast).
	static constexpr int kPeriodCount = static_cast<int>(TimeOfDayPeriod::Count);

	/// Display names for each period — must match TimeOfDayPeriod order.
	static constexpr std::array<const char*, kPeriodCount> kPeriodNames = {
		"Dawn", "Sunrise", "Day", "Sunset", "Dusk", "Night"
	};

	/// Hour boundaries for each period [start, end).  Night wraps around midnight (21–28 i.e. 21–4).
	static constexpr float kPeriodHours[kPeriodCount][2] = {
		{ 4.0f, 6.0f },    // Dawn
		{ 6.0f, 8.0f },    // Sunrise
		{ 8.0f, 17.0f },   // Day
		{ 17.0f, 19.0f },  // Sunset
		{ 19.0f, 21.0f },  // Dusk
		{ 21.0f, 28.0f }   // Night (wraps past midnight)
	};

	/// Transition blend zone in hours at each period boundary.
	static constexpr float kTransitionHours = 0.5f;

	// --- Event Handler ---

	/**
	 * @brief Listens for LoadingMenu close to detect cell transitions.
	 *
	 * Same pattern as Skylighting::MenuOpenCloseEventHandler.
	 */
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		/** @brief Handles menu open/close events, queuing cell transitions on loading screen close. */
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		/** @brief Registers this handler with the UI event source. */
		static bool Register()
		{
			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;
			if (!ui) {
				logger::error("[SceneSettings] UI event source not found");
				return false;
			}
			auto eventSource = ui->GetEventSource<RE::MenuOpenCloseEvent>();
			if (!eventSource) {
				logger::error("[SceneSettings] MenuOpenCloseEvent source not found");
				return false;
			}
			eventSource->AddEventSink(&singleton);
			logger::info("[SceneSettings] Registered MenuOpenCloseEventHandler");
			return true;
		}
	};

	// --- Setting Entry ---

	enum class EntrySource
	{
		User,      // User-added via UI
		Overwrite  // Loaded from overwrite file
	};

	struct SettingEntry
	{
		std::string featureShortName;          // Feature's GetShortName()
		std::vector<std::string> settingPath;  // Feature-owned subfeature/object path
		std::string settingKey;                // Feature-owned scene setting key
		std::string displayName;               // Cached UI label
		json value;                            // Override value (bool, float, int, etc.)
		json originalValue;                    // Value at time of creation, for revert
		bool paused = false;                   // Temporarily disabled
		EntrySource source = EntrySource::User;
		std::string sourceFilename;                       // For overwrites: the filename it came from
		std::filesystem::path sourcePath;                 // For overwrites: exact file path
		TimeOfDayPeriod period = TimeOfDayPeriod::Count;  // Which period this entry belongs to (TimeOfDay only)
	};

	// --- Generic Entry Management (scene-type agnostic) ---

	/** @brief Gets the read-only entry list for the given scene type. */
	const std::vector<SettingEntry>& GetEntries(SceneType type) const;

	/** @brief Checks whether an entry with the given source already exists for a feature+setting pair. */
	bool HasEntryFromSource(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, EntrySource source) const;

	/** @brief Checks whether an active (non-paused) overwrite entry exists for a feature+setting pair. */
	bool HasActiveOverwrite(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey) const;

	/**
	 * @brief Adds a user setting override, persists it, and reapplies if scene is active.
	 * @param type Scene type to add the setting to.
	 * @param featureShortName Target feature's short name.
	 * @param settingKey JSON key within the feature's settings.
	 * @param value Override value.
	 * @param period For TimeOfDay settings, which period this entry belongs to. Ignored for InteriorOnly.
	 * @param deferCommit If true, skips applying changes immediately (for batched UI edits like sliders).
	 * @return True if the setting was added successfully, false otherwise.
	 */
	bool AddSetting(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count, bool deferCommit = false);

	/** @brief Removes an entry by index, deleting its overwrite file if applicable. */
	void RemoveSetting(SceneType type, size_t index);

	/** @brief Toggles the paused state of an entry by index. */
	void TogglePauseEntry(SceneType type, size_t index);

	/**
	 * @brief Updates an entry's override value.
	 * @param deferSave If true, skips persisting to disk (for batched UI edits like sliders).
	 */
	void UpdateEntryValue(SceneType type, size_t index, const json& newValue, bool deferSave = false);
	void CommitSceneSettingChanges();

	/** @brief Revert an entry's value to its originalValue (captured at creation). */
	void RevertEntryToDefault(SceneType type, size_t index);

	/** @brief Checks if an entry already exists for a specific period (TimeOfDay). */
	bool HasEntryForPeriod(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey,
		TimeOfDayPeriod period, EntrySource source) const;

	/** @brief Pauses or unpauses all overwrite-sourced entries for a scene type. */
	void SetAllOverwritesPaused(SceneType type, bool paused);

	/** @brief Checks whether all overwrites are currently paused for a scene type. */
	bool AreAllOverwritesPaused(SceneType type) const;

	/** @brief Deletes all overwrite entries and their backing files for a scene type. */
	void DeleteAllOverwrites(SceneType type);

	/** @brief Pauses or unpauses all user-sourced entries for a scene type. */
	void SetAllUserPaused(SceneType type, bool paused);

	/** @brief Checks whether all user entries are currently paused for a scene type. */
	bool AreAllUserPaused(SceneType type) const;

	/** @brief Deletes all user-sourced entries for a scene type and persists the change. */
	void DeleteAllUserSettings(SceneType type);

	/// Export selected user entries to grouped per-feature overwrite JSON files.
	void ExportUserSettingsToOverwrites(SceneType type, const std::vector<size_t>& indices, const std::string& modName);
	void ExportWeatherUserSettingsToOverwrites(RE::FormID weatherId, const std::vector<size_t>& indices, const std::string& modName);

	// --- Scene Application ---

	/**
	 * @brief Called each frame from State::Draw() to process deferred cell transitions.
	 *
	 * Cell data is not yet available when the LoadingMenu close event fires,
	 * so the actual transition check is deferred to the next rendered frame.
	 */
	void Update();

	/** @brief Processes a deferred cell transition, applying or reverting interior overrides. */
	void OnCellTransition();

	/** @brief Checks if a specific feature+setting is currently being overridden by any active scene setting. */
	bool IsSettingControlled(const std::string& featureShortName, const std::string& settingKey) const;

	/** @brief Checks if any scene settings are active for a given feature. */
	bool HasActiveSettingsForFeature(const std::string& featureShortName) const;
	bool IsActiveSceneSetting(std::string_view featureShortName,
		std::string_view settingPath, std::string_view settingKey) const;
	bool IsActiveSceneSetting(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey) const;

	/** @brief Checks whether all scene-specific settings are temporarily disabled for a feature. */
	bool IsFeaturePaused(const std::string& featureShortName) const;

	/** @brief Temporarily disables or re-enables all scene-specific settings for a feature. */
	void SetFeaturePaused(const std::string& featureShortName, bool paused);

	class SceneLayerGuard
	{
	public:
		explicit SceneLayerGuard(SceneSettingsManager& manager);
		~SceneLayerGuard();

		SceneLayerGuard(const SceneLayerGuard&) = delete;
		SceneLayerGuard& operator=(const SceneLayerGuard&) = delete;

	private:
		SceneSettingsManager& manager;
	};

	// --- Persistence ---

	/** @brief Persists all user-sourced entries for a scene type to disk as JSON. */
	void SaveAllUserSettings();

	/** @brief Scans the overwrites directory for JSON overwrite files and loads them. */
	void DiscoverOverwrites(SceneType type);

	/** @brief Discover weather-specific overwrite files from Weather/{SPID}/ folders. */
	void DiscoverWeatherOverwrites();

	/** @brief Loads overwrites and user settings for all scene types. */
	void LoadAll();

	// --- Path Resolution ---

	/** @brief Returns the human-readable name for a scene type (e.g. "InteriorOnly"). */
	static std::string GetSceneTypeName(SceneType type);

	/** @brief Returns the JSON file path for user settings of a scene type. */
	static std::filesystem::path GetUserSettingsFilePath();

	/** @brief Returns the directory path where overwrite files are discovered for a scene type. */
	static std::filesystem::path GetOverwritesPath(SceneType type);

	// --- Time of Day Helpers (public for UI) ---

	static const char* GetPeriodName(TimeOfDayPeriod period);
	static TimeOfDayPeriod GetPeriodFromName(const std::string& name);
	static float GetCurrentGameHour();
	void GetTimeOfDayFactors(float outFactors[static_cast<int>(TimeOfDayPeriod::Count)]);
	TimeOfDayPeriod GetDominantPeriod();

	/// Returns the period whose hour range contains the current game hour.
	static TimeOfDayPeriod GetCurrentPeriod();

	// --- Feature Metadata ---

	/** @brief Gets loaded feature short names filtered to only interior-relevant features. */
	static std::vector<std::string> GetInteriorRelevantFeatureNames();

	/** @brief Gets loaded feature short names filtered to exterior/TOD-relevant features. */
	static std::vector<std::string> GetExteriorRelevantFeatureNames();

	/** @brief Check if a feature is allowed for the given scene type (whitelist check). */
	static bool IsFeatureAllowedForType(SceneType type, const std::string& featureShortName);

	/** @brief Get the display name for a feature (e.g. "Screen Space GI" from "ScreenSpaceGI"). */
	static std::string GetFeatureDisplayName(const std::string& featureShortName);

	/** @brief Get scene-safe setting descriptors for a feature. */
	static std::vector<SceneSettingDescriptor> GetFeatureSceneSettings(const std::string& featureShortName);

	/** @brief Get scene-safe float setting descriptors for time/weather blending. */
	static std::vector<SceneSettingDescriptor> GetTransitionableSceneSettings(const std::string& featureShortName);

	/** @brief Get a UI-friendly display label for a setting key. */
	static std::string GetSettingDisplayName(const std::string& settingKey);

	/** @brief Gets the current value of a specific setting from a feature via JSON round-trip. */
	static json GetFeatureSettingValue(const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey);

	/** @brief Classifies JSON value types for scene settings UI rendering. */
	enum class SettingType
	{
		Boolean,
		Integer,
		Float,
		String,
		Unknown
	};

	/** @brief Detects the JSON type of a setting value for UI rendering. */
	static SettingType DetectSettingType(const json& value);

	// --- Per-Weather Scene Settings ---

	/// Per-weather configuration: all entries are per-period (TOD).
	/// The UI flat/TOD toggle is a view-only preference, not a data mode.
	struct WeatherSceneConfig
	{
		std::vector<SettingEntry> entries;
	};

	const WeatherSceneConfig& GetWeatherConfig(RE::FormID weatherId);
	bool HasWeatherConfig(RE::FormID weatherId);

	/// Add a weather setting.  Requires a valid period (all entries are per-period).
	bool AddWeatherSetting(RE::FormID weatherId, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, const json& value, TimeOfDayPeriod period,
		bool deferSave = false);
	void RemoveWeatherSetting(RE::FormID weatherId, size_t index);
	void TogglePauseWeatherEntry(RE::FormID weatherId, size_t index);
	void UpdateWeatherEntryValue(RE::FormID weatherId, size_t index, const json& newValue, bool deferSave = false);
	void RevertWeatherEntryToDefault(RE::FormID weatherId, size_t index);
	void DeleteAllWeatherSettings(RE::FormID weatherId);

	bool HasWeatherEntryForPeriod(RE::FormID weatherId, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey, TimeOfDayPeriod period,
		std::optional<EntrySource> source = std::nullopt);

	/// Weather UI preference: show TOD table vs flat view (view-only, data is always per-period).
	bool IsWeatherShowTimeOfDay(RE::FormID weatherId);
	void SetWeatherShowTimeOfDay(RE::FormID weatherId, bool show);

	static std::filesystem::path GetWeatherOverwritesDir();

private:
	SceneSettingsManager() = default;
	~SceneSettingsManager() = default;
	SceneSettingsManager(const SceneSettingsManager&) = delete;
	SceneSettingsManager& operator=(const SceneSettingsManager&) = delete;

	// --- Per scene-type storage ---
	std::map<SceneType, std::vector<SettingEntry>> entries;
	std::map<SceneType, std::vector<json>> unresolvedUserEntries;
	std::map<SceneType, bool> allOverwritesPausedMap;
	std::map<SceneType, bool> allUserPausedMap;

	// --- Interior state tracking ---
	bool queuedCellTransition = false;
	bool isCurrentlyApplied = false;

	// Stored exterior settings per-feature (only the overridden keys)
	std::map<std::string, json> savedExteriorSettings;

	// --- Time of Day state ---
	bool isTimeOfDayActive = false;
	TimeOfDayPeriod lastDominantPeriod = TimeOfDayPeriod::Count;

	/// Baseline settings saved before TOD activation, for reverting on deactivate.
	std::map<std::string, json> savedTimeOfDayBaseline;

	/// Cache of last-applied blended float values per feature+key.
	/// Used with epsilon comparison to skip redundant LoadSettings calls.
	std::map<std::string, std::map<std::string, float>> lastAppliedTODFloats;

	/// Float epsilon — changes smaller than this skip the LoadSettings call.
	static constexpr float kBlendEpsilon = 1e-3f;

	/// Cached game hour from last blend update.  Used to skip redundant
	/// per-frame map rebuilds when the game hour hasn't moved enough.
	float lastBlendedHour = -1.0f;

	/// Minimum game-hour delta before re-running the blend.  At default
	/// timescale (20×) this equals ~0.36 real seconds — imperceptible yet
	/// saves 98%+ of per-frame map construction work.
	static constexpr float kHourUpdateThreshold = 1e-3f;

	// --- Pause states ---
	std::map<std::string, bool> featurePauseStates;
	int sceneLayerSuspendDepth = 0;
	bool suspendedInteriorLayer = false;
	bool suspendedTimeOfDayLayer = false;
	bool suspendedWeatherLayer = false;

	// --- Per-Weather Scene storage ---
	std::map<RE::FormID, WeatherSceneConfig> weatherSceneConfigs;
	static const WeatherSceneConfig kEmptyWeatherConfig;

	/// UI preference per weather: show TOD table vs flat view (keyed by FormID for fast access).
	std::map<RE::FormID, bool> weatherShowTimeOfDay_;
	json unresolvedWeatherUserSettings = json::object();

	/// Baseline settings saved before weather scene activation, for reverting.
	std::map<std::string, json> savedWeatherBaseline;

	/// Cache of last-applied weather blend values per feature+key.
	std::map<std::string, std::map<std::string, float>> lastAppliedWeatherFloats;

	/// Last weather FormIDs used for blending — detect weather changes.
	RE::FormID lastCurrentWeatherId = 0;
	RE::FormID lastLastWeatherId = 0;
	float lastWeatherLerp = -1.0f;
	float lastBlendedWeatherHour = -1.0f;
	bool isWeatherSceneActive = false;
	bool weatherDataLoaded = false;

	// --- Per-Weather helpers ---
	/// Load weather overwrites/user settings once game data is available for SPID resolution.
	bool TryEnsureWeatherDataLoaded();
	void LoadWeatherData();
	WeatherSceneConfig& GetWeatherConfigMut(RE::FormID weatherId);
	void UpdateWeatherScene();
	void ActivateWeatherScene();
	void DeactivateWeatherScene();
	void SaveWeatherBaseline();
	void RevertWeatherBaseline();

	/// Compute a single float override for a feature+key across two transitioning weathers.
	/// Returns true if an override was computed, with the result in outValue.
	bool ComputeWeatherBlendedFloat(const std::string& shortName,
		const std::vector<std::string>& settingPath, const std::string& key,
		RE::FormID currentId, RE::FormID lastId, float weatherLerp, float& outValue);
	bool IsActiveWeatherSetting(const std::string& shortName, const std::vector<std::string>& settingPath, const std::string& key);
	float GetTimeOfDayPeriodFallbackFloat(float baseVal, const std::string& shortName,
		const std::vector<std::string>& settingPath, const std::string& key, int periodIdx) const;

	// --- Helpers ---
	std::vector<SettingEntry>& GetEntriesMut(SceneType type);
	bool IsEntryActive(const SettingEntry& entry) const;
	bool HasActiveEntries(SceneType type) const;
	bool HasDuplicateEntry(SceneType type, const std::string& featureShortName,
		const std::vector<std::string>& settingPath, const std::string& settingKey,
		EntrySource source, TimeOfDayPeriod period = TimeOfDayPeriod::Count) const;

	void ReapplyIfActive();
	void SuspendSceneLayer();
	void ResumeSceneLayer();
	void ApplyActiveSettings(SceneType type);
	void SaveBaselineForEntries(const std::vector<SettingEntry>& sourceEntries,
		std::map<std::string, json>& outBaseline);
	void SavePartialBaseline(SceneType type, std::map<std::string, json>& outBaseline);
	void RevertFromBaseline(std::map<std::string, json>& baseline);
	void RevertToExteriorSettings();
	void SaveExteriorSettings(SceneType type);

	// --- Time of Day lifecycle ---
	void UpdateTimeOfDay();
	void ActivateTimeOfDay();
	void DeactivateTimeOfDay();
	void SaveTimeOfDayBaseline();
	void RevertTimeOfDayBaseline();
	void ApplyTimeOfDayBlended();

	// --- Time of Day blending helpers ---

	/// Lightweight ref to a TOD period entry, used during blending
	/// to avoid copying JSON values from the entry storage.
	struct PeriodRef
	{
		int periodIdx;
		const json* value;
	};

	/// Look up the saved baseline value for a feature+key pair.
	/// @return Pointer to the baseline JSON, or nullptr if not found.
	const json* FindTODBaseline(const std::string& shortName, const std::vector<std::string>& settingPath, const std::string& key) const;

	/// Compute a weighted blend of float values across active TOD periods.
	/// Uncovered periods fall back to @p baseVal so the sum is always complete.
	float BlendFloatForPeriods(float baseVal, const std::vector<PeriodRef>& periodRefs,
		const float* factors, const std::string& shortName, const std::vector<std::string>& settingPath, const std::string& key) const;
	float BlendFloatForWeatherPeriods(float baseVal, const std::vector<PeriodRef>& periodRefs,
		const float* factors, const std::string& shortName, const std::vector<std::string>& settingPath, const std::string& key) const;

	// --- Overwrite discovery helper ---
	void DiscoverOverwritesInDir(SceneType type, const std::filesystem::path& dir,
		TimeOfDayPeriod period = TimeOfDayPeriod::Count);

	/// Discover overwrite files for a single weather SPID folder.
	void DiscoverWeatherOverwritesForSpid(RE::FormID weatherId, const std::filesystem::path& weatherDir);

	/// Load non-weather user settings from unified SceneManager.json.
	void LoadAllUserSettings();

	/// Load weather user settings from SceneManager.json. Requires TESDataHandler.
	void LoadWeatherUserSettings();
};
