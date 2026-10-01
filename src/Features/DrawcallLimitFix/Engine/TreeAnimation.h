#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::TreeAnimation
{
	/**
	 * @brief The tree manager's animation list without DCLF's trees (drawcall-limit-fix.md, "Owned trees off the tree
	 * manager's animation list"). AE only.
	 *
	 * BSTreeManager (0x1420F6A18) keeps every BSLeafAnimNode the cells load on a list (+0x50, count +0x60), which its
	 * update in Main::Update (FUN_1404381e0) walks every frame: for a node whose kAccumulated bit is set, the wind clock
	 * (+0x164), the camera distance (+0x158) and the gust (+0x15C). A root DCLF stands in has kAccumulated cleared and its
	 * wind on the GPU (TreeWindCS), so the walk only tests it. Those roots are taken off the list while owned and put back
	 * when not, under the manager's own lock (+0x18), and the engine's own adds (FUN_1404376f0), removes (FUN_140437840)
	 * and clear (FUN_140437ae0) are detoured to keep that state theirs: a node the engine removes while it is off is
	 * forgotten, one it adds is taken off again, and a clear drops them all.
	 */
	void Install();
	bool Installed();
	/** @brief The roots the list should not hold (PrimaryCull's owned roots without engine-drawn parts), as a whole set. */
	void SetOwned(const std::vector<const RE::NiAVObject*>& a_roots);
	/** @brief Parity: owned roots still on the list (a missed add). Render thread, parity frames only. */
	void CheckParity();
	/** @brief One report line, counters reset. */
	std::string Report();
}
