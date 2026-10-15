#pragma once

// The LOD gates, coordinator side (CoordinatorLodGates): the open and flipped gates, their roots, the gated slots, flips and
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
		 * roots (ApplyGateEvent); the commit withholds its incoming slots (GateWithhold) until all are whole members or ineligible and
		 * mirrored, then flips it (UpdateGates): held phases applied, outgoing blocks held out until their detach, the flip published. It
		 * retires with its outcome (ApplyGateOutcomes).
		 */
		static constexpr std::uint8_t kGateIncoming = 1, kGateOutgoing = 2;
		struct LodGate
		{
			enum class State : std::uint8_t
			{
				Open,     // the incoming withheld, the outgoing members
				Flipped,  // the incoming applied, the outgoing held out (Tracked::gateHeldOut), awaiting the outcome
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
		struct GateSlot
		{
			const RE::BSGeometry* geometry = nullptr;  // the gated geometry the commit last evaluated
			std::uint8_t phases = 0;                   // its phases then (held while the gate is open)
			bool ready = false;                        // all its phases, not waiting
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
		struct GateRoot
		{
			std::uint64_t token = 0;
			std::uint8_t side = 0;  // kGateIncoming, kGateOutgoing
		};
		ankerl::unordered_dense::map<const void*, GateRoot> gateOfRoot;
		std::vector<GateSlot> gateSlots;                                     // by object slot
		std::vector<GateOutcome> gateOutcomesHeld;                           // taken, not applied yet
		std::uint32_t gateHeldOutCount = 0;                                  // entries with Tracked::gateHeldOut
		// Slots a gate's tag or retirement wants evaluated again, queued at the next commit's start.
		std::vector<std::uint32_t> gateQueued;
		GateStats gateStats;
	};
}
