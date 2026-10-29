#include "Vram/Debug.h"

#include "Globals.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "Utils/SwitchFile.h"
#include "Vram/DxvkMemory.h"

#include <DbgHelp.h>
#include <RE/N/NiSourceTexture.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace Vram::Debug
{
	namespace
	{
		std::string Lower(std::string a_text)
		{
			for (auto& c : a_text)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return a_text;
		}

		// ----- Call sites (DbgHelp is single-threaded: everything under one lock) -----

		std::mutex g_symbolMutex;
		bool g_symbolsReady = false;
		std::unordered_map<const void*, std::pair<std::string, std::string>> g_frameCache;  // address -> module, text

		const std::pair<std::string, std::string>& DescribeFrame(const void* a_address)
		{
			if (const auto it = g_frameCache.find(a_address); it != g_frameCache.end())
				return it->second;

			std::string module = "(unknown)";
			std::uintptr_t base = 0;
			HMODULE handle = nullptr;
			if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCWSTR>(a_address), &handle)) {
				wchar_t path[MAX_PATH]{};
				if (::GetModuleFileNameW(handle, path, MAX_PATH))
					module = Lower(std::filesystem::path(path).filename().string());
				base = reinterpret_cast<std::uintptr_t>(handle);
			}

			if (!g_symbolsReady) {
				::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
				g_symbolsReady = ::SymInitialize(::GetCurrentProcess(), nullptr, TRUE) != FALSE;
			}

			std::string text;
			alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
			auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 255;
			DWORD64 displacement = 0;
			const auto address = reinterpret_cast<DWORD64>(a_address);
			if (g_symbolsReady && ::SymFromAddr(::GetCurrentProcess(), address, &displacement, symbol)) {
				text = fmt::format("{}!{}+0x{:X}", module, symbol->Name, displacement);
				IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
				DWORD lineDisplacement = 0;
				if (::SymGetLineFromAddr64(::GetCurrentProcess(), address, &lineDisplacement, &line))
					text += fmt::format(" ({}:{})", std::filesystem::path(line.FileName).filename().string(), line.LineNumber);
			} else {
				text = fmt::format("{}+0x{:X}", module, address - base);
			}
			return g_frameCache.emplace(a_address, std::make_pair(std::move(module), std::move(text))).first->second;
		}

		bool InDxvk(const std::string& a_module)
		{
			return a_module == "dxvk_d3d11.dll" || a_module == "dxvk_dxgi.dll";
		}

		// The first frame outside DXVK names the creator; the two after it give the context.
		CallSite Describe(const void* const* a_frames, std::size_t a_count, bool a_skipDxvk)
		{
			std::lock_guard lock(g_symbolMutex);
			CallSite site;
			int described = 0;
			for (std::size_t i = 0; i < a_count && described < 3; ++i) {
				const auto& [module, text] = DescribeFrame(a_frames[i]);
				if (a_skipDxvk && described == 0 && InDxvk(module))
					continue;
				if (described == 0)
					site.module = module;
				site.text += (described ? " < " : "") + text;
				++described;
			}
			return site;
		}

		// ----- ORG's device memory ledger -----

		PFN_vkGetInstanceProcAddr g_realGetInstanceProcAddr = nullptr;
		PFN_vkGetDeviceProcAddr g_realGetDeviceProcAddr = nullptr;
		PFN_vkAllocateMemory g_realAllocateMemory = nullptr;
		PFN_vkFreeMemory g_realFreeMemory = nullptr;
		VkPhysicalDeviceMemoryProperties g_memoryProperties{};
		bool g_ledgerInstalled = false;

		struct LedgerEntry
		{
			std::uint64_t bytes = 0;
			std::uint32_t memoryType = 0;
			std::vector<const void*> stack;
		};
		std::mutex g_ledgerMutex;
		std::unordered_map<VkDeviceMemory, LedgerEntry> g_ledger;

		VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice a_device, const VkMemoryAllocateInfo* a_info, const VkAllocationCallbacks* a_allocator,
			VkDeviceMemory* a_memory)
		{
			const VkResult result = g_realAllocateMemory(a_device, a_info, a_allocator, a_memory);
			if (result == VK_SUCCESS && a_memory && *a_memory != VK_NULL_HANDLE) {
				LedgerEntry entry;
				entry.bytes = a_info->allocationSize;
				entry.memoryType = a_info->memoryTypeIndex;
				void* frames[24];
				const USHORT count = ::RtlCaptureStackBackTrace(1, 24, frames, nullptr);
				entry.stack.assign(frames, frames + count);
				std::lock_guard lock(g_ledgerMutex);
				g_ledger[*a_memory] = std::move(entry);
			}
			return result;
		}

		VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice a_device, VkDeviceMemory a_memory, const VkAllocationCallbacks* a_allocator)
		{
			if (a_memory != VK_NULL_HANDLE) {
				std::lock_guard lock(g_ledgerMutex);
				g_ledger.erase(a_memory);
			}
			g_realFreeMemory(a_device, a_memory, a_allocator);
		}

		VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice a_device, const char* a_name)
		{
			const auto real = g_realGetDeviceProcAddr(a_device, a_name);
			if (!real || !a_name)
				return real;
			if (std::strcmp(a_name, "vkAllocateMemory") == 0) {
				g_realAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(real);
				return reinterpret_cast<PFN_vkVoidFunction>(&AllocateMemory);
			}
			if (std::strcmp(a_name, "vkFreeMemory") == 0) {
				g_realFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(real);
				return reinterpret_cast<PFN_vkVoidFunction>(&FreeMemory);
			}
			return real;
		}

		VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance a_instance, const char* a_name)
		{
			const auto real = g_realGetInstanceProcAddr(a_instance, a_name);
			if (real && a_name && std::strcmp(a_name, "vkGetDeviceProcAddr") == 0) {
				g_realGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(real);
				return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
			}
			// A device function looked up through the instance would bypass the ledger: route the two through the device path.
			if (real && a_name && (std::strcmp(a_name, "vkAllocateMemory") == 0 || std::strcmp(a_name, "vkFreeMemory") == 0)) {
				if (std::strcmp(a_name, "vkAllocateMemory") == 0) {
					g_realAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(real);
					return reinterpret_cast<PFN_vkVoidFunction>(&AllocateMemory);
				}
				g_realFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(real);
				return reinterpret_cast<PFN_vkVoidFunction>(&FreeMemory);
			}
			return real;
		}

		// ----- Game texture loads -----

		struct PendingTexture
		{
			RE::NiPointer<RE::NiTexture> texture;
			std::string path;
			std::chrono::steady_clock::time_point queued;
		};
		std::mutex g_textureMutex;
		std::vector<PendingTexture> g_pendingTextures;

		const char* TextureOwner(std::string_view a_path)
		{
			// "textures\landscape\dirt02.dds" (or "data\textures\...") -> "Game/Textures/landscape"
			std::string path = Lower(std::string(a_path));
			std::ranges::replace(path, '/', '\\');
			for (std::string_view prefix : { "data\\", "textures\\" })
				if (path.starts_with(prefix))
					path.erase(0, prefix.size());
			const auto slash = path.find('\\');
			const std::string folder = slash == std::string::npos ? "(root)" : path.substr(0, slash);

			static std::mutex mutex;
			static std::unordered_map<std::string, std::string> owners;
			std::lock_guard lock(mutex);
			auto& owner = owners[folder];
			if (owner.empty())
				owner = "Game/Textures/" + folder;
			return owner.c_str();
		}

		// Whether the texture's D3D11 resource exists yet; labels it if so.
		bool TryTag(RE::NiTexture* a_texture, std::string_view a_path)
		{
			auto* source = netimmerse_cast<RE::NiSourceTexture*>(a_texture);
			if (!source)
				return true;  // not a file texture: nothing to wait for
			if (!source->rendererTexture || !source->rendererTexture->texture)
				return false;
			ID3D11Resource* resource = source->rendererTexture->texture;
			SetOwner(resource, TextureOwner(a_path), true);
			UINT size = 0;
			if (resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, nullptr) != S_OK || size == 0)
				resource->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(a_path.size()), a_path.data());
			return true;
		}

		struct BSShaderManager_GetTexture
		{
			static void thunk(const char* a_path, bool a_demand, RE::NiPointer<RE::NiTexture>& a_textureOut, bool a_isHeightMap)
			{
				func(a_path, a_demand, a_textureOut, a_isHeightMap);
				if (!a_path || !a_textureOut)
					return;
				// A queued load finishes on a loader thread later: hold the texture until it has its D3D11 resource.
				if (!TryTag(a_textureOut.get(), a_path)) {
					std::lock_guard lock(g_textureMutex);
					g_pendingTextures.push_back({ a_textureOut, a_path, std::chrono::steady_clock::now() });
				}
				ProcessPendingTextures();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Enabled()
	{
		static const bool enabled = Util::SwitchFile::Enabled("CS_VRAM_DEBUG");
		return enabled;
	}

	void ConfigureDxvk(HMODULE a_dxvkD3D11)
	{
		if (!Enabled() || !a_dxvkD3D11)
			return;
		const auto configure = reinterpret_cast<PFN_dxvkConfigureMemoryDebug>(reinterpret_cast<void*>(::GetProcAddress(a_dxvkD3D11, "dxvkConfigureMemoryDebug")));
		if (!configure) {
			logger::warn("[VRAM] CS_VRAM_DEBUG: dxvk_d3d11.dll has no dxvkConfigureMemoryDebug; no creation stacks");
			return;
		}
		configure(DXVK_MEMORY_DEBUG_CREATION_STACKS);
		logger::info("[VRAM] CS_VRAM_DEBUG: DXVK records every resource's creation stack");
	}

	void InstallGameHooks()
	{
		if (!Enabled())
			return;
		stl::detour_thunk<BSShaderManager_GetTexture>(REL::RelocationID(98986, 105640));
		logger::info("[VRAM] CS_VRAM_DEBUG: game texture loads are labeled with their paths");
	}

	void ProcessPendingTextures()
	{
		if (!Enabled())
			return;
		std::vector<PendingTexture> ready;
		{
			std::lock_guard lock(g_textureMutex);
			const auto now = std::chrono::steady_clock::now();
			std::erase_if(g_pendingTextures, [&](PendingTexture& a_pending) {
				// Gives up on a load that never finishes rather than keeping its texture alive.
				if (TryTag(a_pending.texture.get(), a_pending.path) || now - a_pending.queued > std::chrono::seconds(60)) {
					ready.push_back(std::move(a_pending));
					return true;
				}
				return false;
			});
		}
		// The last reference may free the texture: released outside the lock.
		ready.clear();
	}

	CallSite DxvkCreationSite(std::uint64_t a_cookie)
	{
		static const auto getStack = [] {
			HMODULE d3d11 = ::GetModuleHandleW(L"dxvk_d3d11.dll");
			return d3d11 ? reinterpret_cast<PFN_dxvkGetResourceCreationStack>(reinterpret_cast<void*>(::GetProcAddress(d3d11, "dxvkGetResourceCreationStack"))) :
			               nullptr;
		}();
		if (!getStack || !globals::d3d::device)
			return {};
		void* frames[48];
		std::uint32_t count = 0;
		if (getStack(globals::d3d::device, a_cookie, frames, 48, &count) != S_OK)
			return {};
		return Describe(frames, std::min<std::uint32_t>(count, 48), true);
	}

	PFN_vkGetInstanceProcAddr OrgProcAddr(PFN_vkGetInstanceProcAddr a_real, VkInstance a_instance, VkPhysicalDevice a_physicalDevice)
	{
		if (!Enabled() || !a_real)
			return a_real;
		g_realGetInstanceProcAddr = a_real;
		// The heaps' device-local flag per memory type, through the real loader (the instance owns the physical device).
		if (auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(a_real(a_instance, "vkGetPhysicalDeviceMemoryProperties")))
			getProperties(a_physicalDevice, &g_memoryProperties);
		g_ledgerInstalled = true;
		logger::info("[VRAM] CS_VRAM_DEBUG: the render graph's vkAllocateMemory/vkFreeMemory go through the VRAM ledger");
		return &GetInstanceProcAddr;
	}

	bool HasOrgLedger()
	{
		return g_ledgerInstalled && g_realAllocateMemory;
	}

	std::vector<OrgAllocation> OrgAllocations()
	{
		std::vector<std::pair<LedgerEntry, VkDeviceMemory>> entries;
		{
			std::lock_guard lock(g_ledgerMutex);
			entries.reserve(g_ledger.size());
			for (const auto& [memory, entry] : g_ledger)
				entries.emplace_back(entry, memory);
		}
		std::vector<OrgAllocation> out;
		out.reserve(entries.size());
		for (const auto& [entry, memory] : entries) {
			OrgAllocation allocation;
			allocation.bytes = entry.bytes;
			allocation.memoryType = entry.memoryType;
			if (entry.memoryType < g_memoryProperties.memoryTypeCount) {
				const auto heap = g_memoryProperties.memoryTypes[entry.memoryType].heapIndex;
				allocation.deviceLocal = g_memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
			}
			allocation.site = Describe(entry.stack.data(), entry.stack.size(), false).text;
			out.push_back(std::move(allocation));
		}
		return out;
	}
}
