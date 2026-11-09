#include "D3DX9MathUpgrade.h"
#include "Deferred.h"
#include "Features/DrawcallLimitFix.h"
#include "Features/RemoteControl/NvPerfTrace.h"
#include "Features/RenderDoc.h"
#include "Features/Upscaling.h"
#include "FrameAnnotations.h"
#include "Globals.h"
#include "Hooks.h"
#include "I18n/I18n.h"
#include "Menu.h"
#include "Menu/ThemeManager.h"
#include "NativeMenu/NativeMenu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/LogTee.h"
#include "CrashDump.h"
#include "HangWatchdog.h"

#include <cstdlib>

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

/// Development switches (the CS_* environment variables) from `CommunityShaders.env` beside the log, one
/// NAME=value per line, '#' for comments. A launcher such as an already-running MO2 does not pass a new
/// environment to the game, so a test run sets its switches here instead. The real environment wins.
void ApplyDevEnvironmentFile()
{
	auto path = logger::log_directory();
	if (!path)
		return;
	*path /= std::format("{}.env"sv, Plugin::NAME);
	std::ifstream file(*path);
	if (!file)
		return;

	std::string line;
	while (std::getline(file, line)) {
		const auto first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line[first] == '#')
			continue;
		const auto equals = line.find('=', first);
		if (equals == std::string::npos)
			continue;
		std::string name = line.substr(first, equals - first);
		std::string value = line.substr(equals + 1);
		while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
			name.pop_back();
		while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
			value.pop_back();
		if (name.empty())
			continue;
		if (GetEnvironmentVariableA(name.c_str(), nullptr, 0) != 0) {
			logger::info("[DevEnv] {} is already set by the environment; ignoring the file's value", name);
			continue;
		}
		SetEnvironmentVariableA(name.c_str(), value.c_str());
		// Statically linked libraries (including BasicRHI) use the CRT's environment copy through getenv.
		_putenv_s(name.c_str(), value.c_str());
		logger::info("[DevEnv] {}={} (from {})", name, value, path->filename().string());
	}
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
#ifndef NDEBUG
	while (!REX::W32::IsDebuggerPresent()) {};
#endif
	InitializeLog();
	CrashDump::Start();
	logger::info("Community Shaders {} | branch: {} | describe: {} | hash: {}{}",
		Util::GetFormattedVersion(Plugin::VERSION),
		Plugin::BUILD_BRANCH.empty() ? "(detached)" : Plugin::BUILD_BRANCH,
		Plugin::BUILD_DESCRIBE,
		Plugin::BUILD_HASH.empty() ? "unknown" : Plugin::BUILD_HASH,
		Plugin::BUILD_DIRTY ? " (dirty)" : "");
	ApplyDevEnvironmentFile();
	SKSE::Init(a_skse);
	// SKSE::Init replaced the log with CommonLib's; install the diagnostic capture's fan-out sink afterward.
	Util::LogTee::Install();
	// Every write_call/write_branch stub (14 bytes) comes from this block; 1 KiB ran out at about 73 hooks. DCLF's
	// switch events take about 400 bytes of it for their patched stores' stubs.
	SKSE::AllocTrampoline(1 << 12);
	return Load();
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() noexcept {
	SKSE::PluginVersionData v;
	v.PluginName(Plugin::NAME.data());
	v.PluginVersion(Plugin::VERSION);
	v.UsesAddressLibrary();
	v.UsesNoStructs();
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
	case SKSE::MessagingInterface::kPreLoadGame:
	case SKSE::MessagingInterface::kNewGame:
		{
			if (errors.empty())
				Feature::ForEachLoadedFeature("GameReset", [](Feature* feature) { feature->GameReset(); });
		}
		break;
	case SKSE::MessagingInterface::kPostLoad:
		{
			// Every plugin is loaded: SceneWriterFramework's copies elect the one that patches the engine's scene writers.
			if (errors.empty() && globals::features::drawcallLimitFix.loaded)
				globals::features::drawcallLimitFix.PostLoad();
			break;
		}
	case SKSE::MessagingInterface::kPostPostLoad:
		{
			if (errors.empty()) {
				Deferred::Hooks::Install();
				Hooks::Install();
				EngineFix::InstallOnPostPostLoadFixes();
				FrameAnnotations::OnPostPostLoad();

				globals::state->flatWorldMapLoaded = GetModuleHandleW(L"FlatMapMarkersSSE.dll") != nullptr;
				if (globals::state->flatWorldMapLoaded)
					logger::info("FlatMapMarkersSSE detected, treating the world map as flat art");

				auto shaderCache = globals::shaderCache;

				// Run feature PostPostLoad() first so features can disable themselves if needed
				Feature::ForEachLoadedFeature("PostPostLoad", [](Feature* feature) { feature->PostPostLoad(); });

				if (shaderCache->UseFileWatcher())
					shaderCache->StartFileWatcher();
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
				// Catch d3dx9 math importers that loaded after our plugin-load sweep.
				D3DX9MathUpgrade::Sweep("data loaded");
				globals::OnDataLoaded();
				// The render thread's watchdog, from the first Present after the data loaded (HangWatchdog.h).
				HangWatchdog::Start();
				EngineFix::InstallOnDataLoadedFixes();
				FrameAnnotations::OnDataLoaded();

				auto shaderCache = globals::shaderCache;
				shaderCache->menuLoaded = true;

				while (shaderCache->IsCompiling() && !shaderCache->backgroundCompilation && !globals::game::quitGame) {
					std::this_thread::sleep_for(100ms);
				}

				if (globals::game::quitGame) {
					logger::info("Game was closed, skipping feature DataLoaded methods");
					break;
				}

				Feature::ForEachLoadedFeature("DataLoaded", [](Feature* feature) { feature->DataLoaded(); });

				NativeMenu::Register();
			}

			break;
		}
	case SKSE::MessagingInterface::kPostLoadGame:
		{
			if (errors.empty())
				Feature::ForEachLoadedFeature("GameLoaded", [](Feature* feature) { feature->GameLoaded(); });
			NvPerf::Trace::OnGameLoaded();
			break;
		}
	}
}

bool Load()
{
	auto privateProfileRedirectorVersion = Util::GetDllVersion(L"Data/SKSE/Plugins/PrivateProfileRedirector.dll");
	if (privateProfileRedirectorVersion.has_value() && privateProfileRedirectorVersion.value().compare(REL::Version(0, 6, 2)) == std::strong_ordering::less) {
		stl::report_and_fail("Old version of PrivateProfileRedirector detected, 0.6.2+ required if using it."sv);
	}

	auto messaging = SKSE::GetMessagingInterface();
	messaging->RegisterListener("SKSE", MessageHandler);

	globals::OnInit();
	globals::ReInit();

	auto state = globals::state;

	// Initialize i18n system (loads English fallback and discovers available locales)
	I18n::GetSingleton()->Init();

	state->Load();
	state->LoadTheme();  // Load the active theme preset

	// Initialize theme system - create default themes and discover existing ones
	globals::menu->CreateDefaultThemes();  // Creates JSON files if they don't exist
	auto themeManager = ThemeManager::GetSingleton();
	themeManager->DiscoverThemes();  // Discover all available themes

	auto log = spdlog::default_logger();
	log->set_level(state->GetLogLevel());

	const std::array incompatibleDLLs = {
		L"Data/SKSE/Plugins/ShaderTools.dll",
		L"Data/SKSE/Plugins/SSEShaderTools.dll",
		L"Data/SKSE/Plugins/SkyrimUpscaler.dll",
		L"Data/SKSE/Plugins/EVLaS.dll",
		L"Data/SKSE/Plugins/AELAS.dll",
		L"Data/SKSE/Plugins/SSEReShadeHelper.dll",
		L"Data/SKSE/Plugins/TAASharpen.dll",
		L"Data/SKSE/Plugins/NVIDIA_Reflex.dll",
		L"Data/SKSE/Plugins/MARA.dll",
		L"Data/SKSE/Plugins/NativeWaterLightStabilizer.dll",
		L"Data/SKSE/Plugins/DynamicWetness.dll",
		L"Data/SKSE/Plugins/NMN_DeformableTerrain.dll"
	};

	for (const auto dll : incompatibleDLLs) {
		if (std::filesystem::exists(dll)) {
			auto errorMessage = std::format("Incompatible DLL {} detected", stl::utf16_to_utf8(dll).value_or("<unicode conversion error>"s));
			logger::error("{}", errorMessage);
			errors.push_back(errorMessage);
		}
	}

	const auto path = std::filesystem::path("Data/SKSE/Plugins/SexLabUtil.dll");

	DWORD dummy;
	const auto size = GetFileVersionInfoSizeW(path.c_str(), &dummy);

	if (size) {
		std::vector<std::byte> data(size);

		if (GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
			VS_FIXEDFILEINFO* info = nullptr;
			UINT infoSize = 0;

			if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) && info) {
				const auto major = HIWORD(info->dwFileVersionMS);

				if (major < 2) {
					auto errorMessage = std::format("Incompatible version of SexLabUtil.dll detected. Use SexLab P+ instead");
					logger::error("{}", errorMessage);
					errors.push_back(errorMessage);
				}
			}
		}
	}

	auto pushMissingDllError = [&](std::string_view dllName) {
		auto errorMessage = std::format("Required DLL {} was missing", dllName);
		logger::error("{}", errorMessage);
		errors.push_back(errorMessage);
	};

	if (!LoadLibrary(L"Data/SKSE/Plugins/EngineFixes.dll")) {
		pushMissingDllError(stl::utf16_to_utf8(L"Data/SKSE/Plugins/EngineFixes.dll").value_or("<unicode conversion error>"s));
	}

	// Empty RequiredDLLs array, if necessary we can add a dll here in the future without needing to modify the plugin loading logic.
	const std::array<LPCWSTR, 0> requiredDLLs{};

	for (const auto dll : requiredDLLs) {
		if (!LoadLibrary(dll)) {
			pushMissingDllError(stl::utf16_to_utf8(dll).value_or("<unicode conversion error>"s));
		}
	}

	if (errors.empty()) {
		// RenderDoc patches the D3D imports, so load it before capturing the IAT originals.
		globals::features::renderDoc.Load();
		Hooks::InstallEarlyHooks();
		logger::info("Calling feature Load methods");
		Feature::ForEachLoadedFeature("Load", [](Feature* feature) { feature->Load(); });
	}

	return true;
}
