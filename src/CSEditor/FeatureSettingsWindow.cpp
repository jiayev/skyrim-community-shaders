#include "FeatureSettingsWindow.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <vector>

#include "../I18n/I18n.h"
#include "EditorWindow.h"
#include "Feature.h"
#include "FeatureCategories.h"
#include "FeatureDebugFilter.h"
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "IconsLucide.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Fonts.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Presets/UnifiedPresetCatalog.h"
#include "SceneManager/FeatureOverwritesPanel.h"
#include "State.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "cs_editor."

namespace
{
	/// First-use size as a share of the display; the user can resize afterwards.
	constexpr float kWindowWidthRatio = 0.3f;
	constexpr float kWindowHeightRatio = 0.6f;

	/// Starting width of the feature column at the baseline font size; the user can drag it.
	constexpr float kFeatureListWidth = 180.0f;

	/// The divider doubles as the resize grip, so the feature column and the panel share one border.
	constexpr ImGuiTableFlags kLayoutFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;

	/// Post-Processing has its own window; Utility and Display hold editor and output plumbing, not scene look.
	constexpr std::array kExcludedCategories = { FeatureCategories::kUtility, FeatureCategories::kDisplay, FeatureCategories::kPostProcessing };

	std::string featureSearch;
	/// Short name of the feature shown in the Features window; also the constraint dialog's navigation target.
	std::string selectedFeature;
	std::map<std::string, bool> categoryExpansion;
	/// Pack id chosen for writing disableAtBoot; empty until the user picks one.
	std::string baselineBootPackId;

	/** @brief Centers the next window at its first-use size, re-applied on Reset Window Layout. */
	void SetNextWindowLayout()
	{
		const auto& displaySize = ImGui::GetIO().DisplaySize;
		const auto layoutCond = EditorWindow::GetSingleton()->resetLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
		ImGui::SetNextWindowSize({ displaySize.x * kWindowWidthRatio, displaySize.y * kWindowHeightRatio }, layoutCond);
		ImGui::SetNextWindowPos({ displaySize.x / 2.0f, displaySize.y / 2.0f }, layoutCond, { 0.5f, 0.5f });
	}

	Feature* FindAnyFeatureByShortName(const std::string& shortName)
	{
		for (auto* feature : Feature::GetFeatureList()) {
			if (feature && feature->GetShortName() == shortName)
				return feature;
		}
		return nullptr;
	}

	/** @brief Baseline packs that can receive a disableAtBoot entry, preferring enabled ones. */
	std::vector<const UnifiedPresetCatalog::PackInfo*> ListBaselineAuthoringPacks()
	{
		auto& catalog = UnifiedPresetCatalog::GetSingleton();
		std::vector<const UnifiedPresetCatalog::PackInfo*> packs;
		for (const auto& pack : catalog.GetPacks()) {
			if (pack.IsBaseline() || pack.hasBaseline)
				packs.push_back(&pack);
		}
		std::ranges::sort(packs, [&](const auto* a, const auto* b) {
			const bool aEnabled = catalog.IsBaselineEnabled(a->id);
			const bool bEnabled = catalog.IsBaselineEnabled(b->id);
			if (aEnabled != bEnabled)
				return aEnabled && !bEnabled;
			return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
		});
		return packs;
	}

	void EnsureBaselineBootPackSelection(const std::vector<const UnifiedPresetCatalog::PackInfo*>& packs)
	{
		if (packs.empty()) {
			baselineBootPackId.clear();
			return;
		}
		if (std::ranges::any_of(packs, [](const auto* pack) { return pack->id == baselineBootPackId; }))
			return;
		auto& catalog = UnifiedPresetCatalog::GetSingleton();
		for (const auto& id : catalog.GetBaselinePackIds() | std::views::reverse) {
			if (std::ranges::any_of(packs, [&](const auto* pack) { return pack->id == id; })) {
				baselineBootPackId = id;
				return;
			}
		}
		baselineBootPackId = packs.front()->id;
	}

	/** @brief Boot enable pill only (label lives in the tooltip). Returns false when not drawn. */
	bool DrawBootToggle(Feature* feature)
	{
		if (!feature || feature->IsAlwaysEnabled())
			return false;

		const auto featureName = feature->GetShortName();
		bool bootEnabled = !globals::state->IsFeatureDisabled(featureName);

		if (Util::FeatureToggle("##BootToggle", &bootEnabled, ImVec2(ImGui::GetFrameHeight() * 1.6f, 0.0f))) {
			const bool nowDisabled = feature->ToggleAtBootSetting();
			logger::info("[CSEditor] {}: {} at boot.", featureName, nowDisabled ? "Disabled" : "Enabled");
		}
		Util::AddTooltip(std::format("{}\n\n{}",
			bootEnabled ?
				T(TKEY("feature_boot_enabled"), "Enabled at boot") :
				T(TKEY("feature_boot_disabled"), "Disabled at boot"),
			T(TKEY("feature_boot_toggle_tooltip"),
				"Toggle whether this feature loads when the game starts.\n"
				"Restart required for changes to take effect.\n"
				"Same as Disable at Boot in the main Community Shaders menu."))
			.c_str());
		return true;
	}

	/** @brief Optional write of boot state into a baseline pack's disableAtBoot. */
	void DrawBaselineBootSection(Feature* feature)
	{
		if (!feature || feature->IsAlwaysEnabled())
			return;

		const auto packs = ListBaselineAuthoringPacks();
		if (packs.empty())
			return;

		EnsureBaselineBootPackSelection(packs);
		ImGui::Spacing();
		ImGui::SeparatorText(T(TKEY("baseline_boot_section"), "Baseline pack"));
		Util::Text::WrappedSecondary("%s",
			T(TKEY("baseline_boot_section_desc"),
				"Store this feature's boot state in a Baseline pack so applying the pack defines which features load."));

		std::string preview = baselineBootPackId;
		for (const auto* pack : packs) {
			if (pack->id == baselineBootPackId) {
				preview = pack->name;
				break;
			}
		}
		ImGui::SetNextItemWidth(-1.0f);
		if (ImGui::BeginCombo("##BaselineBootPack", preview.c_str())) {
			for (const auto* pack : packs) {
				const bool selected = pack->id == baselineBootPackId;
				auto label = pack->name;
				if (UnifiedPresetCatalog::GetSingleton().IsBaselineEnabled(pack->id))
					label += std::format(" ({})", T(TKEY("baseline_boot_pack_enabled"), "enabled"));
				if (ImGui::Selectable(label.c_str(), selected))
					baselineBootPackId = pack->id;
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		const auto featureName = feature->GetShortName();
		auto* state = globals::state;
		const auto* selectedPack = UnifiedPresetCatalog::GetSingleton().FindPack(baselineBootPackId);
		if (!selectedPack)
			return;

		bool packDisabled = state->IsFeatureDisabled(featureName);
		if (const auto it = selectedPack->disableAtBoot.find(featureName); it != selectedPack->disableAtBoot.end())
			packDisabled = it->second;

		if (ImGui::Checkbox(T(TKEY("baseline_boot_store"), "Disable at boot in this pack"), &packDisabled)) {
			if (UnifiedPresetCatalog::GetSingleton().SetPackFeatureDisabledAtBoot(baselineBootPackId, featureName, packDisabled)) {
				if (state->IsFeatureDisabled(featureName) != packDisabled)
					feature->ToggleAtBootSetting();
			}
		}
		Util::AddTooltip(T(TKEY("baseline_boot_store_tooltip"),
			"Writes disableAtBoot into the pack manifest. Applying this Baseline pack later sets the same boot state "
			"(restart still required for load/unload)."));
	}

	/** @brief Draw the shared export-overwrite icon button (tooltip carries the old label). */
	void DrawExportOverwriteIcon(Feature* feature, const char* id)
	{
		const float iconSize = ImGui::GetFrameHeight();
		{
			auto _style = Util::TransparentIconButtonStyle();
			if (Icons::Button(id, Icons::LC(ICON_LC_SHARE), ImVec2(iconSize, iconSize)))
				FeatureOverwritesPanel::BeginExport(feature);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"%s",
				T("menu.features.export_overwrite_tooltip",
					"Export selected settings as a feature overwrite file.\n"
					"Overwrites are loaded at startup."));
		}
	}

	/** @brief Export overwrite icon, boot controls, and the settings body of one feature. */
	void DrawSettingsPanel(Feature* feature)
	{
		const bool canExport = feature->loaded && FeatureOverwritesPanel::HasExportableSettings(feature);
		const float iconSize = ImGui::GetFrameHeight();
		const float rowStartX = ImGui::GetCursorPosX();
		const float rowAvail = ImGui::GetContentRegionAvail().x;

		const bool drewBoot = DrawBootToggle(feature);
		if (canExport) {
			if (drewBoot)
				ImGui::SameLine();
			ImGui::SetCursorPosX(rowStartX + std::max(0.0f, rowAvail - iconSize));
			DrawExportOverwriteIcon(feature, "##FeatureExportOverwrite");
		}

		DrawBaselineBootSection(feature);

		if (!feature->loaded) {
			ImGui::Spacing();
			Util::Text::WrappedSecondary("%s",
				T(TKEY("feature_boot_unloaded_hint"),
					"This feature is not loaded. Boot enable/disable still applies after a restart; settings stay hidden until then."));
			if (!feature->failedLoadedMessage.empty()) {
				ImGui::Spacing();
				Util::Text::WrappedError("%s", feature->failedLoadedMessage.c_str());
			}
			return;
		}

		ImGui::Spacing();
		if (ImGui::BeginChild("##FeatureSettingsBody")) {
			const bool hideDebug = !EditorWindow::GetSingleton()->settings.showFeatureDebug;
			FeatureDebugFilter::Scope hideDebugScope(hideDebug);
			FeatureListRenderer::DrawFeatureBody(feature);
		}
		ImGui::EndChild();

		// Drawn after the body so the modal cannot clobber its hover state.
		if (canExport)
			FeatureOverwritesPanel::DrawExport();
	}

	/** @brief Search bar and collapsible category groups of the features this window edits. */
	void DrawFeatureList()
	{
		Util::DrawFeatureSearchBar(featureSearch);

		for (const auto category : FeatureCategories::kMenuOrder) {
			if (std::ranges::find(kExcludedCategories, category) != kExcludedCategories.end())
				continue;

			auto features = Feature::GetFeatureList() | std::views::filter([&](Feature* feature) {
				return feature->IsInMenu() && !feature->IsHiddenUnreleased() && feature->GetCategory() == category &&
				       Util::FeatureMatchesSearch(feature, featureSearch);
			}) | std::ranges::to<std::vector>();
			if (features.empty())
				continue;
			std::ranges::sort(features, {}, &Feature::GetDisplayName);

			const std::string categoryKey(category);
			auto& expanded = categoryExpansion.try_emplace(categoryKey, true).first->second;
			{
				MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Heading);
				Util::DrawCategoryHeader(categoryKey.c_str(), FeatureListRenderer::TranslateCategory(category).c_str(), expanded, static_cast<int>(features.size()));
			}
			if (!expanded)
				continue;

			MenuFonts::FontRoleGuard fontGuard(Menu::FontRole::Subheading);
			auto& theme = globals::menu->GetSettings().Theme;
			for (auto* feature : features) {
				const auto shortName = feature->GetShortName();
				const bool disabledAtBoot = globals::state->IsFeatureDisabled(shortName);
				ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
				if (disabledAtBoot)
					color = theme.StatusPalette.Disable;
				else if (!feature->loaded)
					color = feature->installed ? theme.StatusPalette.RestartNeeded : theme.StatusPalette.Disable;

				ImGui::PushStyleColor(ImGuiCol_Text, color);
				if (ImGui::Selectable(std::format("{}##{}", feature->GetDisplayName(), shortName).c_str(), shortName == selectedFeature))
					selectedFeature = shortName;
				ImGui::PopStyleColor();
			}
		}
	}

	/** @brief Small grey title-bar trailer: Features | Create or enable a Baseline pack… */
	void DrawBaselinePackTitleHint(bool inTitleBar)
	{
		if (!ListBaselineAuthoringPacks().empty())
			return;

		const char* hint = T(TKEY("baseline_boot_no_packs"),
			"Create or enable a Baseline pack to store this boot state in a preset.");
		const ImVec4 secondary = Util::Colors::GetSecondary();

		if (inTitleBar) {
			ImGui::TextColored(secondary, "|");
			ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
		}

		MenuFonts::FontRoleGuard subtext(Menu::FontRole::Subtext);
		if (inTitleBar) {
			// Title/| use the body line box; subtext is smaller, so a shared top cursor leaves it
			// sitting high. Re-anchor to the same capital-ink midline as the preceding chrome.
			const float midY = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
			const float ascent = ImGui::GetFontBaked()->Ascent;
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, midY - ascent * 0.5f));
		}
		ImGui::TextColored(secondary, "%s", hint);
	}

	/** @brief Feature list beside the selected feature's settings. */
	void DrawFeaturesWindow(bool& open)
	{
		SetNextWindowLayout();
		const auto title = std::format("{}###CSEditorFeatures", T(TKEY("features_window"), "Features"));
		const bool visible = Util::BeginWithCustomHeader(title.c_str(), &open, [] {
			DrawBaselinePackTitleHint(true);
		});
		if (visible) {
			// Docked windows keep the native tab title, so surface the same hint under it.
			if (ImGui::IsWindowDocked())
				DrawBaselinePackTitleHint(false);

			if (ImGui::BeginTable("##FeaturesLayout", 2, kLayoutFlags)) {
				ImGui::TableSetupColumn("##FeatureList", ImGuiTableColumnFlags_WidthFixed, kFeatureListWidth * Util::GetUIScale());
				ImGui::TableSetupColumn("##FeatureSettings", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();

				// Each column scrolls on its own, so a long feature list never drags the settings with it.
				ImGui::TableSetColumnIndex(0);
				if (ImGui::BeginChild("##FeatureListColumn"))
					DrawFeatureList();
				ImGui::EndChild();

				// Resolved by name rather than from the list, so constraint navigation to an unlisted feature still shows it.
				ImGui::TableSetColumnIndex(1);
				if (auto* feature = FindAnyFeatureByShortName(selectedFeature))
					DrawSettingsPanel(feature);
				else
					Util::Text::WrappedDisabled("%s", T(TKEY("features_window_select"), "Select a feature on the left."));

				ImGui::EndTable();
			}
		}
		ImGui::End();
	}

	/** @brief The whole Post-Processing stack's settings. */
	void DrawPostProcessingWindow(bool& open)
	{
		SetNextWindowLayout();
		const auto title = std::format("{}###CSEditorPostProcessing", T(TKEY("post_processing_window"), "Post Processing"));
		if (Util::BeginWithRoundedClose(title.c_str(), &open)) {
			if (auto& postProcessing = globals::features::postProcessing; postProcessing.loaded)
				DrawSettingsPanel(&postProcessing);
			else
				Util::Text::WrappedDisabled("%s", T(TKEY("scene_feature_unloaded"), "This feature is not loaded, so it has nothing to edit."));
		}
		ImGui::End();
	}
}

void FeatureSettingsWindow::Draw(bool& featuresOpen, bool& postProcessingOpen)
{
	if (!featuresOpen && !postProcessingOpen)
		return;

	FeatureDebugFilter::Install();

	if (featuresOpen)
		DrawFeaturesWindow(featuresOpen);
	if (postProcessingOpen)
		DrawPostProcessingWindow(postProcessingOpen);

	// Once per frame at top level, so two open windows cannot stack two copies of the modal.
	const std::string featureBeforeDialog = selectedFeature;
	FeatureListRenderer::DrawConstraintWarningDialog(selectedFeature);
	if (selectedFeature != featureBeforeDialog)
		featuresOpen = true;
}

#undef I18N_KEY_PREFIX
