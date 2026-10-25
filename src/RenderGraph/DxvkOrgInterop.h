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

#define DXVK_ORG_INTEROP_VERSION 4u

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
	// Version 3: a queue of a compute-only family that DXVK never uses, or null. The client submits to it itself, under a
	// lock of its own; DXVK's resources stay exclusive to its graphics and transfer families.
	VkQueue computeQueue;
	uint32_t computeQueueFamily;
	uint32_t computeQueueIndex;
	// Version 4: a transfer-capable queue for the client's uploads that neither DXVK nor computeQueue uses, or null. The client
	// submits to it itself, under DXVK's submission lock.
	VkQueue uploadQueue;
	uint32_t uploadQueueFamily;
	uint32_t uploadQueueIndex;
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

// Descriptor binding models; the values equal BasicRHI's rhi::vulkan::BindingModelBits.
enum DxvkOrgInteropBindingModelBits : uint32_t
{
	DXVK_ORG_BINDING_MODEL_DESCRIPTOR_HEAP = 1u << 0,    // the client is granted descriptorHeap
	DXVK_ORG_BINDING_MODEL_DESCRIPTOR_BUFFER = 1u << 1,  // the client is granted descriptorBuffer and mutableDescriptorType
};

// During each device creation attempt, once DXVK picked its physical device and its own model (dxvkPreferredModel: one
// bit, or 0 for legacy descriptor sets), before the VkDevice exists: the models the client can record with. Instance-level
// Vulkan queries only; must not call into D3D11 or DXVK, and must not throw.
typedef uint32_t (*PFN_dxvkOrgInteropBindingModelCallback)(void* user, VkInstance instance, VkPhysicalDevice physicalDevice,
	PFN_vkGetInstanceProcAddr getInstanceProcAddr, uint32_t dxvkPreferredModel);

#define DXVK_ORG_BINDING_MODEL_NEGOTIATION_VERSION 1u

struct DxvkOrgInteropBindingModelNegotiation
{
	uint32_t version;  // DXVK_ORG_BINDING_MODEL_NEGOTIATION_VERSION
	PFN_dxvkOrgInteropBindingModelCallback callback;  // null clears it
	void* user;
};

// Before the device is created: DXVK runs on its own choice if accepted, else (heap <-> buffer only) on the other accepted model the
// device supports (dxvk.enableDescriptorHeap/enableDescriptorBuffer keep precedence), else on its own choice; legacy sets stay. The client is only ever granted
// DXVK's model: descriptorHeap on heaps, descriptorBuffer + mutableDescriptorType on descriptor buffers, neither on legacy sets.
typedef HRESULT(__stdcall* PFN_dxvkSetBindingModelCallback)(const DxvkOrgInteropBindingModelNegotiation* pNegotiation);

// enabledFeatures chains Vulkan 1.1-1.3 and the descriptor heap, descriptor buffer, mutable descriptor type, device-generated
// commands and robustness2 feature structs; walk it by sType.
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

// Real work recorded into DXVK's command buffer (dxvkEmitExternalCommands). The callback runs as dxvkEmitCommandBufferCallback's,
// outside any render pass (ended, not suspended: every image in DxvkOrgInteropImageInfo::layout, deferred clears done), after DXVK
// flushed its barriers. The client leaves every image it touched in that layout, ends what it began, and releases its own accesses
// with barriers (or sets FULL_BARRIER): DXVK's barrier tracker never sees them.
enum DxvkOrgInteropExternalCommandsFlagBits
{
	// The callback binds pipelines, layouts, descriptor heaps or buffers, push data, vertex/index buffers or dynamic state: DXVK
	// forgets every binding afterwards and rebinds its descriptor heaps or buffers.
	DXVK_ORG_EXTERNAL_COMMANDS_BINDS_STATE = 1u << 0,
	// DXVK records a full memory barrier (all commands, writes -> reads and writes) after the callback.
	DXVK_ORG_EXTERNAL_COMMANDS_FULL_BARRIER = 1u << 1,
};

#define DXVK_ORG_EXTERNAL_COMMANDS_VERSION 1u

struct DxvkOrgInteropExternalCommands
{
	uint32_t version;  // DXVK_ORG_EXTERNAL_COMMANDS_VERSION
	uint32_t flags;    // DxvkOrgInteropExternalCommandsFlagBits
	PFN_dxvkOrgInteropCommandBufferCallback callback;
	void* user;
	// Timeline waits and signals joined to the graphics submission that executes the commands (stages 0: all commands); copied.
	// A signal completes once that whole submission did; values grow in stream order per semaphore.
	uint32_t waitCount;
	const VkSemaphoreSubmitInfo* waits;
	uint32_t signalCount;
	const VkSemaphoreSubmitInfo* signals;
	const char* label;  // optional debug label region around the commands; copied
};
typedef HRESULT(__stdcall* PFN_dxvkEmitExternalCommands)(ID3D11Device* pDevice, const DxvkOrgInteropExternalCommands* pCommands);
// On DXVK's worker thread: onEnd records into each command buffer right before it ends, onBegin into the next right
// after it begins, so that what a client opened in one stays balanced across DXVK's flushes. Null clears them.
typedef HRESULT(__stdcall* PFN_dxvkSetCommandBufferBoundaryCallbacks)(ID3D11Device* pDevice, PFN_dxvkOrgInteropCommandBufferCallback pOnEnd,
	PFN_dxvkOrgInteropCommandBufferCallback pOnBegin, void* pUser);
typedef HRESULT(__stdcall* PFN_dxvkGetInteropResourceInfo)(ID3D11Device* pDevice, IUnknown* pObject, DxvkOrgInteropResourceInfo* pInfo);
// dxvkGetInteropResourceInfo for many objects: the buffers needing a stable address are pinned by one chunk on DXVK's worker
// thread, one synchronization in all. pResults[i] is object i's result.
typedef HRESULT(__stdcall* PFN_dxvkGetInteropResourceInfos)(ID3D11Device* pDevice, UINT count, IUnknown* const* pObjects, DxvkOrgInteropResourceInfo* pInfos, HRESULT* pResults);
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
