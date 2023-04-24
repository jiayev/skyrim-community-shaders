#include "Menu.h"

#include <dinput.h>
#include <magic_enum.hpp>

#include "ShaderCache.h"
#include "State.h"

#include "Features/GrassLighting.h"
#include "Features/DistantTreeLighting.h"
#include "Features/GrassCollision.h"

#define SETTING_MENU_TOGGLEKEY "Toggle Key"

void SetupImGuiStyle()
{
	ImGuiStyle& style = ImGui::GetStyle();
	style.Alpha = 0.9f;
}
bool IsEnabled = false;

void Menu::Load(json& o_json)
{
	if (o_json[SETTING_MENU_TOGGLEKEY].is_number_unsigned()) {
		toggleKey = o_json[SETTING_MENU_TOGGLEKEY];
	}
}

void Menu::Save(json& o_json)
{
	json menu;
	menu[SETTING_MENU_TOGGLEKEY] = toggleKey;

	o_json["Menu"] = menu;
}

RE::BSEventNotifyControl Menu::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_eventSource)
{
	if (!a_event || !a_eventSource)
		return RE::BSEventNotifyControl::kContinue;

	for (auto event = *a_event; event; event = event->next) {
		if (event->eventType == RE::INPUT_EVENT_TYPE::kChar) {
		} else if (event->eventType == RE::INPUT_EVENT_TYPE::kButton) {
			const auto button = static_cast<RE::ButtonEvent*>(event);
			if (!button || (button->IsPressed() && !button->IsDown()))
				continue;

			auto scan_code = button->GetIDCode();
			uint32_t key = MapVirtualKeyEx(scan_code, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));

			switch (scan_code) {
			case DIK_LEFTARROW:
				key = VK_LEFT;
				break;
			case DIK_RIGHTARROW:
				key = VK_RIGHT;
				break;
			case DIK_UPARROW:
				key = VK_UP;
				break;
			case DIK_DOWNARROW:
				key = VK_DOWN;
				break;
			case DIK_DELETE:
				key = VK_DELETE;
				break;
			case DIK_END:
				key = VK_END;
				break;
			case DIK_HOME:
				key = VK_HOME;
				break;  // pos1
			case DIK_PRIOR:
				key = VK_PRIOR;
				break;  // page up
			case DIK_NEXT:
				key = VK_NEXT;
				break;  // page down
			case DIK_INSERT:
				key = VK_INSERT;
				break;
			case DIK_NUMPAD0:
				key = VK_NUMPAD0;
				break;
			case DIK_NUMPAD1:
				key = VK_NUMPAD1;
				break;
			case DIK_NUMPAD2:
				key = VK_NUMPAD2;
				break;
			case DIK_NUMPAD3:
				key = VK_NUMPAD3;
				break;
			case DIK_NUMPAD4:
				key = VK_NUMPAD4;
				break;
			case DIK_NUMPAD5:
				key = VK_NUMPAD5;
				break;
			case DIK_NUMPAD6:
				key = VK_NUMPAD6;
				break;
			case DIK_NUMPAD7:
				key = VK_NUMPAD7;
				break;
			case DIK_NUMPAD8:
				key = VK_NUMPAD8;
				break;
			case DIK_NUMPAD9:
				key = VK_NUMPAD9;
				break;
			case DIK_DECIMAL:
				key = VK_DECIMAL;
				break;
			case DIK_RMENU:
				key = VK_RMENU;
				break;  // right alt
			case DIK_RCONTROL:
				key = VK_RCONTROL;
				break;  // right control
			case DIK_LWIN:
				key = VK_LWIN;
				break;  // left win
			case DIK_RWIN:
				key = VK_RWIN;
				break;  // right win
			case DIK_APPS:
				key = VK_APPS;
				break;
			default:
				break;
			}

			switch (button->device.get()) {
			case RE::INPUT_DEVICE::kKeyboard:
				if (key == toggleKey && !button->IsPressed()) {

					// Avoid closing menu when setting the toggle key
					if (settingToggleKey) {
						settingToggleKey = false;
						break;
					}

					IsEnabled = !IsEnabled;
					if (const auto controlMap = RE::ControlMap::GetSingleton()) {
						controlMap->ignoreKeyboardMouse = IsEnabled;
					}
				}
				break;
			default:
				continue;
			}
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}

void Menu::DrawSettings()
{
	ImGuiStyle oldStyle = ImGui::GetStyle();
	SetupImGuiStyle();
	static bool visible = false;

	ImGui::SetNextWindowSize({ 1024, 1024 }, ImGuiCond_Once);
	ImGui::Begin(std::format("Skyrim Community Shaders {}", Plugin::VERSION.string(".")).c_str(), &IsEnabled);

	auto& shaderCache = SIE::ShaderCache::Instance();

	if (ImGui::Button("Save Settings")) {
		State::GetSingleton()->Save();
	}

	ImGui::SameLine();

	if (ImGui::Button("Load Settings")) {
		State::GetSingleton()->Load();
	}
	ImGui::SameLine();

	if (ImGui::Button("Clear Shader Cache")) {
		shaderCache.Clear();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::Text("The Shader Cache is the collection of compiled shaders which replace the vanilla shaders at runtime.");
		ImGui::Text("Clearing the shader cache will mean that shaders are recompiled only when the game re-encounters them.");
		ImGui::Text("This is only needed for hot-loading shaders for development purposes.");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	ImGui::SameLine();

	if (ImGui::Button("Clear Disk Cache")) {
		shaderCache.DeleteDiskCache();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::Text("The Disk Cache is a collection of compiled shaders on disk, which are automatically created when shaders are added to the Shader Cache.");
		ImGui::Text("If you do not have a Disk Cache, or it is outdated or invalid, you will see \"Compiling Shaders\" in the upper-left corner.");
		ImGui::Text("After this has completed you will no longer see this message apart from when loading from the Disk Cache.");
		ImGui::Text("Only delete the Disk Cache manually if you are encountering issues.");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

	ImGui::Spacing();

	if (ImGui::CollapsingHeader("Menu", ImGuiTreeNodeFlags_DefaultOpen)) {
		
		// Check if we're waiting for input
		if (settingToggleKey) {
			// Loop over all the keys and check if any of them are pressed
			for (int i = 0; i < IM_ARRAYSIZE(ImGui::GetIO().KeysDown); i++) {
				if (ImGui::IsKeyPressed(i)) {
					// If a key is pressed, set the selected key code and break out of the loop
					toggleKey = i;
					break;
				}
			}
		}
		if (settingToggleKey) 
		{
			ImGui::Text("Press any key to set as toggle key...");
		} 
		else 
		{			
			ImGui::Text("Toggle Key:");
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", KeyIdToString(toggleKey));

			ImGui::SameLine();
			if (ImGui::Button("Change")) {
				settingToggleKey = true;
			}
		}
	}

	if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		bool useCustomShaders = shaderCache.IsEnabled();
		if (ImGui::Checkbox("Enable Shaders", &useCustomShaders)) {
			shaderCache.SetEnabled(useCustomShaders);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Disabling this effectively disables all features.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		bool useDiskCache = shaderCache.IsDiskCache();
		if (ImGui::Checkbox("Enable Disk Cache", &useDiskCache)) {
			shaderCache.SetDiskCache(useDiskCache);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Disabling this stops shaders from being loaded from disk, as well as stops shaders from being saved to it.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		bool useAsync = shaderCache.IsAsync();
		if (ImGui::Checkbox("Enable Async", &useAsync)) {
			shaderCache.SetAsync(useAsync);
		}
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Skips a shader being replaced if it hasn't been compiled yet. Also makes compilation blazingly fast!");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	if (ImGui::CollapsingHeader("Replace Original Shaders", ImGuiTreeNodeFlags_DefaultOpen)) {
		auto state = State::GetSingleton();
		for (int classIndex = 0; classIndex < RE::BSShader::Type::Total - 1; ++classIndex) {
			auto type = (RE::BSShader::Type)(classIndex + 1);
			if (SIE::ShaderCache::IsSupportedShader(type)) {
				ImGui::Checkbox(std::format("{}", magic_enum::enum_name(type)).c_str(), &state->enabledClasses[classIndex]);
			}
		}
	}

	if (ImGui::BeginTabBar("Features", ImGuiTabBarFlags_None)) {
		GrassLighting::GetSingleton()->DrawSettings();
		DistantTreeLighting::GetSingleton()->DrawSettings();
		GrassCollision::GetSingleton()->DrawSettings();
	}

	ImGui::End();
	ImGuiStyle& style = ImGui::GetStyle();
	style = oldStyle;
}

void Menu::DrawOverlay()
{
	uint64_t totalShaders = 0;
	uint64_t compiledShaders = 0;

	auto& shaderCache = SIE::ShaderCache::Instance();

	compiledShaders = shaderCache.GetCompletedTasks();
	totalShaders = shaderCache.GetTotalTasks();

	if (compiledShaders != totalShaders) {
		ImGui::SetNextWindowBgAlpha(1);
		ImGui::SetNextWindowPos(ImVec2(10, 10));
		if (!ImGui::Begin("ShaderCompilationInfo", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::End();
			return;
		}

		ImGui::Text("Compiling Shaders: %d / %d", compiledShaders, totalShaders);

		ImGui::End();
	}

	if (IsEnabled)
	{
	    ImGui::GetIO().MouseDrawCursor = true;
		DrawSettings();
	}
}

const char* Menu::KeyIdToString(uint32_t key)
{
	if (key >= 256)
		return std::string().c_str();

	static const char* keyboard_keys_international[256] = {
		"", "Left Mouse", "Right Mouse", "Cancel", "Middle Mouse", "X1 Mouse", "X2 Mouse", "", "Backspace", "Tab", "", "", "Clear", "Enter", "", "",
		"Shift", "Control", "Alt", "Pause", "Caps Lock", "", "", "", "", "", "", "Escape", "", "", "", "",
		"Space", "Page Up", "Page Down", "End", "Home", "Left Arrow", "Up Arrow", "Right Arrow", "Down Arrow", "Select", "", "", "Print Screen", "Insert", "Delete", "Help",
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "", "", "", "", "", "",
		"", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
		"P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Left Windows", "Right Windows", "Apps", "", "Sleep",
		"Numpad 0", "Numpad 1", "Numpad 2", "Numpad 3", "Numpad 4", "Numpad 5", "Numpad 6", "Numpad 7", "Numpad 8", "Numpad 9", "Numpad *", "Numpad +", "", "Numpad -", "Numpad Decimal", "Numpad /",
		"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16",
		"F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "", "", "", "", "", "", "", "",
		"Num Lock", "Scroll Lock", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
		"Left Shift", "Right Shift", "Left Control", "Right Control", "Left Menu", "Right Menu", "Browser Back", "Browser Forward", "Browser Refresh", "Browser Stop", "Browser Search", "Browser Favorites", "Browser Home", "Volume Mute", "Volume Down", "Volume Up",
		"Next Track", "Previous Track", "Media Stop", "Media Play/Pause", "Mail", "Media Select", "Launch App 1", "Launch App 2", "", "", "OEM ;", "OEM +", "OEM ,", "OEM -", "OEM .", "OEM /",
		"OEM ~", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
		"", "", "", "", "", "", "", "", "", "", "", "OEM [", "OEM \\", "OEM ]", "OEM '", "OEM 8",
		"", "", "OEM <", "", "", "", "", "", "", "", "", "", "", "", "", "",
		"", "", "", "", "", "", "Attn", "CrSel", "ExSel", "Erase EOF", "Play", "Zoom", "", "PA1", "OEM Clear", ""
	};

	return keyboard_keys_international[key];
}
