#pragma once

#include <d3d11.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <winrt/base.h>

struct DxvkOrgInteropResourceInfo;

namespace org::services
{
	class ShaderCompiler;
}

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
 * objects (WrapBuffer). Game resources the graph reads directly (Drawcall Limit Fix's
 * static geometry and textures) are described through DescribeResource, which marks
 * them stable in DXVK; nothing else the game or DXVK owns is pinned or relocated.
 *
 * Header deliberately free of ORG/BasicRHI/Vulkan includes: those require volk
 * ahead of any Vulkan header, which only the implementation files arrange.
 */
class RenderGraphRuntime
{
public:
	/**
	 * @brief Where in the frame an epoch runs. The graph holds every feature's passes and each epoch
	 * executes all of them (an empty pass is effectively free), so passes record work only in their own
	 * segment (CurrentSegment) and include it in their invocation revision.
	 */
	enum class Segment : std::uint32_t
	{
		LightCulling,  // Light Limit Fix, before the main pass
		ZPrepass,      // Drawcall Limit Fix's depth, at the first draw of the main (deferred) pass
		MainOpaque,    // Drawcall Limit Fix's colour, before the deferred composite
		DebugView,     // Drawcall Limit Fix debug view, before the deferred composite
		ShadowView,    // Drawcall Limit Fix's shadow casters, inside one shadow view's draw
		SkyOcclusion,  // Drawcall Limit Fix's variant of Skylighting's occlusion map, after the engine's RenderMask
	};

	static RenderGraphRuntime& Get();

	/**
	 * @brief CS_ORG_EPOCHS (default on, =0 off): each segment is an ORG host epoch. The graph compiles once
	 * over the whole frame - scheduling and transient aliasing see every segment's lifetimes, in frame order -
	 * and each ExecuteEpoch prepares, admits, records and submits only its own segment's passes. Off, every
	 * epoch runs every pass of every feature and the passes no-op outside their segment, as before.
	 */
	static bool EpochsEnabled();

	/** @brief The ORG epoch a segment's passes declare (ExternalPassDesc::Epoch); every epoch when EpochsEnabled is off. */
	static std::uint32_t EpochOf(Segment a_segment);

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
	bool ExecuteEpoch(Segment a_segment, const std::function<void(org::RenderGraph&)>& a_beforePrepare = {});

	/** @brief The segment of the epoch being executed (valid while passes prepare and record). */
	Segment CurrentSegment() const { return segment; }

	/**
	 * @brief CS_ORG_EPOCH_STATS: a feature's whole call around one epoch (its inputs, joins and bookkeeping
	 * before and after ExecuteEpoch), on the render thread. Counted only when an epoch ran inside it.
	 */
	class EpochBodyScope
	{
	public:
		explicit EpochBodyScope(Segment a_segment);
		~EpochBodyScope();
		EpochBodyScope(const EpochBodyScope&) = delete;
		EpochBodyScope& operator=(const EpochBodyScope&) = delete;

	private:
		Segment segment;
		std::chrono::steady_clock::time_point start;
	};

	/** @brief Time the render thread spent waiting on a worker inside the current EpochBodyScope. */
	static void AddEpochJoinWait(std::chrono::steady_clock::duration a_waited);

	/**
	 * @brief The Vulkan resource behind a D3D11 buffer, texture or SRV (dxvkGetInteropResourceInfo).
	 * Marks it stable in DXVK; keep a reference on the D3D11 object while using the result. Fails for
	 * buffers the game can map. Synchronizes with DXVK's worker thread: resolve once and cache.
	 */
	bool DescribeResource(IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info);

	/**
	 * @brief Runtime DXC compilation to SPIR-V with BasicRHI's ABI (ORGModuleServices), content-addressed
	 * with a disk cache under Data/ShaderCache/ORG. Null when inactive, or when the build or the
	 * installation has no DXC (dxcompiler.dll beside the DXVK DLLs).
	 */
	org::services::ShaderCompiler* ShaderCompiler();

	/** @brief The directory the runtime builds compile from (Data/Shaders), and every .hlsl/.hlsli in it, listed once. */
	static constexpr const char* kShaderDirectory = "Data/Shaders";
	static const std::vector<std::filesystem::path>& ShaderSourceFiles();

	/** @brief Wraps a materialized graph-owned buffer as a D3D11 buffer (DEFAULT usage, no CPU access). */
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(org::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc);

	/**
	 * @brief Totals (and with a_log, logs) the GPU time of every segment and of the passes inside it, from ORG's own pass timestamps,
	 * averaged over `a_frames` frames, and starts a new window. Render thread.
	 *
	 * The measurement to hold a capture tool's attribution against: timestamps bracket each pass as the
	 * graph records it, each epoch's are read back separately once its slot's GPU work is known complete,
	 * and a pass's exclusive time is taken along the queue (from the previous pass's end, not its own begin,
	 * which can run ahead of it), so neither overlapping passes nor epochs sharing a pass double-count.
	 */
	void ReportGpuTimings(std::uint32_t a_frames, bool a_log);

	/** @brief One line per segment from the last ReportGpuTimings, for the menu; empty before the first. */
	const std::string& GpuTimingSummary() const { return gpuTimingSummary; }

	~RenderGraphRuntime();

private:
	RenderGraphRuntime() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
	std::string disabledReason = "not initialized";
	std::string gpuTimingSummary;
	Segment segment = Segment::LightCulling;
	bool attempted = false;
};
