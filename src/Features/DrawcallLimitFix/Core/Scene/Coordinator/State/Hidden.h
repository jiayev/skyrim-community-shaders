#pragma once

// The hidden events (CoordinatorHidden): actor entries by the nodes on their chain, the applied batches' hidden stores and those
// announced again at a later capture. ApplyEvents.cpp (DrainHiddenEvents), read by the walk.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorHidden
	{
		// Actor entries by the nodes on their chain, for the hidden events.
		ankerl::unordered_dense::map<const void*, std::vector<RE::BSGeometry*>> hiddenDependents;
		bool hiddenGating = false;
		bool hiddenWitness = false;
		/** @brief The applied batches' hidden stores (mirror updates with a site), taken by DrainHiddenEvents. */
		struct BatchHidden
		{
			const void* key = nullptr;
			std::uint32_t site = 0;
			bool hidden = false;
			std::uint64_t sequence = 0;  // SceneCapture::NextSequence after the store
		};
		std::vector<BatchHidden> batchHidden;
		/**
		 * @brief Hidden events newer than the capture the pass judged by (cullHidden) that differed: announced again at the first pass with
		 * a later capture (cullHiddenSequence). Keys only.
		 */
		ankerl::unordered_dense::map<const void*, std::uint64_t> hiddenAhead;
	};
}
