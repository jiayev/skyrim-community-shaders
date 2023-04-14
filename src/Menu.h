#pragma once

#define IMGUI_DISABLE_INCLUDE_IMCONFIG_H
#include "imgui.h"
#include "reshade/reshade.hpp"

class Menu : public RE::BSTEventSink<RE::InputEvent*>
{
public:
	static Menu* GetSingleton()
	{
		static Menu menu;
		return &menu;
	}

	void Load(json& o_json);
	void Save(json& o_json);

	RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
		RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

	void DrawSettings();
	void DrawOverlay();

private:

	uint32_t toggleKey = VK_END;
	bool settingToggleKey = false;

	Menu() { }
	const char* KeyIdToString(uint32_t key);
};
