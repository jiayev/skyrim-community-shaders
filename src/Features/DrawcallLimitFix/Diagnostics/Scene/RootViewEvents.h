#pragma once

// A CategoryRoot view's hook points (Hooks.h's mechanism; the first person's: Features/FirstPerson/Diagnostics observes them): its root's
// subtree bits at the render thread's engine moments (FrameGlobals), a hidden verdict inside its subtree (the classification) and the set
// commit parity's decisions that leave a slot out of it. Core announces them; the view's feature keeps the counts, logs and report lines.

#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <cstdint>
#include <span>
#include <utility>

namespace DCLF::Diagnostics
{
	/**
	 * @brief Render thread, at an engine moment with the tree alive (the frame's start, `frameStart`, after the capture's cull-hidden bits
	 * are merged; or the root view's culls, NoteFirstPersonCulled): the root's subtree brought up to now by its events (`nodes` by key;
	 * `culled`: the culls were seen for this root; `walks`, `changes`: the subtree's counts). Only with a root.
	 */
	struct RootSubtreeSynced
	{
		const char* moment;
		bool frameStart;
		const RE::NiAVObject* root;
		std::span<const RootSubtreeNode> nodes;
		bool culled;
		std::uint64_t walks, changes;
	};

	/**
	 * @brief An evaluation (any thread of the pass), ClassifyFrame: an entry filed under the root view's root (`sameRoot`) or in its
	 * subtree's geometry hidden at chain depth `depth` by node `key` (the mirror's bit: `recordHidden`); with the frame's cull-hidden list.
	 * Pointers only, never dereferenced.
	 */
	struct RootViewHidden
	{
		const Scene::Coordinator& store;
		const void* geometry;
		const void* category;
		const void* root;
		bool sameRoot;
		const void* key;
		std::uint32_t depth;
		bool recordHidden;
		std::span<const std::pair<const RE::NiAVObject*, bool>> cullHidden;
	};

	/**
	 * @brief The coordinator, CS_DCLF_SET_PARITY's commit check: a slot decided again that its decision leaves out of a root view
	 * (rootViewExcluded, rootViewHeld), with the waiting reason the commit left (the commit's own decisions: SetSlotDecided).
	 */
	struct SetSlotRedecided
	{
		const Scene::Coordinator& store;
		std::uint32_t slot;
		const SceneView::SlotDecision& decision;
		std::uint8_t waitWhy;
	};
}
