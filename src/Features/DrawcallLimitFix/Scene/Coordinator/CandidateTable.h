#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <vector>

#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"

namespace DCLF::Scene
{
	/**
	 * @brief A candidate set's stable index space (SunCandidates' indices), the coordinator's: the entries' and their geometries' indices
	 * with their free lists, each entry's geometries and its version (moved by every change of the index, from a counter the caller
	 * shares between tables). A snapshot is brought up to it at the indices whose versions moved (Publish), reusing one of the pool no
	 * reader holds any more.
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
		bool changed = true;                              // an index moved since the last snapshot
		std::uint64_t entriesWritten = 0, snapshots = 0;  // since the last report

		/** @brief Every index freed under a new version: the pooled snapshots drop their entries when brought up to it. */
		void Reset(std::uint32_t& a_versions);
		/**
		 * @brief a_root's entry as its dependents now are (a_dependents; null: not a candidate): written again, its geometries with it,
		 * under a new version. a_geometry(geometry) gives a new geometry's object slot and primary verdict.
		 */
		template <class Geometry>
		void Set(const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, std::uint32_t& a_versions, Geometry&& a_geometry);
		/** @brief A snapshot, a new generation (a_generation), brought up to the table at the indices that moved; a_ownedRoot holds each entry. */
		std::shared_ptr<const SunCandidates> Publish(std::uint32_t& a_generation, const std::function<RE::NiPointer<RE::NiAVObject>(const RE::NiAVObject*)>& a_ownedRoot);
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
