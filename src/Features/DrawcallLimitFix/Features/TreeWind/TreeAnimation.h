#pragma once

// The engine's tree manager stood down for the trees DCLF stands in for (tree wind's engine side). Included by the feature's sources.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Features::TreeWind::TreeAnimation
{
	/**
	 * @brief The tree manager's animation list without DCLF's trees (dclf-architecture.md, "Owned trees off the tree
	 * manager's animation list").
	 *
	 * BSTreeManager (0x1420F6A18) keeps every BSLeafAnimNode the cells load on a list (+0x50, count +0x60), which its
	 * update in Main::Update (FUN_1404381e0) walks every frame: for a node whose kAccumulated bit is set, the wind clock
	 * (+0x164), the camera distance (+0x158) and the gust (+0x15C). A root DCLF stands in has its wind on the GPU (TreeWindCS),
	 * and its kAccumulated is DCLF's verdict, so the walk must not advance it. Those roots are taken off the list while owned and put back
	 * when not, under the manager's own lock (+0x18), and the engine's own adds (FUN_1404376f0), removes (FUN_140437840)
	 * and clear (FUN_140437ae0) are detoured to keep that state theirs: a node the engine removes while it is off is
	 * forgotten, one it adds is taken off again, and a clear drops them all.
	 *
	 * The tree list (+0x20, BSTreeNodes): its loop (FUN_140437e50) visits a tree only when its first child's multibound has the cull
	 * result 1, and a stood-in tree's multibound is reached by no cull, so the loop would read a stale result (a stale "in" animates
	 * its bones forever, a stale "out" freezes them and its LOD switch). The stood-in BSTreeNodes stay on the list -
	 * the loop's stores are what DCLF follows for them (the LOD switch by event, the near list's bones that DCLF's skinned draws read) -
	 * but around the loop's call (TreeList's detour of 0x140437df1, Main::Update, main thread, the manager's update: no list cull or
	 * cell animation runs) each one's multibound result is DCLF's verdict, the root's kAccumulated as DCLF wrote it at Main::Update's
	 * entry (Extensions::StandInVerdict: 1 in view, 2 out), and the engine's value is put back right after: the loop decides
	 * from DCLF's visibility, and no other reader of the cache sees DCLF's value. Written under the tree list's lock (+0x10), each
	 * written tree held by a reference until the value is put back (released on the main thread).
	 *
	 * Installed by the feature's install (RegisterAnimation), with the primary cull's stand-in it follows (Api::StandInObserver), which
	 * tells it the owned roots (SetOwned, UpdateOwned) and Main::Update's entry (AtUpdateEntry).
	 */
	void Install();
	bool Installed();
	/**
	 * @brief The roots the animation list should not hold (PrimaryCull's owned roots without engine-drawn parts), as a whole set, and
	 * the BSTreeNodes among them (a_trees: the tree list's, whose multibound result DCLF gives the loop). Render (main) thread. The
	 * stand-in's clear gives the empty set.
	 */
	void SetOwned(const std::vector<const RE::NiAVObject*>& a_roots, const std::vector<const RE::NiAVObject*>& a_trees);
	/**
	 * @brief SetOwned by delta: the roots that left and joined the owned set (the BSTreeNodes among them the tree list's). Left first, then
	 * joined (a root in both stays owned). Render (main) thread; the animation list is scanned only when a root joined.
	 */
	void UpdateOwned(const std::vector<const RE::NiAVObject*>& a_left, const std::vector<const RE::NiAVObject*>& a_joined);
	/** @brief The main thread at Main::Update's entry (PrimaryCull::AtUpdateEntry): whether DCLF runs this frame (off: no tree is written). */
	void AtUpdateEntry(bool a_running);
	/**
	 * @brief The main thread around the tree list's loop (TreeList's detour; nothing installed off AE): before, the stood-in trees'
	 * multibound results written from DCLF's verdicts; after, the engine's put back. CS_DCLF_TREE_LIST_ALL writes every
	 * BSTreeNode on the list that DCLF lists, from Extensions::ListedVerdictOf (the wide writes' verdicts).
	 */
	void BeforeListLoop(std::byte* a_manager);
	void AfterListLoop();
	/**
	 * @brief The owned roots still on the animation list (a missed add), counted under the manager's lock after the pending adds are
	 * drained; false (none counted) when not installed or without the manager. Render thread (the report, under CS_DCLF_PERSISTENT_PARITY).
	 */
	bool OwnedOnList(std::uint64_t& a_count);

	/** @brief The counters since the last Take, reset (the feature's tree animation line, at Api::ReportPoint::PrimaryCull). */
	struct Counters
	{
		bool installed = false;
		std::uint32_t listed = 0, held = 0;  // on the list now, owned trees off it
		float updateMs = 0.0f;               // the manager's own measure of its last update
		std::size_t ownedTrees = 0;
		std::uint64_t engineAdds = 0, engineRemoves = 0, clears = 0;
		std::uint64_t syncs = 0, takenOff = 0, putBack = 0, takenOffAgain = 0;
		std::uint64_t treeLoops = 0, treesIn = 0, treesOut = 0, noMultiBound = 0;
		std::array<std::uint64_t, 3> replaced{};
		std::uint64_t wideIn = 0, wideOut = 0, wideNoVerdict = 0, wideNoMultiBound = 0;
		std::array<std::uint64_t, 3> wideReplaced{};
		std::uint64_t drySame = 0, dryInOut = 0, dryOutIn = 0, dryStale = 0;
	};
	Counters Take();
}
