#pragma once

#include <optional>
#include <span>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <winrt/base.h>

namespace DCLF
{
	/**
	 * @brief The game's D3D11 buffers as the render graph sees them.
	 *
	 * Resolves a buffer once through RenderGraphRuntime::DescribeResource, which marks it stable in DXVK
	 * (never relocated or renamed from then on), and hands out a lease that holds a reference on it, so the
	 * Vulkan handle and device address stay valid. An entry lives exactly as long as a lease on it does (the
	 * geometry slots that draw the buffer, and the publications they are in): the registry holds it weakly, and
	 * the last lease's release queues it for removal. Buffers the game can map cannot be made stable; they are
	 * not remembered (the object's verdict is, SceneStore's UnstableBuffer).
	 *
	 * Render thread only, except that a lease may be released on any thread.
	 */
	class GpuResources
	{
	public:
		struct Buffer
		{
			std::uint64_t vkBuffer = 0;  // VkBuffer
			std::uint64_t offset = 0;    // of the D3D11 buffer within vkBuffer
			std::uint64_t size = 0;
			std::uint64_t address = 0;   // device address of the first byte
		};
		struct LeasedBuffer
		{
			Buffer buffer;
			std::uint64_t generation = 0;  // unique on successful import, including pointer reuse
			std::shared_ptr<const void> owner;  // pins the D3D11 buffer and keeps the entry
		};

		struct Stats
		{
			std::uint32_t cached = 0;
			std::uint32_t rejected = 0;
			std::uint32_t resolvedThisFrame = 0;
			double resolveMs = 0.0;  // this frame
			std::uint64_t resolvedTotal = 0;
			std::uint64_t prefetchBatches = 0, prefetched = 0;  // batched resolutions, and the buffers they resolved
			double resolveMsTotal = 0.0;
			double resolveMsMax = 0.0;  // slowest single resolution
		};

		static GpuResources& Get();

		/** @brief Whether resolution is possible (the render graph runs on DXVK's device). */
		bool Enabled() const;

		/**
		 * @brief A lease on the buffer's Vulkan view, resolving it when no lease on it is held; nullopt if it cannot be
		 * made stable. The buffer stays resolved while the caller holds the lease.
		 */
		std::optional<LeasedBuffer> Acquire(ID3D11Buffer* a_buffer);
		/**
		 * @brief Resolves the buffers no lease holds, all in one synchronization with DXVK's worker thread
		 * (RenderGraphRuntime::DescribeResources), for the Acquire calls that follow this frame: one per buffer otherwise. The
		 * results are kept until BeginFrame; a buffer named twice, or already held, costs nothing.
		 */
		void Prefetch(std::span<ID3D11Buffer* const> a_buffers);

		/** @brief Once per frame, before the tables are built: removes the entries whose last lease went. */
		void BeginFrame();

		const Stats& GetStats() const { return stats; }

		/** @brief Releases everything (feature off, device teardown). */
		void Clear();

	private:
		struct Entry
		{
			Buffer buffer;
			std::uint64_t generation = 0;
			std::weak_ptr<const void> owner;
		};
		// The keys whose last lease went, from whichever thread released it.
		struct Released
		{
			std::mutex mutex;
			std::vector<ID3D11Buffer*> keys;
		};

		/** @brief A new entry for a resolved buffer, and its first lease. */
		LeasedBuffer Insert(ID3D11Buffer* a_buffer, const Buffer& a_resolved);
		ankerl::unordered_dense::map<ID3D11Buffer*, Entry> entries;
		// Prefetch's results until BeginFrame: the buffer as resolved, or nullopt when it cannot be made stable.
		ankerl::unordered_dense::map<ID3D11Buffer*, std::optional<Buffer>> prefetched;
		std::shared_ptr<Released> released = std::make_shared<Released>();
		std::uint64_t nextGeneration = 1;
		Stats stats;
	};
}
