#pragma once

#include "../Widget.h"

class WeatherWidget : public Widget
{
public:
	RE::TESWeather* weather = nullptr;
	float color[3];

	WeatherWidget(RE::TESWeather* a_weather)
	{
		weather = a_weather;

		name = weather->GetFormEditorID();

		auto& sunlight = weather->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTimes::kDay];
		color[0] = float(sunlight.red) / 255.0f;
		color[1] = float(sunlight.green) / 255.0f;
		color[2] = float(sunlight.blue) / 255.0f;
	}

	~WeatherWidget();

	WeatherWidget(const WeatherWidget& other)
	{
		weather = other.weather;

		name = std::format("{} COPY", other.name);

		color[0] = other.color[0];
		color[1] = other.color[1];
		color[2] = other.color[2];
	}

	virtual int GetID()
	{
		return (int)weather->GetFormID();
	}

	virtual Widget* Clone() const override
	{
		return new WeatherWidget(*this);  // Return a copy of this object
	}

	virtual void DrawWidget() override;
};