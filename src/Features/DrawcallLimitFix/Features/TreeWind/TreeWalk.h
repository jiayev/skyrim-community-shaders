#pragma once

// The tree manager's animation walk as a move writer (tree wind's engine side). Included by the feature's sources.

namespace DCLF::Api
{
	class InstallContext;
}

namespace DCLF::Features::TreeWind::TreeWalk
{
	/**
	 * @brief The tree manager's walk: a walked tree's update pass, followed by a move event (the report's "tree animation" writer).
	 *
	 * SE's BSTreeManager walks its animation list (FUN_1403df7d0, from the manager's update FUN_1403ddc30, Main::Update) and, after each
	 * listed tree's wind (the trees with kAccumulated set that DCLF does not own: TreeAnimation.h), runs the tree's
	 * UpdateSelectedDownwardPass (`call [rax + 0x168]` at 0x1403df8de). That pass sets the tree's multibound node's bound to its
	 * children's union, where the full update after the reference's 3D was attached (FUN_140294000, from FUN_140157030) left its
	 * multibound shape's sphere, about 15% larger. The root's bound, its sun entry, changes with no other event, and a placement row
	 * sampled between the two keeps the larger sphere (dclf-open-defects.md, "SE 1.5.97: sun entry on the GPU"). The call is replaced by
	 * the thunk's, which makes it and then pushes the tree's reference as a move event: the root is listed as a still root
	 * (Coordinator::QueueRoots) and FrameValues samples its entry again.
	 *
	 * AE (1.6.1170 and 1.7.104) makes the same call in the manager's update instead (FUN_1404381e0's near list loop, `call [rax + 0x168]`
	 * at 0x1404384f5, right after the bones' wind FUN_140438ed0), on each near tree with bones: its root's bound shrinks the same way,
	 * and its bones' palette and bound change with it. The moved-only parity named these trees on 1.7.104 (2026-10-23: TreePineForest*,
	 * not due, palettes and sun entries differing). The same thunk replaces it.
	 */
	void Install(Api::InstallContext& a_context);
	bool Installed();
}
