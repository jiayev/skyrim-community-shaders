#pragma once

namespace DCLF::TreeList
{
	/**
	 * @brief T7b5: the engine's tree manager list loop (drawcall-limit-fix.md, "T7b5"; skyrim-engine-notes.md, "Tree manager list
	 * (T7b5)"). AE only.
	 *
	 * BSTreeManager's update (FUN_140437d40, Main::Update 0x140646176, main thread) runs FUN_140437e50 over its tree list (+0x20:
	 * BSTreeNodes, exact RTTI): a tree is visited only when its first child's multibound has the cull result 1 (or the process's cull
	 * mode is 3). FUN_140437e50's call is detoured always: around it TreeAnimation gives the loop the stood-in trees' multibound
	 * results from DCLF's verdicts and puts the engine's back (T7b5's fix; TreeAnimation.h).
	 */
	void Install();
	bool Installed();
}
