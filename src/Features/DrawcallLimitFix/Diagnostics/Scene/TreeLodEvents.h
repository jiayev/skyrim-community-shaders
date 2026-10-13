#pragma once

namespace DCLF::TreeLod
{
	class Mirror;
}

namespace DCLF::Diagnostics
{
	// Tree LOD's mirror (TreeLod::Mirror, render thread), for its parity's observers.

	/** @brief Render thread, TreeLod::Mirror::Drain: an event of `shape` applied. */
	struct TreeLodEventDrained
	{
		const void* shape;
	};

	/** @brief Render thread: the mirror's events drained on frame `frame`. */
	struct TreeLodDrained
	{
		const TreeLod::Mirror& mirror;
		std::uint32_t frame;
	};
}
