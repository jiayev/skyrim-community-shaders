#pragma once

// The engine's tree manager list loop, detoured for the stood-in trees (tree wind's engine side). Included by the feature's sources.

namespace DCLF::Features::TreeWind::TreeList
{
	/**
	 * @brief The engine's tree manager list loop (skyrim-engine-notes.md, "Tree manager list").
	 *
	 * BSTreeManager's update (FUN_140437d40, Main::Update 0x140646176, main thread) runs FUN_140437e50 over its tree list (+0x20:
	 * BSTreeNodes, exact RTTI): a tree is visited only when its first child's multibound has the cull result 1 (or the process's cull
	 * mode is 3). FUN_140437e50's call is detoured always: around it TreeAnimation gives the loop the stood-in trees' multibound
	 * results from DCLF's verdicts and puts the engine's back (TreeAnimation.h).
	 */
	void Install();
	bool Installed();
}
