#include "WeatherWidget.h"

std::string WeatherWidget::GetName()
{
	return std::format("{} ({:X})", weather->GetFormEditorID(), weather->GetFormID());
}

void WeatherWidget::DrawWidget()
{
	weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTimes::kDay].red = uint8_t(color[0] * 255.0f);
	weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTimes::kDay].green = uint8_t(color[1] * 255.0f);
	weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTimes::kDay].blue = uint8_t(color[2] * 255.0f);

	ImGui::ColorEdit3("Sunlight Color Day", color);
}
