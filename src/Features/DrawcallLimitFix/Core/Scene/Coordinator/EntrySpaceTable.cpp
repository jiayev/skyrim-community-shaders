// EntrySpaceTable's reset and snapshot publication (coordinator thread). The readers of its published log are Core/Scene/EntrySpace's.

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/EntrySpaceTable.h"

#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <Tracy/Tracy.hpp>

namespace DCLF::Scene
{
	namespace
	{
		// The log and the published runs keep at most this many indices (a reader behind them is brought up index by index, the
		// journals' contract); the runs at most this many generations.
		constexpr std::size_t kMaxLogEntries = 1u << 16;
		constexpr std::size_t kMaxRuns = 64;

		/** @brief What a snapshot is brought up to: the table's arrays, or a published snapshot's (the same layout). */
		struct CandidateRows
		{
			const std::vector<const RE::NiAVObject*>& nodes;
			const std::vector<std::uint32_t>& version;
			const std::vector<std::vector<std::uint32_t>>& geometries;
			const std::vector<const RE::BSGeometry*>& geometryNodes;
			const std::vector<std::uint32_t>& geometryEntry;
			const std::vector<std::int32_t>& geometrySlot;
			const std::vector<std::uint8_t>& primaryGeometry;
		};

		/**
		 * @brief a_s brought up to a_rows at the indices a_moved names (each once; every index whose version differs among them),
		 * a_held(index, root) holding a root it newly lists. Each index lets go first (a geometry index can pass from one entry to
		 * another among them), then is written as a_rows has it. An index still under the same root keeps its map entry and its held
		 * root (counted in a_kept), and a geometry still the same entry's at the same index keeps its map entry: a rewrite writes rows,
		 * not maps.
		 */
		template <class Held>
		void BringUp(EntrySpace& a_s, const CandidateRows& a_rows, const std::vector<std::uint32_t>& a_moved, Held&& a_held, std::uint64_t& a_kept)
		{
			auto& s = a_s;
			const std::uint32_t capacity = static_cast<std::uint32_t>(a_rows.nodes.size());
			const std::uint32_t geometryCapacity = static_cast<std::uint32_t>(a_rows.geometryNodes.size());
			const std::uint32_t span = std::max(capacity, s.Capacity());
			s.entryNodes.resize(span, nullptr);
			s.entryVersion.resize(span, 0);
			s.held.resize(span);
			s.entryGeometries.resize(span);
			s.geometryNodes.resize(std::max(geometryCapacity, s.GeometryCapacity()), nullptr);
			s.geometryEntry.resize(s.geometryNodes.size(), EntrySpace::kNone);
			s.geometrySlot.resize(s.geometryNodes.size(), -1);
			s.primaryGeometry.resize(s.geometryNodes.size(), 0);
			for (const std::uint32_t e : a_moved) {
				const auto* root = e < capacity ? a_rows.nodes[e] : nullptr;
				const bool same = root && s.entryNodes[e] == root;
				if (!same)
					if (const auto* old = s.entryNodes[e])
						s.entries.erase(old);
				for (const std::uint32_t g : s.entryGeometries[e]) {
					if (g >= s.geometryNodes.size() || s.geometryEntry[g] != e)
						continue;
					if (g < geometryCapacity && a_rows.geometryEntry[g] == e && a_rows.geometryNodes[g] == s.geometryNodes[g])
						continue;  // stays: the same geometry, the same entry's
					if (const auto it = s.geometries.find(s.geometryNodes[g]); it != s.geometries.end() && it->second == g)
						s.geometries.erase(it);
					s.geometryNodes[g] = nullptr;
					s.geometryEntry[g] = EntrySpace::kNone;
				}
				if (same) {
					++a_kept;
					continue;
				}
				s.entryNodes[e] = nullptr;
				if (s.held[e])
					EngineReleases::Push(std::move(s.held[e]));
			}
			for (const std::uint32_t e : a_moved) {
				s.entryVersion[e] = e < capacity ? a_rows.version[e] : 0u;
				const auto* root = e < capacity ? a_rows.nodes[e] : nullptr;
				if (!root) {
					s.entryGeometries[e].clear();
					continue;
				}
				if (s.entryNodes[e] != root) {
					s.entryNodes[e] = root;
					s.entries.insert_or_assign(root, e);
					s.held[e] = a_held(e, root);
				}
				s.entryGeometries[e] = a_rows.geometries[e];
				for (const std::uint32_t g : a_rows.geometries[e]) {
					const bool listed = s.geometryNodes[g] == a_rows.geometryNodes[g] && s.geometryEntry[g] == e;
					s.geometryNodes[g] = a_rows.geometryNodes[g];
					s.geometryEntry[g] = e;
					s.geometrySlot[g] = a_rows.geometrySlot[g];
					s.primaryGeometry[g] = a_rows.primaryGeometry[g];
					if (!listed)
						s.geometries.insert_or_assign(a_rows.geometryNodes[g], g);
				}
			}
			s.entryNodes.resize(capacity);
			s.entryVersion.resize(capacity);
			s.held.resize(capacity);
			s.entryGeometries.resize(capacity);
			s.geometryNodes.resize(geometryCapacity);
			s.geometryEntry.resize(geometryCapacity);
			s.geometrySlot.resize(geometryCapacity);
			s.primaryGeometry.resize(geometryCapacity);
		}

		/**
		 * @brief A spare (preparation pool; nothing else writes a_spare meanwhile) brought up to a_source, a published snapshot: by the
		 * source's runs after the spare's own generation of the same log (a_state.log, generation), else by comparing every index. Free
		 * indices included: a version moves when its index is freed, and the next catch-up compares versions.
		 */
		void CatchUpSpare(EntrySpace& a_spare, const EntrySpace& a_source, Scene::EntrySpaceTable::Pooled::State& a_state)
		{
			ZoneScopedN("CS.DCLF.Scene.SunCandidates.Spare");
			const auto& s = a_spare;
			const std::uint32_t span = std::max(s.Capacity(), a_source.Capacity());
			auto differs = [&](std::uint32_t e) {
				return (e < s.Capacity() ? s.entryVersion[e] : 0u) != (e < a_source.Capacity() ? a_source.entryVersion[e] : 0u);
			};
			std::vector<std::uint32_t> moved;
			if (a_source.changes && a_source.changes->Reaches(a_state.log, a_state.generation)) {
				for (const auto& run : a_source.changes->runs)
					if (run->generation > a_state.generation)
						moved.insert(moved.end(), run->entries.begin(), run->entries.end());
				std::sort(moved.begin(), moved.end());
				moved.erase(std::unique(moved.begin(), moved.end()), moved.end());
				std::erase_if(moved, [&](std::uint32_t e) { return e >= span || !differs(e); });
			} else {
				for (std::uint32_t e = 0; e < span; ++e)
					if (differs(e))
						moved.push_back(e);
			}
			std::uint64_t kept = 0;
			BringUp(a_spare,
				CandidateRows{ a_source.entryNodes, a_source.entryVersion, a_source.entryGeometries, a_source.geometryNodes, a_source.geometryEntry, a_source.geometrySlot,
					a_source.primaryGeometry },
				moved, [&](std::uint32_t a_index, const RE::NiAVObject*) { return a_source.held[a_index]; }, kept);
			a_state.log = a_source.changes ? a_source.changes->log : 0;
			a_state.generation = a_source.generation;
			a_state.caughtUp = static_cast<std::uint32_t>(moved.size());
		}
	}

	std::uint64_t EntrySpaceTable::NextLogId()
	{
		static std::atomic<std::uint64_t> ids{ 0 };
		return ids.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	void EntrySpaceTable::Reset(std::uint32_t& a_versions)
	{
		released.clear();
		for (std::uint32_t e = 0; e < nodes.size(); ++e)
			version[e] = ++a_versions;
		index.clear();
		std::fill(nodes.begin(), nodes.end(), nullptr);
		for (auto& entry : geometries)
			entry.clear();
		free.resize(nodes.size());
		std::iota(free.rbegin(), free.rend(), 0u);
		geometryIndex.clear();
		std::fill(geometryNodes.begin(), geometryNodes.end(), nullptr);
		std::fill(geometryEntry.begin(), geometryEntry.end(), EntrySpace::kNone);
		geometryFree.resize(geometryNodes.size());
		std::iota(geometryFree.rbegin(), geometryFree.rend(), 0u);
		// Every index moved: the pooled snapshots are brought up whole, and the published log starts again (no reader chains across).
		log.Invalidate();
		logId = NextLogId();
		changed = true;
	}

	std::shared_ptr<const EntrySpace> EntrySpaceTable::Publish(std::uint32_t& a_generation, const std::function<RE::NiPointer<RE::NiAVObject>(const RE::NiAVObject*)>& a_ownedRoot)
	{
		ZoneScopedN("CS.DCLF.Scene.SunCandidates.Snapshot");
		// A pooled snapshot no reader holds (the pool's own reference only), the one with the least of the log to read: the spare,
		// brought up to the last publication (finished here if the pool has not yet), else a new one.
		std::size_t at = pool.size();
		for (std::size_t i = 0; i < pool.size(); ++i) {
			const auto& p = pool[i];
			if (p.snapshot.use_count() != 1)
				continue;
			if (at == pool.size()) {
				at = i;
				continue;
			}
			const auto& best = pool[at].cursor;
			if (p.cursor.Continues(log, 0) && (!best.Continues(log, 0) || p.cursor.position > best.position))
				at = i;
		}
		if (at == pool.size()) {
			pool.emplace_back();
			++poolGrowths;
		}
		auto& pooled = pool[at];
		FinishSpare(pooled);
		spareEntries += std::exchange(pooled.state->caughtUp, 0u);
		pooled.source.reset();
		const std::shared_ptr<EntrySpace> snapshot = pooled.snapshot;
		auto& cursor = pooled.cursor;
		auto& s = *snapshot;
		const std::uint32_t capacity = static_cast<std::uint32_t>(nodes.size());
		const std::uint32_t span = std::max(capacity, s.Capacity());
		// The indices whose versions moved since this snapshot was brought up last: those the log names since its cursor, or every
		// index when the cursor is not readable.
		std::vector<std::uint32_t> moved;
		auto differs = [&](std::uint32_t e) { return (e < s.Capacity() ? s.entryVersion[e] : 0u) != (e < capacity ? version[e] : 0u); };
		if (cursor.Continues(log, 0)) {
			marks.resize(std::max<std::size_t>(marks.size(), span), 0);
			const auto unread = cursor.Unread(log);
			for (const std::uint32_t e : unread)
				if (e < span && !std::exchange(marks[e], std::uint8_t{ 1 }) && differs(e))
					moved.push_back(e);
			for (const std::uint32_t e : unread)
				if (e < span)
					marks[e] = 0;
		} else {
			for (std::uint32_t e = 0; e < span; ++e)
				if (differs(e))
					moved.push_back(e);
			++wholeCatchUps;
		}
		entriesCaughtUp += moved.size();
		BringUp(s, CandidateRows{ nodes, version, geometries, geometryNodes, geometryEntry, geometrySlot, primaryGeometry }, moved,
			[&](std::uint32_t, const RE::NiAVObject* a_root) { return a_ownedRoot(a_root); }, entriesKept);
		s.generation = ++a_generation;
		cursor.Restart(0);
		cursor.Advance(log);

		// The run of indices written since the last publication, published with the earlier runs; with none to chain to (the first
		// publication, or after a Reset) the log starts at this generation.
		auto next = std::make_shared<EntrySpaceChanges>();
		next->log = logId;
		next->generation = s.generation;
		if (changes && changes->log == logId && log.Readable(published)) {
			auto run = std::make_shared<EntrySpaceChanges::Run>();
			run->generation = s.generation;
			marks.resize(std::max<std::size_t>(marks.size(), capacity), 0);
			for (const std::uint32_t e : log.From(published))
				if (e < capacity && !std::exchange(marks[e], std::uint8_t{ 1 }))
					run->entries.push_back(e);
			for (const std::uint32_t e : run->entries)
				marks[e] = 0;
			std::sort(run->entries.begin(), run->entries.end());
			// The earlier runs, newest back, while within the bounds; the floor moves to the newest left out.
			next->floor = changes->floor;
			std::size_t total = run->entries.size(), from = 0;
			for (std::size_t k = changes->runs.size(); k-- > 0;) {
				if (changes->runs.size() - k >= kMaxRuns || total + changes->runs[k]->entries.size() > kMaxLogEntries) {
					from = k + 1;
					next->floor = changes->runs[k]->generation;
					break;
				}
				total += changes->runs[k]->entries.size();
			}
			next->runs.reserve(changes->runs.size() - from + 1);
			next->runs.insert(next->runs.end(), changes->runs.begin() + std::ptrdiff_t(from), changes->runs.end());
			next->runs.push_back(std::move(run));
		} else {
			next->floor = s.generation;
		}
		published = log.End();
		changes = next;
		s.changes = std::move(next);
		pooled.state->log = logId;
		pooled.state->generation = s.generation;
		// The log keeps what a pooled snapshot has not read (one behind the bound is brought up whole).
		std::uint64_t oldest = log.End();
		for (const auto& p : pool)
			if (p.cursor.Continues(log, 0))
				oldest = std::min(oldest, p.cursor.position);
		if (const auto drop = static_cast<std::size_t>(oldest - log.base)) {
			log.entries.erase(log.entries.begin(), log.entries.begin() + std::ptrdiff_t(drop));
			log.base = oldest;
		}
		log.Trim(kMaxLogEntries);
		last = snapshot;
		changed = false;
		++snapshots;
		DispatchSpare(at);
		return snapshot;
	}

	EntrySpaceTable::~EntrySpaceTable()
	{
		for (auto& p : pool)
			FinishSpare(p);
	}

	void EntrySpaceTable::FinishSpare(Pooled& a_pooled)
	{
		auto& state = *a_pooled.state;
		if (!state.busy.load(std::memory_order_acquire))
			return;
		if (!state.claimed.exchange(true, std::memory_order_acq_rel)) {
			// Not started (the pool is busy): the lane runs it, the job finding it claimed.
			CatchUpSpare(*a_pooled.snapshot, *a_pooled.source, state);
			state.busy.store(false, std::memory_order_relaxed);
			++sparesRunByLane;
			return;
		}
		// Running on a worker: waited for, as ParallelFor waits for a claimed chunk.
		state.busy.wait(true, std::memory_order_acquire);
		++sparesWaited;
	}

	void EntrySpaceTable::DispatchSpare(std::size_t a_published)
	{
		// The free pooled snapshot nearest the log (else a new one), brought up to this publication on the pool while the lane goes on:
		// the next publication then reads one run on the lane, not every run since the spare's own publication (snapshots held in
		// turn) nor the whole table (a snapshot the pool grew by).
		std::size_t spare = pool.size();
		for (std::size_t i = 0; i < pool.size(); ++i) {
			const auto& p = pool[i];
			if (i == a_published || p.snapshot.use_count() != 1 || p.state->busy.load(std::memory_order_acquire))
				continue;
			if (spare == pool.size()) {
				spare = i;
				continue;
			}
			const auto& best = pool[spare].cursor;
			if (p.cursor.Continues(log, 0) && (!best.Continues(log, 0) || p.cursor.position > best.position))
				spare = i;
		}
		if (spare == pool.size()) {
			pool.emplace_back();
			++poolGrowths;
		}
		auto& p = pool[spare];
		// No reader's earlier generation names it while the pool writes it (parity keeps weak references by generation); the lane
		// writes its generation again as it publishes it. Its cursor is where this publication left the log, the state it will hold.
		p.snapshot->generation = 0;
		p.cursor.Restart(0);
		p.cursor.Advance(log);
		p.source = last;
		auto next = std::make_shared<Pooled::State>();
		next->log = p.state->log;
		next->generation = p.state->generation;
		next->busy.store(true, std::memory_order_relaxed);
		p.state = next;
		++spareCatchUps;
		// The job reads the snapshots by address: the pool holds the spare, and p.source the source, until the lane finishes it.
		auto job = [spareSnapshot = p.snapshot.get(), source = p.source.get(), state = std::move(next)]() {
			if (state->claimed.exchange(true, std::memory_order_acq_rel))
				return;
			CatchUpSpare(*spareSnapshot, *source, *state);
			state->busy.store(false, std::memory_order_release);
			state->busy.notify_all();
		};
		// Parity runs keep it on the pass's thread (Policy::SerialFanout, as the fan-outs do); a refused job runs here too.
		if (Policy::SerialFanout() || !SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
										  "DCLF candidate spare", [job](const auto&) { job(); }))
			job();
	}

	std::string EntrySpaceTable::CheckSnapshot(const EntrySpace& a_snapshot) const
	{
		const auto& s = a_snapshot;
		if (s.Capacity() != nodes.size() || s.GeometryCapacity() != geometryNodes.size())
			return fmt::format("{} entries, {} geometries; the table {}, {}", s.Capacity(), s.GeometryCapacity(), nodes.size(), geometryNodes.size());
		if (s.entries.size() != index.size())
			return fmt::format("{} live entries, the table {}", s.entries.size(), index.size());
		for (std::uint32_t e = 0; e < nodes.size(); ++e) {
			if (s.entryVersion[e] != version[e] || s.entryNodes[e] != nodes[e])
				return fmt::format("entry {} at version {} ({}), the table's {} ({})", e, s.entryVersion[e], static_cast<const void*>(s.entryNodes[e]), version[e],
					static_cast<const void*>(nodes[e]));
			if (!nodes[e])
				continue;
			if (const auto it = s.entries.find(nodes[e]); it == s.entries.end() || it->second != e)
				return fmt::format("entry {} not listed", e);
			if (s.entryGeometries[e] != geometries[e])
				return fmt::format("entry {} with {} geometries, the table's {}", e, s.entryGeometries[e].size(), geometries[e].size());
		}
		for (std::uint32_t g = 0; g < geometryNodes.size(); ++g) {
			if (s.geometryNodes[g] != geometryNodes[g] || s.geometryEntry[g] != geometryEntry[g])
				return fmt::format("geometry {} of entry {}, the table's of {}", g, s.geometryEntry[g], geometryEntry[g]);
			if (!geometryNodes[g])
				continue;
			if (s.geometrySlot[g] != geometrySlot[g] || s.primaryGeometry[g] != primaryGeometry[g])
				return fmt::format("geometry {} at slot {} verdict {}, the table's {} {}", g, s.geometrySlot[g], s.primaryGeometry[g], geometrySlot[g], primaryGeometry[g]);
			if (const auto it = s.geometries.find(geometryNodes[g]); it == s.geometries.end() || it->second != g)
				return fmt::format("geometry {} not listed", g);
		}
		return {};
	}
}
