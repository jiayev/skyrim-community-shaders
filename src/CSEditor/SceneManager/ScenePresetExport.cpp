#include "ScenePresetExport.h"
#include "SceneSettingsManager.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <Windows.h>
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

#include <imgui.h>
#include <imgui_stdlib.h>

#include "../../I18n/I18n.h"
#include "../EditorWindow.h"
#include "Feature.h"
#include "FeatureOverwritesPanel.h"
#include "Features/Effects11.h"
#include "Features/Effects11/PresetManager.h"
#include "Features/PostProcessing.h"
#include "Menu/PresetsPageRenderer.h"
#include "Presets/PostProcessingPresets.h"
#include "Presets/PresetCompatibility.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SettingsOverrideManager.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/SettingsCatalog.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using PresetExportInfo = SceneSettingsManager::PresetExportInfo;
	using PresetType = SceneSettingsManager::PresetType;

	constexpr const char* kExportPopupId = "##ScenePresetExport";
	constexpr const char* kPickerPopupId = "##ScenePresetExportPicker";

	/// The list scrolls rather than growing past the screen, like the copy preview does.
	constexpr float kModListHeight = 120.0f;
	constexpr float kModalWidth = 560.0f;
	constexpr float kDescriptionLines = 3.0f;
	constexpr float kPickerWidth = 420.0f;
	constexpr float kPickerListHeight = 260.0f;
	/// Rows of the base settings list: the full dialog shares space with the scene options, the simplified one does not.
	constexpr float kFeatureLines = 4.5f;
	constexpr float kSimplifiedFeatureLines = 9.0f;

	/// Files a destructive confirmation names before it stops listing and counts the rest.
	constexpr size_t kMaxListedFiles = 8;

	SceneSettingsManager::RevisionCache<std::vector<std::string>> modListCache;

	const std::vector<std::string>& GetCachedModNames(SceneSettingsManager* manager)
	{
		return modListCache.Get(manager->GetEntryPresentationRevision(),
			[manager] { return manager->GetOverwriteModNames(); });
	}

	/// The name is kept as typed and tags stay unparsed until export.
	PresetExportInfo form;
	std::string presetTags;
	/// Existing relative paths from a prior export of this name (shown when no new pick).
	std::string existingLogo;
	std::string existingCover;
	std::vector<std::string> existingScreenshots;
	std::vector<std::filesystem::path> collidingFiles;
	std::string pickerSearch;

	bool dialogActive = false;
	bool pendingOpen = false;
	bool exportRequested = false;
	/// Also write the base Post Processing settings as the pack's Baseline file: how Post Processing presets are made.
	bool includePostProcessing = false;
	/// Opened from Base Settings: a Baseline export with the form cut down to what a Baseline needs.
	bool simplified = false;
	/// Features whose base settings go into the pack's Baseline folder, ticked by the user.
	std::vector<std::string> baselineFeatures;
	Util::ConfirmationPopup exportConfirmation;

	std::string DescribeCollision(std::string name, const std::vector<std::filesystem::path>& files)
	{
		std::string listed;
		for (size_t index = 0; index < files.size() && index < kMaxListedFiles; ++index)
			listed += std::format("\n  {}", files[index].filename().string());
		if (files.size() > kMaxListedFiles) {
			auto remaining = files.size() - kMaxListedFiles;
			listed += std::vformat(
				T(TKEY("scene_export_replace_more"), "\n  ... and {} more"), std::make_format_args(remaining));
		}

		auto count = files.size();
		return std::vformat(T(TKEY("scene_export_replace_message"),
								 "'{}' already has {} file(s) on disk. Exporting deletes every one of "
								 "them and writes this preset in their place. If another mod owns "
								 "these files, they are gone.{}"),
			std::make_format_args(name, count, listed));
	}

	/** @brief Shows a success or failure notification for an export. */
	void ReportExportResult(std::string name, bool exported)
	{
		auto message = exported ?
		                   std::vformat(T(TKEY("scene_export_result_success"), "Preset '{}' exported."),
						   std::make_format_args(name)) :
		                   std::vformat(T(TKEY("scene_export_result_failure"),
										   "Preset '{}' export failed. Check the log for details."),
						   std::make_format_args(name));
		EditorWindow::GetSingleton()->ShowNotification(
			message, exported ? Util::Colors::GetSuccess() : Util::Colors::GetError());
	}

	/** @brief Draws one preset type choice, selecting it on click. */
	void DrawPresetTypeOption(const char* label, PresetType type)
	{
		if (ImGui::RadioButton(label, form.type == type))
			form.type = type;
	}

	bool IsFeatureRequired(const std::string& shortName)
	{
		return std::find(form.requiredFeatures.begin(), form.requiredFeatures.end(), shortName) !=
		       form.requiredFeatures.end();
	}

	void SetFeatureRequired(const std::string& shortName, bool required)
	{
		const auto it = std::find(form.requiredFeatures.begin(), form.requiredFeatures.end(), shortName);
		if (required && it == form.requiredFeatures.end())
			form.requiredFeatures.push_back(shortName);
		else if (!required && it != form.requiredFeatures.end())
			form.requiredFeatures.erase(it);
	}

	/// Core features ship with every Community Shaders install, so a pack never needs to require them.
	bool IsCoreFeature(const std::string& shortName)
	{
		const auto* feature = Feature::FindFeatureByShortName(shortName);
		return feature && feature->IsCore();
	}

	/// Features the required list offers: loaded, user-facing, and not core.
	bool IsRequirableFeature(const Feature* feature)
	{
		return feature && feature->loaded && feature->IsInMenu() && !feature->IsCore();
	}

	/// Whether the export would carry settings for a feature, from the user or any mod layer.
	bool HasExportedSettings(const std::string& shortName)
	{
		const auto* manager = SceneSettingsManager::GetSingleton();
		return manager && manager->HasAnySceneEntriesForFeature(shortName);
	}

	/// Features whose base settings can go in a Baseline folder. Post Processing is its own option.
	bool IsBaselineCandidate(Feature* feature)
	{
		return feature && feature->loaded && feature->GetShortName() != PostProcessingPresets::kFeatureShortName &&
		       FeatureOverwritesPanel::HasExportableSettings(feature);
	}

	bool HasAnyBaselineCandidate()
	{
		const auto& features = Feature::GetFeatureList();
		return std::ranges::any_of(features, IsBaselineCandidate);
	}

	bool IsBaselineSelected(const std::string& shortName)
	{
		return std::ranges::find(baselineFeatures, shortName) != baselineFeatures.end();
	}

	void SetBaselineSelected(const std::string& shortName, bool selected)
	{
		const auto it = std::ranges::find(baselineFeatures, shortName);
		if (selected && it == baselineFeatures.end())
			baselineFeatures.push_back(shortName);
		else if (!selected && it != baselineFeatures.end())
			baselineFeatures.erase(it);
	}

	/** @brief Every requirable feature the export carries settings for, which a fresh form starts with. */
	std::vector<std::string> DetectRequiredFeatures()
	{
		std::vector<std::string> detected;
		for (auto* feature : Feature::GetFeatureList()) {
			if (!IsRequirableFeature(feature))
				continue;
			auto shortName = feature->GetShortName();
			if (HasExportedSettings(shortName))
				detected.push_back(std::move(shortName));
		}
		return detected;
	}

	/** @brief Clears the form and everything loaded from an existing preset; the type follows the live pipeline. */
	void ResetFormFields()
	{
		form = {};
		const bool e11Ready = globals::features::effects11.loaded &&
		                      PresetManager::GetSingleton().CanExportActivePreset();
		form.type = (e11Ready && globals::state->GetTonemapOwner() == State::TonemapOwner::kEffects11) ?
		                PresetType::E11 :
		                PresetType::CS;
		form.csVersion = PresetCompatibility::CurrentCsVersionString();
		form.requiredFeatures = DetectRequiredFeatures();
		includePostProcessing = false;
		simplified = false;
		baselineFeatures.clear();
		presetTags.clear();
		existingLogo.clear();
		existingCover.clear();
		existingScreenshots.clear();
		collidingFiles.clear();
	}

	/** @brief Fills empty fields from an existing preset and adopts its artwork, dropping any pending picks. */
	void PrefillFromExisting(const SceneSettingsManager::PresetMetadata& meta)
	{
		if (form.author.empty())
			form.author = meta.author;
		if (form.description.empty())
			form.description = meta.description;
		if (presetTags.empty() && !meta.tags.empty()) {
			presetTags.clear();
			for (size_t i = 0; i < meta.tags.size(); ++i) {
				if (i > 0)
					presetTags += ", ";
				presetTags += meta.tags[i];
			}
		}
		if (!meta.version.empty())
			form.version = meta.version;
		if (!meta.csVersion.empty())
			form.csVersion = meta.csVersion;
		// The pack's own list joins the detected one: it may require features this session has no
		// settings for. Core entries from older exports are dropped.
		for (const auto& shortName : meta.requiredFeatures) {
			if (!shortName.empty() && !IsCoreFeature(shortName) && !IsFeatureRequired(shortName))
				form.requiredFeatures.push_back(shortName);
		}
		existingLogo = meta.logo;
		existingCover = meta.cover;
		existingScreenshots = meta.screenshots;
		form.clearLogo = false;
		form.clearCover = false;
		form.clearScreenshots = false;
		form.logoSource.clear();
		form.coverSource.clear();
		form.screenshotSources.clear();
	}

	/** @brief Prefills from the pack the typed name targets, when one exists on disk. */
	void PrefillFromName()
	{
		const auto packId = Util::FileHelpers::SanitizeFileName(form.name);
		// Re-exporting a pack keeps the base settings it already carries unless the user unticks them.
		if (const auto* pack = UnifiedPresetCatalog::GetSingleton().FindPack(packId)) {
			for (const auto& shortName : pack->baselineFeatures) {
				if (shortName == PostProcessingPresets::kFeatureShortName)
					includePostProcessing = true;
				else if (IsBaselineCandidate(Feature::FindFeatureByShortName(shortName)))
					SetBaselineSelected(shortName, true);
			}
		}
		if (const auto meta = SceneSettingsManager::ReadPresetMetadata(Util::PathHelpers::GetUnifiedPackPath(packId)))
			PrefillFromExisting(*meta);
	}

	/** @brief Orphan and legacy Effects 11 installs live outside Presets/, and CS and E11 packs never take the other's export. */
	bool IsExportTarget(const UnifiedPresetCatalog::PackInfo& pack)
	{
		return pack.source == UnifiedPresetCatalog::SourceKind::UnifiedPack && pack.AcceptsExport(form.type);
	}

	/** @brief One pack row: name and type badges over author and version. @return Whether it was clicked. */
	bool DrawPickerRow(const UnifiedPresetCatalog::PackInfo& pack, bool selected)
	{
		ImGui::PushID(pack.id.c_str());
		const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
		const float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
		const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, rowHeight));

		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + ImGui::GetStyle().FramePadding.x, rowOrigin.y));
		ImGui::BeginGroup();
		ImGui::TextUnformatted(pack.name.c_str());
		ImGui::SameLine();
		PresetsPageRenderer::DrawBackendBadges(pack.IsE11(), pack.IsCS(), pack.IsBaseline(), true);
		if (!pack.author.empty() || !pack.version.empty()) {
			const auto meta = pack.author.empty() ? std::format("v{}", pack.version) :
			                  pack.version.empty() ? pack.author :
			                                         std::format("{} · v{}", pack.author, pack.version);
			ImGui::TextDisabled("%s", meta.c_str());
		}
		ImGui::EndGroup();

		// Park the cursor past the row so the next highlight cannot overlap this one.
		ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y + rowHeight + ImGui::GetStyle().ItemSpacing.y));
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
		ImGui::PopID();
		return clicked;
	}

	/** @brief Button that opens the picker, disabled while no pack can be exported into. */
	void DrawPickerButton(const char* label)
	{
		const auto& packs = UnifiedPresetCatalog::GetSingleton().GetPacks();
		ImGui::BeginDisabled(std::ranges::none_of(packs, IsExportTarget));
		if (ImGui::Button(label)) {
			pickerSearch.clear();
			ImGui::OpenPopup(kPickerPopupId);
		}
		ImGui::EndDisabled();
		Util::AddTooltip(T(TKEY("scene_export_existing_tooltip"), "Export into an existing preset."),
			Util::kTooltipWhenDisabled);
	}

	/** @brief Searchable list of the installed packs; picking one targets it as the export name. */
	void DrawPresetPicker()
	{
		const float scale = Util::GetUIScale();
		ImGui::SetNextWindowSize(ImVec2(kPickerWidth * scale, 0.0f), ImGuiCond_Always);
		auto popup = Util::CenteredPopupModal(kPickerPopupId);
		if (!popup || EditorWindow::ClosePopupOnEscape())
			return;

		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##ScenePresetExportPickerSearch", T("menu.presets.search", "Search presets..."), &pickerSearch);

		auto& catalog = UnifiedPresetCatalog::GetSingleton();
		const auto currentId = Util::FileHelpers::SanitizeFileName(form.name);
		const UnifiedPresetCatalog::PackInfo* picked = nullptr;
		if (ImGui::BeginChild("##ScenePresetExportPickerList", ImVec2(0.0f, kPickerListHeight * scale), ImGuiChildFlags_Borders)) {
			bool anyShown = false;
			for (const size_t index : catalog.Query(std::nullopt, pickerSearch)) {
				const auto& pack = catalog.GetPacks()[index];
				if (!IsExportTarget(pack))
					continue;
				anyShown = true;
				if (DrawPickerRow(pack, pack.id == currentId))
					picked = &pack;
			}
			if (!anyShown)
				ImGui::TextDisabled("%s", T(TKEY("scene_export_picker_empty"), "No presets match."));
		}
		ImGui::EndChild();

		if (picked) {
			form.name = picked->id;
			PrefillFromName();
			ImGui::CloseCurrentPopup();
		}
		if (ImGui::Button(T(TKEY("cancel"), "Cancel")))
			ImGui::CloseCurrentPopup();
	}

	/// Compact CS version + feature checklist, sized like the artwork column beside it.
	void DrawCompatibilityBox()
	{
		ImGui::BeginChild("##SceneExportCompat", ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
		ImGui::TextUnformatted(T(TKEY("scene_export_compat"), "Compatibility"));
		Util::AddTooltip(T(TKEY("scene_export_compat_tooltip"),
			"Records which Community Shaders build and features this pack expects. The Presets browser warns when they do not match."));

		ImGui::TextUnformatted(T(TKEY("scene_export_cs_version"), "CS version"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputTextWithHint("##ScenePresetExportCsVersion",
			T(TKEY("scene_export_cs_version_hint"), "e.g. 0.8.0"), &form.csVersion);
		Util::AddTooltip(T(TKEY("scene_export_cs_version_tooltip"),
			"Community Shaders version this pack was made with. Defaults to your current build."));

		ImGui::TextUnformatted(T(TKEY("scene_export_features"), "Required features"));
		const float listHeight = ImGui::GetTextLineHeightWithSpacing() * 5.5f + ImGui::GetStyle().FramePadding.y * 2.0f;
		if (ImGui::BeginChild("##SceneExportFeatures", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
			auto features = Feature::GetFeatureList();
			std::sort(features.begin(), features.end(), [](Feature* a, Feature* b) {
				return a->GetDisplayName() < b->GetDisplayName();
			});
			const char* usedLabel = T(TKEY("scene_export_feature_used"), "has settings");
			for (auto* feature : features) {
				if (!IsRequirableFeature(feature))
					continue;
				const auto shortName = feature->GetShortName();
				bool required = IsFeatureRequired(shortName);
				ImGui::PushID(shortName.c_str());
				if (ImGui::Checkbox(feature->GetDisplayName().c_str(), &required))
					SetFeatureRequired(shortName, required);
				if (HasExportedSettings(shortName)) {
					ImGui::SameLine();
					ImGui::TextDisabled("%s", usedLabel);
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%s",
			I18n::GetSingleton()->Format("cs_editor.scene_export_features_count",
									{ { "count", std::to_string(form.requiredFeatures.size()) } },
									"{count} selected")
				.c_str());
		Util::AddTooltip(T(TKEY("scene_export_features_tooltip"),
			"Features this export has settings for start ticked. Core features ship with every install, so they are not listed."));
		ImGui::EndChild();
	}

	/** @brief Splits on ',' or ';', trimming whitespace and dropping empty tags. */
	std::vector<std::string> ParseTags(const std::string& text)
	{
		std::vector<std::string> tags;
		std::string current;
		const auto flush = [&] {
			while (!current.empty() && std::isspace(static_cast<unsigned char>(current.front())))
				current.erase(current.begin());
			while (!current.empty() && std::isspace(static_cast<unsigned char>(current.back())))
				current.pop_back();
			if (!current.empty())
				tags.push_back(current);
			current.clear();
		};
		for (char ch : text) {
			if (ch == ',' || ch == ';')
				flush();
			else
				current.push_back(ch);
		}
		flush();
		return tags;
	}

	std::string DisplayPath(const std::filesystem::path& path)
	{
		if (path.empty())
			return {};
		return path.filename().string();
	}

	/** @brief Opens the Windows image picker. @return Whether any file was chosen. */
	bool BrowseImageFiles(bool allowMultiple, std::vector<std::filesystem::path>& outPaths)
	{
		outPaths.clear();
		constexpr DWORD kBufferChars = 32768;
		std::wstring buffer(kBufferChars, L'\0');

		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetActiveWindow();
		ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.dds)\0*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.dds\0"
						  L"All Files (*.*)\0*.*\0";
		ofn.lpstrFile = buffer.data();
		ofn.nMaxFile = kBufferChars;
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
		if (allowMultiple)
			ofn.Flags |= OFN_ALLOWMULTISELECT;

		if (!GetOpenFileNameW(&ofn))
			return false;

		const wchar_t* ptr = buffer.c_str();
		std::filesystem::path directory = ptr;
		ptr += directory.native().size() + 1;
		if (!*ptr) {
			// Single selection: buffer is the full path.
			outPaths.push_back(directory);
			return true;
		}
		while (*ptr) {
			outPaths.push_back(directory / ptr);
			ptr += std::wcslen(ptr) + 1;
		}
		return !outPaths.empty();
	}

	/** @brief Draws an artwork slot's status with Browse and Clear buttons; `multi` edits the screenshot list. */
	void DrawArtworkRow(const char* label, bool multi, std::filesystem::path* singleSource, bool& cleared,
		const std::string& existingRel)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::TextUnformatted(label);

		std::string status;
		if (multi) {
			if (!form.screenshotSources.empty())
				status = std::format("{} file(s) selected", form.screenshotSources.size());
			else if (!cleared && !existingScreenshots.empty())
				status = std::format("{} existing", existingScreenshots.size());
			else
				status = T(TKEY("scene_export_artwork_none"), "None");
		} else if (singleSource && !singleSource->empty()) {
			status = DisplayPath(*singleSource);
		} else if (!cleared && !existingRel.empty()) {
			status = existingRel;
		} else {
			status = T(TKEY("scene_export_artwork_none"), "None");
		}

		ImGui::TextDisabled("%s", status.c_str());
		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		if (ImGui::SmallButton(std::format("{}##Browse{}", T(TKEY("scene_export_browse"), "Browse"), label).c_str())) {
			std::vector<std::filesystem::path> picked;
			if (BrowseImageFiles(multi, picked)) {
				cleared = false;
				if (multi) {
					form.screenshotSources = std::move(picked);
				} else if (singleSource && !picked.empty()) {
					*singleSource = std::move(picked.front());
				}
			}
		}
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		if (ImGui::SmallButton(std::format("{}##Clear{}", T(TKEY("scene_export_clear"), "Clear"), label).c_str())) {
			if (multi)
				form.screenshotSources.clear();
			else if (singleSource)
				singleSource->clear();
			cleared = true;
		}
	}

	/// Post Processing ships as a pack's Baseline file. Effects 11 replaces its pipeline, so E11 presets leave it out.
	bool CanIncludePostProcessing()
	{
		return form.type != PresetType::E11 && globals::features::postProcessing.loaded;
	}

	/** @brief Opt-in that adds the base Post Processing settings to the pack. Absent for E11 presets. */
	void DrawPostProcessingOption()
	{
		if (!CanIncludePostProcessing())
			return;
		bool include = includePostProcessing;
		if (ImGui::Checkbox(T(TKEY("scene_export_include_pp"), "Include Post Processing settings"), &include))
			includePostProcessing = include;
		Util::AddTooltip(T(TKEY("scene_export_include_pp_tooltip"),
			"Saves your current Post Processing settings in the pack, so applying it on the Presets page "
			"sets them. This is how Post Processing presets are made."));
	}

	/** @brief Feature checklist for the Baseline folder: each ticked feature's base settings go in the pack. */
	void DrawBaselineFeatureList(float visibleLines)
	{
		ImGui::TextUnformatted(T(TKEY("scene_export_baseline_features"), "Base settings to include"));
		Util::AddTooltip(T(TKEY("scene_export_baseline_features_tooltip"),
			"Saves each ticked feature's current base settings, and whether it loads at boot, in the preset's Baseline "
			"folder. Applying the preset sets them. Scene layers are not part of this."));

		const float listHeight = ImGui::GetTextLineHeightWithSpacing() * visibleLines + ImGui::GetStyle().FramePadding.y * 2.0f;
		if (ImGui::BeginChild("##SceneExportBaselineFeatures", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
			auto features = Feature::GetFeatureList();
			std::sort(features.begin(), features.end(), [](Feature* a, Feature* b) {
				return a->GetDisplayName() < b->GetDisplayName();
			});
			bool any = false;
			for (auto* feature : features) {
				if (!IsBaselineCandidate(feature))
					continue;
				any = true;
				const auto shortName = feature->GetShortName();
				bool selected = IsBaselineSelected(shortName);
				ImGui::PushID(shortName.c_str());
				if (ImGui::Checkbox(feature->GetDisplayName().c_str(), &selected))
					SetBaselineSelected(shortName, selected);
				ImGui::PopID();
			}
			if (!any)
				ImGui::TextDisabled("%s", T(TKEY("scene_export_baseline_none"), "No loaded feature has base settings to export."));
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%s",
			I18n::GetSingleton()->Format("cs_editor.scene_export_features_count",
									{ { "count", std::to_string(baselineFeatures.size()) } },
									"{count} selected")
				.c_str());
	}

	/** @brief Whether the form carries any base settings: a ticked feature, or Post Processing. */
	bool HasBaselinePayload()
	{
		return !baselineFeatures.empty() || (includePostProcessing && CanIncludePostProcessing());
	}

	/** @brief The form as export info, with the sanitized name and parsed tags. */
	PresetExportInfo BuildExportInfo(const std::string& sanitizedName)
	{
		auto info = form;
		info.name = sanitizedName;
		info.tags = ParseTags(presetTags);
		// A pack that sets a feature's base settings needs that feature to apply them.
		if (simplified)
			info.requiredFeatures.clear();
		auto require = [&](const std::string& shortName) {
			if (std::ranges::find(info.requiredFeatures, shortName) == info.requiredFeatures.end())
				info.requiredFeatures.push_back(shortName);
		};
		for (const auto& shortName : baselineFeatures)
			require(shortName);
		if (includePostProcessing && CanIncludePostProcessing())
			require(PostProcessingPresets::kFeatureShortName);
		std::erase_if(info.requiredFeatures, IsCoreFeature);
		return info;
	}

	/** @brief Writes each ticked feature's base settings into the pack's Baseline folder. */
	bool WriteBaselineFeatures(const std::string& packName)
	{
		auto* overrides = SettingsOverrideManager::GetSingleton();
		bool wroteAll = true;
		for (const auto& shortName : baselineFeatures) {
			auto* feature = Feature::FindFeatureByShortName(shortName);
			if (!IsBaselineCandidate(feature))
				continue;

			json settings;
			{
				// The author's own values, not whatever scene layer is applying right now.
				SceneSettingsManager::SceneLayerGuard sceneLayerGuard;
				feature->SaveSettings(settings);
			}
			std::vector<std::string> paths;
			for (const auto& setting : Util::Settings::GetExportSettings(shortName, settings))
				paths.push_back(setting.path);

			if (!overrides->ExportSettings(packName, shortName, paths, settings, true)) {
				logger::error("[ScenePresetExport] Could not write the Baseline settings of {} into '{}'", shortName, packName);
				wroteAll = false;
			}
		}
		return wroteAll;
	}

	/** @brief Records whether each exported feature loads at boot, so applying the pack restores it. */
	void WriteBootStates(const std::string& packId)
	{
		auto& catalog = UnifiedPresetCatalog::GetSingleton();
		auto names = baselineFeatures;
		if (includePostProcessing && CanIncludePostProcessing())
			names.push_back(PostProcessingPresets::kFeatureShortName);
		for (const auto& shortName : names) {
			auto* feature = Feature::FindFeatureByShortName(shortName);
			if (!feature || feature->IsAlwaysEnabled())
				continue;
			catalog.SetPackFeatureDisabledAtBoot(packId, shortName, globals::state->IsFeatureDisabled(shortName));
		}
	}

	/** @brief Preset type choice: CS, E11 or Baseline. */
	void DrawTypeSection()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::TextUnformatted(T(TKEY("scene_export_type"), "Preset type"));
		DrawPresetTypeOption(T(TKEY("scene_export_type_cs"), "CS Preset"), PresetType::CS);
		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		const bool e11Ready = globals::features::effects11.loaded &&
		                      PresetManager::GetSingleton().CanExportActivePreset();
		{
			ImGui::BeginDisabled(!e11Ready);
			DrawPresetTypeOption(T(TKEY("scene_export_type_e11"), "E11 Preset"), PresetType::E11);
			ImGui::EndDisabled();
			if (!e11Ready)
				Util::AddTooltip(T(TKEY("scene_export_type_e11_unavailable"),
									 "Load Effects 11 with a valid ENB preset (enbseries.ini + enbseries/) to export as E11."),
					Util::kTooltipWhenDisabled);
		}
		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		DrawPresetTypeOption(T(TKEY("scene_export_type_baseline"), "Baseline"), PresetType::Baseline);
		Util::AddTooltip(T(TKEY("scene_export_type_baseline_tooltip"),
			"Base settings only: the features you tick, without scene layers or Effects 11 files."));
		if (!e11Ready && form.type == PresetType::E11)
			form.type = PresetType::CS;
		if (form.type == PresetType::E11) {
			ImGui::TextDisabled("%s", T(TKEY("scene_export_type_e11_hint"),
				"Copies the active ENB files into Presets/<Name>/effects11/."));
		}
		ImGui::Separator();

		const char* scope = T(TKEY("scene_export_scope"), "Exports every setting from every context, not just this page.");
		if (form.type == PresetType::E11)
			scope = T(TKEY("scene_export_scope_e11"),
				"Exports the active Effects 11 ENB files, plus any Scene Manager settings that are present.");
		else if (form.type == PresetType::Baseline)
			scope = T(TKEY("scene_export_scope_baseline"),
				"Exports the base settings of the features ticked below, as a Baseline preset.");
		ImGui::TextWrapped("%s", scope);
	}

	/** @brief Mods supplying scene values, which a Baseline leaves out. */
	void DrawModList(SceneSettingsManager* manager)
	{
		const auto& modNames = GetCachedModNames(manager);
		if (modNames.empty()) {
			Util::Text::Disabled("%s", T(TKEY("scene_export_no_mods"), "No mods are supplying values."));
			return;
		}
		ImGui::TextUnformatted(T(TKEY("scene_export_mod_list"), "Mods supplying values, last one wins:"));
		if (ImGui::BeginChild("##ScenePresetExportMods",
				ImVec2(0.0f, kModListHeight * Util::GetUIScale()), ImGuiChildFlags_Borders)) {
			for (size_t index = 0; index < modNames.size(); ++index)
				ImGui::Text("%zu. %s", index + 1, modNames[index].c_str());
		}
		ImGui::EndChild();
	}

	/** @brief Name, version, author, description and tags. */
	void DrawMetadataFields()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::TextUnformatted(T(TKEY("scene_export_name"), "Preset name"));
		const char* existingLabel = T(TKEY("scene_export_existing"), "Existing...");
		ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(existingLabel).x + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x));
		ImGui::InputText("##ScenePresetExportName", &form.name);
		if (ImGui::IsItemDeactivatedAfterEdit())
			PrefillFromName();
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		DrawPickerButton(existingLabel);
		DrawPresetPicker();

		ImGui::TextUnformatted(T(TKEY("scene_export_version"), "Version"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##ScenePresetExportVersion", &form.version);

		ImGui::TextUnformatted(T(TKEY("scene_export_author"), "Author"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##ScenePresetExportAuthor", &form.author);

		ImGui::TextUnformatted(T(TKEY("scene_export_description"), "Description"));
		ImGui::InputTextMultiline("##ScenePresetExportDescription", &form.description,
			ImVec2(-1.0f, ImGui::GetTextLineHeight() * kDescriptionLines + style.FramePadding.y * 2.0f));

		ImGui::TextUnformatted(T(TKEY("scene_export_tags"), "Tags (comma-separated)"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##ScenePresetExportTags", &presetTags);
		Util::AddTooltip(T(TKEY("scene_export_tags_tooltip"), "Example: interior, weather, cinematic"));
	}

	/** @brief Artwork slots beside the compatibility box. */
	void DrawArtworkAndCompatibility()
	{
		if (!ImGui::BeginTable("##SceneExportArtCompat", 2, ImGuiTableFlags_SizingStretchSame))
			return;
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		DrawArtworkRow(T(TKEY("scene_export_logo"), "Logo"), false, &form.logoSource, form.clearLogo, existingLogo);
		DrawArtworkRow(T(TKEY("scene_export_cover"), "Cover (poster)"), false, &form.coverSource, form.clearCover,
			existingCover);
		DrawArtworkRow(T(TKEY("scene_export_screenshots"), "Screenshots"), true, nullptr, form.clearScreenshots, {});

		ImGui::TableNextColumn();
		DrawCompatibilityBox();
		ImGui::EndTable();
	}

	/** @brief The Export button with the reasons it can be disabled, and the confirmation it raises. */
	void DrawExportButton(SceneSettingsManager* manager, bool hasMods)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		auto sanitizedName = Util::FileHelpers::SanitizeFileName(form.name);
		const bool reservedName = SceneSettingsManager::IsReservedPresetName(sanitizedName);
		const bool validVersion = SceneSettingsManager::IsValidPresetVersion(form.version);
		const auto* existingPack = UnifiedPresetCatalog::GetSingleton().FindPack(sanitizedName);
		const bool typeMismatch = existingPack && !existingPack->AcceptsExport(form.type);
		// A CS export with no scene settings and no base settings would write an empty pack.
		const bool nothingToExport = form.type == PresetType::Baseline ? !HasBaselinePayload() :
		                             form.type == PresetType::CS      ? !manager->HasAnyUserEntries() && !hasMods && !HasBaselinePayload() :
		                                                                false;
		ImGui::BeginDisabled(sanitizedName.empty() || reservedName || !validVersion || typeMismatch || nothingToExport);
		if (ImGui::Button(T(TKEY("scene_export_confirm"), "Export"))) {
			collidingFiles = SceneSettingsManager::FindPresetFiles(sanitizedName);
			exportConfirmation.title = T(TKEY("scene_export_title"), "Export preset");
			exportConfirmation.message = collidingFiles.empty() ?
			                                 std::vformat(T(TKEY("scene_export_create_message"),
															 "Write your settings out as the preset '{}'?"),
													 std::make_format_args(sanitizedName)) :
			                                 DescribeCollision(sanitizedName, collidingFiles);
			exportConfirmation.confirmLabel = collidingFiles.empty() ?
			                                      T(TKEY("scene_export_confirm"), "Export") :
			                                      T(TKEY("scene_export_replace_confirm"), "Delete and replace");
			exportConfirmation.cancelLabel = T(TKEY("cancel"), "Cancel");
			exportRequested = true;
			exportConfirmation.Request();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		if (reservedName)
			Util::AddTooltip(T(TKEY("scene_export_hidden_name"), "Names starting with '_' or '.' are hidden from the Presets browser."),
				Util::kTooltipWhenDisabled);
		else if (!validVersion)
			Util::AddTooltip(T(TKEY("scene_export_invalid_version"), "Use a MAJOR.MINOR.PATCH version, such as 1.0.0."),
				Util::kTooltipWhenDisabled);
		else if (typeMismatch)
			Util::AddTooltip(T(TKEY("scene_export_type_mismatch"),
								 "A preset with this name already exists for the other pipeline. Pick a different name or switch the preset type."),
				Util::kTooltipWhenDisabled);
		else if (nothingToExport)
			Util::AddTooltip(T(TKEY("scene_export_nothing"),
								 "Nothing to export yet: author scene settings, or tick base settings to include."),
				Util::kTooltipWhenDisabled);

		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		if (ImGui::Button(T(TKEY("cancel"), "Cancel")))
			ImGui::CloseCurrentPopup();
	}

	/** @brief Everything inside the export popup. */
	void DrawForm(SceneSettingsManager& manager)
	{
		const bool baselineOnly = form.type == PresetType::Baseline;
		if (!simplified) {
			DrawTypeSection();
			DrawPostProcessingOption();
			ImGui::Separator();
			if (!baselineOnly) {
				DrawModList(&manager);
				ImGui::Separator();
			}
		} else {
			DrawPostProcessingOption();
		}

		DrawBaselineFeatureList(simplified ? kSimplifiedFeatureLines : kFeatureLines);
		ImGui::Separator();
		DrawMetadataFields();

		if (!simplified) {
			ImGui::Separator();
			DrawArtworkAndCompatibility();
		}

		DrawExportButton(&manager, !GetCachedModNames(&manager).empty());
	}
}

bool ScenePresetExport::CanExport()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager)
		return false;
	if (manager->HasAnyUserEntries() || !GetCachedModNames(manager).empty())
		return true;
	// E11-only export: no scene layer yet, but the live Effects 11 preset can still be written out.
	// Post Processing alone is enough: its base settings can always go out as a preset.
	if (globals::features::postProcessing.loaded || HasAnyBaselineCandidate())
		return true;
	return globals::features::effects11.loaded && PresetManager::GetSingleton().CanExportActivePreset();
}

void ScenePresetExport::Open()
{
	dialogActive = true;
	pendingOpen = true;
	ResetFormFields();
	// The Presets page may never have scanned this session, and packs may have changed since.
	UnifiedPresetCatalog::GetSingleton().Discover();
}

void ScenePresetExport::OpenBaseline(const std::string& featureShortName)
{
	Open();
	simplified = true;
	form.type = PresetType::Baseline;
	// The scene layer detection is not part of a Baseline; the base settings choose what is required.
	form.requiredFeatures.clear();
	if (featureShortName == PostProcessingPresets::kFeatureShortName)
		includePostProcessing = globals::features::postProcessing.loaded;
	else if (IsBaselineCandidate(Feature::FindFeatureByShortName(featureShortName)))
		SetBaselineSelected(featureShortName, true);
}

void ScenePresetExport::Draw()
{
	if (!dialogActive)
		return;

	auto* manager = SceneSettingsManager::GetSingleton();
	if (!manager) {
		dialogActive = false;
		return;
	}

	if (pendingOpen) {
		ImGui::OpenPopup(kExportPopupId);
		pendingOpen = false;
	}

	ImGui::SetNextWindowSize(ImVec2(kModalWidth * Util::GetUIScale(), 0.0f), ImGuiCond_Always);
	if (ImGui::BeginPopupModal(kExportPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		// The name picker is a popup of its own and takes the first Escape.
		const bool closing = !ImGui::IsPopupOpen(kPickerPopupId) && EditorWindow::ClosePopupOnEscape();
		if (!closing)
			DrawForm(*manager);
		ImGui::EndPopup();
	}

	if (exportConfirmation.Draw()) {
		auto sanitizedName = Util::FileHelpers::SanitizeFileName(form.name);
		bool exported = manager->ExportPreset(BuildExportInfo(sanitizedName));
		if (exported && includePostProcessing && CanIncludePostProcessing())
			exported = PostProcessingPresets::WriteBaseline(Util::PathHelpers::GetUnifiedPackPath(sanitizedName));
		if (exported)
			exported = WriteBaselineFeatures(sanitizedName);
		ReportExportResult(sanitizedName, exported);
		// The Presets page lists the new or updated pack without a manual refresh.
		UnifiedPresetCatalog::GetSingleton().Discover();
		// After the scan: the boot states live in the manifest of a pack the catalog now knows.
		if (exported)
			WriteBootStates(sanitizedName);
		exportRequested = false;
	} else if (!exportConfirmation.IsOpen()) {
		exportRequested = false;
	}

	if (!ImGui::IsPopupOpen(kExportPopupId) && !exportRequested)
		dialogActive = false;
}

#undef I18N_KEY_PREFIX
