#pragma once

#include <cstddef>
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
	 *
	 * The tree list (+0x20, BSTreeNodes; T7b5's fix, dclf-open-defects.md): its loop (FUN_140437e50) visits a tree only when its first
	 * child's multibound has the cull result 1, and a stood-in tree's multibound is reached by no cull, so the loop read a stale result
	 * (a stale "in" animated its bones forever, a stale "out" froze them and its LOD switch). The stood-in BSTreeNodes stay on the list -
	 * the loop's stores are what DCLF follows for them (the LOD switch by event, the near list's bones that DCLF's skinned draws read) -
	 * but around the loop's call (TreeList's detour of 0x140437df1, Main::Update, main thread, the manager's update: no list cull or
	 * cell animation runs) each one's multibound result is DCLF's verdict, the root's kAccumulated as T7b2 wrote it at Main::Update's
	 * entry (IndirectDraws::AccumulatedVerdict: 1 in view, 2 out), and the engine's value is put back right after: the loop decides
	 * from DCLF's visibility, and no other reader of the cache sees DCLF's value. Written under the tree list's lock (+0x10), each
	 * written tree held by a reference until the value is put back (released on the main thread).
	 */
	void Install();
	bool Installed();
	/**
	 * @brief The roots the animation list should not hold (PrimaryCull's owned roots without engine-drawn parts), as a whole set, and
	 * the BSTreeNodes among them (a_trees: the tree list's, whose multibound result DCLF gives the loop). Render (main) thread.
	 */
	void SetOwned(const std::vector<const RE::NiAVObject*>& a_roots, const std::vector<const RE::NiAVObject*>& a_trees);
	/** @brief The main thread at Main::Update's entry (PrimaryCull::AtUpdateEntry): whether DCLF runs this frame (off: no tree is written). */
	void AtUpdateEntry(bool a_running);
	/**
	 * @brief The main thread around the tree list's loop (TreeList's detour; nothing installed off AE): before, the stood-in trees'
	 * multibound results written from DCLF's verdicts; after (and after CS_DCLF_TREE_PARITY's sample), the engine's put back.
	 * T7f preparation: CS_DCLF_TREE_LIST_ALL writes every BSTreeNode on the list that DCLF lists, from IndirectDraws::AccumulatedVerdictOf
	 * (the wide writes' verdicts); without it CS_DCLF_TREE_PARITY compares those verdicts with the engine's results (a dry run).
	 */
	void BeforeListLoop(std::byte* a_manager);
	void AfterListLoop();
	/** @brief Whether a_node is a stood-in tree whose multibound result DCLF wrote for this loop (CS_DCLF_TREE_PARITY's sample, between the two). */
	bool WroteResult(const void* a_node);
	/** @brief Parity: owned roots still on the list (a missed add). Render thread, parity frames only. */
	void CheckParity();
	/** @brief One report line, counters reset. */
	std::string Report();
}
