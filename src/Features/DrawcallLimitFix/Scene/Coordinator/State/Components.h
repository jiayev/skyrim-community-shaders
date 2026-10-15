#pragma once

// The features' coordinator components (CoordinatorComponents): their states and their seed requests. Components.cpp, called from the
// residency points (Residents.cpp, ApplyEvents.cpp, Slots.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorComponents
	{
		// The components' states (Api::CoordinatorComponent::make), by registration: made before the first hook runs.
		std::vector<std::shared_ptr<void>> components;
		// Their seed requests (into the deltas: PublicationDeltas::componentSeeds), by registration; empty while none asked.
		std::vector<std::vector<FadeSeedItem>> componentSeedRequests;
		// Their deltas for the next publication (PublicationDeltas::componentDeltas: CoordinatorComponentContext::PublishDelta), by
		// registration; empty while none published.
		std::vector<std::vector<std::shared_ptr<const void>>> componentDeltaRequests;
		// The marks on scene roots (CoordinatorComponentContext::MarkRoot), and whether the mark owns the root's hide (mirror.SetGated:
		// ungated with it). Written only while the mirror's events are applied (the stacked and detached events, the outcomes after the
		// batch), never while the adds prepare: the parallel prepares read it as data.
		struct RootMark
		{
			Api::Mark mark;
			bool ownHide = false;
		};
		ankerl::unordered_dense::map<const void*, RootMark> rootMarks;
	};
}
