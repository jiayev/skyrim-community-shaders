#pragma once

// The sun and light candidates (CoordinatorCandidates): the entries DCLF can take out of the engine's cascade and light culls,
// their signatures, dirty lists and snapshots, and their index spaces (CandidateTable). SunCandidates.cpp, fed by the events
// (ApplyEvents.cpp) and the walk.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorCandidates
	{
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::vector<RE::BSGeometry*>> lightDependents;  // by light entry
		ankerl::unordered_dense::set<const RE::NiAVObject*> lightCandidateSet;
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> lightSignature;  // its geometries, as a set
		std::vector<const RE::NiAVObject*> lightEntriesDirty;
		std::shared_ptr<const SunCandidates> lightCandidates;
		std::uint32_t lightCandidatesGeneration = 0;
		std::uint64_t lightEntriesAppeared = 0;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunCandidateSet;
		// Per sun candidate, a signature of its geometries' PrimaryEntryAllows verdicts: a change bumps the generation.
		ankerl::unordered_dense::map<const RE::NiAVObject*, std::uint64_t> primarySignature;
		std::vector<const RE::NiAVObject*> sunEntriesDirty;
		ankerl::unordered_dense::set<const RE::NiAVObject*> sunEntriesForced;  // candidates whose subtree an attach or detach changed
		std::shared_ptr<const SunCandidates> sunCandidates;
		std::uint32_t sunCandidatesGeneration = 0;
		// The sun's and the point lights' candidate index spaces (CandidateTable.h), one version counter for both.
		CandidateTable sunTable, lightTable;
		std::uint32_t candidateVersions = 0;
		using LightEntryChange = DCLF::LightEntryChange;
		std::vector<LightEntryChange> lightEntryChanges;               // since the last publication
	};
}
