#include "Hooks.h"

#include "Deferred.h"
#include "FrameAnnotations.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Upscaling.h"

#include "ENB/ENBSeriesAPI.h"
#include "Features/ExtendedMaterials.h"
#include "Features/LightLimitFIx/ParticleLights.h"
#include "Features/LightLimitFix.h"
#define DLLEXPORT __declspec(dllexport)

std::list<std::string> errors;

bool Load();

void InitializeLog([[maybe_unused]] spdlog::level::level_enum a_level = spdlog::level::info)
{
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		util::report_and_fail("Failed to find standard logging directory"sv);
	}

	*path /= std::format("{}.log"sv, Plugin::NAME);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

#ifndef NDEBUG
	const auto level = spdlog::level::trace;
#else
	const auto level = a_level;
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
	log->set_level(level);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] [%s:%#] %v");
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
#ifndef NDEBUG
	while (!REX::W32::IsDebuggerPresent()) {};
#endif
	InitializeLog();
	logger::info("Loaded {} {}", Plugin::NAME, Plugin::VERSION.string());
	SKSE::Init(a_skse);
	return Load();
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginName(Plugin::NAME.data());
	v.PluginVersion(Plugin::VERSION);
	v.UsesAddressLibrary(true);
	v.HasNoStructUse();
	return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface*, SKSE::PluginInfo* pluginInfo)
{
	pluginInfo->name = SKSEPlugin_Version.pluginName;
	pluginInfo->infoVersion = SKSE::PluginInfo::kVersion;
	pluginInfo->version = SKSEPlugin_Version.pluginVersion;
	return true;
}

void MessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kPostPostLoad:
		{
			if (errors.empty()) {
				auto state = State::GetSingleton();
				state->PostPostLoad();  // state should load first so basic information is populated
				Deferred::Hooks::Install();
				TruePBR::GetSingleton()->PostPostLoad();
				if (!state->IsFeatureDisabled("Upscaling")) {
					Upscaling::InstallHooks();
				}
				Hooks::Install();
				FrameAnnotations::OnPostPostLoad();

				auto& shaderCache = SIE::ShaderCache::Instance();

				shaderCache.ValidateDiskCache();

				if (shaderCache.UseFileWatcher())
					shaderCache.StartFileWatcher();

				for (auto* feature : Feature::GetFeatureList()) {
					if (feature->loaded) {
						feature->PostPostLoad();
					}
				}
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
				FrameAnnotations::OnDataLoaded();

				auto& shaderCache = SIE::ShaderCache::Instance();
				shaderCache.menuLoaded = true;
				while (shaderCache.IsCompiling() && !shaderCache.backgroundCompilation) {
					std::this_thread::sleep_for(100ms);
				}

				if (shaderCache.IsDiskCache()) {
					shaderCache.WriteDiskCacheInfo();
				}

				TruePBR::GetSingleton()->DataLoaded();
				for (auto* feature : Feature::GetFeatureList()) {
					if (feature->loaded) {
						feature->DataLoaded();
					}
				}
			}

			break;
		}
	}
}

bool Load()
{
	if (ENB_API::RequestENBAPI()) {
		logger::info("ENB detected, disabling all hooks and features");
		return true;
	}

	if (REL::Module::IsVR()) {
		REL::IDDatabase::get().IsVRAddressLibraryAtLeastVersion("0.155.0", true);
	}

	auto messaging = SKSE::GetMessagingInterface();
	messaging->RegisterListener("SKSE", MessageHandler);

	auto state = State::GetSingleton();
	state->Load();
	auto log = spdlog::default_logger();
	log->set_level(state->GetLogLevel());

	const std::array dlls = {
		L"Data/SKSE/Plugins/ShaderTools.dll",
		L"Data/SKSE/Plugins/SSEShaderTools.dll"
	};

	for (const auto dll : dlls) {
		if (LoadLibrary(dll)) {
			auto errorMessage = std::format("Incompatible DLL {} detected", stl::utf16_to_utf8(dll).value_or("<unicode conversion error>"s));
			logger::error("{}", errorMessage);
			errors.push_back(errorMessage);
		}
	}

	if (errors.empty())
		Hooks::InstallD3DHooks();
	return true;
}