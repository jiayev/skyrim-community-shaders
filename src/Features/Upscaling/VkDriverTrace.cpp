#include "VkDriverTrace.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace VkDriverTrace
{
	namespace
	{
		PFN_vkAllocateMemory realAllocate = nullptr;
		PFN_vkFreeMemory realFree = nullptr;
		PFN_vkQueuePresentKHR realPresent = nullptr;
		PFN_vkSetDeviceMemoryPriorityEXT realSetPriority = nullptr;
		PFN_vkGetPhysicalDeviceMemoryProperties2 getMemory2 = nullptr;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

		VkPhysicalDeviceMemoryProperties memory{};
		// CS_VK_PRIORITY=module=priority,...: replaces the residency priority of that module's device memory.
		std::map<std::string, float> priorities;
		// Other clients' video memory (DLSS, DLSS-G, FidelityFX, Streamline) gets this: their memory is default, so the driver demotes it
		// before DXVK's and the graph's, and frame generation collapses (docs/development/vram-spill-lab.md). CS_VK_CLIENT_PRIORITY replaces
		// it; 0 leaves them alone.
		float clientPriority = 1.0f;
		bool tracing = false;
		// CS_VK_SPILL=module,...: that module's device memory goes in system memory instead, as if the driver had demoted it.
		std::vector<std::string> spilled;
		std::uint32_t systemType = UINT32_MAX;
		VkDevice ballastDevice = VK_NULL_HANDLE;
		std::vector<VkDeviceMemory> ballast;
		std::atomic<uint64_t> presents = 0;

		struct Allocation
		{
			VkDeviceSize size;
			const std::string* owner;
			bool deviceLocal;
		};
		struct Owner
		{
			int64_t deviceLocal = 0;
			int64_t other = 0;
			int64_t count = 0;
			VkDeviceSize largest = 0;
		};
		std::mutex lock;
		std::unordered_map<VkDeviceMemory, Allocation> allocations;
		std::map<std::string, Owner> owners;

		// The first caller outside the loader, the interposer and the driver.
		std::string CallerModule()
		{
			void* frames[16];
			const USHORT count = RtlCaptureStackBackTrace(2, ARRAYSIZE(frames), frames, nullptr);
			for (USHORT i = 0; i < count; ++i) {
				HMODULE module = nullptr;
				if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						static_cast<const char*>(frames[i]), &module))
					continue;
				char path[MAX_PATH]{};
				(void)GetModuleFileNameA(module, path, MAX_PATH);
				std::string name = path;
				name = std::filesystem::path(name).filename().string();
				if (name != "vulkan-1.dll" && name != "sl.interposer.dll" && !name.starts_with("nvoglv"))
					return name;
			}
			return "(unknown)";
		}

		VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice a_device, const VkMemoryAllocateInfo* a_info, const VkAllocationCallbacks* a_callbacks, VkDeviceMemory* a_memory)
		{
			auto name = CallerModule();
			VkMemoryAllocateInfo info = *a_info;
			if (systemType != UINT32_MAX && memory.memoryTypes[info.memoryTypeIndex].propertyFlags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT &&
				std::ranges::find(spilled, name) != spilled.end()) {
				info.memoryTypeIndex = systemType;
				logger::info("[VkTrace] {}: {} MiB in system memory", name, info.allocationSize >> 20);
			}
			const VkResult result = realAllocate(a_device, &info, a_callbacks, a_memory);
			if (result == VK_SUCCESS) {
				const uint32_t heap = memory.memoryTypes[info.memoryTypeIndex].heapIndex;
				const bool deviceLocal = (memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
				const bool hostVisible = (memory.memoryTypes[info.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
				if (const auto found = priorities.find(name); deviceLocal && realSetPriority && found != priorities.end())
					realSetPriority(a_device, *a_memory, found->second);
				else if (deviceLocal && !hostVisible && realSetPriority && clientPriority > 0.0f && !name.starts_with("dxvk_") && name != "CommunityShaders.dll")
					realSetPriority(a_device, *a_memory, clientPriority);
				if (!tracing)
					return result;
				std::lock_guard guard(lock);
				const auto entry = owners.try_emplace(std::move(name)).first;
				(deviceLocal ? entry->second.deviceLocal : entry->second.other) += a_info->allocationSize;
				++entry->second.count;
				entry->second.largest = std::max(entry->second.largest, a_info->allocationSize);
				allocations[*a_memory] = { a_info->allocationSize, &entry->first, deviceLocal };
			}
			return result;
		}

		VKAPI_ATTR void VKAPI_CALL Free(VkDevice a_device, VkDeviceMemory a_memory, const VkAllocationCallbacks* a_callbacks)
		{
			if (a_memory != VK_NULL_HANDLE) {
				std::lock_guard guard(lock);
				if (const auto found = allocations.find(a_memory); found != allocations.end()) {
					auto& owner = owners[*found->second.owner];
					(found->second.deviceLocal ? owner.deviceLocal : owner.other) -= found->second.size;
					--owner.count;
					allocations.erase(found);
				}
			}
			realFree(a_device, a_memory, a_callbacks);
		}

		VKAPI_ATTR void VKAPI_CALL SetPriority(VkDevice a_device, VkDeviceMemory a_memory, float a_priority)
		{
			const auto found = priorities.find(CallerModule());
			realSetPriority(a_device, a_memory, found != priorities.end() ? found->second : a_priority);
		}

		VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue a_queue, const VkPresentInfoKHR* a_info)
		{
			presents.fetch_add(1, std::memory_order_relaxed);
			return realPresent(a_queue, a_info);
		}

		void Report()
		{
			uint64_t last = 0;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(5));
				const uint64_t now = presents.load(std::memory_order_relaxed);
				std::string line = std::format("[VkTrace] driver presents {:.1f}/s;", static_cast<double>(now - last) / 5.0);
				last = now;
				if (getMemory2) {
					VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
					VkPhysicalDeviceMemoryProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget };
					getMemory2(physicalDevice, &properties);
					for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
						line += std::format(" heap {}: usage {} of budget {} MB;", i, budget.heapUsage[i] >> 20, budget.heapBudget[i] >> 20);
				}
				{
					std::lock_guard guard(lock);
					for (const auto& [name, owner] : owners)
						line += std::format(" {}: {} MB local, {} MB other, {} allocs, largest {} MB;", name, owner.deviceLocal >> 20,
							owner.other >> 20, owner.count, owner.largest >> 20);
				}
				logger::info("{}", line);
			}
		}
	}

	void Install(VkInstance a_instance, VkPhysicalDevice a_physicalDevice, VkDevice a_device)
	{
		char v[2] = {};
		tracing = GetEnvironmentVariableA("CS_VK_DRIVER_TRACE", v, sizeof(v)) && v[0] == '1';
		char clientValue[16] = {};
		if (GetEnvironmentVariableA("CS_VK_CLIENT_PRIORITY", clientValue, sizeof(clientValue)))
			clientPriority = std::strtof(clientValue, nullptr);

		wchar_t system[MAX_PATH]{};
		(void)GetSystemDirectoryW(system, MAX_PATH);
		const HMODULE loader = LoadLibraryW((std::filesystem::path(system) / L"vulkan-1.dll").c_str());
		const auto getInstanceProc = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr")) : nullptr;
		const auto getDeviceProc = getInstanceProc ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstanceProc(a_instance, "vkGetDeviceProcAddr")) : nullptr;
		const auto getMemory = getInstanceProc ? reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(getInstanceProc(a_instance, "vkGetPhysicalDeviceMemoryProperties")) : nullptr;
		if (!getDeviceProc || !getMemory) {
			logger::warn("[VkTrace] the system loader did not resolve the driver's entry points");
			return;
		}
		getMemory(a_physicalDevice, &memory);
		for (uint32_t i = 0; i < memory.memoryTypeCount && systemType == UINT32_MAX; ++i)
			if (memory.memoryTypes[i].propertyFlags == 0)
				systemType = i;
		ballastDevice = a_device;
		realAllocate = reinterpret_cast<PFN_vkAllocateMemory>(getDeviceProc(a_device, "vkAllocateMemory"));
		realFree = reinterpret_cast<PFN_vkFreeMemory>(getDeviceProc(a_device, "vkFreeMemory"));
		realPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(getDeviceProc(a_device, "vkQueuePresentKHR"));
		realSetPriority = reinterpret_cast<PFN_vkSetDeviceMemoryPriorityEXT>(getDeviceProc(a_device, "vkSetDeviceMemoryPriorityEXT"));
		getMemory2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(getInstanceProc(a_instance, "vkGetPhysicalDeviceMemoryProperties2"));
		physicalDevice = a_physicalDevice;

		char list[256] = {};
		if (GetEnvironmentVariableA("CS_VK_PRIORITY", list, sizeof(list))) {
			std::string text = list;
			for (size_t at = 0; at < text.size();) {
				const size_t end = std::min(text.find(',', at), text.size());
				const size_t equals = text.find('=', at);
				if (equals != std::string::npos && equals < end)
					priorities[text.substr(at, equals - at)] = std::stof(text.substr(equals + 1, end - equals - 1));
				at = end + 1;
			}
		}
		char spill[256] = {};
		if (GetEnvironmentVariableA("CS_VK_SPILL", spill, sizeof(spill))) {
			std::string text = spill;
			for (size_t at = 0; at < text.size();) {
				const size_t end = std::min(text.find(',', at), text.size());
				spilled.push_back(text.substr(at, end - at));
				at = end + 1;
			}
		}
		// Without the driver's priorities (VK_EXT_pageable_device_local_memory) there is nothing to set, and only tracing hooks anything.
		if (!tracing && (!realSetPriority || clientPriority <= 0.0f))
			return;
		if (!realAllocate || !realFree || !realPresent) {
			logger::warn("[VkTrace] the driver's entry points did not resolve");
			return;
		}

		(void)DetourTransactionBegin();
		(void)DetourUpdateThread(GetCurrentThread());
		(void)DetourAttach(reinterpret_cast<PVOID*>(&realAllocate), reinterpret_cast<PVOID>(Allocate));
		if (tracing) {
			(void)DetourAttach(reinterpret_cast<PVOID*>(&realFree), reinterpret_cast<PVOID>(Free));
			(void)DetourAttach(reinterpret_cast<PVOID*>(&realPresent), reinterpret_cast<PVOID>(Present));
		}
		if (realSetPriority && !priorities.empty())
			(void)DetourAttach(reinterpret_cast<PVOID*>(&realSetPriority), reinterpret_cast<PVOID>(SetPriority));
		const LONG result = DetourTransactionCommit();
		if (result != NO_ERROR) {
			logger::warn("[VkTrace] hooking the driver failed ({})", result);
			return;
		}
		logger::info("[VkTrace] hooked the driver's memory allocation: other clients' video memory at priority {}{}", clientPriority, tracing ? ", tracing" : "");
		if (tracing)
			std::thread(Report).detach();
	}

	std::uint32_t SetBallast(std::uint32_t a_mib, float a_priority)
	{
		if (!ballastDevice || !realAllocate)
			return 0;
		for (const auto block : ballast)
			realFree(ballastDevice, block, nullptr);
		ballast.clear();
		uint32_t type = UINT32_MAX;
		for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
			if (memory.memoryTypes[i].propertyFlags == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
				type = i;
		if (type == UINT32_MAX)
			return 0;
		constexpr std::uint32_t kBlockMib = 256;
		for (std::uint32_t held = 0; held < a_mib; held += kBlockMib) {
			VkMemoryAllocateInfo info{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			info.allocationSize = VkDeviceSize(kBlockMib) << 20;
			info.memoryTypeIndex = type;
			VkDeviceMemory block = VK_NULL_HANDLE;
			if (realAllocate(ballastDevice, &info, nullptr, &block) != VK_SUCCESS)
				break;
			if (realSetPriority)
				realSetPriority(ballastDevice, block, a_priority);
			ballast.push_back(block);
		}
		return static_cast<std::uint32_t>(ballast.size()) * kBlockMib;
	}
}
