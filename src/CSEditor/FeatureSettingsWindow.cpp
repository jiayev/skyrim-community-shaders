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
#include "Features/PostProcessing.h"
#include "Globals.h"
#include "Menu.h"
#include "Menu/FeatureListRenderer.h"
#include "Menu/Fonts.h"
#include "SceneManager/FeatureOverwritesPanel.h"
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

	/** @brief Centers the next window at its first-use size, re-applied on Reset Window Layout. */
	void SetNextWindowLayout()
	{
		const auto& displaySize = ImGui::GetIO().DisplaySize;
		const auto layoutCond = EditorWindow::GetSingleton()->resetLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
		ImGui::SetNextWindowSize({ displaySize.x * kWindowWidthRatio, displaySize.y * kWindowHeightRatio }, layoutCond);
		ImGui::SetNextWindowPos({ displaySize.x / 2.0f, displaySize.y / 2.0f }, layoutCond, { 0.5f, 0.5f });
	}

	/** @brief Export Overwrite button and the settings body of one loaded feature. */
	void DrawSettingsPanel(Feature* feature)
	{
		const bool canExport = FeatureOverwritesPanel::HasExportableSettings(feature);
		if (canExport && ImGui::Button(T("menu.features.export_overwrite", "Export Overwrite")))
			FeatureOverwritesPanel::BeginExport(feature);

		if (ImGui::BeginChild("##FeatureSettingsBody"))
			FeatureListRenderer::DrawFeatureBody(feature);
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
				return feature->loaded && feature->IsInMenu() && feature->GetCategory() == category &&
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
			for (auto* feature : features) {
				const auto shortName = feature->GetShortName();
				if (ImGui::Selectable(std::format("{}##{}", feature->GetDisplayName(), shortName).c_str(), shortName == selectedFeature))
					selectedFeature = shortName;
			}
		}
	}

	/** @brief Feature list beside the selected feature's settings. */
	void DrawFeaturesWindow(bool& open)
	{
		SetNextWindowLayout();
		const auto title = std::format("{}###CSEditorFeatures", T(TKEY("features_window"), "Features"));
		if (Util::BeginWithRoundedClose(title.c_str(), &open) && ImGui::BeginTable("##FeaturesLayout", 2, kLayoutFlags)) {
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
			if (auto* feature = Feature::FindFeatureByShortName(selectedFeature); feature && feature->loaded)
				DrawSettingsPanel(feature);
			else
				Util::Text::WrappedDisabled("%s", T(TKEY("features_window_select"), "Select a feature on the left."));

			ImGui::EndTable();
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
