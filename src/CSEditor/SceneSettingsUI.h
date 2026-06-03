#pragma once

#include <array>
#include <functional>
#include <map>
#include <tuple>

#include "SceneSettingsManager.h"
#include "Utils/UI.h"

/// Shared UI utilities for scene-settings panels.
namespace SceneSettingsUI
{
	using SceneType = SceneSettingsManager::SceneType;
	using EntrySource = SceneSettingsManager::EntrySource;
	using Period = SceneSettingsManager::TimeOfDayPeriod;
	static constexpr int kPeriodCount = SceneSettingsManager::kPeriodCount;

	/// Unique setting identifier for TOD table ordering.
	struct SettingId
	{
		std::string feature;
		std::vector<std::string> path;
		std::string key;
		std::string displayName;
		std::string categoryName;
		std::vector<std::string> parentPath;
		bool operator<(const SettingId& o) const { return std::tie(feature, path, key) < std::tie(o.feature, o.path, o.key); }
	};

	/// Period-indexed entry map built from a set of entries.
	struct SourceGroup
	{
		std::vector<SettingId> order;
		std::map<SettingId, std::array<size_t, kPeriodCount>> map;
	};

	/// Build a SourceGroup from entries, optionally filtered to a single source.
	SourceGroup BuildSourceGroup(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		EntrySource sourceFilter, bool filterBySource = true, bool transitionOnly = false);

	/// Split entry indices by source (Overwrite vs User).
	void SplitBySource(const std::vector<SceneSettingsManager::SettingEntry>& entries,
		std::vector<size_t>& overwriteOut, std::vector<size_t>& userOut, bool transitionOnly = false);

	/// Remove entries by indices in reverse order.
	void RemoveIndicesReversed(const std::vector<size_t>& indices,
		std::function<void(size_t)> removeFn);

	struct AddSettingNode
	{
		std::map<std::string, AddSettingNode> children;
		std::vector<size_t> settings;
	};

	/// Persistent state for the "+" add-setting dialog.
	struct AddSettingState
	{
		bool dialogOpen = false;
		int selectedFeatureIdx = -1;
		std::vector<std::string> cachedFeatureNames;
		std::vector<SceneSettingDescriptor> cachedSettings;
		std::vector<int> selectedSubFeaturePath;
		AddSettingNode settingTree;
		std::vector<bool> selectedSettings;  // Checkbox state per setting key

		void Reset()
		{
			dialogOpen = false;
			selectedFeatureIdx = -1;
			cachedFeatureNames.clear();
			cachedSettings.clear();
			selectedSubFeaturePath.clear();
			settingTree = {};
			selectedSettings.clear();
		}
	};

	/// Shared confirmation popup state for a panel.
	struct PopupState
	{
		Util::ConfirmationPopup deleteAllOverwrites;
		Util::ConfirmationPopup deleteSingleOverwrite{ "Delete Overwrite File?", "", "Delete" };
		Util::ConfirmationPopup deleteRowOverwrite{ "Delete Overwrite Row?", "", "Delete" };
		Util::ConfirmationPopup deleteAllUser;
		size_t pendingDeleteIndex = SIZE_MAX;
		std::vector<size_t> pendingDeleteRow;

		PopupState(const char* overwriteMsg, const char* userMsg) :
			deleteAllOverwrites("Delete All Overwrites?", overwriteMsg, "Delete All"),
			deleteAllUser("Delete All User Settings?", userMsg, "Delete All") {}
	};

	/// Reset and open the add-setting dialog.
	void OpenAddDialog(SceneType type, AddSettingState& state);
	void OpenWeatherAddDialog(RE::FormID weatherId, AddSettingState& state);

	/// Draw the modal add-setting dialog. Call each frame for each active dialog state.
	void DrawAddSettingDialog(SceneType type, AddSettingState& state,
		Period period = Period::Count, bool addToAllPeriods = false);
	void DrawWeatherAddDialog(RE::FormID weatherId, AddSettingState& state,
		Period period = Period::Count, bool addToAllPeriods = false);

	/// Result from DrawFlyoutControls indicating which action the user triggered.
	struct FlyoutResult
	{
		bool toggled = false;
		bool reverted = false;
		bool deleted = false;
	};

	/// Flyout state for the shared table renderer (one set per table instance).
	struct TableFlyoutState
	{
		Util::FlyoutState cell;  // Per-value-cell flyout
		Util::FlyoutState row;   // Row-level flyout (setting name)
		Util::FlyoutState col;   // Column-header flyout (period names)
	};

	/// Callbacks for the shared table renderer, abstracting manager operations.
	struct TableCallbacks
	{
		std::function<void(size_t idx, float width, bool readOnly)> drawEditor;
		std::function<void(const std::vector<size_t>& indices, float width, bool readOnly)> drawEditorMulti;  // optional: for collapsed single-column mode
		std::function<void(size_t idx)> togglePause;
		std::function<void(size_t idx)> revert;
		std::function<void(size_t idx)> remove;
		// Optional: called when user clicks + in an empty period cell (multi-column only)
		std::function<void(const std::string& feature, const std::vector<std::string>& path, const std::string& key, int period)> onAddPeriod;
	};

	/// Draw flyout controls (toggle + revert + delete). Works for both single and group.
	FlyoutResult DrawFlyoutControls(bool paused, bool isGroup, bool isOverwrite);

	void DrawValueEditor(SceneType type, size_t index, float inputWidth, bool readOnly = false);
	void DrawWeatherValueEditor(RE::FormID weatherId, size_t index, float inputWidth, bool readOnly = false);
	void DrawWeatherValueEditor(RE::FormID weatherId, const std::vector<size_t>& indices, float inputWidth, bool readOnly = false);
	void DrawPopups(SceneType type, PopupState& popups);

	bool DrawSectionHeader(const char* label, const char* idSuffix,
		bool allPaused, std::function<void()> onTogglePause, std::function<void()> onDeleteAll,
		int numValueColumns, std::function<void()> onExportAll = nullptr,
		bool hasActiveOverrides = false);

	/// State for the export-to-overwrites selection popup.
	struct ExportAllPopupState
	{
		static constexpr size_t kModNameBufferSize = 128;

		bool dialogOpen = false;
		std::vector<size_t> userIndices;
		std::vector<uint8_t> selected;
		char modName[kModNameBufferSize] = "";

		void Open(const std::vector<size_t>& indices)
		{
			dialogOpen = true;
			userIndices = indices;
			selected.assign(indices.size(), 1);
		}
	};

	void DrawExportAllPopup(SceneType type, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state);
	void DrawWeatherExportAllPopup(RE::FormID weatherId, const std::vector<SceneSettingsManager::SettingEntry>& entries, ExportAllPopupState& state, bool showTod);

	/// Draw a source table with feature-grouped rows and per-cell value editing.
	/// @param numValueColumns 1 for single-value (Interior), kPeriodCount for TOD.
	/// When numValueColumns == 1, row actions are drawn in a fixed right-side column.
	void DrawSourceTable(
		const SourceGroup& group,
		const std::vector<SceneSettingsManager::SettingEntry>& entries,
		const char* tableId,
		EntrySource source,
		int numValueColumns,
		PopupState* popups,
		TableFlyoutState& flyout,
		const TableCallbacks& cb);

	bool DrawCategoryPanel(const char* category, const std::string& selected,
		void (*drawFn)());

	// --- Consolidated Panel Functions ---

	/// Draw the full Interior Only settings panel.
	void DrawInteriorOnlyPanel();

	/// Draw the full Time of Day settings panel.
	void DrawTimeOfDayPanel();

	/// Draw the per-weather scene settings panel.
	void DrawWeatherScenePanel(RE::FormID weatherId);
}
