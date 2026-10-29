#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <d3d11.h>

#ifndef VK_VERSION_1_0
#	include <vulkan/vulkan.h>
#endif

/**
 * @brief VRAM attribution's debug mode (CS_VRAM_DEBUG=1, from the environment or the switches file): finds the creators of memory
 * no owner scope or name claims. Decided before the D3D11 device exists, since it changes what DXVK records per resource.
 *
 * - DXVK records each buffer's and image's creation stack; unattributed resources are traced to the first frame outside DXVK
 *   (SkyrimSE.exe -> the game, another module -> that plugin, CS -> the function).
 * - Every vkAllocateMemory/vkFreeMemory the render graph makes goes through a ledger, so ORG's device memory is exact and its
 *   allocations no tracked resource claims are listed by call site.
 * - The game's texture loads (BSShaderManager::GetTexture) label their textures "Game/Textures/<folder>" with their path.
 */
namespace Vram::Debug
{
	/** @brief Whether debug mode is on (CS_VRAM_DEBUG=1). */
	bool Enabled();

	/** @brief Turns DXVK's creation stacks on; right after dxvk_d3d11.dll loads, before the device is created. */
	void ConfigureDxvk(HMODULE a_dxvkD3D11);

	/** @brief Installs the game texture-load hook; with the other game hooks. */
	void InstallGameHooks();

	/** @brief Labels textures whose queued loads have finished since the last call. Any thread. */
	void ProcessPendingTextures();

	/** @brief Where a stack enters from outside DXVK: its module, and a description of the frame (and the two after it). */
	struct CallSite
	{
		std::string module;  // file name, lower case
		std::string text;
	};

	/** @brief A DXVK resource's creation call site; empty module if no stack was recorded. */
	CallSite DxvkCreationSite(std::uint64_t a_cookie);

	/** @brief The vkGetInstanceProcAddr to give BasicRHI: a_real, with vkAllocateMemory and vkFreeMemory routed through the ledger. */
	PFN_vkGetInstanceProcAddr OrgProcAddr(PFN_vkGetInstanceProcAddr a_real, VkInstance a_instance, VkPhysicalDevice a_physicalDevice);

	/** @brief One live ORG device memory allocation. */
	struct OrgAllocation
	{
		std::uint64_t bytes = 0;
		std::uint32_t memoryType = 0;
		bool deviceLocal = true;
		std::string site;  // the call site in BasicRHI or ORG
	};

	/** @brief Whether the ledger is installed (debug mode, and the render graph adopted the device). */
	bool HasOrgLedger();

	/** @brief The ledger's live allocations. */
	std::vector<OrgAllocation> OrgAllocations();
}
