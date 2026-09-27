#include "DxvkLoader.h"

#include "Aftermath.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <filesystem>

namespace DxvkLoader
{
	namespace
	{
		bool g_attempted = false;
		bool g_loaded = false;
		decltype(&D3D11CreateDeviceAndSwapChain) g_d3d11Create = nullptr;
		decltype(&CreateDXGIFactory) g_createFactory = nullptr;

		std::string ModuleNameAt(const void* a_address)
		{
			HMODULE owner = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &owner))
				return "<no module>";
			wchar_t buf[MAX_PATH]{};
			const DWORD n = ::GetModuleFileNameW(owner, buf, MAX_PATH);
			return n ? std::filesystem::path(std::wstring(buf, n)).filename().string() : "<unknown module>";
		}

		/**
		 * @brief An export's address read from the module's own export table, not through GetProcAddress.
		 *
		 * Capture tools that hook D3D11 (RenderDoc, loaded in-process by the RenderDoc feature) hook
		 * GetProcAddress too, and match the libraries they hook by name substring: `dxvk_d3d11.dll` counts
		 * as `d3d11.dll` and `dxvk_dxgi.dll` as `dxgi.dll`. GetProcAddress on the DXVK DLLs then returns the
		 * tool's D3D11 entry point, which forwards to the *system* runtime: the game silently runs on the
		 * native driver, with DXVK loaded but unused, and fails later on whatever only DXVK supports. The
		 * export table is data in the mapped image and cannot be redirected. A mismatch with GetProcAddress
		 * is logged with the module that answered, so an interposer is named in the log instead of guessed.
		 */
		FARPROC ResolveExport(HMODULE a_module, const char* a_name)
		{
			const auto base = reinterpret_cast<const std::byte*>(a_module);
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

			FARPROC direct = nullptr;
			if (dir.VirtualAddress && dir.Size) {
				const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
				const auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
				const auto ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
				const auto functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
				for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
					if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), a_name) != 0)
						continue;
					const DWORD rva = functions[ordinals[i]];
					// A forwarder's RVA points at a "dll.function" string inside the export directory.
					if (rva < dir.VirtualAddress || rva >= dir.VirtualAddress + dir.Size)
						direct = reinterpret_cast<FARPROC>(const_cast<std::byte*>(base + rva));
					break;
				}
			}

			const FARPROC viaLoader = ::GetProcAddress(a_module, a_name);
			if (!direct)
				return viaLoader;
			if (viaLoader != direct)
				logger::warn("[DXVK] GetProcAddress({}) is interposed: it returns {} in {} instead of DXVK's {}; calling DXVK directly",
					a_name, fmt::ptr(viaLoader), viaLoader ? ModuleNameAt(viaLoader) : "<null>", fmt::ptr(direct));
			return direct;
		}
	}

	// Resolve relative to the plugin for mod-manager VFS compatibility.
	std::filesystem::path GetRuntimeDir()
	{
		HMODULE self = nullptr;
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetRuntimeDir),
				&self)) {
			return {};
		}
		wchar_t buf[MAX_PATH]{};
		const DWORD n = ::GetModuleFileNameW(self, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			return {};
		}
		return std::filesystem::path(buf).parent_path() / L"CommunityShaders" / L"bin";
	}

	bool NativeModeRequested()
	{
		// Debug override for testing against the native D3D11 runtime.
		static const bool s_native = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_NATIVE_D3D11", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return s_native;
	}

	bool Load()
	{
		if (g_attempted) {
			return g_loaded;
		}
		g_attempted = true;

		if (NativeModeRequested()) {
			logger::info("[DXVK] CS_NATIVE_D3D11=1 -- skipping DXVK, using the system D3D11 runtime");
			return false;
		}

		if (!::SetEnvironmentVariableW(L"DXVK_HDR", L"1")) {
			logger::warn("[DXVK] Failed to enable HDR color-space support (error {})", ::GetLastError());
		}

		// DXVK reads DXVK_DEBUG once at instance creation, so the request has to be in place before
		// the game creates its device.
		//
		// Not conditional on Aftermath having armed. Aftermath is Nvidia-only, but AMD's Radeon GPU
		// Detective reads the same debug-utils labels as the [APP] half of its execution marker
		// tree, and on both vendors those labels are what turn "the GPU faulted" into "the GPU
		// faulted during this pass". The vendor-specific parts are DXVK's business: it knows which
		// GPU it is talking to and enables only what that GPU supports.
		// Put DXVK's own output where the rest of a bug report already is. DXVK defaults to the
		// directory holding the exe, so SkyrimSE_d3d11.log, SkyrimSE_dxgi.log and -- on a device
		// loss with VK_EXT_device_fault -- SkyrimSE_device_fault.bin all land in the game folder,
		// somewhere separate from CommunityShaders.log that nobody thinks to send. Pointing DXVK at
		// the SKSE log directory means "zip your SKSE logs folder" collects every artifact we
		// produce, the GPU crash dumps included.
		if (const auto logDir = logger::log_directory()) {
			const auto path = logDir->wstring();
			if (!::SetEnvironmentVariableW(L"DXVK_LOG_PATH", path.c_str()))
				logger::warn("[DXVK] Failed to redirect DXVK logs to the SKSE log folder (error {})", ::GetLastError());
		}

		// Never clobber a DXVK_DEBUG the developer set. DXVK reads a single mode from this variable,
		// so overwriting it silently disables whatever they were trying to use -- DXVK_DEBUG=pipestats
		// looked like a broken extension for a while because crash analysis had already taken the slot.
		wchar_t existingDebug[64]{};
		const DWORD existingDebugLen = ::GetEnvironmentVariableW(L"DXVK_DEBUG", existingDebug, ARRAYSIZE(existingDebug));

		if (existingDebugLen != 0 && existingDebugLen < ARRAYSIZE(existingDebug)) {
			logger::info("[DXVK] DXVK_DEBUG already set externally; leaving it alone");
		} else if (Aftermath::WantsCrashAnalysis()) {
			if (::SetEnvironmentVariableW(L"DXVK_DEBUG", L"crashanalysis"))
				logger::info("[DXVK] DXVK_DEBUG=crashanalysis -- requesting GPU crash analysis support");
			else
				logger::warn("[DXVK] Failed to request GPU crash analysis (error {})", ::GetLastError());
		}

		const auto dir = GetRuntimeDir();
		if (dir.empty()) {
			logger::error("[DXVK] Could not resolve plugin directory for DXVK DLLs");
			return false;
		}

		const auto dxgiPath = (dir / L"dxvk_dxgi.dll").wstring();
		const auto d3d11Path = (dir / L"dxvk_d3d11.dll").wstring();

		// dxvk_d3d11.dll imports dxvk_dxgi.dll by base name.
		const HMODULE dxgiMod = ::LoadLibraryExW(dxgiPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!dxgiMod) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_dxgi.dll from '{}' (error {})", dir.string(), err);
			return false;
		}
		const HMODULE d3d11Mod = ::LoadLibraryExW(d3d11Path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!d3d11Mod) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_d3d11.dll from '{}' (error {})", dir.string(), err);
			return false;
		}

		g_d3d11Create = reinterpret_cast<decltype(g_d3d11Create)>(ResolveExport(d3d11Mod, "D3D11CreateDeviceAndSwapChain"));
		g_createFactory = reinterpret_cast<decltype(g_createFactory)>(ResolveExport(dxgiMod, "CreateDXGIFactory"));

		if (!g_d3d11Create || !g_createFactory) {
			logger::error("[DXVK] Resolved DXVK DLLs but missing exports (d3d11create={}, createfactory={})",
				g_d3d11Create != nullptr, g_createFactory != nullptr);
			return false;
		}

		// The render graph adopts DXVK's device; the features it needs must be requested
		// before the game creates that device.
		RenderGraphRuntime::RequestDeviceFeatures(d3d11Mod);

		// Frame generation enables synchronous present when it takes ownership.
		if (auto setSync = reinterpret_cast<void (*)(uint32_t)>(::GetProcAddress(d3d11Mod, "dxvkSetSyncPresent")))
			setSync(0u);
		else
			logger::warn("[DXVK] dxvkSetSyncPresent export missing -- present stays at the fork default (sync)");

		logger::info("[DXVK] Loaded DXVK from '{}' (dxvk_d3d11.dll + dxvk_dxgi.dll)", dir.string());
		g_loaded = true;
		return true;
	}

	bool IsLoaded() { return g_loaded; }
	decltype(&D3D11CreateDeviceAndSwapChain) GetD3D11CreateDeviceAndSwapChain() { return g_d3d11Create; }
	decltype(&CreateDXGIFactory) GetCreateDXGIFactory() { return g_createFactory; }
}
