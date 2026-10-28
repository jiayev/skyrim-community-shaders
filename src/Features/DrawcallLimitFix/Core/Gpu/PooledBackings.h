#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace org
{
	class Buffer;
}

namespace org::services
{
	struct ShrinkPolicy;
	struct BackingAllocation;
	struct BackingPoolHooks;
	struct BackingRequest;
}

namespace DCLF
{
	/**
	 * @brief DCLF's ties for an org::services::BackingPool (D1/D3): how a backing is made and where a released one is destroyed,
	 * shared by every DCLF pool (FrameValues' buffers, the payload's parts, the scene set's growth).
	 */
	namespace PooledBackings
	{
		/** @brief The device address of a_buffer (0 without a graph host). */
		std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer);

		/**
		 * @brief BackingPoolHooks::allocate: a materialized buffer for a_request, without an ECS entity (an acquirer off the render
		 * thread may not touch the host's world). With a_request.like: a buffer of its kind (UnmaterializedLike), resized to the
		 * class; else a new structured buffer of the class's elements (stride), or a raw one of the class's bytes (stride 0), with
		 * flags.unorderedAccess. Named a_request.name; reports its SRV slot and, for flags.deviceAddress, its device address. Throws
		 * when it cannot be made.
		 */
		org::services::BackingAllocation Allocate(const org::services::BackingRequest& a_request);

		/**
		 * @brief BackingPoolHooks::retire: a buffer the pool released (already retired on the GPU) is destroyed on the graph host's
		 * resource cleanup lane (PersistentGraphHost::ResourceCleanup), never on the thread that released it (the render thread
		 * acquires FrameValues' pools). Lock-free. Without a graph host (process exit) the reference drops here.
		 */
		void Retire(std::shared_ptr<org::Buffer> a_buffer);

		/** @brief Hooks with Allocate and Retire set (the caller may add tag/telemetry). */
		org::services::BackingPoolHooks Hooks();

		/**
		 * @brief BackingPool::Config::spareFree for a pool that publishes about once a frame (the payload's parts, FrameValues' buffers):
		 * a free backing per game frame in flight but the newest stays warm. Retired by exact GPU completion (FrameRetirement), the
		 * frames in flight hand their backings back in turn; with fewer spares an Acquire that finds them reusable at once releases the
		 * surplus, and a frame later the pool makes it again and writes it whole.
		 */
		std::uint32_t PerFrameSpares();

		/**
		 * @brief The copies a per-frame pool keeps of a class (CS_DCLF_SCENE_COPIES; default 3, clamped to 2 through the game frames in
		 * flight): its BackingPool::Config::maxPerClass with reuseBound, so a frame's producer reuses the copy the oldest in-flight frame
		 * bound and its uploads wait on the GPU for that frame (FrameValues).
		 */
		std::uint32_t SceneCopies();

		/**
		 * @brief The one shrink policy of every growable scene buffer (CS_DCLF_SHRINK): the scene set's registrations, the payload's parts,
		 * the CPU pipeline's sequences, FrameValues' pools and the layouts' capacities. Default: a buffer asked at most a quarter of its
		 * capacity for 5 s (and 2 asks) shrinks to its growth policy's fresh size for the window's peak; off: grow-only; eager: at once.
		 */
		const org::services::ShrinkPolicy& Shrink();
	}
}
