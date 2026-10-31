#pragma once

#include <optional>
#include <span>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Core/Gpu/ImportRelocations.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"

struct DxvkOrgInteropTrackedResource;

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
	 * not remembered (the object's verdict is, Coordinator's UnstableBuffer).
	 *
	 * Prefetch describes on the preparation pool: the batch is dispatched, and the Acquire that first needs one of its answers runs it
 * itself if no worker has started it, else waits for the worker (DXVK's batched description synchronizes with its worker thread, which
 * may be busy for a while: the wait then overlaps the pass's evaluation instead of stalling the lane). The job touches only its batch.
 *
 * One instance per thread domain, each its maps' only user (no lock): Scene() is the scene work's - the pass's thread (the scene
	 * lane, or the render thread for an inline pass): the walk's prologue, prefetch and merges (ResolveGeometrySource); Frame() is the
	 * render thread's - tree LOD's meshes, at the depth commit (Features/TreeLod's Mirror::TakeChanges). A buffer both lease is resolved by each
	 * under a reference of its own (a description is idempotent; DXVK's interop is callable from any thread). A lease may be released on any thread (its removal queued, lock-free); GetStats may be read on any thread.
	 *
	 * Every description is taken under a reference on the buffer, held until a lease takes it over: a description is only the
	 * buffer's while the buffer lives, and the key is the game's pointer, which a released buffer's successor may reuse.
	 *
	 * Pinless (RenderGraphRuntime::PinlessImports): a description tracks the buffer instead of pinning it, so DXVK may still move it.
	 * Each move (ImportRelocations) is taken at the next BeginFrame: the moved entry's leases keep reading the old storage, which their
	 * owner holds from then on, and the entry goes, so the next Acquire imports the buffer's new storage. TakeRelocated names the moved
	 * buffers, for the leases' holders to acquire again. A prefetched description older than a move this instance took is described
	 * again. The import ends (UntrackResources) with its last lease.
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
			// Under the claims or scene observers (Diagnostics::Observe): prefetched descriptions taken by Acquire, and those whose buffer
			// the game had released by then (Prefetch's reference the last one): without it, a freed buffer's address.
			std::uint64_t prefetchTaken = 0, prefetchReleased = 0;
			// Prefetch batches the owner found unstarted (and described itself) or running (and waited for), and the time it waited.
			std::uint64_t prefetchRunByOwner = 0, prefetchWaited = 0;
			double prefetchWaitMs = 0.0;
			// Pinless: entries a relocation moved, relocations naming no entry of this instance (the other's, or released), and prefetched
			// descriptions a relocation superseded.
			std::uint64_t relocated = 0, relocationsIgnored = 0, prefetchSuperseded = 0;
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
		 * (RenderGraphRuntime::DescribeResources), for the Acquire calls that follow: one per buffer otherwise. Returns at once:
		 * the description runs on the preparation pool (Acquire finishes it). The results are kept through the next BeginFrame (a
		 * prefetch made ahead of a pass's prologue serves that pass); a buffer named twice, or already held, costs nothing.
		 */
		void Prefetch(std::span<ID3D11Buffer* const> a_buffers);

		/**
		 * @brief Before the leases are taken (Scene: a pass's prologue; Frame: a depth commit's TakeChanges): drops the prefetched
		 * descriptions and removes the entries whose last lease went.
		 */
		void BeginFrame();

		/** @brief Hands DXVK's buffer relocations to the instance, which takes them at its next BeginFrame. Any thread. */
		void PostRelocations(std::span<const ImportRelocations::Relocation> a_relocations);

		/**
		 * @brief The buffers whose entries the relocations the last BeginFrame took moved: their leases read the old storage, which
		 * they keep alive; an Acquire imports the new one. The owner's thread, after BeginFrame (the next one drops them).
		 */
		std::vector<ID3D11Buffer*> TakeRelocated() { return std::exchange(relocatedBuffers, {}); }

		/** @brief The counters, as the owner last wrote them. Any thread. */
		Stats GetStats() const;

		/** @brief Releases everything (feature off, device teardown). The owner's thread. */
		void Clear();

	private:
		// A lease's owner: the reference the description was taken under, and (pinless) the import and the old storages its leases
		// may still read. Its last release ends the import, then lets the storages go.
		struct Held
		{
			winrt::com_ptr<ID3D11Buffer> reference;
			bool tracked = false;
			std::mutex mutex;  // storages: the owner's thread appends, the last release clears
			std::vector<std::shared_ptr<const void>> storages;
			~Held();
		};
		struct Entry
		{
			Buffer buffer;
			std::uint64_t generation = 0;
			std::weak_ptr<Held> owner;
			std::uint64_t cookie = 0, serial = 0;  // pinless: the tracked resource, and the storage described
		};
		// The keys whose last lease went, pushed by whichever thread released it (the lease's deleter holds it), drained by BeginFrame.
		using Released = EventQueue<ID3D11Buffer*, 1024>;
		// Stats' fields: written by the owner alone (a load and a store, relaxed), read by any thread.
		struct Counters
		{
			std::atomic<std::uint32_t> cached{ 0 }, rejected{ 0 };
			std::atomic<std::uint64_t> resolvedTotal{ 0 }, prefetchBatches{ 0 }, prefetched{ 0 }, prefetchTaken{ 0 }, prefetchReleased{ 0 };
			std::atomic<std::uint64_t> prefetchRunByOwner{ 0 }, prefetchWaited{ 0 };
			std::atomic<std::uint64_t> relocated{ 0 }, relocationsIgnored{ 0 }, prefetchSuperseded{ 0 };
			std::atomic<double> resolveMsTotal{ 0.0 }, resolveMsMax{ 0.0 }, prefetchWaitMs{ 0.0 };
		};

		/**
		 * @brief A new entry for a resolved buffer, held by a_reference (a reference on a_buffer), and its first lease. a_tracked: the
		 * import's tracked description (pinless), which the lease's owner ends.
		 */
		LeasedBuffer Insert(ID3D11Buffer* a_buffer, winrt::com_ptr<ID3D11Buffer> a_reference, const Buffer& a_resolved,
			const DxvkOrgInteropTrackedResource* a_tracked);
		/** @brief Takes the posted relocations (BeginFrame): each moved entry's owner holds the old storage, and the entry goes. */
		void TakeRelocations();
		/** @brief Whether a relocation this instance took replaced the storage a description of a_cookie at a_serial names. */
		bool Superseded(std::uint64_t a_cookie, std::uint64_t a_serial) const;
		/** @brief One resolution's time, into the totals and the slowest. */
		void NoteResolve(double a_ms, std::uint64_t a_buffers);
		ankerl::unordered_dense::map<ID3D11Buffer*, Entry> entries;
		// One Prefetch's description, shared with its job on the pool (GpuResources.cpp).
		struct Batch;
		/** @brief a_batch described: by the owner if no worker took it, else waited for; then counted once. The owner's thread. */
		void Finish(Batch& a_batch);
		// Prefetch's batches, by buffer, until the BeginFrame after next: the current pass's and the one before (prefetchedBefore).
		struct Prefetched
		{
			std::shared_ptr<Batch> batch;
			std::uint32_t index = 0;
		};
		ankerl::unordered_dense::map<ID3D11Buffer*, Prefetched> prefetched, prefetchedBefore;
		std::shared_ptr<Released> released = std::make_shared<Released>();
		// Pinless: the tracked entries by resource, the newest storage serial of each relocation taken (kept a few passes, for the
		// prefetched descriptions still to be taken), and the posted relocations.
		ankerl::unordered_dense::map<std::uint64_t, ID3D11Buffer*> byCookie;
		struct Moved
		{
			std::uint64_t serial = 0, pass = 0;
		};
		ankerl::unordered_dense::map<std::uint64_t, Moved> moved;
		std::uint64_t passes = 0;
		std::vector<ID3D11Buffer*> relocatedBuffers;
		std::mutex inboxMutex;
		std::vector<ImportRelocations::Relocation> inbox;
		Counters counters;
	};
}
