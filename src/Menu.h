#pragma once

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <chrono>
#include <shared_mutex>

using namespace std::chrono;
#define BUFFER_VIEWER_NODE(a_value, a_scale)                                                                 \
	if (ImGui::TreeNode(#a_value)) {                                                                         \
		ImGui::Image(a_value->srv.get(), { a_value->desc.Width * a_scale, a_value->desc.Height * a_scale }); \
		ImGui::TreePop();                                                                                    \
	}

#define BUFFER_VIEWER_NODE_BULLET(a_value, a_scale) \
	ImGui::BulletText(#a_value);                    \
	ImGui::Image(a_value->srv.get(), { a_value->desc.Width * a_scale, a_value->desc.Height * a_scale });

#define ADDRESS_NODE(a_value)                                                                        \
	if (ImGui::Button(#a_value)) {                                                                   \
		ImGui::SetClipboardText(std::format("{0:x}", reinterpret_cast<uintptr_t>(a_value)).c_str()); \
	}                                                                                                \
	if (ImGui::IsItemHovered())                                                                      \
		ImGui::SetTooltip(std::format("Copy {} Address to Clipboard", #a_value).c_str());

class Menu
{
public:
	~Menu();

	static Menu* GetSingleton()
	{
		static Menu menu;
		return &menu;
	}

	bool initialized = false;

	void Load(json& o_json);
	void Save(json& o_json);

	void Init(IDXGISwapChain* swapchain, ID3D11Device* device, ID3D11DeviceContext* context);
	void DrawSettings();
	void DrawOverlay();

	void ProcessInputEvents(RE::InputEvent* const* a_events);
	bool ShouldSwallowInput();
	void OnFocusLost();

	struct ThemeSettings
	{
		float FontScale = REL::Module::IsVR() ? -0.5f : 0.f;  // exponential
		ImVec4 BackgroundColor{ 0.f, 0.f, 0.f, 0.5f };
		ImVec4 TextColor{ 1.f, 1.f, 1.f, 1.f };
		ImVec4 BorderColor{ 0.569f, 0.545f, 0.506f, 0.5f };
		float BorderSize{ 3.f };
		float FrameBorderSize{ 1.5f };
		ImVec2 WindowPadding{ 16.f, 16.f };
		float WindowRounding{ 0.f };
		float IndentSpacing{ 8.f };
		ImVec2 FramePadding{ 4.0f, 4.0f };
		ImVec2 CellPadding{ 16.f, 2.f };
		ImVec2 ItemSpacing{ 8.f, 12.f };
	};

	struct Settings
	{
		uint32_t ToggleKey = VK_END;
		uint32_t SkipCompilationKey = VK_ESCAPE;
		uint32_t EffectToggleKey = VK_MULTIPLY;  // toggle all effects
		ThemeSettings Theme;
	};

private:
	Settings settings;

	uint32_t priorShaderKey = VK_PRIOR;  // used for blocking shaders in debugging
	uint32_t nextShaderKey = VK_NEXT;    // used for blocking shaders in debugging

	bool settingToggleKey = false;
	bool settingSkipCompilationKey = false;
	bool settingsEffectsToggle = false;
	uint32_t testInterval = 0;     // Seconds to wait before toggling user/test settings
	bool inTestMode = false;       // Whether we're in test mode
	bool usingTestConfig = false;  // Whether we're using the test config

	std::chrono::steady_clock::time_point lastTestSwitch = high_resolution_clock::now();  // Time of last test switch

	Menu() = default;
	void SetupImGuiStyle() const;
	const char* KeyIdToString(uint32_t key);
	const ImGuiKey VirtualKeyToImGuiKey(WPARAM vkKey);

	void DrawGeneralSettings();
	void DrawAdvancedSettings();
	void DrawDisplaySettings();
	void DrawFooter();

	class CharEvent : public RE::InputEvent
	{
	public:
		uint32_t keyCode;  // 18 (ascii code)
	};

	struct KeyEvent
	{
		explicit KeyEvent(const RE::ButtonEvent* a_event) :
			keyCode(a_event->GetIDCode()),
			device(a_event->GetDevice()),
			eventType(a_event->GetEventType()),
			value(a_event->Value()),
			heldDownSecs(a_event->HeldDuration()) {}

		explicit KeyEvent(const CharEvent* a_event) :
			keyCode(a_event->keyCode),
			device(a_event->GetDevice()),
			eventType(a_event->GetEventType()) {}

		[[nodiscard]] constexpr bool IsPressed() const noexcept { return value > 0.0F; }
		[[nodiscard]] constexpr bool IsRepeating() const noexcept { return heldDownSecs > 0.0F; }
		[[nodiscard]] constexpr bool IsDown() const noexcept { return IsPressed() && (heldDownSecs == 0.0F); }
		[[nodiscard]] constexpr bool IsHeld() const noexcept { return IsPressed() && IsRepeating(); }
		[[nodiscard]] constexpr bool IsUp() const noexcept { return (value == 0.0F) && IsRepeating(); }

		uint32_t keyCode;
		RE::INPUT_DEVICE device;
		RE::INPUT_EVENT_TYPE eventType;
		float value = 0;
		float heldDownSecs = 0;
	};
	const uint32_t DIKToVK(uint32_t DIK);
	mutable std::shared_mutex _inputEventMutex;
	std::vector<KeyEvent> _keyEventQueue{};
	void addToEventQueue(KeyEvent e);
	void ProcessInputEventQueue();
};