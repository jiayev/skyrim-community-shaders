#pragma once

// Tree LOD's mirror's hook points (Mirror, render thread), for its parity's observers (Diagnostics/TreeLodParity.cpp).

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
}
