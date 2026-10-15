#pragma once

// The structural events (CoordinatorEvents): properties and nodes changed, the drained switch events and those applied, object
// LOD's drawn ranges. Collected and applied in ApplyEvents.cpp, taken by the walk (Walk.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorEvents
	{
		// Object LOD (dclf-lod.md): each tracked BSSubIndexTriShape's drawn index ranges, taken again at its segment events.
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::vector<LodSegments::Range>> lodRanges;
		struct LodSegmentStats
		{
			std::uint64_t events = 0, changed = 0;
		} lodSegmentStats;
		// The structural events (SceneEvents) ApplyEvents applies: properties by key, nodes held.
		std::vector<const void*> propertyChanged;
		std::vector<RE::NiPointer<RE::NiAVObject>> nodeChanged;
		// Drained switch events, one per node: the index before its oldest event and whether a child changed; applied by the next walk.
		struct SwitchPending
		{
			RE::NiPointer<RE::NiAVObject> node;
			std::int32_t before = -1;
			bool structural = false;
		};
		std::vector<SwitchPending> switchPending;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> switchPendingIndex;
		// Switches the walks applied, for PrimaryCull via the deltas (FrameView::TakeSwitchChanges); switchResync: it must read them all.
		std::vector<const RE::NiAVObject*> switchesApplied;
		bool switchResync = true;
	};
}
