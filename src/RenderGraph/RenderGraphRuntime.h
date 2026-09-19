#pragma once

#include <d3d11.h>
#include <functional>
#include <memory>
#include <string>
#include <winrt/base.h>

namespace org
{
	class PersistentGraphHost;
	class RenderGraph;
	class Resource;
}

/**
 * @brief OpenRenderGraph running on DXVK's own Vulkan device.
 *
 * BasicRHI adopts the VkDevice and graphics queue DXVK created for the game's D3D11
 * device, so graph passes record native Vulkan work that shares memory and the
 * queue with the translated D3D11 stream. Synchronization is coarse: each epoch
 * flushes DXVK (everything recorded so far is submitted ahead of the graph), then
 * the graph submits on the same queue under DXVK's submission lock, with full
 * memory barriers at its entry and exit. Later D3D11 work is therefore ordered
 * after the graph without semaphores.
 *
 * Graph-owned outputs are handed to D3D11 as wrappers over the graph's own Vulkan
 * objects (WrapBuffer); nothing the game or DXVK owns is pinned or relocated.
 *
 * Header deliberately free of ORG/BasicRHI/Vulkan includes: those require volk
 * ahead of any Vulkan header, which only the implementation files arrange.
 */
class RenderGraphRuntime
{
public:
	static RenderGraphRuntime& Get();

	/** @brief Asks DXVK to enable the device features BasicRHI needs. Call before the D3D11 device exists. */
	static void RequestDeviceFeatures(HMODULE a_dxvkD3D11);

	/** @brief Adopts DXVK's Vulkan device. Safe to call repeatedly; returns whether the runtime is active. */
	bool Initialize();

	/** @brief Retires all graph work and releases the adopted device (never destroys it). */
	void Shutdown();

	/** @brief Whether graph epochs can run. */
	bool IsActive() const;

	/** @brief Why the runtime is inactive (empty when active). */
	const std::string& GetDisabledReason() const;

	/** @brief The persistent graph host; null when inactive. */
	org::PersistentGraphHost* Host();

	/**
	 * @brief Runs one graph epoch at the current point of the D3D11 stream.
	 * @param a_beforePrepare Called with the graph's services active, before its passes
	 *        prepare: queue this frame's uploads (BUFFER_UPLOAD) here.
	 * @return false when inactive or the graph failed; callers fall back to their D3D11 path.
	 */
	bool ExecuteEpoch(const std::function<void(org::RenderGraph&)>& a_beforePrepare = {});

	/** @brief Wraps a materialized graph-owned buffer as a D3D11 buffer (DEFAULT usage, no CPU access). */
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(org::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc);

	~RenderGraphRuntime();

private:
	RenderGraphRuntime() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string disabledReason = "not initialized";
	bool attempted = false;
};
