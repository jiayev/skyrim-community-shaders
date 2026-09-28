#include "ScenePresetExport.h"

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
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	using SceneContextId = SceneSettingsManager::SceneContextId;
	using PresetExportInfo = SceneSettingsManager::PresetExportInfo;
	using PresetType = SceneSettingsManager::PresetType;

	constexpr const char* kExportPopupId = "##ScenePresetExport";

	/// The list scrolls rather than growing past the screen, like the copy preview does.
	constexpr float kModListHeight = 120.0f;
	constexpr float kModalWidth = 560.0f;
	constexpr float kDescriptionLines = 3.0f;

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

	SceneContextId exportContext;
	bool dialogActive = false;
	bool pendingOpen = false;
	bool exportRequested = false;
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

	/** @brief Clears the form and everything loaded from an existing preset; the type follows the live pipeline. */
	void ResetFormFields()
	{
		form = {};
		form.type = globals::state->GetTonemapOwner() == State::TonemapOwner::kEffects11 ? PresetType::E11 : PresetType::CS;
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

	/** @brief The form as export info, with the sanitized name and parsed tags. */
	PresetExportInfo BuildExportInfo(const std::string& sanitizedName)
	{
		auto info = form;
		info.name = sanitizedName;
		info.tags = ParseTags(presetTags);
		return info;
	}
}

bool ScenePresetExport::CanExport()
{
	auto* manager = SceneSettingsManager::GetSingleton();
	return manager && (manager->HasAnyUserEntries() || !GetCachedModNames(manager).empty());
}

void ScenePresetExport::Open(const SceneContextId& context)
{
	exportContext = context;
	dialogActive = true;
	pendingOpen = true;
	ResetFormFields();
}

void ScenePresetExport::Draw(const SceneContextId& context)
{
	if (!dialogActive || context != exportContext)
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

	const float scale = Util::GetUIScale();
	const ImGuiStyle& style = ImGui::GetStyle();
	ImGui::SetNextWindowSize(ImVec2(kModalWidth * scale, 0.0f), ImGuiCond_Always);
	if (ImGui::BeginPopupModal(kExportPopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(T(TKEY("scene_export_type"), "Preset type"));
		DrawPresetTypeOption(T(TKEY("scene_export_type_cs"), "CS Preset"), PresetType::CS);
		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		DrawPresetTypeOption(T(TKEY("scene_export_type_e11"), "E11 Preset"), PresetType::E11);
		ImGui::Separator();

		ImGui::TextWrapped(
			"%s", T(TKEY("scene_export_scope"), "Exports every setting from every context, not just this page."));
		ImGui::Separator();

		const auto& modNames = GetCachedModNames(manager);
		if (modNames.empty()) {
			Util::Text::Disabled("%s", T(TKEY("scene_export_no_mods"), "No mods are supplying values."));
		} else {
			ImGui::TextUnformatted(T(TKEY("scene_export_mod_list"), "Mods supplying values, last one wins:"));
			if (ImGui::BeginChild("##ScenePresetExportMods",
					ImVec2(0.0f, kModListHeight * scale), ImGuiChildFlags_Borders)) {
				for (size_t index = 0; index < modNames.size(); ++index)
					ImGui::Text("%zu. %s", index + 1, modNames[index].c_str());
			}
			ImGui::EndChild();
		}

		ImGui::Separator();
		ImGui::TextUnformatted(T(TKEY("scene_export_name"), "Preset name"));
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##ScenePresetExportName", &form.name);
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			const auto packId = Util::FileHelpers::SanitizeFileName(form.name);
			if (const auto meta = SceneSettingsManager::ReadPresetMetadata(Util::PathHelpers::GetUnifiedPackPath(packId)))
				PrefillFromExisting(*meta);
		}

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

		ImGui::Separator();
		ImGui::TextUnformatted(T(TKEY("scene_export_artwork"), "Artwork (optional)"));
		ImGui::TextDisabled("%s", T(TKEY("scene_export_artwork_hint"),
			"Images are copied into Presets/<Name>/ for the Presets browser."));
		DrawArtworkRow(T(TKEY("scene_export_logo"), "Logo"), false, &form.logoSource, form.clearLogo, existingLogo);
		DrawArtworkRow(T(TKEY("scene_export_cover"), "Cover (poster)"), false, &form.coverSource, form.clearCover,
			existingCover);
		DrawArtworkRow(T(TKEY("scene_export_screenshots"), "Screenshots"), true, nullptr, form.clearScreenshots, {});

		auto sanitizedName = Util::FileHelpers::SanitizeFileName(form.name);
		const bool reservedName = SceneSettingsManager::IsReservedPresetName(sanitizedName);
		const bool validVersion = SceneSettingsManager::IsValidPresetVersion(form.version);
		ImGui::BeginDisabled(sanitizedName.empty() || reservedName || !validVersion);
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

		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		if (ImGui::Button(T(TKEY("cancel"), "Cancel")))
			ImGui::CloseCurrentPopup();

		ImGui::EndPopup();
	}

	if (exportConfirmation.Draw()) {
		auto sanitizedName = Util::FileHelpers::SanitizeFileName(form.name);
		ReportExportResult(sanitizedName, manager->ExportPreset(BuildExportInfo(sanitizedName)));
		exportRequested = false;
	} else if (!exportConfirmation.IsOpen()) {
		exportRequested = false;
	}

	if (!ImGui::IsPopupOpen(kExportPopupId) && !exportRequested)
		dialogActive = false;
}

#undef I18N_KEY_PREFIX
