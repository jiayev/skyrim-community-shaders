// CandidateTable's reset and snapshot publication (coordinator thread), and the readers of its published log (any thread).

#include "Features/DrawcallLimitFix/Scene/Coordinator/CandidateTable.h"

#include "Features/DrawcallLimitFix/Runtime/EngineReleases.h"

#include <algorithm>
#include <atomic>
#include <numeric>

namespace DCLF
{
	void ChangedEntriesByScan(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out)
	{
		if (a_from == &a_to)
			return;
		const std::uint32_t to = a_to.Capacity(), from = a_from ? a_from->Capacity() : 0u;
		for (std::uint32_t e = 0, end = std::max(to, from); e < end; ++e) {
			const bool inFrom = e < from && a_from->entryNodes[e];
			const bool inTo = e < to && a_to.entryNodes[e];
			if ((inFrom || inTo) && (inFrom != inTo || a_from->entryVersion[e] != a_to.entryVersion[e]))
				a_out.push_back(e);
		}
	}

	bool ChangedEntriesByLog(const SunCandidates* a_from, const SunCandidates& a_to, std::vector<std::uint32_t>& a_out)
	{
		if (a_from == &a_to)
			return true;
		if (!a_from || !a_from->changes || !a_to.changes || !a_to.changes->Reaches(a_from->changes->log, a_from->generation)) {
			ChangedEntriesByScan(a_from, a_to, a_out);
			return false;
		}
		// The runs after a_from's generation, merged; then the indices whose versions differ (an index written and freed again
		// between them is not a change).
		const std::size_t first = a_out.size();
		std::size_t runs = 0;
		for (const auto& run : a_to.changes->runs)
			if (run->generation > a_from->generation) {
				a_out.insert(a_out.end(), run->entries.begin(), run->entries.end());
				++runs;
			}
		if (runs > 1) {
			std::sort(a_out.begin() + std::ptrdiff_t(first), a_out.end());
			a_out.erase(std::unique(a_out.begin() + std::ptrdiff_t(first), a_out.end()), a_out.end());
		}
		const std::uint32_t to = a_to.Capacity(), from = a_from->Capacity();
		a_out.erase(std::remove_if(a_out.begin() + std::ptrdiff_t(first), a_out.end(),
						[&](std::uint32_t e) {
							const bool inFrom = e < from && a_from->entryNodes[e];
							const bool inTo = e < to && a_to.entryNodes[e];
							return !((inFrom || inTo) && (inFrom != inTo || a_from->entryVersion[e] != a_to.entryVersion[e]));
						}),
			a_out.end());
		return true;
	}
}

namespace DCLF::Scene
{
	namespace
	{
		// The log and the published runs keep at most this many indices (a reader behind them is brought up index by index, the
		// journals' contract); the runs at most this many generations.
		constexpr std::size_t kMaxLogEntries = 1u << 16;
		constexpr std::size_t kMaxRuns = 64;
	}

	std::uint64_t CandidateTable::NextLogId()
	{
		static std::atomic<std::uint64_t> ids{ 0 };
		return ids.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	void CandidateTable::Reset(std::uint32_t& a_versions)
	{
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
		std::fill(geometryEntry.begin(), geometryEntry.end(), SunCandidates::kNone);
		geometryFree.resize(geometryNodes.size());
		std::iota(geometryFree.rbegin(), geometryFree.rend(), 0u);
		// Every index moved: the pooled snapshots are brought up whole, and the published log starts again (no reader chains across).
		log.Invalidate();
		logId = NextLogId();
		changed = true;
	}

	std::shared_ptr<const SunCandidates> CandidateTable::Publish(std::uint32_t& a_generation, const std::function<RE::NiPointer<RE::NiAVObject>(const RE::NiAVObject*)>& a_ownedRoot)
	{
		ZoneScopedN("CS.DCLF.Scene.SunCandidates.Snapshot");
		// A pooled snapshot no reader holds (the pool's own reference only), else a new one.
		std::size_t at = pool.size();
		for (std::size_t i = 0; i < pool.size(); ++i)
			if (pool[i].use_count() == 1) {
				at = i;
				break;
			}
		if (at == pool.size()) {
			pool.emplace_back(std::make_shared<SunCandidates>());
			poolCursor.emplace_back();
		}
		const std::shared_ptr<SunCandidates> snapshot = pool[at];
		auto& cursor = poolCursor[at];
		auto& s = *snapshot;
		const std::uint32_t capacity = static_cast<std::uint32_t>(nodes.size());
		const std::uint32_t geometryCapacity = static_cast<std::uint32_t>(geometryNodes.size());
		const std::uint32_t span = std::max(capacity, s.Capacity());
		s.entryNodes.resize(span, nullptr);
		s.entryVersion.resize(span, 0);
		s.held.resize(span);
		s.entryGeometries.resize(span);
		s.geometryNodes.resize(std::max(geometryCapacity, s.GeometryCapacity()), nullptr);
		s.geometryEntry.resize(s.geometryNodes.size(), SunCandidates::kNone);
		s.geometrySlot.resize(s.geometryNodes.size(), -1);
		s.primaryGeometry.resize(s.geometryNodes.size(), 0);
		// The indices whose versions moved since this snapshot was brought up last - those the log names since its cursor, or every
		// index when the cursor is not readable - each let go first (a geometry index can pass from one entry to another among
		// them), then written as the table has it.
		std::vector<std::uint32_t> moved;
		auto differs = [&](std::uint32_t e) { return s.entryVersion[e] != (e < capacity ? version[e] : 0u); };
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
		for (const std::uint32_t e : moved) {
			if (const auto* old = s.entryNodes[e])
				s.entries.erase(old);
			for (const std::uint32_t g : s.entryGeometries[e]) {
				if (g >= s.geometryNodes.size() || s.geometryEntry[g] != e)
					continue;
				if (const auto it = s.geometries.find(s.geometryNodes[g]); it != s.geometries.end() && it->second == g)
					s.geometries.erase(it);
				s.geometryNodes[g] = nullptr;
				s.geometryEntry[g] = SunCandidates::kNone;
			}
			s.entryGeometries[e].clear();
			s.entryNodes[e] = nullptr;
			if (s.held[e])
				EngineReleases::Push(std::move(s.held[e]));
		}
		for (const std::uint32_t e : moved) {
			s.entryVersion[e] = e < capacity ? version[e] : 0u;
			const auto* root = e < capacity ? nodes[e] : nullptr;
			if (!root)
				continue;
			s.entryNodes[e] = root;
			s.entries.insert_or_assign(root, e);
			s.held[e] = a_ownedRoot(root);
			s.entryGeometries[e] = geometries[e];
			for (const std::uint32_t g : geometries[e]) {
				s.geometryNodes[g] = geometryNodes[g];
				s.geometryEntry[g] = e;
				s.geometrySlot[g] = geometrySlot[g];
				s.primaryGeometry[g] = primaryGeometry[g];
				s.geometries.insert_or_assign(geometryNodes[g], g);
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
		s.generation = ++a_generation;
		cursor.Restart(0);
		cursor.Advance(log);

		// The run of indices written since the last publication, published with the earlier runs; with none to chain to (the first
		// publication, or after a Reset) the log starts at this generation.
		auto next = std::make_shared<CandidateChanges>();
		next->log = logId;
		next->generation = s.generation;
		if (changes && changes->log == logId && log.Readable(published)) {
			auto run = std::make_shared<CandidateChanges::Run>();
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
		// The log keeps what a pooled snapshot has not read (one behind the bound is brought up whole).
		std::uint64_t oldest = log.End();
		for (const auto& c : poolCursor)
			if (c.Continues(log, 0))
				oldest = std::min(oldest, c.position);
		if (const auto drop = static_cast<std::size_t>(oldest - log.base)) {
			log.entries.erase(log.entries.begin(), log.entries.begin() + std::ptrdiff_t(drop));
			log.base = oldest;
		}
		log.Trim(kMaxLogEntries);
		last = snapshot;
		changed = false;
		++snapshots;
		return snapshot;
	}

	std::string CandidateTable::CheckSnapshot(const SunCandidates& a_snapshot) const
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
