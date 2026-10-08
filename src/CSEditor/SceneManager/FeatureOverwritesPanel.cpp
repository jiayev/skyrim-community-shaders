#include "FeatureOverwritesPanel.h"

#include "CSEditor/Browser/BrowserWidgets.h"
#include "Feature.h"
#include "I18n/I18n.h"
#include "IconsFontAwesome5.h"
#include "Menu/Icons/helpers/SceneActionIcons.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SceneSettingsManager.h"
#include "SettingsOverrideManager.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <format>
#include <tuple>
#include <unordered_map>

#define I18N_KEY_PREFIX "feature.scene_manager.overwrites."

namespace
{
	constexpr int kColumnCount = 6;
	constexpr int kActionButtonCount = 2;

	/** @brief One overwrite file as the table shows it, copied so actions that rediscover mid-frame are safe. */
	struct OverwriteRow
	{
		std::string feature;
		std::string featureName;
		std::string modName;
		std::string packId;
		std::string filePath;
		/// Newline-joined keys the feature does not have; while any exist the file is not applied.
		std::string unknownKeys;
		size_t settingCount = 0;
		bool enabled = true;
	};

	SceneSettingsManager::RevisionCache<std::vector<OverwriteRow>> rowCache;
	Util::ConfirmationPopup deletePopup;
	std::string deletePath;
	bool actionFailed = false;
	bool notAppliedOnly = false;

	std::vector<OverwriteRow> BuildRows()
	{
		auto* manager = SettingsOverrideManager::GetSingleton();
		std::vector<OverwriteRow> rows;
		for (const auto& info : manager->GetOverrides()) {
			if (!manager->IsApplicable(info))
				continue;
			auto* feature = info.isGlobal ? nullptr : Feature::FindFeatureByShortName(info.featureName);
			OverwriteRow row{ .feature = feature ? feature->GetDisplayName() : std::string(T(TKEY("global"), "Global")),
				.featureName = info.featureName,
				.modName = info.modName,
				.packId = info.packId,
				.filePath = info.filePath,
				.settingCount = Util::Settings::GetExportSettings(info.featureName, info.overrideData).size(),
				.enabled = info.enabled };
			auto unknownKeys = info.unknownKeys;
			if (feature && unknownKeys.empty()) {
				// Rediscovery clears the verdict until the next load applies the file, so recheck against the live shape.
				json shape;
				feature->SaveSettings(shape);
				Util::Settings::CollectUnknownSettingKeys(info.overrideData, shape, "", unknownKeys);
			}
			for (const auto& key : unknownKeys)
				row.unknownKeys += (row.unknownKeys.empty() ? "" : "\n") + key;
			rows.push_back(std::move(row));
		}
		std::ranges::sort(rows, {}, [](const OverwriteRow& row) { return std::tie(row.feature, row.modName); });
		return rows;
	}

	/** @brief Export button, and a chip narrowing the table to files that are not applied. */
	void DrawToolbar(const std::vector<OverwriteRow>& rows)
	{
		if (Icons::LabeledButton("##export", Icons::FA(ICON_FA_FILE_EXPORT), T(TKEY("export.button"), "Export Settings")))
			FeatureOverwritesPanel::BeginExport();
		Util::AddTooltip(T(TKEY("export.toolbar_tooltip"),
			"Writes settings of one or more features to files of their own: Overrides files loaded at every game start, "
			"or a Baseline preset pack. Scene layers are left out.\n"
			"For a full preset with scene layers, use Export Preset in the CS Editor (Ctrl+Shift+S)."));

		auto count = static_cast<size_t>(std::ranges::count_if(rows, [](const OverwriteRow& row) { return row.enabled && !row.unknownKeys.empty(); }));
		notAppliedOnly &= count != 0;
		if (count == 0)
			return;
		const auto label = std::vformat(T(TKEY("not_applied_count"), "{} not applied"), std::make_format_args(count));
		const ImVec4 warning = Util::Colors::GetWarning();
		BrowserUI::RightAlign(BrowserUI::MeasureChip(label.c_str(), true));
		if (BrowserUI::Chip("##notApplied", label.c_str(), Icons::FA(ICON_FA_TIMES), &warning, notAppliedOnly ? &warning : nullptr))
			notAppliedOnly = !notAppliedOnly;
		Util::AddTooltip(notAppliedOnly ? T("cs_editor.scene_copy_filter_clear_tooltip", "Click to list every setting again.") :
										  T(TKEY("not_applied_tooltip"), "These name settings their feature does not have, so they are skipped. Click to list only them."));
	}

	void DrawEnableToggle(const OverwriteRow& row)
	{
		const bool isPack = !row.packId.empty();
		bool enabled = row.enabled;
		{
			const Util::DisableGuard pack(isPack);
			if (ImGui::Checkbox("##enabled", &enabled))
				actionFailed = !SettingsOverrideManager::GetSingleton()->SetOverrideEnabled(row.filePath, enabled);
		}
		Util::AddTooltip(isPack ? T(TKEY("enable_pack_tooltip"), "Part of a preset pack. Enable or disable the pack from the Presets page.") :
								  T(TKEY("enable_tooltip"), "Loads this overwrite at startup. Changes take effect on the next game start."),
			Util::kTooltipWhenDisabled);
	}

	void DrawSource(const OverwriteRow& row)
	{
		ImGui::TextUnformatted(row.modName.c_str());
		Util::AddTooltip(row.filePath.c_str());
		if (row.packId.empty())
			return;
		BrowserUI::InlineBadge(T(TKEY("pack_badge"), "PACK"), Util::Colors::GetInfo());
		Util::AddTooltip(T(TKEY("preset_pack_tooltip"), "Applied from a Baseline preset pack. Manage it from the Presets page."));
	}

	void DrawStatus(const OverwriteRow& row)
	{
		if (!row.enabled) {
			Util::TextUnformattedDisabled(T(TKEY("status_disabled"), "Disabled"));
			Util::AddTooltip(T(TKEY("status_disabled_tooltip"), "Not loaded from the next game start."));
		} else if (!row.unknownKeys.empty()) {
			ImGui::TextColored(Util::Colors::GetWarning(), "%s", T(TKEY("status_not_applied"), "Not applied"));
			const auto tooltip = std::format("{}\n{}",
				T(TKEY("status_not_applied_tooltip"), "Names settings this feature does not have, so none of it is applied:"), row.unknownKeys);
			Util::AddTooltip(tooltip.c_str());
		} else {
			ImGui::TextColored(Util::Colors::GetSuccess(), "%s", T(TKEY("status_applied"), "Applied"));
		}
	}

	void RequestDelete(const OverwriteRow& row)
	{
		const auto filename = std::filesystem::path(row.filePath).filename().string();
		deletePath = row.filePath;
		deletePopup.title = T(TKEY("delete_title"), "Delete Feature Overwrite?");
		deletePopup.message = std::vformat(T(TKEY("delete_message"), "Delete '{0}' from disk? It stops applying on the next load; values already in use are kept."),
			std::make_format_args(filename));
		deletePopup.confirmLabel = T(TKEY("delete"), "Delete");
		deletePopup.cancelLabel = T(TKEY("cancel"), "Cancel");
		deletePopup.Request();
	}

	/** @brief Edit, then Delete for Overrides files or Open folder for pack files, which the Presets page owns. */
	void DrawActions(const OverwriteRow& row)
	{
		auto* feature = row.featureName.empty() ? nullptr : Feature::FindFeatureByShortName(row.featureName);
		const bool editable = feature && FeatureOverwritesPanel::HasExportableSettings(feature);
		{
			const Util::DisableGuard global(!editable);
			if (BrowserUI::IconButton("##edit", Icons::FA(ICON_FA_PEN),
					editable ? T(TKEY("edit_tooltip"), "Edit: opens the export on this file with its settings ticked.") :
							   T(TKEY("edit_global_tooltip"), "Global overwrites span several features and cannot be edited here.")))
				FeatureOverwritesPanel::BeginExportInto(feature, row.modName, !row.packId.empty());
		}
		ImGui::SameLine();
		if (row.packId.empty()) {
			if (BrowserUI::IconButton("##delete", SceneActionIcons::kDelete, T(TKEY("delete"), "Delete"), false,
					ImGui::GetColorU32(Util::Colors::GetError())))
				RequestDelete(row);
		} else if (BrowserUI::IconButton("##folder", Icons::FA(ICON_FA_FOLDER_OPEN),
					   T(TKEY("open_pack_tooltip"), "Open the pack folder. Remove the pack from the Presets page."))) {
			UnifiedPresetCatalog::GetSingleton().OpenPackFolder(row.packId);
		}
	}

	void DrawRow(const OverwriteRow& row)
	{
		const float rowHeight = BrowserUI::RowHeight();
		ImGui::PushID(row.filePath.c_str());
		ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
		ImGui::TableNextColumn();
		if (row.enabled)
			BrowserUI::RowStripe(ImGui::GetColorU32(row.unknownKeys.empty() ? Util::Colors::GetSuccess() : Util::Colors::GetWarning()), rowHeight);
		DrawEnableToggle(row);

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(row.feature.c_str());

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		DrawSource(row);

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%zu", row.settingCount);

		ImGui::TableNextColumn();
		ImGui::AlignTextToFramePadding();
		DrawStatus(row);

		ImGui::TableNextColumn();
		DrawActions(row);
		ImGui::PopID();
	}

	void DrawTable(const std::vector<OverwriteRow>& rows)
	{
		BrowserUI::RowShadeScope shade;
		if (!ImGui::BeginTable("##OverwriteFiles", kColumnCount, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchProp))
			return;
		const float actionsWidth = BrowserUI::IconButtonSize() * kActionButtonCount + ImGui::GetStyle().ItemSpacing.x * (kActionButtonCount - 1);
		ImGui::TableSetupColumn("##enabled", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("feature"), "Feature"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("source"), "Source"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T(TKEY("settings"), "Settings"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(T(TKEY("status"), "Status"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
		ImGui::TableHeadersRow();
		for (const auto& row : rows)
			if (!notAppliedOnly || (row.enabled && !row.unknownKeys.empty()))
				DrawRow(row);
		ImGui::EndTable();
	}
}

bool FeatureOverwritesPanel::HasExportableSettings(Feature* feature)
{
	assert(feature);
	// The settings schema is fixed per feature, so one SaveSettings probe is enough.
	static std::unordered_map<Feature*, bool> cache;
	auto [it, inserted] = cache.try_emplace(feature, false);
	if (inserted && feature->UsesMainSettings()) {
		json settings;
		feature->SaveSettings(settings);
		it->second = settings.is_object() && !settings.empty();
	}
	return it->second;
}

void FeatureOverwritesPanel::Draw()
{
	auto* manager = SettingsOverrideManager::GetSingleton();
	const auto& rows = rowCache.Get(manager->GetRevision(), BuildRows);

	DrawToolbar(rows);
	if (actionFailed)
		Util::Text::WrappedError("%s", T(TKEY("action_failed"), "Could not update the overwrite. Check the log and try again."));

	if (rows.empty())
		BrowserUI::EmptyState(T(TKEY("empty"), "No feature overwrites yet."),
			T(TKEY("empty_hint"), "Export settings to create one, or install a mod that ships them."));
	else
		DrawTable(rows);

	if (deletePopup.Draw())
		actionFailed = !manager->DeleteFile(deletePath);

	DrawExport();
}

#undef I18N_KEY_PREFIX
