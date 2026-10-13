// CandidateTable's reset and snapshot publication (coordinator thread).

#include "Features/DrawcallLimitFix/Scene/Coordinator/CandidateTable.h"

#include "Features/DrawcallLimitFix/Runtime/EngineReleases.h"

#include <numeric>

namespace DCLF::Scene
{
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
		changed = true;
	}

	std::shared_ptr<const SunCandidates> CandidateTable::Publish(std::uint32_t& a_generation, const std::function<RE::NiPointer<RE::NiAVObject>(const RE::NiAVObject*)>& a_ownedRoot)
	{
		ZoneScopedN("CS.DCLF.Scene.SunCandidates.Snapshot");
		// A pooled snapshot no reader holds (the pool's own reference only), else a new one.
		std::shared_ptr<SunCandidates> snapshot;
		for (const auto& pooled : pool)
			if (pooled.use_count() == 1) {
				snapshot = pooled;
				break;
			}
		if (!snapshot)
			snapshot = pool.emplace_back(std::make_shared<SunCandidates>());
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
		// The indices whose versions moved since this snapshot was brought up last: each let go first (a geometry index can pass from
		// one entry to another among them), then written as the table has it.
		std::vector<std::uint32_t> moved;
		for (std::uint32_t e = 0; e < span; ++e)
			if (s.entryVersion[e] != (e < capacity ? version[e] : 0u))
				moved.push_back(e);
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
		changed = false;
		++snapshots;
		return snapshot;
	}
}
