#include "DXVKInterop.h"

#include "Globals.h"
#include "GpuIdleTrace.h"
#include "Profiler.h"
#include "Features/DrawcallLimitFix/Switches.h"

#include <algorithm>

namespace
{
	/// Times a buffer that has no profiler label while GpuIdleTrace runs; never published to the profiler.
	constexpr const char kTraceOnlyLabel[] = "Streamline interop (unlabelled)";

	struct QueueSubmitAttempt
	{
		VkResult endResult = VK_ERROR_DEVICE_LOST;
		VkResult submitResult = VK_ERROR_DEVICE_LOST;
		DWORD exceptionCode = 0;
		bool queueLockAcquired = false;
		bool faulted = false;
	};

	struct VulkanResultAttempt
	{
		VkResult result = VK_ERROR_DEVICE_LOST;
	};

	VulkanResultAttempt CreateCommandPoolSEH(VkDevice a_device,
		const VkCommandPoolCreateInfo* a_createInfo, VkCommandPool* a_commandPool) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkCreateCommandPool(a_device, a_createInfo, nullptr, a_commandPool);
		return attempt;
	}

	VulkanResultAttempt AllocateCommandBuffersSEH(VkDevice a_device,
		const VkCommandBufferAllocateInfo* a_allocateInfo, VkCommandBuffer* a_commandBuffers) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkAllocateCommandBuffers(a_device, a_allocateInfo, a_commandBuffers);
		return attempt;
	}

	struct CounterAttempt
	{
		VkResult result = VK_ERROR_DEVICE_LOST;
		uint64_t value = 0;
	};

	CounterAttempt GetTimelineValueSEH(VkDevice a_device, VkSemaphore a_semaphore) noexcept
	{
		CounterAttempt attempt{};
		attempt.result = vkGetSemaphoreCounterValue(a_device, a_semaphore, &attempt.value);
		return attempt;
	}

	VulkanResultAttempt WaitForTimelineSEH(VkDevice a_device, VkSemaphore a_semaphore, uint64_t a_value) noexcept
	{
		VulkanResultAttempt attempt{};
		VkSemaphoreWaitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		waitInfo.semaphoreCount = 1;
		waitInfo.pSemaphores = &a_semaphore;
		waitInfo.pValues = &a_value;
		attempt.result = vkWaitSemaphores(a_device, &waitInfo, UINT64_MAX);
		return attempt;
	}

	VulkanResultAttempt ResetCommandBufferSEH(VkCommandBuffer a_commandBuffer) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkResetCommandBuffer(a_commandBuffer, 0);
		return attempt;
	}

	VulkanResultAttempt BeginCommandBufferSEH(VkCommandBuffer a_commandBuffer,
		const VkCommandBufferBeginInfo* a_beginInfo) noexcept
	{
		VulkanResultAttempt attempt{};
		attempt.result = vkBeginCommandBuffer(a_commandBuffer, a_beginInfo);
		return attempt;
	}

	void DestroyImageView(PFN_vkDestroyImageView a_destroyImageView,
		VkDevice a_device, VkImageView a_view) noexcept
	{
		if (a_destroyImageView && a_view != VK_NULL_HANDLE)
			a_destroyImageView(a_device, a_view, nullptr);
	}

	void DestroyCommandPool(VkDevice a_device, VkCommandPool a_commandPool) noexcept
	{
		if (a_commandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(a_device, a_commandPool, nullptr);
	}

	void FreeCommandBuffers(VkDevice a_device, VkCommandPool a_commandPool,
		uint32_t a_count, const VkCommandBuffer* a_commandBuffers) noexcept
	{
		if (a_count)
			vkFreeCommandBuffers(a_device, a_commandPool, a_count, a_commandBuffers);
	}

	void ReleaseSubmissionQueue(IDXGIVkInteropDevice* a_interopDevice) noexcept
	{
		a_interopDevice->ReleaseSubmissionQueue();
	}

	/// Waits until DXVK has handed every enqueued ring submission to the queue (DXVKInterop::streamPending).
	void WaitForStreamPending(std::atomic<uint32_t>& a_pending) noexcept
	{
		for (uint32_t pending = a_pending.load(std::memory_order_acquire); pending; pending = a_pending.load(std::memory_order_acquire))
			a_pending.wait(pending, std::memory_order_acquire);
	}

	/// Submits a recorded command buffer directly on DXVK's queue through the COM interop device, for a thread other than
	/// the stream thread. FlushRenderingCommands (DXVK's Flush and SynchronizeCsThread(SynchronizeAll)) waits until DXVK has
	/// submitted every D3D11 command recorded so far, which is what orders this buffer after them; then the submission
	/// queue's lock is taken for the submit.
	QueueSubmitAttempt DirectQueueSubmitSEH(IDXGIVkInteropDevice* a_interopDevice, VkQueue a_queue,
		VkCommandBuffer a_commandBuffer, VkSemaphore a_timeline, uint64_t a_value, std::atomic<uint32_t>& a_streamPending) noexcept
	{
		QueueSubmitAttempt attempt{};
		__try {
			attempt.endResult = vkEndCommandBuffer(a_commandBuffer);
			if (attempt.endResult == VK_SUCCESS) {
				a_interopDevice->FlushRenderingCommands();
				// FlushRenderingCommands has DXVK's worker process the enqueued ones; its submission thread submits them.
				WaitForStreamPending(a_streamPending);
				__try {
					a_interopDevice->LockSubmissionQueue();
					attempt.queueLockAcquired = true;
					VkTimelineSemaphoreSubmitInfo timelineInfo{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
					timelineInfo.signalSemaphoreValueCount = 1;
					timelineInfo.pSignalSemaphoreValues = &a_value;
					VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
					submitInfo.pNext = &timelineInfo;
					submitInfo.commandBufferCount = 1;
					submitInfo.pCommandBuffers = &a_commandBuffer;
					submitInfo.signalSemaphoreCount = 1;
					submitInfo.pSignalSemaphores = &a_timeline;
					attempt.submitResult = vkQueueSubmit(a_queue, 1, &submitInfo, VK_NULL_HANDLE);
				} __finally {
					if (attempt.queueLockAcquired)
						ReleaseSubmissionQueue(a_interopDevice);
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			attempt.faulted = true;
			attempt.exceptionCode = GetExceptionCode();
		}
		return attempt;
	}

	struct StreamSubmitAttempt
	{
		VkResult endResult = VK_ERROR_DEVICE_LOST;
		HRESULT enqueueResult = E_FAIL;
		DWORD exceptionCode = 0;
		bool faulted = false;
	};

	/// Puts a recorded command buffer into DXVK's command stream (dxvkEnqueueInteropSubmission) on the stream thread:
	/// DXVK closes its command list and submits the buffer after it, on its submission thread, before whatever D3D11
	/// records next. Nothing is waited for.
	StreamSubmitAttempt StreamSubmitSEH(PFN_dxvkEnqueueInteropSubmission a_enqueue, const DxvkOrgInteropSubmission& a_submission,
		VkCommandBuffer a_commandBuffer) noexcept
	{
		StreamSubmitAttempt attempt{};
		__try {
			attempt.endResult = vkEndCommandBuffer(a_commandBuffer);
			if (attempt.endResult == VK_SUCCESS)
				attempt.enqueueResult = a_enqueue(globals::d3d::device, &a_submission);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			attempt.faulted = true;
			attempt.exceptionCode = GetExceptionCode();
		}
		return attempt;
	}

	/// Everything written on the queue before this point is visible to everything after it: the ring buffers open and close
	/// with one, which is what makes submission order enough between them and D3D11's work (SubmitFrameCommandBuffer).
	void FullMemoryBarrier(VkCommandBuffer a_commandBuffer) noexcept
	{
		VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		vkCmdPipelineBarrier(a_commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
			nullptr, 0, nullptr);
	}

	struct DeviceIdleAttempt
	{
		VkResult result = VK_ERROR_INITIALIZATION_FAILED;
		DWORD exceptionCode = 0;
		bool queueLockAcquired = false;
		bool functionAvailable = false;
		bool faulted = false;
	};

	DeviceIdleAttempt WaitDeviceIdleSEH(IDXGIVkInteropDevice* a_interopDevice,
		PFN_vkGetDeviceProcAddr a_getDeviceProcAddr, VkDevice a_device) noexcept
	{
		DeviceIdleAttempt attempt{};
		__try {
			a_interopDevice->FlushRenderingCommands();
			__try {
				a_interopDevice->LockSubmissionQueue();
				attempt.queueLockAcquired = true;
				auto waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(
					a_getDeviceProcAddr(a_device, "vkDeviceWaitIdle"));
				attempt.functionAvailable = waitIdle != nullptr;
				if (waitIdle)
					attempt.result = waitIdle(a_device);
			} __finally {
				if (attempt.queueLockAcquired)
					ReleaseSubmissionQueue(a_interopDevice);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			attempt.faulted = true;
			attempt.exceptionCode = GetExceptionCode();
		}
		return attempt;
	}

	VkResult CreateTimelineSemaphore(VkDevice a_device, VkSemaphore* a_semaphore) noexcept
	{
		*a_semaphore = VK_NULL_HANDLE;
		VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		typeInfo.initialValue = 0;
		VkSemaphoreCreateInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		info.pNext = &typeInfo;
		return vkCreateSemaphore(a_device, &info, nullptr, a_semaphore);
	}
}

DXVKInterop* DXVKInterop::GetSingleton()
{
	static DXVKInterop singleton;
	return &singleton;
}

VkColorSpaceKHR DXVKInterop::RequestedPresenterColorSpace(bool a_hdr)
{
	return a_hdr ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

DXVKInterop::PresenterEncoding DXVKInterop::ClassifyPresenterEncoding(const PresenterSurfaceState& a_state)
{
	if (!a_state.serial)
		return PresenterEncoding::kUnknown;

	if (a_state.requestedColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
		a_state.effectiveColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		return PresenterEncoding::kSDR;

	if (a_state.requestedColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
		if (a_state.effectiveColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
			return PresenterEncoding::kHDR10;
		if (a_state.effectiveColorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
			return PresenterEncoding::kHDR10ScRGBFallback;
	}

	return PresenterEncoding::kUnknown;
}

bool DXVKInterop::PresenterStateMatches(
	const PresenterSurfaceState& a_state, VkColorSpaceKHR a_requestedColorSpace)
{
	if (!a_state.serial || a_state.requestedColorSpace != a_requestedColorSpace)
		return false;

	const PresenterEncoding encoding = ClassifyPresenterEncoding(a_state);
	if (a_requestedColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
		return encoding == PresenterEncoding::kHDR10 ||
		       encoding == PresenterEncoding::kHDR10ScRGBFallback;
	return a_requestedColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
	       encoding == PresenterEncoding::kSDR;
}

bool DXVKInterop::RefreshPresenterSurfaceState()
{
	if (!getPresenterSurfaceState)
		return false;

	uint32_t format = VK_FORMAT_UNDEFINED;
	uint32_t requestedColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	uint32_t effectiveColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	const uint64_t serial = getPresenterSurfaceState(&format, &requestedColorSpace, &effectiveColorSpace);
	if (!serial)
		return false;

	std::lock_guard lock(presenterStateMutex);
	if (serial <= observedPresenterState.serial)
		return false;

	observedPresenterState.serial = serial;
	observedPresenterState.format = static_cast<VkFormat>(format);
	observedPresenterState.requestedColorSpace = static_cast<VkColorSpaceKHR>(requestedColorSpace);
	observedPresenterState.effectiveColorSpace = static_cast<VkColorSpaceKHR>(effectiveColorSpace);
	logger::info("[DXVKInterop] Observed presenter surface serial {}: format={}, requestedColorSpace={}, effectiveColorSpace={}",
		serial, format, requestedColorSpace, effectiveColorSpace);
	return true;
}

void DXVKInterop::CommitPresenterSurfaceStateForRenderFrame()
{
	std::lock_guard lock(presenterStateMutex);

	if (presenterTransitionPending) {
		const bool satisfied =
			observedPresenterState.serial > presenterTransitionBaselineSerial &&
			observedPresenterState.requestedColorSpace == presenterTransitionRequestedColorSpace;
		if (!satisfied) {
			if (++presenterTransitionFrameCount > 120) {
				logger::warn("[DXVKInterop] presenter color-space transition timed out after {} frames; cancelling",
					presenterTransitionFrameCount);
				presenterTransitionPending = false;
				presenterTransitionFrameCount = 0;
			}
			return;
		}
	}

	if (!observedPresenterState.serial ||
		observedPresenterState.serial <= committedPresenterState.serial)
		return;

	if (presenterTransitionPending) {
		presenterTransitionPending = false;
		presenterTransitionFrameCount = 0;
	}

	committedPresenterState = observedPresenterState;
	logger::info("[DXVKInterop] Committed presenter surface serial {} for render frames",
		committedPresenterState.serial);
	if (ClassifyPresenterEncoding(committedPresenterState) == PresenterEncoding::kHDR10ScRGBFallback) {
		logger::warn("[DXVKInterop] HDR frame generation disabled for the scRGB presenter fallback; "
		             "a HUD-less image rendered directly in the presenter encoding is required");
	}
}

void DXVKInterop::BeginPresenterColorSpaceTransition(bool a_hdr, bool a_requireNewSerial)
{
	const VkColorSpaceKHR requestedColorSpace = RequestedPresenterColorSpace(a_hdr);
	std::lock_guard lock(presenterStateMutex);

	if (presenterTransitionPending &&
		presenterTransitionRequestedColorSpace == requestedColorSpace)
		return;

	if (!presenterTransitionPending && !a_requireNewSerial &&
		(PresenterStateMatches(committedPresenterState, requestedColorSpace) ||
		 PresenterStateMatches(observedPresenterState, requestedColorSpace)))
		return;

	presenterTransitionPending = true;
	presenterTransitionFrameCount = 0;
	presenterTransitionRequestedColorSpace = requestedColorSpace;
	presenterTransitionBaselineSerial = observedPresenterState.serial > committedPresenterState.serial
	                                    ? observedPresenterState.serial
	                                    : committedPresenterState.serial;
	logger::info("[DXVKInterop] Presenter color-space transition started: target={}, baselineSerial={}",
		static_cast<uint32_t>(requestedColorSpace), presenterTransitionBaselineSerial);
}

void DXVKInterop::CancelPresenterColorSpaceTransition(bool a_hdr)
{
	const VkColorSpaceKHR requestedColorSpace = RequestedPresenterColorSpace(a_hdr);
	std::lock_guard lock(presenterStateMutex);
	if (presenterTransitionPending &&
		presenterTransitionRequestedColorSpace == requestedColorSpace) {
		presenterTransitionPending = false;
		logger::warn("[DXVKInterop] Presenter color-space transition cancelled for target={}",
			static_cast<uint32_t>(requestedColorSpace));
	}
}

DXVKInterop::PresenterEncoding DXVKInterop::GetPresenterEncodingForFrame() const
{
	std::lock_guard lock(presenterStateMutex);
	return presenterTransitionPending ? PresenterEncoding::kUnknown :
	                                    ClassifyPresenterEncoding(committedPresenterState);
}

VkFormat DXVKInterop::GetPresenterFormatForFrame() const
{
	std::lock_guard lock(presenterStateMutex);
	return presenterTransitionPending ? VK_FORMAT_UNDEFINED : committedPresenterState.format;
}

bool DXVKInterop::IsPresenterStateReadyForFrame(bool a_hdr) const
{
	std::lock_guard lock(presenterStateMutex);
	if (presenterTransitionPending ||
		!PresenterStateMatches(committedPresenterState, RequestedPresenterColorSpace(a_hdr)))
		return false;
	return ClassifyPresenterEncoding(committedPresenterState) != PresenterEncoding::kHDR10ScRGBFallback;
}

bool DXVKInterop::Initialize()
{
	if (available)
		return true;

	auto d3dDevice = globals::d3d::device;
	if (!d3dDevice) {
		logger::warn("[DXVKInterop] No D3D11 device available yet");
		return false;
	}

	winrt::com_ptr<IDXGIVkInteropDevice> dev;
	if (FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), dev.put_void()))) {
		logger::info("[DXVKInterop] IDXGIVkInteropDevice not present — not running under DXVK");
		return false;
	}

	interopDevice = dev;
	interopDevice->GetVulkanHandles(&instance, &physicalDevice, &device);
	interopDevice->GetSubmissionQueue(&queue, &queueFamilyIndex);

	if (!instance || !physicalDevice || !device || !queue) {
		logger::error("[DXVKInterop] DXVK returned null Vulkan handles");
		interopDevice = nullptr;
		return false;
	}

	if (HMODULE vk = GetModuleHandleW(L"vulkan-1.dll")) {
		vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
			reinterpret_cast<void*>(GetProcAddress(vk, "vkGetInstanceProcAddr")));
	}
	if (!vkGetInstanceProcAddr) {
		logger::error("[DXVKInterop] Could not resolve vkGetInstanceProcAddr from vulkan-1.dll");
		interopDevice = nullptr;
		return false;
	}

	vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
		vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
	if (!vkGetDeviceProcAddr) {
		logger::error("[DXVKInterop] Could not resolve vkGetDeviceProcAddr");
		interopDevice = nullptr;
		return false;
	}

	vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
		vkGetDeviceProcAddr(device, "vkDestroyImageView"));

	if (HMODULE module = GetModuleHandleW(L"dxvk_d3d11.dll")) {
		synchronousPresentControlAvailable = GetProcAddress(module, "dxvkSetSyncPresent") != nullptr;
		getPresenterSurfaceState = reinterpret_cast<GetPresenterSurfaceStateFn>(
			GetProcAddress(module, "dxvkGetPresenterSurfaceState"));
		// CS_UPSCALE_SUBMIT=direct: every ring submission waits for DXVK's command stream (the previous behaviour).
		if (DCLF::SwitchValue("CS_UPSCALE_SUBMIT") != "direct")
			enqueueSubmission = reinterpret_cast<PFN_dxvkEnqueueInteropSubmission>(GetProcAddress(module, "dxvkEnqueueInteropSubmission"));
	}
	logger::info("[DXVKInterop] Ring submissions from the render thread {}", enqueueSubmission ?
		"go through DXVK's command stream (no wait)" : "flush and wait for DXVK's command stream");
	if (!synchronousPresentControlAvailable)
		logger::warn("[DXVKInterop] dxvkSetSyncPresent is unavailable - DLSS-G disabled");
	if (!getPresenterSurfaceState)
		logger::warn("[DXVKInterop] dxvkGetPresenterSurfaceState is unavailable - frame generation disabled");

	if (auto pfnProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
			vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"))) {
		VkPhysicalDeviceProperties props{};
		pfnProps(physicalDevice, &props);
		logger::info("[DXVKInterop] Bridged to DXVK Vulkan device: '{}' (API {}.{}.{}), queueFamily {}",
			props.deviceName,
			VK_API_VERSION_MAJOR(props.apiVersion),
			VK_API_VERSION_MINOR(props.apiVersion),
			VK_API_VERSION_PATCH(props.apiVersion),
			queueFamilyIndex);
	} else {
		logger::info("[DXVKInterop] Bridged to DXVK Vulkan device (queueFamily {})", queueFamilyIndex);
	}

	available = true;
	return true;
}

bool DXVKInterop::GetVkImage(ID3D11Resource* a_resource, VkImage* a_outImage,
	VkImageLayout* a_outLayout, VkImageCreateInfo* a_outInfo) const
{
	if (!available || !a_resource)
		return false;

	winrt::com_ptr<IDXGIVkInteropSurface> surface;
	if (FAILED(a_resource->QueryInterface(__uuidof(IDXGIVkInteropSurface), surface.put_void())))
		return false;

	VkImageCreateInfo localInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageCreateInfo* info = a_outInfo ? a_outInfo : &localInfo;
	info->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info->pNext = nullptr;
	info->queueFamilyIndexCount = 0;
	info->pQueueFamilyIndices = nullptr;

	return SUCCEEDED(surface->GetVulkanImageInfo(a_outImage, a_outLayout, info));
}

void DXVKInterop::OnStreamSubmitted(void* a_user, VkResult a_result)
{
	auto* self = static_cast<DXVKInterop*>(a_user);
	if (a_result != VK_SUCCESS) {
		self->streamSubmitFailed.store(true, std::memory_order_release);
		logger::error("[DXVKInterop] an enqueued ring submission failed on DXVK's submission thread ({})", static_cast<int>(a_result));
	}
	self->streamPending.fetch_sub(1, std::memory_order_acq_rel);
	self->streamPending.notify_all();
}

bool DXVKInterop::SlotComplete(uint32_t a_slot, VkResult& a_error) const
{
	a_error = VK_SUCCESS;
	if (a_slot >= slotTimelineValues.size() || slotTimelineValues[a_slot] == 0)
		return true;
	const CounterAttempt counter = GetTimelineValueSEH(device, completionTimeline);
	if (counter.result != VK_SUCCESS) {
		a_error = counter.result;
		return false;
	}
	return counter.value >= slotTimelineValues[a_slot];
}

bool DXVKInterop::WaitForSubmissions()
{
	if (completionTimeline == VK_NULL_HANDLE || lastTimelineValue == 0)
		return true;
	// Enqueued submissions reach the queue only once DXVK's worker has processed them.
	if (streamPending.load(std::memory_order_acquire)) {
		interopDevice->FlushRenderingCommands();
		WaitForStreamPending(streamPending);
	}
	const VulkanResultAttempt waitAttempt = WaitForTimelineSEH(device, completionTimeline, lastTimelineValue);
	if (waitAttempt.result != VK_SUCCESS) {
		commandRingFaulted = true;
		logger::error("[DXVKInterop] waiting for the command ring's submissions failed ({})", static_cast<int>(waitAttempt.result));
		return false;
	}
	return true;
}

bool DXVKInterop::WaitDeviceIdle()
{
	std::lock_guard lock(commandRingMutex);
	if (!interopDevice || !vkGetDeviceProcAddr || device == VK_NULL_HANDLE)
		return false;
	if (deviceLost)
		return true;

	const DeviceIdleAttempt attempt = WaitDeviceIdleSEH(interopDevice.get(), vkGetDeviceProcAddr, device);
	if (attempt.faulted) {
		logger::error("[DXVKInterop] device-idle synchronization faulted (SEH {:#x})", attempt.exceptionCode);
		return false;
	} else if (!attempt.functionAvailable) {
		logger::error("[DXVKInterop] vkDeviceWaitIdle is unavailable");
		return false;
	} else if (attempt.result != VK_SUCCESS) {
		if (attempt.result == VK_ERROR_DEVICE_LOST) {
			deviceLost = true;
			commandRingFaulted = true;
			logger::critical("[DXVKInterop] device lost; Vulkan interop is unavailable for this session");
			return true;
		}
		logger::error("[DXVKInterop] vkDeviceWaitIdle failed ({})", static_cast<int>(attempt.result));
		return false;
	}
	return true;
}

bool DXVKInterop::CreateCommandResources(uint32_t a_framesInFlight)
{
	std::lock_guard lock(commandRingMutex);
	if (!available)
		return false;
	if (commandPool != VK_NULL_HANDLE)
		return !commandRingFaulted;

	commandRingFaulted = false;

	framesInFlight = a_framesInFlight ? a_framesInFlight : 1;

	VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamilyIndex;
	VkCommandPool createdCommandPool = VK_NULL_HANDLE;
	const VulkanResultAttempt poolAttempt = CreateCommandPoolSEH(device, &poolInfo, &createdCommandPool);
	const bool poolCreated = poolAttempt.result == VK_SUCCESS;
	if (!poolCreated) {
		createdCommandPool = VK_NULL_HANDLE;
		commandRingFaulted = true;
		logger::error("[DXVKInterop] vkCreateCommandPool failed ({})",
			static_cast<int>(poolAttempt.result));
		return false;
	}
	commandPool = createdCommandPool;

	std::vector<VkCommandBuffer> allocatedCommandBuffers(framesInFlight, VK_NULL_HANDLE);
	VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = framesInFlight;
	const VulkanResultAttempt allocateAttempt =
		AllocateCommandBuffersSEH(device, &allocInfo, allocatedCommandBuffers.data());
	const bool commandBuffersAllocated =
		allocateAttempt.result == VK_SUCCESS;
	if (!commandBuffersAllocated) {
		std::fill(allocatedCommandBuffers.begin(), allocatedCommandBuffers.end(), VK_NULL_HANDLE);
		logger::error("[DXVKInterop] vkAllocateCommandBuffers failed ({})",
			static_cast<int>(allocateAttempt.result));
		commandRingFaulted = true;
		DestroyCommandResources();
		commandRingFaulted = true;
		return false;
	}
	commandBuffers = std::move(allocatedCommandBuffers);

	VkSemaphore createdTimeline = VK_NULL_HANDLE;
	if (const VkResult result = CreateTimelineSemaphore(device, &createdTimeline); result != VK_SUCCESS) {
		logger::error("[DXVKInterop] creating the command ring's timeline semaphore failed ({})", static_cast<int>(result));
		commandRingFaulted = true;
		DestroyCommandResources();
		return false;
	}
	completionTimeline = createdTimeline;
	lastTimelineValue = 0;
	slotTimelineValues.assign(framesInFlight, 0);
	streamSubmitFailed.store(false, std::memory_order_relaxed);

	commandFrameIndex = 0;
	pendingViewDeletes.assign(framesInFlight, {});
	pendingResourceReleases.assign(framesInFlight, {});
	slotTimingLabels.assign(framesInFlight, nullptr);
	slotSubmitQpc.assign(framesInFlight, 0);
	CreateTimingPool();
	logger::info("[DXVKInterop] Command ring created ({} frames in flight, queueFamily {})", framesInFlight, queueFamilyIndex);
	return true;
}

void DXVKInterop::DestroyCommandResources()
{
	std::lock_guard lock(commandRingMutex);
	if (device == VK_NULL_HANDLE)
		return;
	if (commandRingFaulted && !WaitDeviceIdle()) {
		logger::error("[DXVKInterop] command resources remain quarantined because device idle could not be proven");
		return;
	}

	if (!commandRingFaulted && !WaitForSubmissions())
		return;
	if (!vkDestroyImageView) {
		for (const auto& slot : pendingViewDeletes) {
			if (std::find_if(slot.begin(), slot.end(),
					[](VkImageView a_view) { return a_view != VK_NULL_HANDLE; }) != slot.end()) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] vkDestroyImageView is unavailable; command resources remain quarantined");
				return;
			}
		}
	} else {
		for (size_t slotIndex = 0; slotIndex < pendingViewDeletes.size(); ++slotIndex) {
			auto& slot = pendingViewDeletes[slotIndex];
			for (VkImageView& v : slot) {
				if (v == VK_NULL_HANDLE)
					continue;
				DestroyImageView(vkDestroyImageView, device, v);
				v = VK_NULL_HANDLE;
			}
			slot.clear();
			if (slotIndex < pendingResourceReleases.size())
				pendingResourceReleases[slotIndex].clear();
		}
	}
	pendingViewDeletes.clear();
	pendingResourceReleases.clear();
	if (completionTimeline != VK_NULL_HANDLE) {
		vkDestroySemaphore(device, completionTimeline, nullptr);
		completionTimeline = VK_NULL_HANDLE;
	}
	lastTimelineValue = 0;
	slotTimelineValues.clear();

	if (timingPool != VK_NULL_HANDLE) {
		vkDestroyQueryPool(device, timingPool, nullptr);
		timingPool = VK_NULL_HANDLE;
	}
	slotTimingLabels.clear();
	slotSubmitQpc.clear();
	harvestedTimings.clear();

	if (commandPool != VK_NULL_HANDLE) {
		DestroyCommandPool(device, commandPool);
		commandPool = VK_NULL_HANDLE;
	}
	commandBuffers.clear();
	framesInFlight = 0;
	commandFrameIndex = 0;
	commandRingFaulted = false;
}

bool DXVKInterop::DrainCommandRing()
{
	std::lock_guard lock(commandRingMutex);
	if (commandPool == VK_NULL_HANDLE)
		return true;
	if (device == VK_NULL_HANDLE)
		return false;
	if (commandRingFaulted && !WaitDeviceIdle()) {
		logger::error("[DXVKInterop] command-ring resources remain quarantined because device idle could not be proven");
		return false;
	}

	if (!commandRingFaulted && !WaitForSubmissions())
		return false;

	if (!vkDestroyImageView) {
		for (const auto& slot : pendingViewDeletes) {
			if (std::find_if(slot.begin(), slot.end(),
					[](VkImageView a_view) { return a_view != VK_NULL_HANDLE; }) != slot.end()) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] vkDestroyImageView is unavailable; deferred views remain quarantined");
				return false;
			}
		}
	} else {
		for (size_t slotIndex = 0; slotIndex < pendingViewDeletes.size(); ++slotIndex) {
			auto& slot = pendingViewDeletes[slotIndex];
			for (VkImageView& v : slot) {
				if (v == VK_NULL_HANDLE)
					continue;
				DestroyImageView(vkDestroyImageView, device, v);
				v = VK_NULL_HANDLE;
			}
			slot.clear();
			if (slotIndex < pendingResourceReleases.size())
				pendingResourceReleases[slotIndex].clear();
		}
	}
	return true;
}

bool DXVKInterop::CommandResourcesReady() const
{
	std::lock_guard lock(commandRingMutex);
	return commandPool != VK_NULL_HANDLE && !commandRingFaulted;
}

bool DXVKInterop::HasCommandRingFault() const
{
	std::lock_guard lock(commandRingMutex);
	return commandRingFaulted;
}

bool DXVKInterop::RecoverCommandRing()
{
	std::lock_guard lock(commandRingMutex);
	if (!commandRingFaulted)
		return commandPool != VK_NULL_HANDLE;

	const uint32_t ringDepth = framesInFlight ? framesInFlight : 3;
	if (!WaitDeviceIdle())
		return false;
	DestroyCommandResources();
	if (commandPool != VK_NULL_HANDLE)
		return false;
	if (!CreateCommandResources(ringDepth)) {
		logger::error("[DXVKInterop] failed to recreate the quarantined command ring");
		return false;
	}

	logger::warn("[DXVKInterop] recovered the command ring after an ambiguous submission fault");
	return true;
}

bool DXVKInterop::FrameGenerationQueueInteropReady() const
{
	std::lock_guard lock(commandRingMutex);
	return available;
}

void DXVKInterop::CreateTimingPool()
{
	if (timingPool != VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
		return;

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physicalDevice, &props);
	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
	if (queueFamilyIndex >= familyCount || families[queueFamilyIndex].timestampValidBits == 0 ||
		props.limits.timestampPeriod <= 0.0f) {
		logger::info("[DXVKInterop] Queue family {} has no timestamps; interop GPU timings are off", queueFamilyIndex);
		return;
	}

	VkQueryPoolCreateInfo info{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = kMaxTimedSlots * 2;
	if (vkCreateQueryPool(device, &info, nullptr, &timingPool) != VK_SUCCESS) {
		timingPool = VK_NULL_HANDLE;
		logger::warn("[DXVKInterop] vkCreateQueryPool failed; interop GPU timings are off");
		return;
	}
	timestampPeriodNs = props.limits.timestampPeriod;
}

void DXVKInterop::HarvestSlotTiming(uint32_t a_slot)
{
	if (a_slot >= slotTimingLabels.size() || !slotTimingLabels[a_slot])
		return;
	const char* label = std::exchange(slotTimingLabels[a_slot], nullptr);
	if (timingPool == VK_NULL_HANDLE)
		return;

	uint64_t ticks[2] = {};
	if (vkGetQueryPoolResults(device, timingPool, a_slot * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || ticks[1] < ticks[0])
		return;
	if (a_slot < slotSubmitQpc.size())
		GpuIdleTrace::AddInteropSpan(label, ticks[0], ticks[1], slotSubmitQpc[a_slot]);
	if (label == kTraceOnlyLabel)
		return;
	const double ms = static_cast<double>(ticks[1] - ticks[0]) * timestampPeriodNs * 1e-6;
	if (harvestedTimings.size() < 256)
		harvestedTimings.emplace_back(label, static_cast<float>(ms));
}

void DXVKInterop::PublishCommandTimings()
{
	std::vector<std::pair<const char*, float>> timings;
	{
		std::lock_guard lock(commandRingMutex);
		if (device == VK_NULL_HANDLE || commandRingFaulted)
			return;
		for (uint32_t slot = 0; slot < slotTimingLabels.size(); ++slot) {
			VkResult error = VK_SUCCESS;
			if (slotTimingLabels[slot] && SlotComplete(slot, error))
				HarvestSlotTiming(slot);
		}
		timings.swap(harvestedTimings);
	}
	if (!globals::profiler)
		return;
	for (const auto& [label, ms] : timings)
		globals::profiler->AddExternalSample(label, ms);
}

DXVKInterop::CommandTransaction DXVKInterop::BeginFrameCommandBuffer(const char* a_timingLabel)
{
	std::unique_lock ringLock(commandRingMutex);
	if (commandPool == VK_NULL_HANDLE || commandRingFaulted)
		return {};

	if (streamSubmitFailed.exchange(false, std::memory_order_acq_rel)) {
		commandRingFaulted = true;
		return {};
	}

	constexpr uint32_t kMaxRingDepth = 64;
	uint32_t next = (commandFrameIndex + 1) % framesInFlight;
	VkResult slotError = VK_SUCCESS;
	if (!SlotComplete(next, slotError)) {
		if (slotError != VK_SUCCESS) {
			commandRingFaulted = true;
			logger::error("[DXVKInterop] vkGetSemaphoreCounterValue failed ({})", static_cast<int>(slotError));
			return {};
		}
		uint32_t freeSlot = UINT32_MAX;
		for (uint32_t i = 0; i < framesInFlight; ++i) {
			const uint32_t cand = (next + i) % framesInFlight;
			if (SlotComplete(cand, slotError)) {
				freeSlot = cand;
				break;
			}
			if (slotError != VK_SUCCESS) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] vkGetSemaphoreCounterValue failed ({})", static_cast<int>(slotError));
				return {};
			}
		}
		if (freeSlot != UINT32_MAX) {
			next = freeSlot;
		} else if (framesInFlight < kMaxRingDepth) {
			VkCommandBuffer newCb = VK_NULL_HANDLE;
			VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocInfo.commandPool = commandPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;
			const VulkanResultAttempt allocateAttempt = AllocateCommandBuffersSEH(device, &allocInfo, &newCb);
			if (allocateAttempt.result != VK_SUCCESS) {
				commandRingFaulted = true;
				logger::error("[DXVKInterop] command ring growth failed (allocate={})", static_cast<int>(allocateAttempt.result));
				return {};
			}
			commandBuffers.push_back(newCb);
			slotTimelineValues.push_back(0);
			pendingViewDeletes.emplace_back();
			slotTimingLabels.push_back(nullptr);
			slotSubmitQpc.push_back(0);
			pendingResourceReleases.emplace_back();
			next = framesInFlight;
			++framesInFlight;
			logger::info("[DXVKInterop] Command ring grown to {} (all slots in flight)", framesInFlight);
		} else {
			static bool s_warned = false;
			if (!s_warned) {
				s_warned = true;
				logger::error("[DXVKInterop] command ring reached max depth {} - refusing to block the render thread", kMaxRingDepth);
			}
			return {};
		}
	}
	commandFrameIndex = next;
	VkCommandBuffer cb = commandBuffers[commandFrameIndex];
	// The slot's submission has completed, so its previous timestamps are final; take them before reuse.
	HarvestSlotTiming(commandFrameIndex);

	if (commandFrameIndex < pendingViewDeletes.size()) {
		auto& dead = pendingViewDeletes[commandFrameIndex];
		if (vkDestroyImageView) {
			for (VkImageView& v : dead) {
				if (v == VK_NULL_HANDLE)
					continue;
				DestroyImageView(vkDestroyImageView, device, v);
				v = VK_NULL_HANDLE;
			}
		} else if (std::find_if(dead.begin(), dead.end(),
				       [](VkImageView a_view) { return a_view != VK_NULL_HANDLE; }) != dead.end()) {
			commandRingFaulted = true;
			logger::error("[DXVKInterop] vkDestroyImageView is unavailable; deferred views remain quarantined");
			return {};
		}
		dead.clear();
	}
	if (commandFrameIndex < pendingResourceReleases.size())
		pendingResourceReleases[commandFrameIndex].clear();

	const VulkanResultAttempt resetAttempt = ResetCommandBufferSEH(cb);
	if (resetAttempt.result != VK_SUCCESS) {
		commandRingFaulted = true;
		logger::error("[DXVKInterop] vkResetCommandBuffer failed ({})",
			static_cast<int>(resetAttempt.result));
		return {};
	}

	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	const VulkanResultAttempt beginAttempt = BeginCommandBufferSEH(cb, &beginInfo);
	if (beginAttempt.result != VK_SUCCESS) {
		commandRingFaulted = true;
		logger::error("[DXVKInterop] vkBeginCommandBuffer failed ({})",
			static_cast<int>(beginAttempt.result));
		return {};
	}
	CommandTransaction transaction(this, commandFrameIndex, cb, std::move(ringLock));
	// Whatever D3D11 wrote before this buffer is visible to it (SubmitFrameCommandBuffer).
	FullMemoryBarrier(cb);
	if (!a_timingLabel && GpuIdleTrace::Requested())
		a_timingLabel = kTraceOnlyLabel;
	if (a_timingLabel && timingPool != VK_NULL_HANDLE && commandFrameIndex < kMaxTimedSlots) {
		// ALL_COMMANDS: written once every earlier command on the queue has finished, so the pair
		// measures this buffer's work and not the tail of the D3D11 frame flushed ahead of it.
		vkCmdResetQueryPool(cb, timingPool, commandFrameIndex * 2, 2);
		vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timingPool, commandFrameIndex * 2);
		transaction.timingLabel = a_timingLabel;
	}
	return transaction;
}

bool DXVKInterop::SubmitFrameCommandBuffer(CommandTransaction& a_transaction)
{
	if (a_transaction.owner != this || !a_transaction.ringLock.owns_lock() ||
		a_transaction.submitted || a_transaction.slot >= commandBuffers.size() ||
		a_transaction.commandBuffer == VK_NULL_HANDLE ||
		commandBuffers[a_transaction.slot] != a_transaction.commandBuffer ||
		commandPool == VK_NULL_HANDLE || commandRingFaulted)
		return false;
	const uint32_t slot = a_transaction.slot;
	const VkCommandBuffer commandBuffer = a_transaction.commandBuffer;
	const uint64_t value = lastTimelineValue + 1;

	// What this buffer wrote is visible to the D3D11 work after it.
	FullMemoryBarrier(commandBuffer);
	if (a_transaction.timingLabel)
		vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timingPool, slot * 2 + 1);
	LARGE_INTEGER submitQpc{};
	QueryPerformanceCounter(&submitQpc);

	if (enqueueSubmission && ::GetCurrentThreadId() == streamThread.load(std::memory_order_relaxed)) {
		VkCommandBufferSubmitInfo bufferInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
		bufferInfo.commandBuffer = commandBuffer;
		VkSemaphoreSubmitInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
		signalInfo.semaphore = completionTimeline;
		signalInfo.value = value;
		signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		DxvkOrgInteropSubmission submission{};
		submission.version = DXVK_ORG_INTEROP_VERSION;
		submission.commandBufferCount = 1;
		submission.commandBuffers = &bufferInfo;
		submission.signalCount = 1;
		submission.signals = &signalInfo;
		submission.onSubmitted = &DXVKInterop::OnStreamSubmitted;
		submission.user = this;
		submission.label = a_transaction.timingLabel;
		streamPending.fetch_add(1, std::memory_order_acq_rel);
		const StreamSubmitAttempt attempt = StreamSubmitSEH(enqueueSubmission, submission, commandBuffer);
		if (!attempt.faulted && (attempt.endResult != VK_SUCCESS || FAILED(attempt.enqueueResult))) {
			streamPending.fetch_sub(1, std::memory_order_acq_rel);  // not enqueued: no callback will come
			streamPending.notify_all();
		}
		if (attempt.faulted || attempt.endResult != VK_SUCCESS || FAILED(attempt.enqueueResult)) {
			commandRingFaulted = true;
			if (attempt.faulted)
				logger::error("[DXVKInterop] enqueueing a ring submission faulted (SEH {:#x})", attempt.exceptionCode);
			else if (attempt.endResult != VK_SUCCESS)
				logger::error("[DXVKInterop] vkEndCommandBuffer failed ({})", static_cast<int>(attempt.endResult));
			else
				logger::error("[DXVKInterop] dxvkEnqueueInteropSubmission failed ({:#x})", static_cast<uint32_t>(attempt.enqueueResult));
			// A fault inside DXVK may have left it queued.
			a_transaction.submissionMayBeInFlight = attempt.faulted;
			return false;
		}
	} else {
		const QueueSubmitAttempt attempt = DirectQueueSubmitSEH(interopDevice.get(), queue, commandBuffer, completionTimeline, value, streamPending);
		if (attempt.faulted || attempt.endResult != VK_SUCCESS || attempt.submitResult != VK_SUCCESS) {
			commandRingFaulted = true;
			if (attempt.faulted) {
				logger::error("[DXVKInterop] vkQueueSubmit faulted (SEH {:#x})", attempt.exceptionCode);
				a_transaction.submissionMayBeInFlight = attempt.queueLockAcquired;
				return false;
			} else if (attempt.endResult != VK_SUCCESS) {
				logger::error("[DXVKInterop] vkEndCommandBuffer failed ({})",
					static_cast<int>(attempt.endResult));
			} else {
				logger::error("[DXVKInterop] vkQueueSubmit failed ({})",
					static_cast<int>(attempt.submitResult));
			}
			return false;
		}
	}
	lastTimelineValue = value;
	slotTimelineValues[slot] = value;
	a_transaction.submitted = true;
	if (a_transaction.timingLabel && slot < slotTimingLabels.size()) {
		slotTimingLabels[slot] = a_transaction.timingLabel;
		slotSubmitQpc[slot] = submitQpc.QuadPart;
	}
	return true;
}

void DXVKInterop::QueueViewsForDeferredDelete(const CommandTransaction& a_transaction,
	const VkImageView* a_views, uint32_t a_count)
{
	if (a_transaction.owner != this || !a_transaction.ringLock.owns_lock() ||
		(!a_transaction.submitted && !a_transaction.submissionMayBeInFlight) ||
		!a_views || a_transaction.slot >= pendingViewDeletes.size())
		return;
	auto& slot = pendingViewDeletes[a_transaction.slot];
	for (uint32_t i = 0; i < a_count; ++i)
		if (a_views[i] != VK_NULL_HANDLE)
			slot.push_back(a_views[i]);
}

void DXVKInterop::QueueResourcesForDeferredRelease(const CommandTransaction& a_transaction,
	ID3D11Resource* const* a_resources, uint32_t a_count)
{
	if (a_transaction.owner != this || !a_transaction.ringLock.owns_lock() ||
		(!a_transaction.submitted && !a_transaction.submissionMayBeInFlight) ||
		!a_resources || a_transaction.slot >= pendingResourceReleases.size())
		return;
	auto& slot = pendingResourceReleases[a_transaction.slot];
	for (uint32_t i = 0; i < a_count; ++i) {
		if (a_resources[i] == nullptr)
			continue;
		winrt::com_ptr<ID3D11Resource> resource;
		resource.copy_from(a_resources[i]);
		slot.push_back(std::move(resource));
	}
}
