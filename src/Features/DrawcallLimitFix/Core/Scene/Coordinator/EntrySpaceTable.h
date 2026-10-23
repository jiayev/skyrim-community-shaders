#pragma once

// The coordinator's stable index space for a candidate set (the sun's and the point lights' EntrySpace), and the pooled snapshots
// published from it. Coordinator thread only; a published snapshot is read by the frame's builds and released by its last owner.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Scene/EntrySpace.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

namespace DCLF::Scene
{
	/**
	 * @brief The entries' and their geometries' indices with their free lists, each entry's geometries and its version (moved by every
	 * change of the index, from a counter the caller shares between tables). Publish brings a pooled snapshot no reader holds up to the
	 * table at the indices whose versions moved.
	 *
	 * Every index Set or Reset writes is appended to log. Each pooled snapshot keeps a LogCursor into it and catches up by the indices
	 * it has not read; a snapshot whose cursor is no longer readable (new, or behind a Reset or the trim) is brought up index by index.
	 * Each publication also closes a run of the indices written since the last (EntrySpaceChanges), published with the snapshot.
	 *
	 * The lane brings up only one publication's run: after each publication a spare (the free pooled snapshot nearest the log, or a
	 * new one) is brought up to it on the preparation pool from the published snapshot and its runs, and the next publication takes
	 * the spare. An index rewritten under the same root keeps its map entries and its held root (no map churn for a rewrite).
	 */
	struct EntrySpaceTable
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
		/** @brief A pooled snapshot, where it read log to (the lane's), and the spare catch-up on the pool it may be under. */
		struct Pooled
		{
			/**
			 * @brief One catch-up on the pool, shared with its job (a new one per dispatch: a job that finds it claimed by the lane
			 * does nothing, so a stale job never runs a later dispatch's work).
			 */
			struct State
			{
				std::atomic<bool> claimed{ false };  // taken by the job or by the lane (whichever first runs the catch-up)
				std::atomic<bool> busy{ false };     // the catch-up is not done (released once it is: the snapshot's writes with it)
				std::uint64_t log = 0;               // the snapshot holds this log's generation below (0: none a run chains from)
				std::uint32_t generation = 0;
				std::uint32_t caughtUp = 0;  // indices the catch-up brought up (the lane counts them as it takes the spare)
			};
			std::shared_ptr<EntrySpace> snapshot = std::make_shared<EntrySpace>();
			LogCursor cursor;
			std::shared_ptr<State> state = std::make_shared<State>();
			// What the catch-up brings it up to, held by the lane until it is done (the job reads it by address).
			std::shared_ptr<const EntrySpace> source;
		};
		std::vector<Pooled> pool;
		// Every index written, in order (Reset invalidates it: every reader is brought up whole).
		EventLog<std::uint32_t> log;
		std::uint64_t logId = NextLogId();
		std::uint64_t published = 0;  // log's end at the last publication (the next run starts there)
		std::shared_ptr<const EntrySpaceChanges> changes;  // the last publication's
		std::shared_ptr<const EntrySpace> last;        // the last publication (parity's reference; the coordinator holds it too)
		std::vector<std::uint8_t> marks;  // per index, Publish's scratch
		bool changed = true;                              // an index moved since the last snapshot
		std::uint64_t entriesWritten = 0, snapshots = 0;  // since the last report
		std::uint64_t rowsTakenBack = 0;  // since the last report: entries written again for a geometry another let go of (released)
		std::uint64_t entriesCaughtUp = 0, wholeCatchUps = 0;  // since the last report: indices a snapshot was brought up at; snapshots brought up index by index
		// Since the last report, of entriesCaughtUp: indices rewritten under the same root (map entries and held root kept); spares
		// dispatched, the indices they brought up, those the lane found unstarted (and ran) or running (and waited for); the snapshots
		// the pool grew by.
		std::uint64_t entriesKept = 0, spareCatchUps = 0, spareEntries = 0, sparesRunByLane = 0, sparesWaited = 0, poolGrowths = 0;
		std::vector<std::uint8_t> setKept;        // per position, Set's scratch
		std::vector<std::uint32_t> setPrevious;  // Set's scratch: the entry's geometries before
		// The geometries Set let go of since the caller last took them (TakeReleased). One another entry lists has no row there when
		// that entry was written first (a geometry moved between entries in one update: the new entry's Set skipped it, still indexed
		// by the old): the caller writes that entry again.
		std::vector<const RE::BSGeometry*> released;

		static std::uint64_t NextLogId();

		/** @brief Every index freed under a new version: the pooled snapshots drop their entries when brought up to it. */
		void Reset(std::uint32_t& a_versions);
		/** @brief The geometries Set let go of since the last call that no entry holds a row for now. */
		std::vector<const RE::BSGeometry*> TakeReleased()
		{
			auto taken = std::exchange(released, {});
			std::erase_if(taken, [this](const RE::BSGeometry* a_geometry) { return geometryIndex.contains(a_geometry); });
			return taken;
		}
		/**
		 * @brief a_root's entry as its dependents are (a_dependents; null: not a candidate): written again, its geometries with it,
		 * under a new version. a_geometry(geometry) gives a new geometry's object slot and primary verdict.
		 */
		template <class Geometry>
		void Set(const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, std::uint32_t& a_versions, Geometry&& a_geometry);
		/** @brief A snapshot, a new generation (a_generation), brought up to the table at the indices that moved; a_ownedRoot holds each entry. */
		std::shared_ptr<const EntrySpace> Publish(std::uint32_t& a_generation, const std::function<RE::NiPointer<RE::NiAVObject>(const RE::NiAVObject*)>& a_ownedRoot);
		/** @brief Parity: a_snapshot against the table index by index (what a snapshot made whole would hold); the first difference, or empty. */
		std::string CheckSnapshot(const EntrySpace& a_snapshot) const;

		EntrySpaceTable() = default;
		EntrySpaceTable(const EntrySpaceTable&) = delete;
		EntrySpaceTable& operator=(const EntrySpaceTable&) = delete;
		/** @brief Every spare's catch-up finished first (its job reads the pool's snapshots by address). */
		~EntrySpaceTable();

	private:
		/** @brief After a publication (pool[a_published]): a spare brought up to last on the preparation pool (inline under Policy::SerialFanout). */
		void DispatchSpare(std::size_t a_published);
		/** @brief a_pooled's catch-up done: run here if its job has not started, else waited for (it is running). */
		void FinishSpare(Pooled& a_pooled);
	};

	template <class Geometry>
	void EntrySpaceTable::Set(const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, std::uint32_t& a_versions, Geometry&& a_geometry)
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
		// Its geometries: one the entry lists again at the same position keeps its index and its map entry (a rewrite of the same
		// geometries moves none of them; Publish writes only their rows); the others let go (a geometry another entry lists too stays
		// that entry's), and the new ones take indices.
		auto& list = geometries[e];
		setKept.assign(list.size(), 0);
		if (a_dependents)
			for (std::size_t k = 0, n = std::min(list.size(), a_dependents->size()); k < n; ++k)
				setKept[k] = geometryNodes[list[k]] == (*a_dependents)[k] ? 1 : 0;
		for (std::size_t k = 0; k < list.size(); ++k) {
			if (setKept[k])
				continue;
			const std::uint32_t g = list[k];
			if (const auto it = geometryIndex.find(geometryNodes[g]); it != geometryIndex.end() && it->second == g) {
				geometryIndex.erase(it);
				released.push_back(geometryNodes[g]);
			}
			geometryNodes[g] = nullptr;
			geometryEntry[g] = EntrySpace::kNone;
			geometryFree.push_back(g);
		}
		version[e] = ++a_versions;
		log.Push(e);
		changed = true;
		++entriesWritten;
		if (!a_dependents) {
			list.clear();
			index.erase(found);
			nodes[e] = nullptr;
			free.push_back(e);
			return;
		}
		nodes[e] = a_root;
		index.insert_or_assign(a_root, e);
		auto& previous = setPrevious;
		previous.swap(list);
		list.clear();
		for (std::size_t k = 0; k < a_dependents->size(); ++k) {
			auto* geometry = (*a_dependents)[k];
			std::uint32_t g;
			if (k < previous.size() && setKept[k]) {
				g = previous[k];
			} else {
				if (geometryIndex.contains(geometry))
					continue;  // listed by another entry first (or by this one, kept at another position)
				if (!geometryFree.empty()) {
					g = geometryFree.back();
					geometryFree.pop_back();
				} else {
					g = static_cast<std::uint32_t>(geometryNodes.size());
					geometryNodes.push_back(nullptr);
					geometryEntry.push_back(EntrySpace::kNone);
					geometrySlot.push_back(-1);
					primaryGeometry.push_back(0);
				}
				geometryIndex.emplace(geometry, g);
				geometryNodes[g] = geometry;
				geometryEntry[g] = e;
			}
			std::tie(geometrySlot[g], primaryGeometry[g]) = a_geometry(geometry);
			list.push_back(g);
		}
	}
}
