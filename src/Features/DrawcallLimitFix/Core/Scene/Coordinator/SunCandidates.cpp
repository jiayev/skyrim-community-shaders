// The coordinator's sun and point-light candidate sets (scene lane, verdicts across the preparation pool): the dirty entries judged,
// written into their EntrySpaceTable, and a snapshot published for the shadow builds when an entry moved.

#include "Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"

#include <map>
#include <numeric>

namespace DCLF::Scene
{
	namespace
	{
		/**
		 * @brief a_verdict(i) for every dirty entry, into a_out: across the preparation pool from the scene task (the verdicts read
		 * the coordinator's state, which nothing writes meanwhile), else on the calling thread; each chunk under a_scope() (the pass's
		 * ShardScope, as a worker's).
		 */
		template <class Scope, class Verdict>
		void DirtyVerdicts(bool a_parallel, std::size_t a_count, std::vector<std::pair<bool, std::uint64_t>>& a_out, Scope&& a_scope, Verdict&& a_verdict)
		{
			a_out.resize(a_count);
			auto run = [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.Scene.SunCandidates.Verdicts");
				const auto scope = a_scope();
				for (std::size_t i = a_begin; i < a_end; ++i)
					a_out[i] = a_verdict(i);
			};
			if (a_parallel)
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.SunCandidates.Verdicts", a_count, 128, run);
			else
				run(0, a_count);
		}

		/**
		 * @brief After an update's writes: each geometry a_table let go of that no entry holds a row for, while its root (a_rootOf) is an
		 * entry of the table, gives that entry a row: the entry is written again (a_write). Again until none is left: a rewrite lets go
		 * only of the rows it no longer lists.
		 */
		template <class RootOf, class Write>
		void WriteReleasedRoots(EntrySpaceTable& a_table, RootOf&& a_rootOf, Write&& a_write)
		{
			for (auto released = a_table.TakeReleased(); !released.empty(); released = a_table.TakeReleased()) {
				std::vector<const RE::NiAVObject*> roots;
				for (const auto* geometry : released)
					if (const auto* root = a_rootOf(geometry); root && a_table.index.contains(root))
						roots.push_back(root);
				std::sort(roots.begin(), roots.end());
				roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
				a_table.rowsTakenBack += roots.size();
				for (const auto* root : roots)
					a_write(root);
			}
		}
	}

	void Coordinator::DropSunCandidates()
	{
		sunCandidateSet.clear();
		primarySignature.clear();
		sunEntriesDirty.clear();
		sunTable.Reset(candidateVersions);
		sunCandidates.reset();
		++sunCandidatesGeneration;
		lightCandidateSet.clear();
		lightSignature.clear();
		lightEntriesDirty.clear();
		lightTable.Reset(candidateVersions);
		lightCandidates.reset();
		++lightCandidatesGeneration;
	}

	std::uint8_t Coordinator::PrimaryVerdictOf(RE::BSGeometry* a_geometry) const
	{
		const auto entry = tracked.find(a_geometry);
		return entry != tracked.end() ? Eligibility::PrimaryVerdict(VerdictOf(entry->second), [&] { return mirror.Leaf(a_geometry); }) : Eligibility::kPrimaryEngine;
	}

	const std::vector<RE::BSGeometry*>* Coordinator::LightDependentsOf(const RE::NiAVObject* a_root) const
	{
		const auto it = lightDependents.find(a_root);
		if (it == lightDependents.end() || it->second.empty() || CategoryNodeOwn(a_root))
			return nullptr;
		return &it->second;
	}

	void Coordinator::UpdateLightCandidates(bool a_full)
	{
		bool changed = a_full;
		if (a_full) {
			lightTable.Reset(candidateVersions);
			lightCandidateSet.clear();
			lightSignature.clear();
			lightEntriesDirty.clear();
			for (const auto& [root, dependents] : lightDependents)
				lightEntriesDirty.push_back(root);
		}
		if (!lightEntriesDirty.empty()) {
			std::sort(lightEntriesDirty.begin(), lightEntriesDirty.end());
			lightEntriesDirty.erase(std::unique(lightEntriesDirty.begin(), lightEntriesDirty.end()), lightEntriesDirty.end());
			const bool switchNodes = Extensions::ParentAdmits(ActiveToggles(), Api::EngineParent::Switch, Api::ParentLeaves::SelectedLeaves);
			std::vector<std::pair<bool, std::uint64_t>> verdicts;
			DirtyVerdicts(inSceneTask, lightEntriesDirty.size(), verdicts, [this] { return ShardScope(*this, true); }, [&](std::size_t a_index) {
				const auto* dependents = LightDependentsOf(lightEntriesDirty[a_index]);
				bool candidate = dependents != nullptr;
				std::uint64_t signature = 0;
				if (dependents)
					for (auto* geometry : *dependents) {
						const auto entry = tracked.find(geometry);
						if (entry == tracked.end() || !Eligibility::Light(VerdictOf(entry->second), switchNodes, [&] { return mirror.Leaf(geometry); })) {
							candidate = false;
							break;
						}
						Fnv1a member;
						member.Mix(reinterpret_cast<std::uintptr_t>(geometry));
						member.Mix(entry->second.slot);
						signature += member.value;
					}
				return std::pair{ candidate, signature };
			});
			for (std::size_t i = 0; i < lightEntriesDirty.size(); ++i) {
				const auto* root = lightEntriesDirty[i];
				const auto [candidate, signature] = verdicts[i];
				bool written = false;
				if (candidate ? lightCandidateSet.insert(root).second : lightCandidateSet.erase(root) != 0) {
					changed = true;
					written = true;
				}
				if (candidate) {
					const auto [slot, inserted] = lightSignature.try_emplace(root, signature);
					if (!inserted && slot->second != signature) {
						slot->second = signature;
						changed = true;
						written = true;
					}
				} else {
					lightSignature.erase(root);
				}
				// Its entry, written again: its index's version moves (the readers take it entry by entry).
				if (written)
					lightTable.Set(root, candidate ? LightDependentsOf(root) : nullptr, candidateVersions, [&](RE::BSGeometry* a_geometry) { return std::pair{ FindObject(a_geometry), std::uint8_t{ 0 } }; });
			}
			lightEntriesDirty.clear();
		}
		// A geometry moved between entries in this update, its new entry written before its old one let go of it: the new one again.
		WriteReleasedRoots(
			lightTable, [this](const RE::BSGeometry* a_geometry) {
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
				return it != tracked.end() ? it->second.lightRoot : nullptr;
			},
			[this](const RE::NiAVObject* a_root) {
				lightTable.Set(a_root, LightDependentsOf(a_root), candidateVersions, [&](RE::BSGeometry* a_geometry) { return std::pair{ FindObject(a_geometry), std::uint8_t{ 0 } }; });
			});
		// A snapshot for every walk that moved an entry (and the first): the readers bring themselves up to it entry by entry.
		if (lightTable.changed || !lightCandidates)
			lightCandidates = lightTable.Publish(lightCandidatesGeneration, [this](const RE::NiAVObject* a_root) { return OwnedRoot(a_root); });
		Diagnostics::Notify(Diagnostics::CandidatesUpdated{ *this, false });
		(void)changed;
	}

	std::string Coordinator::DescribeSunCandidate(const void* a_geometry) const
	{
		const auto it = tracked.find(static_cast<RE::BSGeometry*>(const_cast<void*>(a_geometry)));
		if (it == tracked.end() || !it->second.geometry)
			return "not tracked";
		const auto& entry = it->second;
		const auto leaf = mirror.Leaf(entry.geometry.get());
		const auto* rtti = leaf.property ? static_cast<const RE::NiRTTI*>(leaf.property->rtti) : nullptr;
		return fmt::format("{}, {} (classified at frame {}), {} property, caster rule {}", entry.slot != kNoObjectSlot ? "record" : "no record",
			kIneligibleNames[static_cast<std::size_t>(entry.candidateReason)], entry.candidateFrame, rtti ? rtti->name : "no", ShadowRejectName(ShadowCasterReject(leaf)));
	}

	void Coordinator::UpdateSunCandidates(bool a_full)
	{
		bool changed = a_full;
		if (a_full) {
			sunTable.Reset(candidateVersions);
			sunCandidateSet.clear();
			primarySignature.clear();
			sunEntriesDirty.clear();
			for (const auto& [root, dependents] : rootDependents)
				sunEntriesDirty.push_back(root);
		}
		if (!sunEntriesDirty.empty()) {
			std::sort(sunEntriesDirty.begin(), sunEntriesDirty.end());
			sunEntriesDirty.erase(std::unique(sunEntriesDirty.begin(), sunEntriesDirty.end()), sunEntriesDirty.end());
			const bool switchNodes = Extensions::ParentAdmits(ActiveToggles(), Api::EngineParent::Switch, Api::ParentLeaves::SelectedLeaves);
			std::vector<std::pair<bool, std::uint64_t>> verdicts;
			DirtyVerdicts(inSceneTask, sunEntriesDirty.size(), verdicts, [this] { return ShardScope(*this, true); }, [&](std::size_t a_index) {
				bool candidate = false;
				// The dependents as a set (a sum of their hashes): a geometry moved between containers is listed again at the
				// end, which changes nothing.
				std::uint64_t signature = 0;
				if (const auto it = rootDependents.find(sunEntriesDirty[a_index]); it != rootDependents.end() && !it->second.empty()) {
					candidate = true;
					for (auto* geometry : it->second) {
						const auto entry = tracked.find(geometry);
						if (entry == tracked.end() || !Eligibility::Sun(VerdictOf(entry->second), switchNodes, [&] { return mirror.Leaf(geometry); })) {
							candidate = false;
							break;
						}
						const std::uint64_t allows = Eligibility::PrimaryVerdict(VerdictOf(entry->second), [&] { return mirror.Leaf(geometry); });
						Fnv1a member;
						member.Mix(reinterpret_cast<std::uintptr_t>(geometry) * 4 + allows);
						member.Mix(entry->second.slot);
						signature += member.value;
					}
				}
				return std::pair{ candidate, signature };
			});
			for (std::size_t i = 0; i < sunEntriesDirty.size(); ++i) {
				const auto* root = sunEntriesDirty[i];
				const auto [candidate, signature] = verdicts[i];
				bool written = false;
				if (candidate ? sunCandidateSet.insert(root).second : sunCandidateSet.erase(root) != 0) {
					changed = true;
					written = true;
				}
				if (candidate) {
					const auto [slot, inserted] = primarySignature.try_emplace(root, signature);
					if (!inserted && slot->second != signature) {
						slot->second = signature;
						changed = true;
						written = true;
					}
				} else {
					primarySignature.erase(root);
				}
				// Its subtree changed under it (an attach or detach below it): written again whatever its verdict, its plan to be made again.
				written = written || (candidate && sunEntriesForced.contains(root));
				if (written) {
					const auto dependents = candidate ? rootDependents.find(root) : rootDependents.end();
					sunTable.Set(root, dependents != rootDependents.end() ? &dependents->second : nullptr, candidateVersions, [&](RE::BSGeometry* a_geometry) { return std::pair{ FindObject(a_geometry), PrimaryVerdictOf(a_geometry) }; });
				}
			}
			sunEntriesDirty.clear();
			sunEntriesForced.clear();
		}
		// As the light table's.
		WriteReleasedRoots(
			sunTable, [this](const RE::BSGeometry* a_geometry) {
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(a_geometry));
				return it != tracked.end() ? it->second.listedRoot : nullptr;
			},
			[this](const RE::NiAVObject* a_root) {
				const auto dependents = rootDependents.find(a_root);
				sunTable.Set(a_root, dependents != rootDependents.end() ? &dependents->second : nullptr, candidateVersions, [&](RE::BSGeometry* a_geometry) { return std::pair{ FindObject(a_geometry), PrimaryVerdictOf(a_geometry) }; });
			});
		// A snapshot for every walk that moved an entry (and the first): the readers bring themselves up to it entry by entry.
		if (sunTable.changed || !sunCandidates)
			sunCandidates = sunTable.Publish(sunCandidatesGeneration, [this](const RE::NiAVObject* a_root) { return OwnedRoot(a_root); });
		Diagnostics::Notify(Diagnostics::CandidatesUpdated{ *this, true });
		(void)changed;
	}
}
