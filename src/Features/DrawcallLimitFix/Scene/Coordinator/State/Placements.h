#pragma once

// Placements, moves and root motion (CoordinatorPlacements): the walk's placement plans and the planned set they diff against,
// the mover set and the light path's gate, moving and still roots, move events and root motion. Placements.cpp, with the walk
// (Walk.cpp) and the events (ApplyEvents.cpp).
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorPlacements
	{
		// The walk's placement plan: filled by its rounds and PublishPlacementPlan, ready once the walk is done.
		std::shared_ptr<PlacementPlan> placementPlan, placementPlanReady;
		// Plans no publication took yet, oldest first; the next publication carries them all.
		std::vector<std::shared_ptr<PlacementPlan>> placementPlansHeld;
		/**
		 * @brief The placement set the plans carry (PublishPlacementPlan): every recorded per-frame mover (IsPlacedMover), every moving
		 * root (movingRoots) and every still root that had a move event since it was listed (stillRoots, until its last dependent goes).
		 * Keys only: a held reference dropped by a later pass would be released on the lane, so the references are copied from the
		 * entries and the root owners when a plan is made.
		 */
		ankerl::unordered_dense::set<RE::BSGeometry*> moverSet;
		ankerl::unordered_dense::set<const RE::NiAVObject*> stillRoots;
		// The set must be made anew (a clear, a full walk, the change log lost, the object indices invalidated): the next plan is a reset.
		bool placementDirty = true;
		/** @brief One plan row as compared (PlacementPlan::Key): movers by geometry, a root's dependents by geometry. */
		using PlannedKey = PlacementPlan::Key;
		/**
		 * @brief The set as the plans so far left it (what FrameValues' kept set holds, the same deltas applied): each plan carries what
		 * the touched keys below changed against it, so a plan costs what changed, never the set.
		 */
		ankerl::unordered_dense::map<const void*, PlannedKey> plannedMovers;                                                   // by geometry
		ankerl::unordered_dense::map<const RE::NiAVObject*, ankerl::unordered_dense::map<const void*, PlannedKey>> plannedRoots;  // by node: dependents
		// The planned slots and layer slots, each with the items naming it (root null: a mover's): a change-log entry on one touches them.
		struct PlannedOwner
		{
			const void* geometry = nullptr;
			const RE::NiAVObject* root = nullptr;
			bool operator==(const PlannedOwner&) const = default;
		};
		ankerl::unordered_dense::map<std::uint32_t, std::vector<PlannedOwner>> plannedSlots;
		std::uint32_t plannedObjectSlots = 0, plannedBoneCapacity = 0, plannedObjectStamp = 0;
		LogCursor placementCursor;  // the change log, read for the planned slots
		// What may differ from the planned set since the last plan, keys only (the membership points: NoteMover, the root lists, the
		// dependents lists, the change log's planned slots), compared against it at the next plan. Duplicates compare equal the second time.
		std::vector<RE::BSGeometry*> placementMoversTouched;
		std::vector<const RE::NiAVObject*> placementRootsTouched;
		std::vector<std::pair<const RE::NiAVObject*, const void*>> placementDependentsTouched;
		/** @brief The plans' deltas and the parity (Policy::HiddenWitness passes) since the last report. */
		struct PlacementDeltaStats
		{
			std::uint64_t plans = 0, resets = 0, sized = 0;
			std::uint64_t moversJoined = 0, moversChanged = 0, moversLeft = 0, rootsListed = 0, rootsLeft = 0, dependentsJoined = 0, dependentsChanged = 0,
						  dependentsLeft = 0;
			std::uint64_t moversTouched = 0, rootsTouched = 0, dependentsTouched = 0, logSlots = 0;
			std::uint64_t parityChecks = 0, parityDiffer = 0;
			std::string parityFirst;
			// ScheduleRoot: dirty roots (attach or detach under them), those whose motion verdict changed (every dependent scheduled), the
			// dependents scheduled against those listed; the parity's witnesses (dependents scheduled only by it) and those it changed.
			std::uint64_t dirtyRoots = 0, dirtyRootsWhole = 0, rootDependentsScheduled = 0, rootDependentsListed = 0;
			std::uint64_t rootWitnessChecks = 0, rootWitnessDiffer = 0, rootWitnessSkipped = 0;  // skipped: scheduled already, or no placement matters
			std::string rootWitnessFirst;
			// GeometryScan: scans, stale slots, object slots checked (indexed) against those a whole scan checks, scheduled.
			std::uint64_t scans = 0, scansWhole = 0, staleSlots = 0, scanChecked = 0, scanObjects = 0, scanScheduled = 0;
			std::uint64_t scanParityChecks = 0, scanParityDiffer = 0;
		} placementDeltaStats;
		std::uint32_t placementDeltaWindow = 0;
		// Mirror updates that change an entry's traits or skin (a node's kBody or kControllers, a geometry's kDismember or kSkin), keys
		// only, for the next delta walk (ApplyMotionUpdate, Reclassify).
		ankerl::unordered_dense::set<const void*> mirrorMotionNodes, mirrorSkinGeometries;
		/** @brief The light path's gate and the plans since the last report (the mover gate line, from ScenePass). */
		struct MoverGateStats
		{
			std::uint64_t perFrameWalks = 0, ungatedWalks = 0, due = 0, moved = 0, owed = 0, firstPerson = 0, fullEveryFrame = 0, skipped = 0;
			std::uint64_t plans = 0, plansWritten = 0, plansChanged = 0, candidatesSame = 0, perFrameWithoutPlan = 0;
			std::uint64_t mirrorNodes = 0, mirrorGeometries = 0;
			// Parity (witness walks, Policy::HiddenWitness): per-frame entries no event named, evaluated anyway, and those whose
			// evaluation changed something the gate would have left (the light path missed, a skin's partitions, the mover set).
			std::uint64_t witnessChecks = 0, witnessDiffer = 0;
			std::string firstDiffer;
		} moverGateStats;
		std::uint32_t moverGateWindow = 0;
		/**
		 * @brief A root whose bound is an entry's only motion (kTraitRootMoves alone) is listed in the placement plan instead of the entry:
		 * every moving root, and a still root once its reference had a move event (QueueRoots, stillRoots).
		 */
		struct MovingRoot
		{
			const void* key = nullptr;  // its reference
			const RE::NiNode* categoryNode = nullptr;
		};
		// Every root RootMoves found moving, until ScheduleRoot or its last dependent forgets it (each change marks the plan dirty).
		ankerl::unordered_dense::map<const RE::NiAVObject*, MovingRoot> movingRoots;
		std::size_t movedKeysQueued = 0;
		struct PlacementStats
		{
			std::uint64_t roots = 0, stillRoots = 0, rootsGated = 0;
			std::uint64_t fadeRootsUnmirrored = 0;  // listed with no mirror record (FadeRootFromMirror)
		} placementStats;
		const RE::NiNode* plannedFirstPerson = nullptr;  // firstPersonCategory as the planned keys took it (a change makes the set anew)
		/** @brief The frame each key last had a move event (drained at the delta walk). */
		ankerl::unordered_dense::map<const void*, std::uint32_t> movedFrame;
		// Keys with a move event this frame and the last: QueueRoots lists those references' roots whatever RootMoves says (cell updates
		// recompute a still root's bound).
		std::array<std::vector<const void*>, 2> movedKeys;
		// movedKeys turn over once a frame (a burst's passes add to this frame's); movedFrame is pruned every 256 frames.
		std::uint32_t movedKeysFrame = 0, movedPruneFrame = 0;
		bool moveGating = false;
		std::uint32_t moveUngatedThrough = 0;
		ankerl::unordered_dense::map<const RE::NiAVObject*, bool> rootMotion;
		// Dependents a witness pass scheduled only for ScheduleRoot's parity: their merge must change nothing (MergeEntry).
		ankerl::unordered_dense::set<const RE::BSGeometry*> rootWitness;
	};
}
