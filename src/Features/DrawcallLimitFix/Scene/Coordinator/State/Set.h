#pragma once

// The DCLF set and the claims (CoordinatorSet): the waiting slots and the commit queue, the commit's decision and its
// application, readiness stamps, the claims pool and the revocations. SceneSet.cpp.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorSet
	{
		// The change log position at the last commit, which the applied set's revocation checks from.
		LogCursor setCommitCursor;
		// The DCLF set (CommitSet): bound slots waiting for readiness and the commit queue. The commit reads the change log from its own
		// cursor, and the waiting slots again when a readiness source moved.
		std::vector<std::uint32_t> setWaiting;
		std::vector<std::uint8_t> setWaitingMark;  // per slot: in setWaiting
		std::vector<std::uint32_t> setQueue;
		// Joined the main phase at the last commit and take part in the reflection's: re-evaluated at this one.
		std::vector<std::uint8_t> setQueueMark;    // per slot: in setQueue
		std::vector<std::uint8_t> setLacking;      // per slot: occluder and reflection phases it takes part in but is not a member of
		std::array<std::uint32_t, 3> setLackingCount{};  // per occlusion map, then the reflection
		std::vector<std::uint8_t> setPartialMark;  // per slot: out of the set with some phases ready
		std::uint32_t setPartialCount = 0;
		// The last commit's decision for the next ApplySet: phases, lacking phases, and the changed slots with their geometry (a slot
		// holding another geometry by then takes nothing).
		std::vector<std::uint8_t> setPhasesNext, setLackingNext;
		std::vector<std::pair<std::uint32_t, const RE::BSGeometry*>> setApply;
		// Per slot: the geometry the last deciding commit decided for.
		std::vector<const RE::BSGeometry*> setGeometryNext;
		std::vector<std::uint32_t> setApplyMark;  // per slot: its index in setApply + 1, 0 when absent
		std::uint32_t setCommitFrame = 0;
		std::uint32_t setCommitToggles = 0;  // the toggles generation the last commit was made under
		// The claims as applied (a freed slot clears Tables::setPhases): what RevokeUndrawnClaims checks against, with each base slot's
		// geometry.
		std::vector<std::uint8_t> setPhasesApplied;
		std::vector<const RE::BSGeometry*> setGeometryApplied;
		LogCursor revokeCursor;
		std::uint64_t revokedGeometries = 0, revokedMain = 0;
		// Slots whose structure (kStructureCauses) changed since the selected revision's join: a claim among them is revoked at the join
		// (its shapes predate the change) until a later revision is selected.
		std::vector<std::uint32_t> structureChanged;
		std::uint64_t revokedStructureGeometries = 0;
		std::vector<const RE::BSGeometry*> setGeometry;  // per slot: the geometry a base member is published under
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> setMemberSlot;  // published geometry -> its slot
		LogCursor setCursor;
		// Readiness sources a waiting slot may wait on (SetWaitCause bits) and their stamps at the last check.
		static constexpr std::uint32_t kWaitSources = 4;
		static constexpr std::uint8_t kWaitLookups = 1, kWaitShadow = 2, kWaitConstants = 4, kWaitFit = 8;
		static constexpr std::uint8_t kWaitOther = 16;  // any source moving retakes it (geometry, decal slot, reflection, layer partner)
		std::array<std::uint64_t, kWaitSources> setReadiness{ ~0ull, ~0ull, ~0ull, ~0ull };
		// Forward views' pipelines ready in the catalog (reason 8): no source above moves with them, so a count change retakes the
		// kWaitOther slots.
		std::uint64_t setForwardReadiness = ~0ull;
		// The catalog setForwardReadiness was counted in (immutable: the count holds while it is the lookups'); held, so no other takes
		// its address.
		std::shared_ptr<const PipelineCatalog> setForwardCatalog;
		std::uint64_t setForwardCount = 0;
		std::vector<std::uint8_t> setWaitCause;  // per slot: what it waits on
		std::vector<std::uint8_t> setWaitWhy;    // per slot: the first reason of its last wait
		std::uint64_t setShadowModes = ~0ull;      // the shadow modes and states caster readiness was last taken under
		std::uint32_t setPhaseMask = ~0u;          // the phases DCLF draws at the last commit
		std::shared_ptr<SetSnapshot> setBuilding;  // the next publication, kept current by each commit
		// Every key written in setBuilding's phases (inserted, reassigned or erased; invalidated when it is cleared): the pooled claims
		// replay from it (PublishClaims).
		EventLog<const RE::BSGeometry*> setPhaseLog;
		// The published claims, pooled as the tables are: an entry nothing else holds any more is brought up to setBuilding by the phases
		// log (its cursor), plus the keys its last publication's revocations edited; one the log no longer reaches is copied whole.
		struct ClaimsEntry
		{
			std::shared_ptr<SetSnapshot> snapshot;
			LogCursor cursor;
			std::vector<const RE::BSGeometry*> edited;  // keys the revocations changed: not setBuilding's
		};
		std::vector<ClaimsEntry> claimsPool;
		std::size_t claimsPublished = ~std::size_t(0);  // publicationClaims' entry
		bool claimsFresh = false;                       // made by this publication (else shared from the last: held, never written)
		// The claims this publication's revocation takes back (RevokeUndrawnClaims), applied to its claims alone.
		std::vector<std::pair<const RE::BSGeometry*, std::uint8_t>> claimsRevoked;
		bool setSnapshotDirty = true;  // setBuilding changed since the last published claims
		/** @brief One slot's decision: phases, whether and why it waits, and the observers' notes of a CategoryRoot view's slot (first person's). */
		struct SlotDecision
		{
			std::uint8_t phases = 0;  // every phase it takes part in if ready for all, else 0
			bool wait = false;        // takes part in a phase it is not ready for
			bool partial = false;
			std::array<std::uint8_t, 8> whys{};  // waiting reasons in order (waitingBy's index)
			std::uint8_t whyCount = 0;
			bool rootViewExcluded = false;       // observed runs: a CategoryRoot view's slot its view takes no part in
			bool rootViewHeld = false;           // a CategoryRoot view's slot held out of its view
			std::uint8_t heldWhyCount = 0;       // the reasons noted before it was held out
			std::uint32_t rootViewSlotWhy = 0;   // IndirectDraws::ViewSlotReady's cause
		};
	};
}
