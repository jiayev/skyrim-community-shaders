#pragma once

// Attach and detach (CoordinatorAttach): AddSubtree's prepares and merges (HeldKeys, GeometryPrep, SubtreePrep), their fan-out
// and the attach burst's counters. Classification.cpp (AddSubtrees, AddGeometry) and ApplyEvents.cpp.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorAttach
	{
		enum class SubtreeSource : std::uint8_t
		{
			AttachEvent,
			CategoryWalk,
			Retry,
		};
		std::vector<RE::NiPointer<RE::NiAVObject>> pendingSubtrees;
		std::vector<RE::BSGeometry*> validationSuspects;  // ValidateSlice's: keys, checked again only while tracked
		std::uint64_t subtreesPended = 0, subtreesDropped = 0;  // since the last report: waiting for the mirror, and still without a chain after
		std::uint64_t categoryChildrenMissing = 0;  // a new category node the capture listed no children of (should not happen)
		// AddSubtree as a prepare (pure over the batch's frozen mirror, any thread under a ShardScope) and a merge in event order.
		/**
		 * @brief HoldProperties' reads: the mirror's property, layer and fade node keys for a geometry, and the batch's pins of each (null:
		 * none). Pure (the mirror and batchPins, frozen while a batch's adds prepare).
		 */
		struct HeldKeys
		{
			const void* property = nullptr;
			const void* layer = nullptr;
			const void* fadeNode = nullptr;
			RE::NiRefObject* propertyPin = nullptr;
			RE::NiRefObject* layerPin = nullptr;
			RE::NiRefObject* fadeNodePin = nullptr;
		};
		/**
		 * @brief What AddGeometry reads of the mirror and the batch's frozen state for one geometry (keys and pins): the merge only stores.
		 * The sun entry and actor root are left to the merge under the mirror-read parity (resolved false).
		 */
		struct GeometryPrep
		{
			RE::BSGeometry* geometry = nullptr;
			Ineligible reason = Ineligible::None;  // what the nodes between the category node and it make of it (MirrorSubtree's)
			const RE::TESObjectREFR* ownerReference = nullptr;  // its owner class's reference (Extensions::OwnerClassOf), or null
			const void* ancestorKey = nullptr;   // AncestorKeyOf
			const RE::NiNode* source = nullptr;  // its entry class's held parent (EntryRule::holdsParent: a face shape's head)
			bool mirrored = false;
			bool ranged = false;
			bool resolved = false;
			const void* sunEntry = nullptr;
			const RE::NiAVObject* lightActorRoot = nullptr;  // the category node's child above it whose reference has an owner class
			// The batch's pins of the geometry and its held parent (null: none).
			RE::NiRefObject* geometryPin = nullptr;
			RE::NiRefObject* sourcePin = nullptr;
			HeldKeys held;
			// Of the sun entry (when resolved): its record's reference, and whether a category node owns it (CategoryNodeOwn).
			const RE::TESObjectREFR* sunEntryReference = nullptr;
			bool sunEntryCategory = false;
			// The nearest marked root up its chain to the category node (rootMarks; none: no mark), and a ranged entry's drawn ranges (object
			// LOD's: as its events' component takes them).
			Api::Mark mark;
			std::vector<Api::EntryRange> ranges;
		};
		/** @brief One subtree's prepare (PrepareSubtree): its category node (none: nothing tracked), and its geometries in walk order. */
		struct SubtreePrep
		{
			RE::NiAVObject* root = nullptr;
			SubtreeSource source = SubtreeSource::AttachEvent;
			RE::NiNode* category = nullptr;
			bool unmirrored = false;  // FindCategoryNode found a record missing on the way up
			std::vector<GeometryPrep> geometries;
		};
		// A subtree's prepare is a whole walk (one root to hundreds of geometries): chunks of a few roots, the pool from 16.
		static constexpr std::size_t kSubtreeGrain = 4;
		static constexpr std::size_t kSubtreeFanoutMin = 16;
		// The geometries' prepares (several chain walks each) across all the round's subtrees: the pool from 64, chunks of 32. A few
		// large attaches go to the pool though their roots stay under kSubtreeFanoutMin.
		static constexpr std::size_t kGeometryGrain = 32;
		static constexpr std::size_t kGeometryFanoutMin = 64;
		std::vector<SubtreePrep> subtreePreps;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> subtreeGeometryIndex;  // PrepareSubtrees' scratch: (prep, geometry) flattened
		/**
		 * @brief The attach burst's work since the report (W8): the mirror's captures prepared (copied off the lane, on the pool, records)
		 * and its apply; the geometries prepared (on the pool); category members indexed and the parity checks of the index.
		 */
		struct AttachStats
		{
			std::uint64_t mirrorBatches = 0, mirrorCaptures = 0, mirrorPooled = 0, mirrorRecords = 0;
			double mirrorPrepareMs = 0.0, mirrorApplyMs = 0.0;
			std::uint64_t usesChecks = 0, usesUnused = 0;  // UnusedRecords' parity (CheckMirrorUses)
			std::uint64_t geometryRounds = 0, geometryPooled = 0, geometries = 0, geometriesPooled = 0;
			std::uint64_t categoriesLeft = 0, categoryMembersDropped = 0, rootsLeftScanned = 0;
			std::uint64_t categoryChecks = 0, categoryDiffer = 0;  // CheckCategoryMembers' checks, and those that differed
			std::string categoryFirst;
			// CheckAncestorKeys: entries compared, kept keys other than the mirror's chain gives now, values other than the map gives the key.
			std::uint64_t ancestorChecks = 0, ancestorEntries = 0, ancestorStaleKeys = 0, ancestorStaleValues = 0;
			std::string ancestorFirst;
			// The mirror's apply by event kind (capture, fill, eviction, detach, gate, update): events and ms (timed where the kind changes).
			static constexpr std::size_t kMirrorKinds = 6;
			std::array<std::uint64_t, kMirrorKinds> mirrorKindEvents{};
			std::array<double, kMirrorKinds> mirrorKindMs{};
			// SceneMirror::Work: captures that looked nothing up (numbered after every kept update) and those that did; updates kept, let go.
			std::uint64_t replaysSkipped = 0, replaysWalked = 0, updatesKept = 0, updatesForgotten = 0;
			std::uint64_t applyChecks = 0, applyWrong = 0;  // SceneMirror's own checks (Policy::CheckMirrorApply), and those that failed
			// The attach/detach merge outside the merges: the events walked, the sun candidates above them (ms), the detached geometries
			// (moves kept, erased) and the detach loop (ms); the leaf holds after (geometries named, tracked ones held, ms).
			std::uint64_t structuralEvents = 0, detachedGeometries = 0, detachedErased = 0, leafKeys = 0, leafHolds = 0;
			double ancestorMs = 0.0, detachMs = 0.0, mergeMs = 0.0, leafMs = 0.0;
		} attachStats;
	};
}
