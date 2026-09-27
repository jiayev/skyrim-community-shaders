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

#define DXVK_ORG_INTEROP_VERSION 2u

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
	uint32_t enabledInstanceExtensionCount;  // version 2
	const char* const* enabledInstanceExtensions;
};

enum DxvkOrgInteropResourceKind : uint32_t
{
	DXVK_ORG_INTEROP_RESOURCE_BUFFER = 1,
	DXVK_ORG_INTEROP_RESOURCE_IMAGE = 2,
};

struct DxvkOrgInteropBufferInfo
{
	VkBuffer buffer;  // a D3D11 buffer is a range of a (possibly shared) VkBuffer
	VkDeviceSize offset;
	VkDeviceSize size;
	VkDeviceAddress address;  // of the range's first byte
	VkBufferUsageFlags usage;
};

struct DxvkOrgInteropImageInfo
{
	VkImage image;
	VkImageType type;
	VkFormat format;
	VkImageCreateFlags flags;
	VkExtent3D extent;
	uint32_t mipLevels;
	uint32_t arrayLayers;
	VkSampleCountFlagBits samples;
	VkImageUsageFlags usage;
	VkImageLayout layout;  // the layout DXVK keeps the image in between its own commands
	VkImageViewType viewType;  // the SRV's view, or the whole image for a texture
	VkFormat viewFormat;
	VkComponentMapping components;
	VkImageSubresourceRange subresourceRange;
};

struct DxvkOrgInteropResourceInfo
{
	uint32_t version;
	uint32_t kind;  // DxvkOrgInteropResourceKind
	DxvkOrgInteropBufferInfo buffer;
	DxvkOrgInteropImageInfo image;
};

typedef void (*PFN_dxvkOrgInteropTeardown)(void* user, VkDevice device);
typedef void (*PFN_dxvkOrgInteropSubmitted)(void* user, VkResult result);
// On DXVK's submission thread in stream order with the graphics queue locked (dxvkEnqueueQueueCallback).
typedef void (*PFN_dxvkOrgInteropQueueCallback)(void* user, VkQueue queue);
// On DXVK's worker thread in D3D11 stream order, outside any render pass, with the command buffer it records
// (dxvkEmitCommandBufferCallback).
typedef void (*PFN_dxvkOrgInteropCommandBufferCallback)(void* user, VkCommandBuffer commandBuffer);

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
	const char* label;  // version 2, optional: a queue label around the submission under a capture tool
};

typedef HRESULT(__stdcall* PFN_dxvkRequestDeviceFeatures)(const DxvkOrgInteropFeatureRequest* pRequest);
typedef HRESULT(__stdcall* PFN_dxvkGetInteropDeviceInfo)(ID3D11Device* pDevice, DxvkOrgInteropDeviceInfo* pInfo);
typedef HRESULT(__stdcall* PFN_dxvkCreateBufferFromVkBuffer)(ID3D11Device* pDevice,
	const D3D11_BUFFER_DESC* pDesc, VkBuffer buffer, ID3D11Buffer** ppBuffer);
typedef HRESULT(__stdcall* PFN_dxvkSetDeviceTeardownCallback)(PFN_dxvkOrgInteropTeardown pCallback, void* pUser);
typedef HRESULT(__stdcall* PFN_dxvkEnqueueInteropSubmission)(ID3D11Device* pDevice, const DxvkOrgInteropSubmission* pSubmission);

// Several submissions as one (dxvkEnqueueInteropSubmissions): one flush, one stream entry, one vkQueueSubmit2.
struct DxvkOrgInteropSubmissionBatch
{
	uint32_t version;
	uint32_t submitCount;
	const VkSubmitInfo2* submits;  // pNext and flags ignored
	PFN_dxvkOrgInteropSubmitted onSubmitted;
	void* user;
	const char* label;
};
typedef HRESULT(__stdcall* PFN_dxvkEnqueueInteropSubmissions)(ID3D11Device* pDevice, const DxvkOrgInteropSubmissionBatch* pBatch);
// Describes a buffer, texture or SRV and marks it stable (never relocated or renamed from then on).
// Buffers the application can map are rejected (E_INVALIDARG): discard maps rename them.
typedef HRESULT(__stdcall* PFN_dxvkEnqueueQueueCallback)(ID3D11Device* pDevice, PFN_dxvkOrgInteropQueueCallback pCallback, void* pUser);
typedef HRESULT(__stdcall* PFN_dxvkEmitCommandBufferCallback)(ID3D11Device* pDevice, PFN_dxvkOrgInteropCommandBufferCallback pCallback, void* pUser);
// On DXVK's worker thread: onEnd records into each command buffer right before it ends, onBegin into the next right
// after it begins, so that what a client opened in one stays balanced across DXVK's flushes. Null clears them.
typedef HRESULT(__stdcall* PFN_dxvkSetCommandBufferBoundaryCallbacks)(ID3D11Device* pDevice, PFN_dxvkOrgInteropCommandBufferCallback pOnEnd,
	PFN_dxvkOrgInteropCommandBufferCallback pOnBegin, void* pUser);
typedef HRESULT(__stdcall* PFN_dxvkGetInteropResourceInfo)(ID3D11Device* pDevice, IUnknown* pObject, DxvkOrgInteropResourceInfo* pInfo);
// Address of the immediate context's submission counter: +1 each time DXVK closes a command list (implicit
// flushes, Flush(), the flush ahead of an enqueued submission). Read on the immediate context's thread.
typedef HRESULT(__stdcall* PFN_dxvkGetSubmissionCounter)(ID3D11Device* pDevice, const volatile uint64_t** ppCounter);

// Submission trace (diagnostics): every submission on DXVK's graphics queue, bracketed by timestamps.
enum DxvkOrgSubmissionKind
{
	DXVK_ORG_SUBMISSION_COMMAND_LIST = 0,
	DXVK_ORG_SUBMISSION_EXTERNAL = 1,
	DXVK_ORG_SUBMISSION_PRESENT = 2,
};

struct DxvkOrgSubmissionTraceRecord
{
	uint32_t kind;          // DxvkOrgSubmissionKind
	uint32_t flushType;     // command lists: the GpuFlushType that closed it, ~0u if unknown
	uint64_t submissionId;  // command lists flushed by the immediate context: its submission counter value
	int64_t appQpc;         // command lists: the application thread issued the flush
	int64_t csQpc;          // command lists: DXVK's CS thread closed the list
	int64_t queueQpc;       // the submission thread handed it to the queue
	uint64_t gpuBegin;      // timestamp ticks when the GPU reached it (0 for presents)
	uint64_t gpuEnd;        // timestamp ticks once everything up to its end completed (0 for presents)
	char label[64];         // flush reason, or the external submission's label
};

typedef HRESULT(__stdcall* PFN_dxvkSetSubmissionTrace)(ID3D11Device* pDevice, BOOL enable);
typedef HRESULT(__stdcall* PFN_dxvkReadSubmissionTrace)(ID3D11Device* pDevice, DxvkOrgSubmissionTraceRecord* pRecords, uint32_t capacity, uint32_t* pCount);
}
