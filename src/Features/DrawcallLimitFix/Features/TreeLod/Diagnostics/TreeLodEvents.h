#pragma once

// Tree LOD's hook points: its mirror's (Mirror, render thread), for its parity's observers (Diagnostics/TreeLodParity.cpp), and its depth
// commit's hole (the commit hook, render thread), for its log and report count (Diagnostics/TreeLodReport.cpp).

#include <cstdint>

namespace DCLF::Features::TreeLod
{
	class Mirror;

	/** @brief Render thread, Mirror::Drain: an event of `shape` applied. */
	struct TreeLodEventDrained
	{
		const void* shape;
	};

	/** @brief Render thread: the mirror's events drained on frame `frame`. */
	struct TreeLodDrained
	{
		const Mirror& mirror;
		std::uint32_t frame;
	};

	/** @brief The depth commit could not draw tree LOD on a frame that withheld the engine's passes (a hole): the tables' shape slots and the buffer's. */
	struct TreeLodMissed
	{
		std::uint32_t frame;
		std::uint64_t shapeSlots, bufferSlots;
	};
}
