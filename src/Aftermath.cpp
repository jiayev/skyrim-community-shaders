#include "Aftermath.h"

#ifdef CS_ENABLE_AFTERMATH

#	include "DxvkLoader.h"
#	include "Globals.h"

#	include <GFSDK_Aftermath.h>
#	include <GFSDK_Aftermath_Defines.h>
#	include <GFSDK_Aftermath_GpuCrashDump.h>
#	include <GFSDK_Aftermath_GpuCrashDumpDecoding.h>

#	include <atomic>
#	include <algorithm>
#	include <array>
#	include <chrono>
#	include <cstring>
#	include <filesystem>
#	include <fstream>
#	include <mutex>
#	include <string>

namespace
{
	std::mutex g_mutex;
	std::atomic<bool> g_enabled{ false };
	std::filesystem::path g_dumpDir;
	uint32_t g_dumpCount = 0u;
	constexpr std::uintptr_t kOrgMarkerTag = std::uintptr_t{ 1 } << 62;
	constexpr std::size_t kOrgMarkerCapacity = 65536;
	struct OrgMarker
	{
		std::array<char, 160> text{};
		std::atomic<bool> published{ false };
	};
	std::array<OrgMarker, kOrgMarkerCapacity> g_orgMarkers{};
	std::atomic<std::uint32_t> g_nextOrgMarker{ 0 };
	std::atomic<std::uint32_t> g_lostOrgMarkers{ 0 };

	// One timestamp per incident, shared by the dump and by every shader debug file belonging to
	// it, so an incident's files group together in a directory that also holds the game's logs.
	std::string g_incidentStamp;

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
		if (const auto lost = g_lostOrgMarkers.load(std::memory_order_relaxed))
			logger::warn("[Aftermath] {} ORG checkpoints were omitted after the bounded marker history filled", lost);
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

	void OnResolveMarker(const void* a_marker, uint32_t a_markerSize, PFN_GFSDK_Aftermath_ResolveMarker a_resolve)
	{
		// DXVK's Vulkan checkpoints are compact integer IDs, not string pointers.
		// Resolve them while the device's checkpoint ring still exists so the dump
		// records draw/dispatch labels instead of opaque "Library Pointer" values.
		if (a_markerSize || !a_resolve)
			return;
		const auto id = reinterpret_cast<std::uintptr_t>(a_marker);
		if (id & kOrgMarkerTag) {
			const auto index = id & ~kOrgMarkerTag;
			if (index && index <= kOrgMarkerCapacity) {
				const auto& marker = g_orgMarkers[index - 1];
				if (marker.published.load(std::memory_order_acquire))
					a_resolve(marker.text.data(), static_cast<uint32_t>(std::strlen(marker.text.data()) + 1));
			}
			return;
		}
		if (!globals::d3d::device)
			return;
		using ResolveCheckpoint = BOOL(__stdcall*)(ID3D11Device*, const void*, char*, UINT);
		const auto module = ::GetModuleHandleW(L"dxvk_d3d11.dll");
		const auto resolve = module ? reinterpret_cast<ResolveCheckpoint>(::GetProcAddress(module, "dxvkResolveCheckpointMarker")) : nullptr;
		char text[120]{};
		if (resolve && resolve(globals::d3d::device, a_marker, text, sizeof(text)))
			a_resolve(text, static_cast<uint32_t>(std::strlen(text) + 1));
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

	void ResolveMarkerCallback(const void* a_marker, uint32_t a_markerSize, void*, PFN_GFSDK_Aftermath_ResolveMarker a_resolve)
	{
		OnResolveMarker(a_marker, a_markerSize, a_resolve);
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
		ResolveMarkerCallback,
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

bool Aftermath::WantsCrashAnalysis()
{
	// Deliberately not gated on g_enabled: on AMD the SDK never arms, and that is exactly the
	// case where the markers matter most, because Radeon GPU Detective is the tool that will read
	// them.
	return true;
}

const void* Aftermath::RegisterOrgCheckpoint(void*, const char* a_name) noexcept
{
	if (!g_enabled.load(std::memory_order_acquire))
		return nullptr;
	const auto index = g_nextOrgMarker.fetch_add(1, std::memory_order_relaxed);
	if (index >= kOrgMarkerCapacity) {
		g_lostOrgMarkers.fetch_add(1, std::memory_order_relaxed);
		return nullptr;
	}
	auto& marker = g_orgMarkers[index];
	const char* source = a_name ? a_name : "ORG unnamed";
	const auto length = std::min(std::strlen(source), marker.text.size() - 1);
	std::memcpy(marker.text.data(), source, length);
	marker.text[length] = '\0';
	marker.published.store(true, std::memory_order_release);
	return reinterpret_cast<const void*>(kOrgMarkerTag | (std::uintptr_t{ index } + 1));
}

void Aftermath::Disable()
{
	if (!g_enabled.exchange(false, std::memory_order_acq_rel))
		return;
	if (const auto lost = g_lostOrgMarkers.load(std::memory_order_relaxed))
		logger::warn("[Aftermath] {} ORG checkpoints omitted after marker history filled", lost);
	GFSDK_Aftermath_DisableGpuCrashDumps();
}

#else

bool Aftermath::Enable() { return false; }
bool Aftermath::IsEnabled() { return false; }
bool Aftermath::WantsCrashAnalysis() { return false; }
const void* Aftermath::RegisterOrgCheckpoint(void*, const char*) noexcept { return nullptr; }
void Aftermath::Disable() {}

#endif
