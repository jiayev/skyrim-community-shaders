#include "Hooks.h"

#include "ShaderCache.h"
#include "State.h"
#include "Menu.h"


std::list<std::string> errors;

void MessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kPostPostLoad:
		{
			const std::array dlls = {
				L"ShaderTools.dll",
				L"SSEShaderTools.dll"
			};

			for (const auto dll : dlls) {
				if (GetModuleHandle(dll)) {
					auto errorMessage = std::format("Incompatible DLL {} detected", stl::utf16_to_utf8(dll).value_or("<unicode conversion error>"s));
					logger::error("{}", errorMessage);

					errors.push_back(errorMessage);
				}
			}

			if (errors.empty()) {
				Hooks::Install();

				auto& shaderCache = SIE::ShaderCache::Instance();

				shaderCache.SetEnabled(true);
				shaderCache.SetAsync(true);
				shaderCache.SetDiskCache(true);

				State::GetSingleton()->Load();

				shaderCache.ValidateDiskCache();
			}

			break;
		}
	case SKSE::MessagingInterface::kDataLoaded:
		{
			for (auto it = errors.begin(); it != errors.end(); ++it) {
				auto& errorMessage = *it;
				RE::DebugMessageBox(std::format("Community Shaders\n{}, will disable all hooks and features", errorMessage).c_str());
			}

			if (errors.empty()) {
				RE::BSInputDeviceManager::GetSingleton()->AddEventSink(Menu::GetSingleton());

				auto& shaderCache = SIE::ShaderCache::Instance();

				while (shaderCache.GetCompletedTasks() != shaderCache.GetTotalTasks()) {
					std::this_thread::sleep_for(100ms);
				}

				if (shaderCache.IsDiskCache()) {
					shaderCache.WriteDiskCacheInfo();
				}
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