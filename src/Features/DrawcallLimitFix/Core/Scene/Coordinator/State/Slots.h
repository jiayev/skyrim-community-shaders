#pragma once

// The slot tables (CoordinatorSlots): the tables, their slot indices by key, object indices, the slot references counted from
// the logs, stale and freed geometry slots and the geometry scan's index. Slots.cpp, written by every phase that writes records.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorSlots
	{
		SceneTables tables;
		// The walk the entries' objectStamp must match; a new walk or a teardown bumps it, invalidating every index at once.
		std::uint32_t objectStamp = 1;
		// The slots by key, and each material's slots.
		ankerl::unordered_dense::map<const RE::BSGraphics::TriShape*, std::uint32_t> geometryIndex;
		ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash> pipelineIndex;
		ankerl::unordered_dense::map<std::pair<const RE::BSShaderMaterial*, std::uint32_t>, std::uint32_t> materialIndex;
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::vector<std::uint32_t>> materialDependents;
		std::uint32_t tablesGeneration = 0;
		std::uint32_t materialVersions = 0;  // the last Tables::materialVersion handed out
		std::array<std::uint64_t, kChangeCauseCount> reportedChangeCounts{};
		std::vector<std::uint32_t> refreshedGeometry;  // geometry slots re-resolved in place this walk
		// Slot liveness, counted from the logs: each object slot's references (slot, generation) and each geometry slot's next skin
		// partition. A geometry slot goes stale by event; objects drawing it are rewritten (FinishDeltaWalk).
		struct SlotReferences
		{
			static constexpr std::uint32_t kNone = ~0u;
			struct Reference
			{
				std::uint32_t slot = kNone, generation = 0;
				bool operator==(const Reference&) const = default;
			};
			struct Counted
			{
				Reference geometry, pipeline, material;
			};
			LogCursor objects, links;
			std::vector<Counted> object;  // per object slot
			std::vector<Reference> link;  // per geometry slot
		} slotReferences;
		std::vector<std::uint32_t> freedGeometry;
		std::vector<std::uint32_t> staleGeometrySlots;
		// Pinless imports: the geometry slots drawing a buffer DXVK moved, stale until resolved again (their leases hold the old
		// storage they read meanwhile), and those still to be scheduled (ScheduleStaleGeometry). The pass's tables' slots: never the
		// dense walk's.
		ankerl::unordered_dense::set<std::uint32_t> relocatedGeometrySlots;
		std::vector<std::uint32_t> relocatedGeometry;
		/**
		 * @brief GeometryScan's index (geometry slot to object slots), keys only and checked again at the scan: an entry may go stale, none
		 * is missing. Every object slot under the head geometry slot its last write gave it (MergeWrite, WriteLayer), and every geometry
		 * slot under each slot its nextPartition link was ever written to name (MergeWrite's partitions, ResolveEntryRangeSlots): a stale
		 * slot's objects are those under the heads its links lead back to. The dense walk's tables are put back, so it indexes nothing.
		 */
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> headObjects;
		std::vector<std::uint32_t> objectHead, objectHeadAt;  // per object slot: its head (kNoPartition: none) and its index under it
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> linkedFrom;  // geometry slot: the slots linked to it
		bool geometryResolvedLastWalk = false;
		// CheckObjectSlots runs only after a slot was freed (or under parity): only then can a bound object reference a dead slot.
		bool slotsFreedThisFrame = true;
		/** @brief What a geometry slot is resolved from. */
		struct GeometrySource
		{
			const RE::BSGraphics::TriShape* key = nullptr;
			ID3D11Buffer* vertexBuffer = nullptr;
			ID3D11Buffer* indexBuffer = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t vertexCount = 0;
			std::uint32_t indexCount = 0;
			std::uint32_t firstIndex = 0;  // an index range (object LOD's visible ranges); 0: the whole
			bool layer = false;            // a non-TriShape key (Tables::geometryLayerKey): a layer's second index list, particle quads
		};
	};
}
