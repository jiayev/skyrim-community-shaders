#include "Menu.h"

#include <dinput.h>
#include <magic_enum.hpp>

#include "ShaderCache.h"
#include "State.h"

#include "Features/GrassLighting.h"
#include "Features/DistantTreeLighting.h"


void SetupImGuiStyle()
{
	ImGuiStyle& style = ImGui::GetStyle();
	style.Alpha = 0.9f;
}
bool IsEnabled = false;

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
				if (key == VK_END && !button->IsPressed()) {
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
	ImGui::Begin("Skyrim Community Shaders", &IsEnabled);

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