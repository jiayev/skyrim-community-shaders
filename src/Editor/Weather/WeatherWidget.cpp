#include "WeatherWidget.h"

WeatherWidget::~WeatherWidget()
{
}

void WeatherWidget::DrawWidget()
{
	if (ImGui::Begin(GetWindowTitleWithID().c_str(), &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) {
		if (ImGui::BeginMenuBar()) {
			if (ImGui::BeginMenu("Menu")) {
				ImGui::EndMenu();
			}
			ImGui::EndMenuBar();
		}
		weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTimes::kDay].red = uint8_t(color[0] * 255.0f);
		weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTimes::kDay].green = uint8_t(color[1] * 255.0f);
		weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTimes::kDay].blue = uint8_t(color[2] * 255.0f);

		ImGui::ColorEdit3("Sunlight Color Day", color);
	}
	ImGui::End();
}
