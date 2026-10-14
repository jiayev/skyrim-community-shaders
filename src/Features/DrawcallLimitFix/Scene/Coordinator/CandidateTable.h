#pragma once

// The coordinator's stable index space for a candidate set (the sun's and the point lights' SunCandidates), and the pooled snapshots
// published from it. Coordinator thread only; a published snapshot is read by the frame's builds and released by its last owner.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Runtime/KeptState.h"

namespace DCLF
{
	/**
	 * @brief A candidate table's change log as one snapshot carries it (SunCandidates::changes), immutable once published: per
	 * generation after floor up to the snapshot's, the entry indices written in between (Set, Reset). Readers in any thread take
	 * from it the indices that moved between an older snapshot of the same log and this one (ChangedEntriesByLog).
	 */
	struct CandidateChanges
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
	 * @brief ChangedEntries by the log: appends, ascending, the entry indices whose versions differ between a_from and a_to (null
	 * a_from: every index live in a_to), the same indices ChangedEntries finds. When a_to's log reaches a_from it reads the runs
	 * between them (O(changes)); otherwise - a_from null, of another table, older than the log's floor - it compares the snapshots
	 * index by index, as a journal's reader behind its floor reads everything again. Returns whether the log answered. Any thread.
	 */
	bool ChangedEntriesByLog(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out);
	/** @brief The same by comparing every index (what ChangedEntriesByLog does when the log does not reach a_from; parity's reference). */
	void ChangedEntriesByScan(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out);
}

namespace DCLF::Scene
{
	/**
	 * @brief The entries' and their geometries' indices with their free lists, each entry's geometries and its version (moved by every
	 * change of the index, from a counter the caller shares between tables). Publish brings a pooled snapshot no reader holds up to the
	 * table at the indices whose versions moved.
	 *
	 * Every index Set or Reset writes is appended to log. Each pooled snapshot keeps a LogCursor into it and catches up by the indices
	 * it has not read; a snapshot whose cursor is no longer readable (new, or behind a Reset or the trim) is brought up index by index.
	 * Each publication also closes a run of the indices written since the last (CandidateChanges), published with the snapshot.
	 */
	struct CandidateTable
	{
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint32_t> index;
		std::vector<const RE::NiAVObject*> nodes;
		std::vector<std::uint32_t> version;
		std::vector<std::vector<std::uint32_t>> geometries;  // per entry
		std::vector<std::uint32_t> free;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> geometryIndex;
		std::vector<const RE::BSGeometry*> geometryNodes;
		std::vector<std::uint32_t> geometryEntry;
		std::vector<std::int32_t> geometrySlot;
		std::vector<std::uint8_t> primaryGeometry;
		std::vector<std::uint32_t> geometryFree;
		std::vector<std::shared_ptr<SunCandidates>> pool;
		std::vector<LogCursor> poolCursor;  // parallel to pool: where each snapshot read log to
		// Every index written, in order (Reset invalidates it: every reader is brought up whole).
		EventLog<std::uint32_t> log;
		std::uint64_t logId = NextLogId();
		std::uint64_t published = 0;  // log's end at the last publication (the next run starts there)
		std::shared_ptr<const CandidateChanges> changes;  // the last publication's
		std::shared_ptr<const SunCandidates> last;        // the last publication (parity's reference; the coordinator holds it too)
		std::vector<std::uint8_t> marks;  // per index, Publish's scratch
		bool changed = true;                              // an index moved since the last snapshot
		std::uint64_t entriesWritten = 0, snapshots = 0;  // since the last report
		std::uint64_t entriesCaughtUp = 0, wholeCatchUps = 0;  // since the last report: indices a snapshot was brought up at; snapshots brought up index by index

		static std::uint64_t NextLogId();

		/** @brief Every index freed under a new version: the pooled snapshots drop their entries when brought up to it. */
		void Reset(std::uint32_t& a_versions);
		/**
		 * @brief a_root's entry as its dependents are (a_dependents; null: not a candidate): written again, its geometries with it,
		 * under a new version. a_geometry(geometry) gives a new geometry's object slot and primary verdict.
		 */
		template <class Geometry>
		void Set(const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, std::uint32_t& a_versions, Geometry&& a_geometry);
		/** @brief A snapshot, a new generation (a_generation), brought up to the table at the indices that moved; a_ownedRoot holds each entry. */
		std::shared_ptr<const SunCandidates> Publish(std::uint32_t& a_generation, const std::function<RE::NiPointer<RE::NiAVObject>(const RE::NiAVObject*)>& a_ownedRoot);
		/** @brief Parity: a_snapshot against the table index by index (what a snapshot made whole would hold); the first difference, or empty. */
		std::string CheckSnapshot(const SunCandidates& a_snapshot) const;
	};

	template <class Geometry>
	void CandidateTable::Set(const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, std::uint32_t& a_versions, Geometry&& a_geometry)
	{
		const auto found = index.find(a_root);
		if (!a_dependents && found == index.end())
			return;
		std::uint32_t e;
		if (found != index.end()) {
			e = found->second;
		} else if (!free.empty()) {
			e = free.back();
			free.pop_back();
		} else {
			e = static_cast<std::uint32_t>(nodes.size());
			nodes.push_back(nullptr);
			version.push_back(0);
			geometries.emplace_back();
		}
		// Its geometries let go (a geometry another entry lists too stays that entry's).
		for (const std::uint32_t g : geometries[e]) {
			if (const auto it = geometryIndex.find(geometryNodes[g]); it != geometryIndex.end() && it->second == g)
				geometryIndex.erase(it);
			geometryNodes[g] = nullptr;
			geometryEntry[g] = SunCandidates::kNone;
			geometryFree.push_back(g);
		}
		geometries[e].clear();
		version[e] = ++a_versions;
		log.Push(e);
		changed = true;
		++entriesWritten;
		if (!a_dependents) {
			index.erase(found);
			nodes[e] = nullptr;
			free.push_back(e);
			return;
		}
		nodes[e] = a_root;
		index.insert_or_assign(a_root, e);
		for (auto* geometry : *a_dependents) {
			if (geometryIndex.contains(geometry))
				continue;  // listed by another entry first
			std::uint32_t g;
			if (!geometryFree.empty()) {
				g = geometryFree.back();
				geometryFree.pop_back();
			} else {
				g = static_cast<std::uint32_t>(geometryNodes.size());
				geometryNodes.push_back(nullptr);
				geometryEntry.push_back(SunCandidates::kNone);
				geometrySlot.push_back(-1);
				primaryGeometry.push_back(0);
			}
			geometryIndex.emplace(geometry, g);
			geometryNodes[g] = geometry;
			geometryEntry[g] = e;
			std::tie(geometrySlot[g], primaryGeometry[g]) = a_geometry(geometry);
			geometries[e].push_back(g);
		}
	}
}
