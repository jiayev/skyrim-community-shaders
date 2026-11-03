#pragma once

#include <vulkan/vulkan.h>

// Sits on the driver's vkAllocateMemory, the one place that sees allocations made inside Streamline, DLSS and FidelityFX: they pass DXVK
// by. Their video memory goes to the top residency priority, as DXVK's protected memory and the graph's are.
// CS_VK_DRIVER_TRACE=1 also logs, every five seconds, the real present rate and the device memory each module holds, as the driver sees them.
namespace VkDriverTrace
{
	/** @brief Hooks the driver's entry points for this device. */
	void Install(VkInstance a_instance, VkPhysicalDevice a_physicalDevice, VkDevice a_device);

	/**
	 * @brief For tests: holds a_mib of device-local memory at a_priority, in blocks of 256 MiB, in place of what it held; 0 frees it. It
	 * pushes the process over its budget, so the driver demotes by priority as it would under real pressure. Needs the switch.
	 * @return The MiB it now holds.
	 */
	std::uint32_t SetBallast(std::uint32_t a_mib, float a_priority);
}
