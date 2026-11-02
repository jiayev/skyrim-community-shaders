#pragma once

#include <vulkan/vulkan.h>

// CS_VK_DRIVER_TRACE=1: logs, every five seconds, the real present rate and the device memory each module holds, as the driver
// sees them. Nothing else counts them: generated frames and allocations made inside Streamline and FidelityFX pass DXVK by.
namespace VkDriverTrace
{
	/** @brief Hooks the driver's entry points for this device when the switch is set. */
	void Install(VkInstance a_instance, VkPhysicalDevice a_physicalDevice, VkDevice a_device);
}
