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

	RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
		RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

	void DrawSettings();
	void DrawOverlay();

private:
	Menu()
	{
	}
};
