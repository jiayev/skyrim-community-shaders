#include "Aftermath.h"

#ifdef CS_ENABLE_AFTERMATH

#	include "DxvkLoader.h"

#	include <GFSDK_Aftermath.h>
#	include <GFSDK_Aftermath_Defines.h>
#	include <GFSDK_Aftermath_GpuCrashDump.h>
#	include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

#	include <atomic>
#	include <chrono>
#	include <filesystem>
#	include <fstream>
#	include <mutex>
#	include <string>
#	include <thread>

namespace
{
	std::mutex g_mutex;
	std::atomic<bool> g_enabled{ false };
	std::filesystem::path g_dumpDir;
	uint32_t g_dumpCount = 0u;

	// One timestamp per incident, shared by the dump and by every shader debug file belonging to
	// it, so an incident's files group together in a directory that also holds the game's logs.
	std::string g_incidentStamp;

	const char* CrashDumpStatusName(GFSDK_Aftermath_CrashDump_Status a_status)
	{
		switch (a_status) {
		case GFSDK_Aftermath_CrashDump_Status_NotStarted:
			return "NotStarted";
		case GFSDK_Aftermath_CrashDump_Status_CollectingData:
			return "CollectingData";
		case GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed:
			return "CollectingDataFailed";
		case GFSDK_Aftermath_CrashDump_Status_InvokingCallback:
			return "InvokingCallback";
		case GFSDK_Aftermath_CrashDump_Status_Finished:
			return "Finished";
		default:
			return "Unknown";
		}
	}

	// Callers hold g_mutex.
	const std::string& IncidentStamp()
	{
		if (g_incidentStamp.empty()) {
			const auto now = std::chrono::system_clock::now();
			g_incidentStamp = std::format("{:%Y-%m-%d-%H-%M-%S}",
				std::chrono::floor<std::chrono::seconds>(std::chrono::current_zone()->to_local(now)));
		}
		return g_incidentStamp;
	}

	// The SDK runtime lives beside DXVK's, in CommunityShaders/bin, which the loader does not
	// search. Bring it in by full path before touching a delay-loaded symbol; if it is not there,
	// every Aftermath entry point below must stay untouched or the delay-load helper terminates
	// the process for a missing diagnostic aid.
	bool LoadRuntime()
	{
		static const bool s_loaded = [] {
			const auto dir = DxvkLoader::GetRuntimeDir();
			if (dir.empty())
				return false;
			const auto path = dir / L"GFSDK_Aftermath_Lib.x64.dll";
			if (::LoadLibraryW(path.c_str()))
				return true;
			logger::warn("[Aftermath] {} could not be loaded (error {}); GPU crash dumps unavailable",
				path.string(), ::GetLastError());
			return false;
		}();
		return s_loaded;
	}

	// Beside CommunityShaders.log in the SKSE log folder, which is where CrashLoggerSSE writes and
	// therefore the first place anyone asked for "your logs" already looks. A dump nobody can find
	// is a dump nobody sends.
	std::filesystem::path ResolveDumpDirectory()
	{
		const auto dir = logger::log_directory();
		if (!dir)
			return {};
		std::error_code ec;
		std::filesystem::create_directories(*dir, ec);
		return ec ? std::filesystem::path{} : *dir;
	}

	bool WriteFile(const std::filesystem::path& a_path, const void* a_data, size_t a_size)
	{
		std::ofstream out(a_path, std::ios::out | std::ios::binary);
		if (!out)
			return false;
		out.write(static_cast<const char*>(a_data), static_cast<std::streamsize>(a_size));
		return out.good();
	}

	void OnCrashDump(const void* a_dump, uint32_t a_size)
	{
		std::lock_guard lock(g_mutex);
		if (g_dumpDir.empty())
			return;
		const auto index = ++g_dumpCount;
		const auto name = index > 1u ?
		                      std::format("gpu-crash-{}-{}.nv-gpudmp", IncidentStamp(), index) :
		                      std::format("gpu-crash-{}.nv-gpudmp", IncidentStamp());
		const auto path = g_dumpDir / name;
		if (WriteFile(path, a_dump, a_size))
			logger::critical("[Aftermath] GPU crash dump written to {}", path.string());
		else
			logger::error("[Aftermath] failed to write GPU crash dump to {}", path.string());
		spdlog::default_logger()->flush();
	}

	void OnShaderDebugInfo(const void* a_info, uint32_t a_size)
	{
		std::lock_guard lock(g_mutex);
		if (g_dumpDir.empty())
			return;

		// The identifier is what the decoder matches a faulting shader against, so the file has to
		// be named for it rather than for the order it arrived in.
		GFSDK_Aftermath_ShaderDebugInfoIdentifier identifier{};
		if (GFSDK_Aftermath_GetShaderDebugInfoIdentifier(GFSDK_Aftermath_Version_API,
				a_info, a_size, &identifier) != GFSDK_Aftermath_Result_Success)
			return;

		// The identifier stays in the name: the decoder matches a faulting shader by it.
		const auto name = std::format("gpu-crash-{}-shader-{:016x}{:016x}.nvdbg",
			IncidentStamp(), identifier.id[0], identifier.id[1]);
		if (!WriteFile(g_dumpDir / name, a_info, a_size))
			logger::error("[Aftermath] failed to write shader debug info {}", name);
	}

	// Describes the build to whoever opens the dump. Keep this to facts that identify the binary:
	// the decoder shows them next to the fault, and a dump that cannot be tied to a build is
	// nearly useless.
	void OnDescription(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription a_add)
	{
		a_add(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName, Plugin::NAME.data());
		a_add(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion, Plugin::VERSION.string().c_str());
	}

	void GpuCrashDumpCallback(const void* a_dump, uint32_t a_size, void*)
	{
		OnCrashDump(a_dump, a_size);
	}

	void ShaderDebugInfoCallback(const void* a_info, uint32_t a_size, void*)
	{
		OnShaderDebugInfo(a_info, a_size);
	}

	void CrashDumpDescriptionCallback(PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription a_add, void*)
	{
		OnDescription(a_add);
	}
}

bool Aftermath::Enable()
{
	if (g_enabled.load(std::memory_order_acquire))
		return true;

	if (!LoadRuntime())
		return false;

	g_dumpDir = ResolveDumpDirectory();
	if (g_dumpDir.empty()) {
		logger::error("[Aftermath] could not create a crash dump directory; GPU crash dumps disabled");
		return false;
	}

	// DeferDebugInfoCallbacks keeps shader debug info in memory and hands it over only when a dump
	// is actually produced. Without it every shader compilation calls back and writes a file, which
	// on a modlist with thousands of shader permutations is a lot of disk for data no one reads.
	const GFSDK_Aftermath_Result result = GFSDK_Aftermath_EnableGpuCrashDumps(
		GFSDK_Aftermath_Version_API,
		GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
		GFSDK_Aftermath_GpuCrashDumpFeatureFlags_DeferDebugInfoCallbacks,
		GpuCrashDumpCallback,
		ShaderDebugInfoCallback,
		CrashDumpDescriptionCallback,
		nullptr,
		nullptr);

	if (!GFSDK_Aftermath_SUCCEED(result)) {
		// Not fatal, and not worth failing startup over: the driver refuses on non-NVIDIA hardware
		// and when a Nsight GPU crash dump monitor already owns the process, both of which are
		// ordinary. Say which, and carry on without dumps.
		logger::warn("[Aftermath] GPU crash dumps unavailable (result {:#x}); continuing without them",
			static_cast<uint32_t>(result));
		g_dumpDir.clear();
		return false;
	}

	g_enabled.store(true, std::memory_order_release);
	logger::info("[Aftermath] GPU crash dumps armed; dumps will be written to {}", g_dumpDir.string());
	return true;
}

bool Aftermath::IsEnabled()
{
	return g_enabled.load(std::memory_order_acquire);
}

void Aftermath::WaitForCrashDump()
{
	if (!IsEnabled())
		return;

	logger::critical("[Aftermath] Device lost; waiting up to 10 s for GPU crash dump collection");
	spdlog::default_logger()->flush();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	auto previousStatus = GFSDK_Aftermath_CrashDump_Status_Unknown;
	bool firstPoll = true;
	for (;;) {
		auto status = GFSDK_Aftermath_CrashDump_Status_Unknown;
		const auto result = GFSDK_Aftermath_GetCrashDumpStatus(&status);
		if (!GFSDK_Aftermath_SUCCEED(result)) {
			logger::error("[Aftermath] Failed to query GPU crash dump status (result {:#x})", static_cast<uint32_t>(result));
			break;
		}
		if (firstPoll || status != previousStatus) {
			logger::info("[Aftermath] GPU crash dump status: {}", CrashDumpStatusName(status));
			previousStatus = status;
			firstPoll = false;
		}
		if (status == GFSDK_Aftermath_CrashDump_Status_Finished)
			break;
		if (status == GFSDK_Aftermath_CrashDump_Status_CollectingDataFailed) {
			logger::error("[Aftermath] GPU crash dump collection failed; no dump callback will follow");
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			logger::error("[Aftermath] Timed out waiting for GPU crash dump collection (status {})", CrashDumpStatusName(status));
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	spdlog::default_logger()->flush();
}

void Aftermath::Disable()
{
	if (!g_enabled.exchange(false, std::memory_order_acq_rel))
		return;
	GFSDK_Aftermath_DisableGpuCrashDumps();
}

#else

bool Aftermath::Enable() { return false; }
bool Aftermath::IsEnabled() { return false; }
void Aftermath::WaitForCrashDump() {}
void Aftermath::Disable() {}

#endif
