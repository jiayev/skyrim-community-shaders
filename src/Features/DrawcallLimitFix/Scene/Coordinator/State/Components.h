#pragma once

// The features' coordinator components (CoordinatorComponents): their states and their seed requests, the root marks and the commit's
// vetoes. Components.cpp, called from the residency points (Residents.cpp, ApplyEvents.cpp, Slots.cpp) and the commit (SceneSet.cpp).
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

		// The commit's vetoes (CoordinatorComponentContext::SetEntryVeto; the byte is Tracked::veto): the tracked entries held and observed,
		// what the commit recorded of each observed slot (by object slot: ObservedSlot), and the slots requeued for the next commit (queued
		// at its start: Requeue).
		std::uint32_t vetoHeld = 0, vetoObserved = 0;
		std::vector<Api::ObservedState> observedSlots;
		std::vector<std::uint32_t> requeued;
		// Whether each component needs the commit live (SetCommitLive), by registration.
		std::vector<std::uint8_t> componentCommitLive;
		// Inside the components' settle: a veto dropped from Observe applies the observed phases, a Hold applies none, in this commit
		// (settleApply, applied after the settle as the commit's own decisions).
		bool settling = false;
		std::vector<std::pair<std::uint32_t, std::uint8_t>> settleApply;
		// The slots whose structure changed since the last publication's revocation (RestructuredSincePublication), read once a commit:
		// restructuredTaken is reset at the settle.
		bool restructuredTaken = false;
		ankerl::unordered_dense::set<std::uint32_t> restructured;
	};
}
