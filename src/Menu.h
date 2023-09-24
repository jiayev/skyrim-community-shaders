#pragma once

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

class Menu : public RE::BSTEventSink<RE::InputEvent*>
{
public:
	~Menu();

	static Menu* GetSingleton()
	{
		static Menu menu;
		return &menu;
	}

	void Load(json& o_json);
	void Save(json& o_json);

	RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
		RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

	void Init(IDXGISwapChain* swapchain, ID3D11Device* device, ID3D11DeviceContext* context);
	void DrawSettings();
	void DrawOverlay();

private:
	uint32_t toggleKey = VK_END;
	uint32_t skipCompilationKey = VK_ESCAPE;
	bool settingToggleKey = false;
	bool settingSkipCompilationKey = false;
	float fontScale = 0.f;  // exponential

	Menu() {}
	const char* KeyIdToString(uint32_t key);
	const ImGuiKey VirtualKeyToImGuiKey(WPARAM vkKey);
};