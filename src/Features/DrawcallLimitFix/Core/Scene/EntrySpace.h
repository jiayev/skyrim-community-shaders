#pragma once

// A published entry space (the sun's and the point lights' candidate sets) and its change log, and the readers that compare two
// snapshots of one. Immutable once published; any thread.

#include <cstdint>
#include <memory>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"

namespace DCLF
{
	struct EntrySpaceChanges;

	/**
	 * @brief The sun entries DCLF could take out of the engine's cascade culls (Coordinator::UpdateSunCandidates); immutable once
	 * published. An entry (a static reference's root, or a terrain block's multibound node) is a candidate when every tracked
	 * geometry under it is a table object (its shadow drawn by the shadow epoch or rejected by the caster rule) or one the engine
	 * never draws into a shadow map (non-Lighting, hidden, blended, fading, unselected switch child).
	 *
	 * Indices are stable while an entry is a candidate, as are its geometries'. Every index change takes a session-unique version
	 * (entryVersion), so snapshots differ exactly where versions differ (ChangedEntries), which consumers update by.
	 */
	struct EntrySpace
	{
		EntrySpace() = default;
		EntrySpace(const EntrySpace&) = delete;
		EntrySpace& operator=(const EntrySpace&) = delete;
		// The last owner may be any thread: the entry nodes are released on the render thread.
		~EntrySpace()
		{
			for (auto& node : held)
				EngineReleases::Push(std::move(node));
		}

		std::uint32_t generation = 0;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> entries;     // entry node -> entry index
		std::vector<const RE::NiAVObject*> entryNodes;                                  // entry index -> entry node (null: free)
		std::vector<std::uint32_t> entryVersion;                                        // entry index -> its version
		// The entry nodes, held as long as the snapshot: readers keyed by them may dereference one after the engine drops it.
		std::vector<RE::NiPointer<RE::NiAVObject>> held;
		std::vector<std::vector<std::uint32_t>> entryGeometries;                        // entry index -> its geometry indices
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometries;  // every tracked geometry under one -> geometry index
		std::vector<const RE::BSGeometry*> geometryNodes;                               // geometry index -> geometry (null: free)
		std::vector<std::uint32_t> geometryEntry;                                       // geometry index -> entry index (kNone: free)
		// geometry index -> its object slot at the snapshot's walk (-1: no record); the frame reads this, not the tracked set
		std::vector<std::int32_t> geometrySlot;
		// Per geometry (Eligibility::PrimaryVerdict): 1, a table object PrimaryCull can give a synthetic pass; 2, a hidden static's,
		// drawn by nobody; 0, the engine's (under a left-out entry, handed over as its cull would).
		std::vector<std::uint8_t> primaryGeometry;
		// The table's change log as published with this snapshot: which indices moved since earlier generations
		// (EntrySpaceChanges, ChangedEntriesByLog). Null for a snapshot no table published.
		std::shared_ptr<const EntrySpaceChanges> changes;

		static constexpr std::uint32_t kNone = ~0u;
		/** @brief The index space: entry indices are below it (free ones among them). */
		std::uint32_t Capacity() const { return static_cast<std::uint32_t>(entryNodes.size()); }
		std::uint32_t GeometryCapacity() const { return static_cast<std::uint32_t>(geometryNodes.size()); }
		/** @brief The live entries. */
		std::uint32_t Count() const { return static_cast<std::uint32_t>(entries.size()); }
	};

	/**
	 * @brief An entry space table's change log as one snapshot carries it (EntrySpace::changes), immutable once published: per
	 * generation after floor up to the snapshot's, the entry indices written in between (Set, Reset). Readers in any thread take
	 * from it the indices that moved between an older snapshot of the same log and this one (ChangedEntriesByLog).
	 */
	struct EntrySpaceChanges
	{
		/** @brief The indices written between generation - 1 and generation, ascending, each once. */
		struct Run
		{
			std::uint32_t generation = 0;
			std::vector<std::uint32_t> entries;
		};
		std::uint64_t log = 0;         // session-unique per table: snapshots of different tables never chain
		std::uint32_t floor = 0;       // the runs reach a snapshot of a generation from floor on (Reset, the trim: floor moves on)
		std::uint32_t generation = 0;  // the snapshot's
		std::vector<std::shared_ptr<const Run>> runs;  // oldest first, contiguous over (floor, generation]

		/** @brief Whether a snapshot of a_log at a_generation is reached: its changes up to this one are in the runs. */
		bool Reaches(std::uint64_t a_log, std::uint32_t a_generation) const { return a_log == log && a_generation >= floor && a_generation <= generation; }
	};

	/**
	 * @brief Appends, in order, the entry indices whose versions differ between a_from and a_to (null a_from: every index), by comparing
	 * every index: ChangedEntriesByScan. Readers that update by the change follow ChangedEntriesByLog instead (O(changes)).
	 */
	void ChangedEntries(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out);
	/**
	 * @brief ChangedEntries by the log: appends, ascending, the entry indices whose versions differ between a_from and a_to (null
	 * a_from: every index live in a_to), the same indices ChangedEntries finds. When a_to's log reaches a_from it reads the runs
	 * between them (O(changes)); otherwise - a_from null, of another table, older than the log's floor - it compares the snapshots
	 * index by index, as a journal's reader behind its floor reads everything again. Returns whether the log answered. Any thread.
	 */
	bool ChangedEntriesByLog(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out);
	/** @brief The same by comparing every index (what ChangedEntriesByLog does when the log does not reach a_from; parity's reference). */
	void ChangedEntriesByScan(const EntrySpace* a_from, const EntrySpace& a_to, std::vector<std::uint32_t>& a_out);
}
