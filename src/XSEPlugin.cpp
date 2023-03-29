#include "Hooks.h"

#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include <fmt/xchar.h>


extern "C" DLLEXPORT const char* NAME = "Skyrim Community Shaders";
extern "C" DLLEXPORT const char* DESCRIPTION = "";

HMODULE m_hModule;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
		m_hModule = hModule;
	return TRUE;
}

void DrawOverlayCallback(reshade::api::effect_runtime*)
{
	Menu::GetSingleton()->DrawOverlay();
}

void MessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kPostPostLoad:
		{
			const std::array dlls = {
				L"ShaderTools",
				L"ENBHelperSE",
				L"ENBHelperVR",
				L"ENBHelperPlus"
			};
			auto incompatible = false;
			for (const auto dll : dlls) {
				if (GetModuleHandle(dll)) {
					logger::info(fmt::runtime("Incompatible DLL {} detected, will disable all hooks and features"), stl::utf16_to_utf8(dll).value_or("<unicode conversion error>"s));
					incompatible = true;
				}
			}
			if (!incompatible) {
				Hooks::Install();

				auto& shaderCache = SIE::ShaderCache::Instance();

				shaderCache.SetEnabled(true);
				shaderCache.SetAsync(true);
				shaderCache.SetDiskCache(true);

				State::GetSingleton()->Load();

				shaderCache.ValidateDiskCache();

				if (reshade::register_addon(m_hModule)) {
					logger::info("ReShade: Registered add-on");
					reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOverlayCallback);
				} else {
					logger::info("ReShade: Could not register add-on");
				}
				break;
			}
		case SKSE::MessagingInterface::kDataLoaded:
			{
				RE::BSInputDeviceManager::GetSingleton()->AddEventSink(Menu::GetSingleton());

				auto& shaderCache = SIE::ShaderCache::Instance();

				while (shaderCache.GetCompletedTasks() != shaderCache.GetTotalTasks()) {
					std::this_thread::sleep_for(100ms);
				}

				if (shaderCache.IsDiskCache()) {
					shaderCache.WriteDiskCacheInfo();
				}

				break;
			}
		}
	}

	bool Load()
	{
		auto messaging = SKSE::GetMessagingInterface();
		messaging->RegisterListener("SKSE", MessageHandler);

		return true;
	}