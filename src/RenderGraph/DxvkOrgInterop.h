#pragma once

// C ABI exported by the DXVK fork (dxvk_d3d11.dll) for clients that record their own
// Vulkan work on DXVK's device. Mirrors extern/dxvk/src/d3d11/d3d11_org_interop.h;
// keep the two identical.
//
// Like DXVKInteropInterfaces.h, Vulkan headers are only included when none are visible,
// so volk-based translation units can include this after rhi_interop_vulkan.h.

#include <d3d11.h>
#include <cstdint>

#ifndef VK_VERSION_1_0
#	include <vulkan/vulkan.h>
#endif

#define DXVK_ORG_INTEROP_VERSION 1u

extern "C" {

struct DxvkOrgInteropFeature
{
	const char* extension;  // null for core features
	const char* feature;    // feature struct member name, e.g. "descriptorHeap"
};

struct DxvkOrgInteropFeatureRequest
{
	uint32_t version;
	uint32_t featureCount;
	const DxvkOrgInteropFeature* features;
};

struct DxvkOrgInteropDeviceInfo
{
	uint32_t version;
	PFN_vkGetInstanceProcAddr getInstanceProcAddr;
	VkInstance instance;
	uint32_t instanceApiVersion;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue graphicsQueue;
	uint32_t graphicsQueueFamily;
	uint32_t graphicsQueueIndex;
	uint32_t enabledExtensionCount;
	const char* const* enabledExtensions;
	const VkPhysicalDeviceFeatures2* enabledFeatures;
	uint32_t grantedFeatureCount;
	uint32_t deniedFeatureCount;
};

typedef void (*PFN_dxvkOrgInteropTeardown)(void* user, VkDevice device);
typedef void (*PFN_dxvkOrgInteropSubmitted)(void* user, VkResult result);

// Client command buffers submitted in D3D11 stream order (dxvkEnqueueInteropSubmission).
struct DxvkOrgInteropSubmission
{
	uint32_t version;
	uint32_t waitCount;
	const VkSemaphoreSubmitInfo* waits;
	uint32_t commandBufferCount;
	const VkCommandBufferSubmitInfo* commandBuffers;
	uint32_t signalCount;
	const VkSemaphoreSubmitInfo* signals;
	PFN_dxvkOrgInteropSubmitted onSubmitted;  // on DXVK's submission thread, after vkQueueSubmit2
	void* user;
};

typedef HRESULT(__stdcall* PFN_dxvkRequestDeviceFeatures)(const DxvkOrgInteropFeatureRequest* pRequest);
typedef HRESULT(__stdcall* PFN_dxvkGetInteropDeviceInfo)(ID3D11Device* pDevice, DxvkOrgInteropDeviceInfo* pInfo);
typedef HRESULT(__stdcall* PFN_dxvkCreateBufferFromVkBuffer)(ID3D11Device* pDevice,
	const D3D11_BUFFER_DESC* pDesc, VkBuffer buffer, ID3D11Buffer** ppBuffer);
typedef HRESULT(__stdcall* PFN_dxvkSetDeviceTeardownCallback)(PFN_dxvkOrgInteropTeardown pCallback, void* pUser);
typedef HRESULT(__stdcall* PFN_dxvkEnqueueInteropSubmission)(ID3D11Device* pDevice, const DxvkOrgInteropSubmission* pSubmission);
}
