#include "Menu.h"

#include <dinput.h>
#include <magic_enum.hpp>

#include "ShaderCache.h"
#include "State.h"

#include "Feature.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/WaterBlending.h"

#define SETTING_MENU_TOGGLEKEY "Toggle Key"
#define SETTING_MENU_FONTSCALE "Font Scale"

void SetupImGuiStyle()
{
	auto& style = ImGui::GetStyle();
	auto& colors = style.Colors;

	// Theme from https://github.com/ArranzCNL/ImprovedCameraSE-NG

	style.WindowTitleAlign = ImVec2(0.5, 0.5);
	style.FramePadding = ImVec2(4, 4);

	// Rounded slider grabber
	style.GrabRounding = 12.0f;

	// Window
	colors[ImGuiCol_WindowBg] = ImVec4{ 0.118f, 0.118f, 0.118f, 0.784f };
	colors[ImGuiCol_ResizeGrip] = ImVec4{ 0.2f, 0.2f, 0.2f, 0.5f };
	colors[ImGuiCol_ResizeGripHovered] = ImVec4{ 0.3f, 0.3f, 0.3f, 0.75f };
	colors[ImGuiCol_ResizeGripActive] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };

	// Header
	colors[ImGuiCol_Header] = ImVec4{ 0.2f, 0.2f, 0.2f, 1.0f };
	colors[ImGuiCol_HeaderHovered] = ImVec4{ 0.3f, 0.3f, 0.3f, 1.0f };
	colors[ImGuiCol_HeaderActive] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };

	// Title
	colors[ImGuiCol_TitleBg] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };
	colors[ImGuiCol_TitleBgActive] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };
	colors[ImGuiCol_TitleBgCollapsed] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };

	// Frame Background
	colors[ImGuiCol_FrameBg] = ImVec4{ 0.2f, 0.2f, 0.2f, 1.0f };
	colors[ImGuiCol_FrameBgHovered] = ImVec4{ 0.3f, 0.3f, 0.3f, 1.0f };
	colors[ImGuiCol_FrameBgActive] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };

	// Button
	colors[ImGuiCol_Button] = ImVec4{ 0.2f, 0.2f, 0.2f, 1.0f };
	colors[ImGuiCol_ButtonHovered] = ImVec4{ 0.3f, 0.3f, 0.3f, 1.0f };
	colors[ImGuiCol_ButtonActive] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };

	// Tab
	colors[ImGuiCol_Tab] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };
	colors[ImGuiCol_TabHovered] = ImVec4{ 0.38f, 0.38f, 0.38f, 1.0f };
	colors[ImGuiCol_TabActive] = ImVec4{ 0.28f, 0.28f, 0.28f, 1.0f };
	colors[ImGuiCol_TabUnfocused] = ImVec4{ 0.15f, 0.15f, 0.15f, 1.0f };
	colors[ImGuiCol_TabUnfocusedActive] = ImVec4{ 0.2f, 0.2f, 0.2f, 1.0f };
}

bool IsEnabled = false;

Menu::~Menu()
{
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void Menu::Load(json& o_json)
{
	if (o_json[SETTING_MENU_TOGGLEKEY].is_number_unsigned()) {
		toggleKey = o_json[SETTING_MENU_TOGGLEKEY];
	}
	if (o_json[SETTING_MENU_FONTSCALE].is_number_float()) {
		fontScale = o_json[SETTING_MENU_FONTSCALE];
	}
}

void Menu::Save(json& o_json)
{
	json menu;
	menu[SETTING_MENU_TOGGLEKEY] = toggleKey;
	menu[SETTING_MENU_FONTSCALE] = fontScale;

	o_json["Menu"] = menu;
}

#define IM_VK_KEYPAD_ENTER (VK_RETURN + 256)
RE::BSEventNotifyControl Menu::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_eventSource)
{
	if (!a_event || !a_eventSource)
		return RE::BSEventNotifyControl::kContinue;

	auto& io = ImGui::GetIO();

	for (auto event = *a_event; event; event = event->next) {
		if (event->eventType == RE::INPUT_EVENT_TYPE::kChar) {
			io.AddInputCharacter(event->AsCharEvent()->keycode);
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
			case DIK_NUMPADENTER:
				key = IM_VK_KEYPAD_ENTER;
				break;
			case DIK_LMENU:
				key = VK_LMENU;
				break;  // left alt
			case DIK_LCONTROL:
				key = VK_LCONTROL;
				break;  // left control
			case DIK_LSHIFT:
				key = VK_LSHIFT;
				break;  // left shift
			case DIK_RMENU:
				key = VK_RMENU;
				break;  // right alt
			case DIK_RCONTROL:
				key = VK_RCONTROL;
				break;  // right control
			case DIK_RSHIFT:
				key = VK_RSHIFT;
				break;  // right shift
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
				if (!button->IsPressed()) {
					if (settingToggleKey) {
						toggleKey = key;
						settingToggleKey = false;
					} else if (key == toggleKey) {
						IsEnabled = !IsEnabled;
						if (const auto controlMap = RE::ControlMap::GetSingleton()) {
							controlMap->GetRuntimeData().ignoreKeyboardMouse = IsEnabled;
						}
					}
				}

				io.AddKeyEvent(VirtualKeyToImGuiKey(key), button->IsPressed());

				if (key == VK_LCONTROL || key == VK_RCONTROL)
					io.AddKeyEvent(ImGuiMod_Ctrl, button->IsPressed());
				else if (key == VK_LSHIFT || key == VK_RSHIFT)
					io.AddKeyEvent(ImGuiMod_Shift, button->IsPressed());
				else if (key == VK_LMENU || key == VK_RMENU)
					io.AddKeyEvent(ImGuiMod_Alt, button->IsPressed());
				break;
			case RE::INPUT_DEVICE::kMouse:
				logger::trace("Detect mouse scan code {} value {} pressed: {}", scan_code, button->Value(), button->IsPressed());
				if (scan_code > 7)  // middle scroll
					io.AddMouseWheelEvent(0, button->Value() * (scan_code == 8 ? 1 : -1));
				else {
					if (scan_code > 5)
						scan_code = 5;
					io.AddMouseButtonEvent(scan_code, button->IsPressed());
				}
				break;
			default:
				continue;
			}
		}
	}

	return RE::BSEventNotifyControl::kContinue;
}

void Menu::Init(IDXGISwapChain* swapchain, ID3D11Device* device, ID3D11DeviceContext* context)
{
	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	auto& imgui_io = ImGui::GetIO();

	imgui_io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
	imgui_io.BackendFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset;

	imgui_io.Fonts->AddFontFromFileTTF("Data\\Interface\\CommunityShaders\\Fonts\\Atkinson-Hyperlegible-Regular-102.ttf", 24);

	DXGI_SWAP_CHAIN_DESC desc;
	swapchain->GetDesc(&desc);

	// Setup Platform/Renderer backends
	ImGui_ImplWin32_Init(desc.OutputWindow);
	ImGui_ImplDX11_Init(device, context);

	float trueScale = exp2(fontScale);
	auto& style = ImGui::GetStyle();
	style.ScaleAllSizes(trueScale);
	auto& io = ImGui::GetIO();
	io.FontGlobalScale = trueScale;
}

void Menu::DrawSettings()
{
	ImGuiStyle oldStyle = ImGui::GetStyle();
	SetupImGuiStyle();
	static bool visible = false;

	ImGui::DockSpaceOverViewport(NULL, ImGuiDockNodeFlags_PassthruCentralNode);

	ImGui::SetNextWindowSize({ 1000, 1000 }, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos({ 0, 0 }, ImGuiCond_FirstUseEver);
	if (ImGui::Begin(std::format("Skyrim Community Shaders {}", Plugin::VERSION.string(".")).c_str(), &IsEnabled)) {
		auto& shaderCache = SIE::ShaderCache::Instance();

		if (ImGui::BeginTable("##LeButtons", 4, ImGuiTableFlags_SizingStretchSame)) {
			ImGui::TableNextColumn();
			if (ImGui::Button("Save Settings", { -1, 0 })) {
				State::GetSingleton()->Save();
			}

			ImGui::TableNextColumn();
			if (ImGui::Button("Load Settings", { -1, 0 })) {
				State::GetSingleton()->Load();
			}

			ImGui::TableNextColumn();
			if (ImGui::Button("Clear Shader Cache", { -1, 0 })) {
				shaderCache.Clear();
				ScreenSpaceShadows::GetSingleton()->ClearComputeShader();
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

			ImGui::TableNextColumn();
			if (ImGui::Button("Clear Disk Cache", { -1, 0 })) {
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

			ImGui::EndTable();
		}

		ImGui::Spacing();

		if (ImGui::CollapsingHeader("Menu", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (settingToggleKey) {
				ImGui::Text("Press any key to set as toggle key...");
			} else {
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Toggle Key:");
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", KeyIdToString(toggleKey));

				ImGui::AlignTextToFramePadding();
				ImGui::SameLine();
				if (ImGui::Button("Change")) {
					settingToggleKey = true;
				}
			}

			if (ImGui::SliderFloat("Font Scale", &fontScale, -2.f, 2.f, "%.2f")) {
				float trueScale = exp2(fontScale);
				auto& style = ImGui::GetStyle();
				style.ScaleAllSizes(trueScale);
				auto& io = ImGui::GetIO();
				io.FontGlobalScale = trueScale;
			}
		}

		if (ImGui::CollapsingHeader("Advanced", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool useDump = shaderCache.IsDump();
			if (ImGui::Checkbox("Dump Shaders", &useDump)) {
				shaderCache.SetDump(useDump);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
				ImGui::Text("Dump shaders at startup. This should be used only when reversing shaders. Normal users don't need this.");
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
			spdlog::level::level_enum logLevel = State::GetSingleton()->GetLogLevel();
			const char* items[] = {
				"trace",
				"debug",
				"info",
				"warn",
				"err",
				"critical",
				"off"
			};
			static int item_current = static_cast<int>(logLevel);
			if (ImGui::Combo("Log Level", &item_current, items, IM_ARRAYSIZE(items))) {
				ImGui::SameLine();
				State::GetSingleton()->SetLogLevel(static_cast<spdlog::level::level_enum>(item_current));
			}
			if (ImGui::IsItemHovered()) {
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
				ImGui::Text("Log level. Trace is most verbose. Default is info.");
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
		}

		if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen)) {
			bool useCustomShaders = shaderCache.IsEnabled();
			if (ImGui::BeginTable("##GeneralToggles", 3, ImGuiTableFlags_SizingStretchSame)) {
				ImGui::TableNextColumn();
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
				ImGui::TableNextColumn();
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
				ImGui::TableNextColumn();
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

				ImGui::EndTable();
			}
		}

		if (ImGui::CollapsingHeader("Replace Original Shaders", ImGuiTreeNodeFlags_DefaultOpen)) {
			auto state = State::GetSingleton();
			if (ImGui::BeginTable("##ReplaceToggles", 3, ImGuiTableFlags_SizingStretchSame)) {
				for (int classIndex = 0; classIndex < RE::BSShader::Type::Total - 1; ++classIndex) {
					ImGui::TableNextColumn();

					auto type = (RE::BSShader::Type)(classIndex + 1);
					if (!(SIE::ShaderCache::IsSupportedShader(type) ||
							// allow all shaders if debug or trace logging
							(state->GetLogLevel()) <= spdlog::level::debug)) {
						ImGui::BeginDisabled();
						ImGui::Checkbox(std::format("{}", magic_enum::enum_name(type)).c_str(), &state->enabledClasses[classIndex]);
						ImGui::EndDisabled();
					} else
						ImGui::Checkbox(std::format("{}", magic_enum::enum_name(type)).c_str(), &state->enabledClasses[classIndex]);
				}

				ImGui::EndTable();
			}
		}

		ImGui::Separator();

		if (ImGui::BeginTable("Feature Table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn("##ListOfFeatures", 0, 3);
			ImGui::TableSetupColumn("##FeatureConfig", 0, 7);

			static size_t selectedFeature = 0;
			auto& featureList = Feature::GetFeatureList();

			ImGui::TableNextColumn();
			if (ImGui::BeginListBox("##FeatureList", { -FLT_MIN, -FLT_MIN })) {
				for (size_t i = 0; i < featureList.size(); i++)
					if (ImGui::Selectable(featureList[i]->GetName().c_str(), selectedFeature == i))
						selectedFeature = i;
				ImGui::EndListBox();
			}

			ImGui::TableNextColumn();
			if (ImGui::BeginChild("##FeatureConfigFrame", { 0, 0 }, true)) {
				bool shownFeature = false;
				for (size_t i = 0; i < featureList.size(); i++)
					if (i == selectedFeature) {
						shownFeature = true;
						featureList[i]->DrawSettings();
					}
				if (!shownFeature)
					ImGui::TextDisabled("Please select a feature on the left.");
			}
			ImGui::EndChild();

			ImGui::EndTable();
		}
	}

	ImGui::End();
	ImGuiStyle& style = ImGui::GetStyle();
	style = oldStyle;

	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void Menu::DrawOverlay()
{
	// Start the Dear ImGui frame
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

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

	if (IsEnabled) {
		ImGui::GetIO().MouseDrawCursor = true;
		DrawSettings();
	} else {
		ImGui::GetIO().MouseDrawCursor = false;
	}

	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

const char* Menu::KeyIdToString(uint32_t key)
{
	if (key >= 256)
		return "";

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

const ImGuiKey Menu::VirtualKeyToImGuiKey(WPARAM vkKey)
{
	switch (vkKey) {
	case VK_TAB:
		return ImGuiKey_Tab;
	case VK_LEFT:
		return ImGuiKey_LeftArrow;
	case VK_RIGHT:
		return ImGuiKey_RightArrow;
	case VK_UP:
		return ImGuiKey_UpArrow;
	case VK_DOWN:
		return ImGuiKey_DownArrow;
	case VK_PRIOR:
		return ImGuiKey_PageUp;
	case VK_NEXT:
		return ImGuiKey_PageDown;
	case VK_HOME:
		return ImGuiKey_Home;
	case VK_END:
		return ImGuiKey_End;
	case VK_INSERT:
		return ImGuiKey_Insert;
	case VK_DELETE:
		return ImGuiKey_Delete;
	case VK_BACK:
		return ImGuiKey_Backspace;
	case VK_SPACE:
		return ImGuiKey_Space;
	case VK_RETURN:
		return ImGuiKey_Enter;
	case VK_ESCAPE:
		return ImGuiKey_Escape;
	case VK_OEM_7:
		return ImGuiKey_Apostrophe;
	case VK_OEM_COMMA:
		return ImGuiKey_Comma;
	case VK_OEM_MINUS:
		return ImGuiKey_Minus;
	case VK_OEM_PERIOD:
		return ImGuiKey_Period;
	case VK_OEM_2:
		return ImGuiKey_Slash;
	case VK_OEM_1:
		return ImGuiKey_Semicolon;
	case VK_OEM_PLUS:
		return ImGuiKey_Equal;
	case VK_OEM_4:
		return ImGuiKey_LeftBracket;
	case VK_OEM_5:
		return ImGuiKey_Backslash;
	case VK_OEM_6:
		return ImGuiKey_RightBracket;
	case VK_OEM_3:
		return ImGuiKey_GraveAccent;
	case VK_CAPITAL:
		return ImGuiKey_CapsLock;
	case VK_SCROLL:
		return ImGuiKey_ScrollLock;
	case VK_NUMLOCK:
		return ImGuiKey_NumLock;
	case VK_SNAPSHOT:
		return ImGuiKey_PrintScreen;
	case VK_PAUSE:
		return ImGuiKey_Pause;
	case VK_NUMPAD0:
		return ImGuiKey_Keypad0;
	case VK_NUMPAD1:
		return ImGuiKey_Keypad1;
	case VK_NUMPAD2:
		return ImGuiKey_Keypad2;
	case VK_NUMPAD3:
		return ImGuiKey_Keypad3;
	case VK_NUMPAD4:
		return ImGuiKey_Keypad4;
	case VK_NUMPAD5:
		return ImGuiKey_Keypad5;
	case VK_NUMPAD6:
		return ImGuiKey_Keypad6;
	case VK_NUMPAD7:
		return ImGuiKey_Keypad7;
	case VK_NUMPAD8:
		return ImGuiKey_Keypad8;
	case VK_NUMPAD9:
		return ImGuiKey_Keypad9;
	case VK_DECIMAL:
		return ImGuiKey_KeypadDecimal;
	case VK_DIVIDE:
		return ImGuiKey_KeypadDivide;
	case VK_MULTIPLY:
		return ImGuiKey_KeypadMultiply;
	case VK_SUBTRACT:
		return ImGuiKey_KeypadSubtract;
	case VK_ADD:
		return ImGuiKey_KeypadAdd;
	case IM_VK_KEYPAD_ENTER:
		return ImGuiKey_KeypadEnter;
	case VK_LSHIFT:
		return ImGuiKey_LeftShift;
	case VK_LCONTROL:
		return ImGuiKey_LeftCtrl;
	case VK_LMENU:
		return ImGuiKey_LeftAlt;
	case VK_LWIN:
		return ImGuiKey_LeftSuper;
	case VK_RSHIFT:
		return ImGuiKey_RightShift;
	case VK_RCONTROL:
		return ImGuiKey_RightCtrl;
	case VK_RMENU:
		return ImGuiKey_RightAlt;
	case VK_RWIN:
		return ImGuiKey_RightSuper;
	case VK_APPS:
		return ImGuiKey_Menu;
	case '0':
		return ImGuiKey_0;
	case '1':
		return ImGuiKey_1;
	case '2':
		return ImGuiKey_2;
	case '3':
		return ImGuiKey_3;
	case '4':
		return ImGuiKey_4;
	case '5':
		return ImGuiKey_5;
	case '6':
		return ImGuiKey_6;
	case '7':
		return ImGuiKey_7;
	case '8':
		return ImGuiKey_8;
	case '9':
		return ImGuiKey_9;
	case 'A':
		return ImGuiKey_A;
	case 'B':
		return ImGuiKey_B;
	case 'C':
		return ImGuiKey_C;
	case 'D':
		return ImGuiKey_D;
	case 'E':
		return ImGuiKey_E;
	case 'F':
		return ImGuiKey_F;
	case 'G':
		return ImGuiKey_G;
	case 'H':
		return ImGuiKey_H;
	case 'I':
		return ImGuiKey_I;
	case 'J':
		return ImGuiKey_J;
	case 'K':
		return ImGuiKey_K;
	case 'L':
		return ImGuiKey_L;
	case 'M':
		return ImGuiKey_M;
	case 'N':
		return ImGuiKey_N;
	case 'O':
		return ImGuiKey_O;
	case 'P':
		return ImGuiKey_P;
	case 'Q':
		return ImGuiKey_Q;
	case 'R':
		return ImGuiKey_R;
	case 'S':
		return ImGuiKey_S;
	case 'T':
		return ImGuiKey_T;
	case 'U':
		return ImGuiKey_U;
	case 'V':
		return ImGuiKey_V;
	case 'W':
		return ImGuiKey_W;
	case 'X':
		return ImGuiKey_X;
	case 'Y':
		return ImGuiKey_Y;
	case 'Z':
		return ImGuiKey_Z;
	case VK_F1:
		return ImGuiKey_F1;
	case VK_F2:
		return ImGuiKey_F2;
	case VK_F3:
		return ImGuiKey_F3;
	case VK_F4:
		return ImGuiKey_F4;
	case VK_F5:
		return ImGuiKey_F5;
	case VK_F6:
		return ImGuiKey_F6;
	case VK_F7:
		return ImGuiKey_F7;
	case VK_F8:
		return ImGuiKey_F8;
	case VK_F9:
		return ImGuiKey_F9;
	case VK_F10:
		return ImGuiKey_F10;
	case VK_F11:
		return ImGuiKey_F11;
	case VK_F12:
		return ImGuiKey_F12;
	default:
		return ImGuiKey_None;
	};
}