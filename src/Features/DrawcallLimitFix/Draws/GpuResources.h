#pragma once

#include <optional>
#include <span>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Common/EventQueue.h"

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
	 * One instance per thread domain, each its maps' only user (no lock): Scene() is the scene work's - the pass's thread (the scene
	 * lane, or the render thread for an inline pass): the walk's prologue, prefetch and merges (ResolveGeometrySource), the slot check
	 * and probe; Frame() is the render thread's - tree LOD's meshes, at the depth commit (TreeLod::Mirror::TakeChanges). A buffer both
	 * lease is resolved by each under a reference of its own (a description is idempotent; DXVK's interop is callable from any
	 * thread). A lease may be released on any thread (its removal queued, lock-free); GetStats may be read on any thread.
	 *
	 * Every description is taken under a reference on the buffer, held until a lease takes it over: a description is only the
	 * buffer's while the buffer lives, and the key is the game's pointer, which a released buffer's successor may reuse.
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

		/** @brief Since the start (an instance's). */
		struct Stats
		{
			std::uint32_t cached = 0;
			std::uint32_t rejected = 0;
			std::uint64_t resolvedTotal = 0;
			std::uint64_t prefetchBatches = 0, prefetched = 0;  // batched resolutions, and the buffers they resolved
			double resolveMsTotal = 0.0;
			double resolveMsMax = 0.0;  // slowest single resolution
			// CS_DCLF_SET_PARITY / CS_DCLF_PERSISTENT_PARITY: prefetched descriptions taken by Acquire, and those whose buffer the game
			// had released by then (Prefetch's reference the last one): without it, a freed buffer's address.
			std::uint64_t prefetchTaken = 0, prefetchReleased = 0;
		};

		/** @brief The scene work's: the geometry slots' buffers, on the pass's thread. */
		static GpuResources& Scene();
		/** @brief The render thread's: tree LOD's meshes. */
		static GpuResources& Frame();

		/** @brief Whether resolution is possible (the render graph runs on DXVK's device). Any thread. */
		bool Enabled() const;

		/**
		 * @brief A lease on the buffer's Vulkan view, resolving it when no lease on it is held; nullopt if it cannot be
		 * made stable. The buffer stays resolved while the caller holds the lease.
		 */
		std::optional<LeasedBuffer> Acquire(ID3D11Buffer* a_buffer);
		/**
		 * @brief Resolves the buffers no lease holds, all in one synchronization with DXVK's worker thread
		 * (RenderGraphRuntime::DescribeResources), for the Acquire calls that follow: one per buffer otherwise. The
		 * results are kept until BeginFrame; a buffer named twice, or already held, costs nothing.
		 */
		void Prefetch(std::span<ID3D11Buffer* const> a_buffers);

		/**
		 * @brief Before the leases are taken (Scene: a pass's prologue; Frame: a depth commit's TakeChanges): drops the prefetched
		 * descriptions and removes the entries whose last lease went.
		 */
		void BeginFrame();

		/** @brief The counters, as the owner last wrote them. Any thread. */
		Stats GetStats() const;

		/** @brief Releases everything (feature off, device teardown). The owner's thread. */
		void Clear();

	private:
		struct Entry
		{
			Buffer buffer;
			std::uint64_t generation = 0;
			std::weak_ptr<const void> owner;
		};
		// The keys whose last lease went, pushed by whichever thread released it (the lease's deleter holds it), drained by BeginFrame.
		using Released = EventQueue<ID3D11Buffer*, 1024>;
		// Stats' fields: written by the owner alone (a load and a store, relaxed), read by any thread.
		struct Counters
		{
			std::atomic<std::uint32_t> cached{ 0 }, rejected{ 0 };
			std::atomic<std::uint64_t> resolvedTotal{ 0 }, prefetchBatches{ 0 }, prefetched{ 0 }, prefetchTaken{ 0 }, prefetchReleased{ 0 };
			std::atomic<double> resolveMsTotal{ 0.0 }, resolveMsMax{ 0.0 };
		};

		/** @brief A new entry for a resolved buffer, held by a_reference (a reference on a_buffer), and its first lease. */
		LeasedBuffer Insert(ID3D11Buffer* a_buffer, winrt::com_ptr<ID3D11Buffer> a_reference, const Buffer& a_resolved);
		/** @brief One resolution's time, into the totals and the slowest. */
		void NoteResolve(double a_ms, std::uint64_t a_buffers);
		ankerl::unordered_dense::map<ID3D11Buffer*, Entry> entries;
		// Prefetch's results until BeginFrame: the buffer as resolved (nullopt when it cannot be made stable), under the reference
		// Prefetch took before describing it.
		struct Prefetched
		{
			winrt::com_ptr<ID3D11Buffer> reference;
			std::optional<Buffer> buffer;
		};
		ankerl::unordered_dense::map<ID3D11Buffer*, Prefetched> prefetched;
		std::shared_ptr<Released> released = std::make_shared<Released>();
		Counters counters;
	};
}
