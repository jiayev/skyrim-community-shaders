#pragma once

#include "EffectManager.h"
#include "WeatherManager.h"

class MenuManager
{
public:
	static MenuManager& GetSingleton();

	// Main UI rendering method
	void RenderImGui();

private:
	// UI section rendering methods
	void RenderSettingsPanel();
	/** @brief Preset combo that hotswaps and persists the selection, with Refresh and Open Folder buttons. */
	void RenderPresetSelector();
	void RenderWeatherControl();
	void RenderDebugControl();

	// Helper UI methods
	void RenderAllSettings();
	float GetTimeOfDayBlendFactor(int timeIndex) const;
};