#include "WeatherPickerWindow.h"

#include "../I18n/I18n.h"
#include "EditorWindow.h"
#include "Features/CSEditor.h"
#include "Globals.h"
#include "IconsFontAwesome5.h"
#include "Menu/Icons/helpers/IconFonts.h"
#include "Menu/ThemeManager.h"
#include "Utils/Game.h"
#include "Utils/UI.h"
#include "imgui_internal.h"

#include <format>

#define I18N_KEY_PREFIX "cs_editor."

void WeatherPickerWindow::Draw()
{
	if (!open)
		return;

	const auto* editor = EditorWindow::GetSingleton();
	const float scale = Util::GetUIScale();
	const float pad = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * scale;
	const auto& displaySize = ImGui::GetIO().DisplaySize;
	const auto layoutCond = editor->resetLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;

	float anchorX = displaySize.x - pad;
	if (ImGuiWindow* palette = ImGui::FindWindowByName("##CSEditorPalette"); palette && palette->WasActive)
		anchorX = palette->Pos.x - pad;

	ImGui::SetNextWindowPos(ImVec2(anchorX, displaySize.y - pad), layoutCond, ImVec2(1.0f, 1.0f));

	constexpr ImGuiWindowFlags kFlags =
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoScrollWithMouse |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoCollapse;

	if (ImGui::Begin("##WeatherPickerWindow", nullptr, kFlags))
		DrawContents();
	ImGui::End();
}

void WeatherPickerWindow::DrawContents()
{
	const float scale = Util::GetUIScale();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 4.0f * scale));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * scale, 2.0f * scale));

	auto* sky = globals::game::sky;
	if (!sky) {
		ImGui::TextDisabled("%s", T(TKEY("sky_not_available"), "Sky not available"));
		ImGui::PopStyleVar(2);
		return;
	}

	auto weatherShortName = [](RE::TESWeather* weather) -> std::string {
		if (!weather)
			return {};
		if (auto* editorId = weather->GetFormEditorID(); editorId && editorId[0] != '\0')
			return editorId;
		return std::format("{:08X}", weather->GetFormID());
	};

	std::string comboPreviewStorage;
	const char* comboPreview = T(TKEY("select_weather"), "Select Weather");
	if (sky->currentWeather) {
		comboPreviewStorage = weatherShortName(sky->currentWeather);
		comboPreview = comboPreviewStorage.c_str();
	}

	static constexpr const char* kWeatherSearchId = "WeatherPickerWindow";
	auto& filteredWeathers = CSEditor::GetFilteredWeathers();
	auto& selectedWeatherIdx = CSEditor::GetSelectedWeatherIdx();

	ImGui::SetNextItemWidth(180.0f * scale);
	if (ImGui::BeginCombo("##WeatherPickerWindowCombo", comboPreview)) {
		auto searchText = Util::DrawComboSearchInput(kWeatherSearchId);
		for (int i = 0; i < static_cast<int>(filteredWeathers.size()); ++i) {
			auto* weather = filteredWeathers[i];
			if (!searchText.empty()) {
				auto editorId = weather->GetFormEditorID() ? std::string(weather->GetFormEditorID()) : "";
				auto name = weather->GetName() ? std::string(weather->GetName()) : "";
				auto formId = std::format("{:08X}", weather->GetFormID());
				if (!Util::StringMatchesSearch(editorId, searchText) &&
					!Util::StringMatchesSearch(name, searchText) &&
					!Util::StringMatchesSearch(formId, searchText))
					continue;
			}

			const bool isSelected = (selectedWeatherIdx == i);
			ImGui::PushStyleColor(ImGuiCol_Text, CSEditor::GetWeatherTypeColor(weather));
			const bool didSelect = ImGui::Selectable(weatherShortName(weather).c_str(), isSelected);
			ImGui::PopStyleColor();

			if (didSelect) {
				selectedWeatherIdx = i;
				auto* selectedWeather = filteredWeathers[i];
				if (CSEditor::GetAccelerateWeatherChange())
					sky->ForceWeather(selectedWeather, false);
				else
					sky->SetWeather(selectedWeather, true, false);

				auto* editorWindow = EditorWindow::GetSingleton();
				if (editorWindow->IsWeatherLocked())
					editorWindow->LockWeather(selectedWeather);

				Util::ClearComboSearch(kWeatherSearchId);
				logger::info("[WeatherPickerWindow] Changed weather to: {}", Util::FormatWeather(selectedWeather));
				break;
			}

			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::Text(T(TKEY("tooltip_weather_name"), "Weather: %s"), weather->GetName() ? weather->GetName() : "Unnamed");
				ImGui::Text(T(TKEY("tooltip_editor_id"), "Editor ID: %s"), weather->GetFormEditorID() ? weather->GetFormEditorID() : "None");
				ImGui::Text(T(TKEY("tooltip_form_id"), "Form ID: 0x%08X"), weather->GetFormID());
				ImGui::EndTooltip();
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	} else {
		Util::ClearComboSearch(kWeatherSearchId);
	}

	ImGui::SameLine(0.0f, 4.0f * scale);
	const float iconSize = ImGui::GetFrameHeight();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
	{
		auto _style = Util::TransparentIconButtonStyle();
		if (Icons::Button("##WeatherPickerWindowReset", Icons::FA(ICON_FA_UNDO), ImVec2(iconSize, iconSize))) {
			sky->ResetWeather();
			selectedWeatherIdx = CSEditor::FindWeatherIndex(sky->defaultWeather);
			logger::info("[WeatherPickerWindow] Reset weather to default");
		}
	}
	ImGui::PopStyleVar();
	Util::AddTooltip(T(TKEY("reset_weather_tooltip"), "Resets weather to default"));

	if (sky->lastWeather)
		CSEditor::GetCachedLastWeather() = sky->lastWeather;

	auto* lastWeather = sky->lastWeather;
	const float lerpFactor = sky->currentWeatherPct;
	const bool isTransitioning = lastWeather && lerpFactor < 1.0f;
	const float displayPct = isTransitioning ? lerpFactor : 1.0f;

	char pctOverlay[16] = {};
	if (isTransitioning)
		std::snprintf(pctOverlay, sizeof(pctOverlay), "%.0f%%", lerpFactor * 100.0f);

	const float progressBarHeight = 6.0f * scale;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
	if (!isTransitioning)
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
	ImGui::ProgressBar(displayPct, ImVec2(180.0f * scale + 4.0f * scale + iconSize, progressBarHeight), "");
	if (isTransitioning) {
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const ImVec2 textSize = ImGui::CalcTextSize(pctOverlay);
		ImGui::GetWindowDrawList()->AddText(
			ImVec2((min.x + max.x - textSize.x) * 0.5f, (min.y + max.y - textSize.y) * 0.5f),
			ImGui::GetColorU32(ImGuiCol_Text), pctOverlay);
	}
	if (!isTransitioning)
		ImGui::PopStyleColor();
	ImGui::PopStyleVar();

	ImGui::PopStyleVar(2);
}

void WeatherPickerWindow::Save() {}
void WeatherPickerWindow::Load() {}

#undef I18N_KEY_PREFIX
