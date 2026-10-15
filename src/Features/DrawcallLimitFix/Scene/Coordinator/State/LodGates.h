#pragma once

// The LOD gates, coordinator side (CoordinatorLodGates): the open and flipped gates, their roots, their tags, flips and
// outcomes. Gates.cpp, with the commit (SceneSet.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorLodGates
	{
		using GateFlip = DCLF::GateFlip;
		/**
		 * @brief The LOD gates, coordinator side (Engine/Cull/LodGates.h; dclf-async-publication.md). A gate tags the geometries under its
		 * roots (ApplyGateEvent); the commit withholds its incoming slots (their Observe veto) until all are whole members or ineligible and
		 * mirrored, then flips it (UpdateGates, the component's settle): observed phases applied, outgoing blocks held out (their Hold veto)
		 * until their detach, the flip published. It retires with its outcome (ApplyGateOutcomes).
		 */
		static constexpr std::uint8_t kGateIncoming = 1, kGateOutgoing = 2;
		struct LodGate
		{
			enum class State : std::uint8_t
			{
				Open,     // the incoming withheld, the outgoing members
				Flipped,  // the incoming applied, the outgoing held out (Api::kVetoHold), awaiting the outcome
			};
			State state = State::Open;
			std::vector<const void*> incoming, outgoing;  // the roots (keys), less those detached since
			ankerl::unordered_dense::set<RE::BSGeometry*> incomingGeometries, outgoingGeometries;  // the tracked ones tagged
			std::uint32_t pending = 0;     // incoming geometries neither whole members nor ineligible
			std::uint32_t unmirrored = 0;  // incoming roots or nodes under them without a mirror record
			std::uint64_t stampNs = 0;     // its event's push, steady clock
			std::uint32_t stampFrame = 0;
			std::uint64_t flipNs = 0;
			std::uint32_t flipFrame = 0;
		};
		/** @brief A tracked geometry's tag: its gate (token) and side (kGateIncoming, kGateOutgoing). */
		struct GateTag
		{
			std::uint64_t token = 0;
			std::uint8_t side = 0;
		};
		struct GateOutcome
		{
			std::uint64_t token = 0;
			bool forced = false;
			std::array<const void*, 2> kept{};  // a held block the engine reused (LodGates::Outcome::kept), by its roots
		};
		struct GateStats
		{
			std::uint64_t opened = 0, flipped = 0, retiredFlipped = 0, retiredForced = 0, forcedOpen = 0, unknownOutcomes = 0, superseded = 0;
			std::uint64_t rootsDetached = 0, taggedIncoming = 0, taggedOutgoing = 0, heldOut = 0, emptyIncoming = 0;
			// Kept notices (ApplyGateKept): taken, geometries released from hold-out, outgoing tags dropped before a flip.
			std::uint64_t keptNotices = 0, keptCleared = 0, keptUntagged = 0;
			// Flips deferred for an incoming record the next publication claims first.
			std::uint64_t structureDeferred = 0;
			std::vector<double> requestToFlipMs;
			std::vector<std::uint32_t> requestToFlipFrames;
		};
		ankerl::unordered_dense::map<std::uint64_t, LodGate> lodGates;
		// The gates' roots are the coordinator's marks (rootMarks: the gates' component's, token and side kGateIncoming/kGateOutgoing; the
		// incoming own their hide).
		// The tracked geometries' tags (erased with the entry: the component's forgot, forgotAll). The incoming of an open gate are observed
		// and the flipped gates' outgoing held (the coordinator's vetoes: Tracked::veto, the observed slots, the requeued slots).
		ankerl::unordered_dense::map<const RE::BSGeometry*, GateTag> gateTags;
		std::vector<const void*> gateEntries;       // a root's tracked entries (SubtreeEntries), reused by each walk
		std::vector<GateOutcome> gateOutcomesHeld;  // taken, not applied yet
		GateStats gateStats;
	};
}
