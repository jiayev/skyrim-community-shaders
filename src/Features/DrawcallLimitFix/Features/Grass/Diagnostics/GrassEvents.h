#pragma once

// Grass's hook points: its sink's (any thread) and its mirror's (Mirror, render thread), for its parity's observers
// (Diagnostics/GrassParity.cpp), and its depth commit's hole (the commit hook, render thread), for its log and report count
// (Diagnostics/GrassReport.cpp).

#include <cstdint>

namespace DCLF::Features::Grass
{
	class Mirror;

	/** @brief Any thread, the capture's sink: `shape`'s records (count of them), removal or rejection pushed, with its seq. */
	struct GrassCaptured
	{
		enum class Kind : std::uint8_t
		{
			Add,
			Remove,
			Rejected
		};
		const void* shape;
		std::uint32_t count;
		std::uint64_t seq;
		Kind kind;
	};

	/** @brief Render thread, Mirror::Drain: an event of `shape` applied (or dropped as stale). */
	struct GrassEventDrained
	{
		const void* shape;
	};

	/** @brief Render thread: the mirror's events drained on frame `frame`. */
	struct GrassDrained
	{
		const Mirror& mirror;
		std::uint32_t frame;
	};

	/**
	 * @brief The depth commit could not draw grass on a frame that withheld the engine's passes (a hole): the mirror's slots and the
	 * tables' (instances, buckets).
	 */
	struct GrassMissed
	{
		std::uint32_t frame;
		std::uint64_t instanceSlots, bufferInstances, bucketSlots, bufferBuckets;
	};
}
