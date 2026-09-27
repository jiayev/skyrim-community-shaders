#pragma once

#include <d3d11.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>
#include <winrt/base.h>

#include "DXVKInteropInterfaces.h"
#include "RenderGraph/DxvkOrgInterop.h"

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
		const char* timingLabel = nullptr;
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

	/** @brief Destroys the command pool, command buffers and the completion timeline. */
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

	/**
	 * @brief Begins an available command buffer from the ring.
	 * @param a_timingLabel Profiler name to time this buffer's GPU work under, or nullptr. The label must
	 *        outlive the submission (a string literal). See PublishCommandTimings.
	 */
	CommandTransaction BeginFrameCommandBuffer(const char* a_timingLabel = nullptr);

	/**
	 * @brief Submits a ring command buffer on DXVK's queue, after every D3D11 command issued before it and before
	 * every later one.
	 *
	 * On the stream thread (BindStreamThread) the buffer goes into DXVK's command stream
	 * (dxvkEnqueueInteropSubmission): DXVK closes its command list and submits the buffer after it, on its own
	 * submission thread, and nothing here waits. From any other thread it is submitted directly, which has to wait for
	 * DXVK to have submitted everything recorded so far (FlushRenderingCommands) to keep that order.
	 *
	 * Either way the order is the only thing the submission provides; the memory dependencies are the buffer's own: it
	 * opens and closes with a full memory barrier (BeginFrameCommandBuffer, and here), so what D3D11 wrote before it is
	 * visible to it, and what it writes is visible to D3D11 after it.
	 */
	bool SubmitFrameCommandBuffer(CommandTransaction& a_transaction);

	/**
	 * @brief Makes the calling thread the stream thread: the one that issues D3D11's immediate-context commands (the
	 * render thread), whose submissions go through DXVK's command stream. Call it on that thread, once a frame.
	 */
	void BindStreamThread() { streamThread.store(::GetCurrentThreadId(), std::memory_order_relaxed); }

	/**
	 * @brief Hands the GPU times of completed, labelled ring submissions to the CS profiler.
	 *
	 * These buffers are submitted between two DXVK submissions (SubmitFrameCommandBuffer flushes D3D11
	 * first), so a D3D11 timestamp pair around them also spans every GPU idle gap until DXVK's next
	 * submission reaches the queue. Timestamps recorded inside the buffer itself measure only its work.
	 * Call on the render thread, which owns the profiler.
	 */
	void PublishCommandTimings();

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
	/// Completion of the ring's submissions: every submission signals the next value, and a slot is free once the
	/// counter has reached the value its last submission signals (slotTimelineValues; 0: never submitted).
	VkSemaphore completionTimeline = VK_NULL_HANDLE;
	uint64_t lastTimelineValue = 0;
	std::vector<uint64_t> slotTimelineValues;
	/// dxvkEnqueueInteropSubmission, when this DXVK build has it (and CS_UPSCALE_SUBMIT is not "direct").
	PFN_dxvkEnqueueInteropSubmission enqueueSubmission = nullptr;
	std::atomic<DWORD> streamThread{ 0 };
	/// Set on DXVK's submission thread when an enqueued submission failed; the next BeginFrameCommandBuffer quarantines
	/// the ring, as a failed direct submission does.
	std::atomic<bool> streamSubmitFailed{ false };
	/// Enqueued submissions DXVK has not handed to the queue yet. A direct submission waits for none to be left, so the
	/// timeline's values reach the queue in increasing order and the direct buffer stays behind the enqueued ones.
	std::atomic<uint32_t> streamPending{ 0 };
	static void OnStreamSubmitted(void* a_user, VkResult a_result);
	/// Whether a_slot's last submission has completed (reads the timeline counter).
	bool SlotComplete(uint32_t a_slot, VkResult& a_error) const;
	/// Waits for every submission made so far; false (and the ring quarantined) when the wait fails.
	bool WaitForSubmissions();
	bool deviceLost = false;
	bool synchronousPresentControlAvailable = false;
	bool commandRingFaulted = false;
	std::vector<std::vector<VkImageView>> pendingViewDeletes;
	std::vector<std::vector<winrt::com_ptr<ID3D11Resource>>> pendingResourceReleases;
	uint32_t framesInFlight = 0;
	uint32_t commandFrameIndex = 0;

	/// Two timestamps per ring slot, sized for the ring's maximum depth.
	static constexpr uint32_t kMaxTimedSlots = 64;
	VkQueryPool timingPool = VK_NULL_HANDLE;
	double timestampPeriodNs = 0.0;
	/// Label of the timed submission each slot still holds results for (nullptr: none).
	std::vector<const char*> slotTimingLabels;
	/// When each slot's timed submission was handed to the queue (QueryPerformanceCounter), for GpuIdleTrace.
	std::vector<int64_t> slotSubmitQpc;
	/// Harvested (label, milliseconds) pairs awaiting PublishCommandTimings.
	std::vector<std::pair<const char*, float>> harvestedTimings;

	void CreateTimingPool();
	void HarvestSlotTiming(uint32_t a_slot);
};
