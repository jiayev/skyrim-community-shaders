#pragma once

#include <d3d11.h>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>
#include <winrt/base.h>

#include "DXVKInteropInterfaces.h"

/** @brief Accesses DXVK's Vulkan device through its D3D11 interop interfaces. */
class DXVKInterop
{
public:
	enum class PresenterEncoding : uint8_t
	{
		kUnknown,
		kSDR,
		kHDR10,
		kHDR10ScRGBFallback,
	};

	class CommandTransaction
	{
	public:
		CommandTransaction() = default;
		CommandTransaction(CommandTransaction&&) noexcept = default;
		CommandTransaction& operator=(CommandTransaction&&) noexcept = default;
		CommandTransaction(const CommandTransaction&) = delete;
		CommandTransaction& operator=(const CommandTransaction&) = delete;

		explicit operator bool() const
		{
			return owner != nullptr && commandBuffer != VK_NULL_HANDLE && ringLock.owns_lock();
		}
		VkCommandBuffer GetCommandBuffer() const { return commandBuffer; }
		/** @brief Whether a fault left this transaction's queue submission potentially in flight. */
		bool SubmissionMayBeInFlight() const { return submissionMayBeInFlight; }

	private:
		friend class DXVKInterop;

		CommandTransaction(DXVKInterop* a_owner, uint32_t a_slot, VkCommandBuffer a_commandBuffer,
			std::unique_lock<std::recursive_mutex>&& a_ringLock) :
			owner(a_owner), slot(a_slot), commandBuffer(a_commandBuffer), ringLock(std::move(a_ringLock))
		{}

		DXVKInterop* owner = nullptr;
		uint32_t slot = UINT32_MAX;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		bool submitted = false;
		bool submissionMayBeInFlight = false;
		std::unique_lock<std::recursive_mutex> ringLock;
	};

	static DXVKInterop* GetSingleton();

	/** @brief Resolves DXVK's interop interfaces and Vulkan handles. */
	bool Initialize();

	/** @brief Whether the DXVK Vulkan device was resolved successfully. */
	bool IsAvailable() const { return available; }

	/** @brief Reads the latest successfully created presenter surface state from DXVK. */
	bool RefreshPresenterSurfaceState();
	/** @brief Latches a presenter state for the render frame at a render boundary. */
	void CommitPresenterSurfaceStateForRenderFrame();
	/** @brief Starts a color-space transition before changing or recreating the swap chain. */
	void BeginPresenterColorSpaceTransition(bool a_hdr, bool a_requireNewSerial = false);
	/** @brief Cancels a failed color-space transition for the requested output mode. */
	void CancelPresenterColorSpaceTransition(bool a_hdr);
	/** @brief Returns the effective presenter encoding latched for this render frame. */
	PresenterEncoding GetPresenterEncodingForFrame() const;
	/** @brief Returns the Vulkan surface format latched for this render frame. */
	VkFormat GetPresenterFormatForFrame() const;
	/** @brief Whether the latched presenter state exactly matches this frame's output mode. */
	bool IsPresenterStateReadyForFrame(bool a_hdr) const;

	VkInstance GetInstance() const { return instance; }
	VkPhysicalDevice GetPhysicalDevice() const { return physicalDevice; }
	VkDevice GetDevice() const { return device; }
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const { return vkGetInstanceProcAddr; }
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr() const { return vkGetDeviceProcAddr; }

	/** @brief Maps a D3D11 resource to its backing DXVK image. */
	bool GetVkImage(ID3D11Resource* a_resource, VkImage* a_outImage,
		VkImageLayout* a_outLayout = nullptr, VkImageCreateInfo* a_outInfo = nullptr) const;

	/** @brief Drains DXVK submissions without racing its queue thread. */
	[[nodiscard]] bool WaitDeviceIdle();

	/** @brief Creates the Streamline command-buffer ring. */
	bool CreateCommandResources(uint32_t a_framesInFlight = 3);

	/** @brief Destroys the command pool, command buffers and fences. */
	void DestroyCommandResources();

	/** @brief Drains ring submissions before interop resources are destroyed. */
	[[nodiscard]] bool DrainCommandRing();

	/** @brief Whether the command ring is ready (CreateCommandResources succeeded). */
	bool CommandResourcesReady() const;
	/** @brief Whether an ambiguous submission fault quarantined the command ring. */
	bool HasCommandRingFault() const;
	/** @brief True once VK_ERROR_DEVICE_LOST has been observed. Terminal for the session. */
	/** @brief Recreates a quarantined command ring after proving the Vulkan device idle. */
	[[nodiscard]] bool RecoverCommandRing();
	/** @brief Whether frame generation shares DXVK's game submission queue. */
	bool FrameGenerationQueueInteropReady() const;

	/** @brief Begins an available command buffer from the ring. */
	CommandTransaction BeginFrameCommandBuffer();

	/** @brief Submits a ring command buffer on DXVK's queue. */
	bool SubmitFrameCommandBuffer(CommandTransaction& a_transaction);

	/** @brief Defers image-view destruction until the current ring slot completes. */
	void QueueViewsForDeferredDelete(const CommandTransaction& a_transaction,
		const VkImageView* a_views, uint32_t a_count);
	/** @brief Holds D3D resources until the current ring slot completes. */
	void QueueResourcesForDeferredRelease(const CommandTransaction& a_transaction,
		ID3D11Resource* const* a_resources, uint32_t a_count);

private:
	DXVKInterop() = default;

	struct PresenterSurfaceState
	{
		uint64_t serial = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkColorSpaceKHR requestedColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		VkColorSpaceKHR effectiveColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	};

	using GetPresenterSurfaceStateFn = uint64_t (*)(uint32_t*, uint32_t*, uint32_t*);

	static VkColorSpaceKHR RequestedPresenterColorSpace(bool a_hdr);
	static PresenterEncoding ClassifyPresenterEncoding(const PresenterSurfaceState& a_state);
	static bool PresenterStateMatches(
		const PresenterSurfaceState& a_state, VkColorSpaceKHR a_requestedColorSpace);

	bool available = false;

	winrt::com_ptr<IDXGIVkInteropDevice> interopDevice;

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamilyIndex = 0;

	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
	PFN_vkDestroyImageView vkDestroyImageView = nullptr;
	GetPresenterSurfaceStateFn getPresenterSurfaceState = nullptr;

	mutable std::mutex presenterStateMutex;
	PresenterSurfaceState observedPresenterState;
	PresenterSurfaceState committedPresenterState;
	bool presenterTransitionPending = false;
	uint64_t presenterTransitionBaselineSerial = 0;
	uint32_t presenterTransitionFrameCount = 0;
	VkColorSpaceKHR presenterTransitionRequestedColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

	mutable std::recursive_mutex commandRingMutex;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> commandBuffers;
	std::vector<VkFence> commandFences;
	bool deviceLost = false;
	bool synchronousPresentControlAvailable = false;
	bool commandRingFaulted = false;
	std::vector<std::vector<VkImageView>> pendingViewDeletes;
	std::vector<std::vector<winrt::com_ptr<ID3D11Resource>>> pendingResourceReleases;
	uint32_t framesInFlight = 0;
	uint32_t commandFrameIndex = 0;
};
