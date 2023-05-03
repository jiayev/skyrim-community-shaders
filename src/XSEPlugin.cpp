#include "Hooks.h"

#include "ShaderCache.h"
#include "State.h"
#include "Menu.h"


extern "C" DLLEXPORT const char* NAME = "Skyrim Community Shaders";
extern "C" DLLEXPORT const char* DESCRIPTION = "";

HMODULE m_hModule;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
		m_hModule = hModule;
	return TRUE;
}

void MessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kPostPostLoad:
		{
			Hooks::Install();

			auto& shaderCache = SIE::ShaderCache::Instance();

			shaderCache.SetEnabled(true);
			shaderCache.SetAsync(true);
			shaderCache.SetDiskCache(true);

			State::GetSingleton()->Load();

			shaderCache.ValidateDiskCache();

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