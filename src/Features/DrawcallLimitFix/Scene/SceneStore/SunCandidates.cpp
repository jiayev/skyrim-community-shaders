#include "Internal.h"

namespace DCLF
{
	bool SceneStore::OcclusionEnabled(std::uint32_t a_view)
	{
		// Skylighting's RenderOcclusion drives both maps (its hook of the engine's precipitation mask, then its own map).
		if (!globals::features::skylighting.loaded)
			return false;
		return a_view == kOcclusionSky ? ActiveToggles().skyOcclusion : a_view == kOcclusionPrecipitation && ActiveToggles().precipitationOcclusion;
	}

	void SceneStore::DropSunCandidates()
	{
		sunCandidateSet.clear();
		primarySignature.clear();
		sunEntriesDirty.clear();
		sunCandidates.reset();
		++sunCandidatesGeneration;
		lightCandidateSet.clear();
		lightSignature.clear();
		lightEntriesDirty.clear();
		lightCandidates.reset();
		++lightCandidatesGeneration;
	}

	bool SceneStore::LightEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_switchNodes)
	{
		if (a_tracked.slot != kNoObjectSlot)
			return true;
		if (a_tracked.candidateFrame == 0)
			return false;
		if (a_tracked.candidateReason == Ineligible::Hidden || (a_tracked.candidateReason == Ineligible::Switch && a_switchNodes))
			return true;
		return !netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
	}

	const std::vector<RE::BSGeometry*>* SceneStore::LightDependentsOf(const RE::NiAVObject* a_root) const
	{
		const auto it = rootDependents.find(a_root);
		if (it == rootDependents.end() || it->second.empty() || categoryNodes.contains(static_cast<RE::NiNode*>(const_cast<RE::NiAVObject*>(a_root))))
			return nullptr;
		return &it->second;
	}

	void SceneStore::UpdateLightCandidates(bool a_full)
	{
		bool changed = a_full;
		if (a_full) {
			lightCandidateSet.clear();
			lightSignature.clear();
			lightEntriesDirty.clear();
			for (const auto& [root, dependents] : rootDependents)
				lightEntriesDirty.push_back(root);
		}
		if (!lightEntriesDirty.empty()) {
			std::sort(lightEntriesDirty.begin(), lightEntriesDirty.end());
			lightEntriesDirty.erase(std::unique(lightEntriesDirty.begin(), lightEntriesDirty.end()), lightEntriesDirty.end());
			const bool switchNodes = ActiveToggles().switchNodes;
			for (const auto* root : lightEntriesDirty) {
				const auto* dependents = LightDependentsOf(root);
				bool candidate = dependents != nullptr;
				std::uint64_t signature = 0;
				if (dependents)
					for (auto* geometry : *dependents) {
						const auto entry = tracked.find(geometry);
						if (entry == tracked.end() || !LightEntryAllows(entry->second, *geometry, switchNodes)) {
							candidate = false;
							break;
						}
						Fnv1a member;
						member.Mix(reinterpret_cast<std::uintptr_t>(geometry));
						signature += member.value;
					}
				if (candidate ? lightCandidateSet.insert(root).second : lightCandidateSet.erase(root) != 0)
					changed = true;
				if (candidate) {
					const auto [slot, inserted] = lightSignature.try_emplace(root, signature);
					if (!inserted && slot->second != signature) {
						slot->second = signature;
						changed = true;
					}
				} else {
					lightSignature.erase(root);
				}
			}
			lightEntriesDirty.clear();
		}
		if (changed) {
			++lightCandidatesGeneration;
			return;
		}
		if (lightCandidatesBuilt == lightCandidatesGeneration && lightCandidates)
			return;
		// A walk that changed nothing: the snapshot for the generation now in force.
		auto snapshot = std::make_shared<SunCandidates>();
		snapshot->generation = lightCandidatesGeneration;
		snapshot->entries.reserve(lightCandidateSet.size());
		std::uint32_t index = 0;
		for (const auto* root : lightCandidateSet) {
			snapshot->entries.emplace(root, index);
			if (const auto* dependents = LightDependentsOf(root))
				for (auto* geometry : *dependents)
					if (snapshot->geometries.emplace(geometry, static_cast<std::uint32_t>(snapshot->geometryEntry.size())).second)
						snapshot->geometryEntry.push_back(index);
			++index;
		}
		lightCandidates = std::move(snapshot);
		lightCandidatesBuilt = lightCandidatesGeneration;
	}

	bool SceneStore::SunEntryAllows(const Tracked& a_tracked, bool a_switchNodes)
	{
		// A table object: its shadow is the shadow epoch's (the claims decide, per frame), or the caster rule rejects it.
		if (a_tracked.slot != kNoObjectSlot)
			return true;
		if (a_tracked.candidateFrame == 0)
			return false;
		switch (a_tracked.candidateReason) {
		case Ineligible::NotLightingShader:  // no class but Lighting casts into the cascades (measured)
		case Ineligible::Hidden:             // the cull skips app-culled nodes
		case Ineligible::AlphaBlend:         // the caster rule: no pass for an alpha-blended property
		case Ineligible::Fading:             // ... nor for a fade below one
			return true;
		case Ineligible::Switch:  // an unselected child: the switch node culls only the selected one
			return a_switchNodes;
		default:
			return false;
		}
	}

	SceneStore::EntryVerdict SceneStore::EntryCensus(const RE::NiAVObject* a_node) const
	{
		EntryVerdict verdict;
		const auto it = rootDependents.find(a_node);
		if (it == rootDependents.end() || it->second.empty()) {
			verdict.kind = 1;
			return verdict;
		}
		verdict.geometries = static_cast<std::uint32_t>(it->second.size());
		const bool switchNodes = ActiveToggles().switchNodes;
		for (auto* geometry : it->second) {
			const auto entry = tracked.find(geometry);
			if (entry == tracked.end() || (entry->second.slot == kNoObjectSlot && entry->second.candidateFrame == 0)) {
				verdict = { 2, Ineligible::Count, verdict.geometries, verdict.slots, geometry };
				return verdict;
			}
			if (entry->second.slot != kNoObjectSlot) {
				++verdict.slots;
				continue;
			}
			if (!SunEntryAllows(entry->second, switchNodes)) {
				verdict = { 2, entry->second.candidateReason, verdict.geometries, verdict.slots, geometry };
				return verdict;
			}
		}
		return verdict;
	}

	bool SceneStore::PrimaryEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		if (a_tracked.slot == kNoObjectSlot || a_tracked.candidateFrame == 0 || a_tracked.candidateReason != Ineligible::None)
			return false;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		if (!property)
			return false;
		// A decal is a member like any other (its group from the settled state); the blended decal group is DCLF's too.
		const bool decal = (property->flags.underlying() & 0xc000000ull) != 0;
		if (decal && !ActiveToggles().decals)
			return false;
		const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		return decal || !(alpha && (alpha->alphaFlags & 1));
	}

	void SceneStore::UpdateSunCandidates(bool a_full)
	{
		bool changed = a_full;
		if (a_full) {
			sunCandidateSet.clear();
			primarySignature.clear();
			sunEntriesDirty.clear();
			for (const auto& [root, dependents] : rootDependents)
				sunEntriesDirty.push_back(root);
		}
		if (!sunEntriesDirty.empty()) {
			std::sort(sunEntriesDirty.begin(), sunEntriesDirty.end());
			sunEntriesDirty.erase(std::unique(sunEntriesDirty.begin(), sunEntriesDirty.end()), sunEntriesDirty.end());
			const bool switchNodes = ActiveToggles().switchNodes;
			for (const auto* root : sunEntriesDirty) {
				bool candidate = false;
				// The dependents as a set (a sum of their hashes): a geometry moved between containers is listed again at the
				// end, which changes nothing.
				std::uint64_t signature = 0;
				if (const auto it = rootDependents.find(root); it != rootDependents.end() && !it->second.empty()) {
					candidate = true;
					for (auto* geometry : it->second) {
						const auto entry = tracked.find(geometry);
						if (entry == tracked.end() || !SunEntryAllows(entry->second, switchNodes)) {
							candidate = false;
							break;
						}
						const std::uint64_t allows = PrimaryEntryAllows(entry->second, *geometry) ? 1 : 0;
						Fnv1a member;
						member.Mix(reinterpret_cast<std::uintptr_t>(geometry) * 2 + allows);
						signature += member.value;
					}
				}
				if (candidate ? sunCandidateSet.insert(root).second : sunCandidateSet.erase(root) != 0)
					changed = true;
				if (candidate) {
					const auto [slot, inserted] = primarySignature.try_emplace(root, signature);
					if (!inserted && slot->second != signature) {
						slot->second = signature;
						changed = true;
					}
				} else {
					primarySignature.erase(root);
				}
			}
			sunEntriesDirty.clear();
		}
		if (changed) {
			++sunCandidatesGeneration;
			return;
		}
		if (sunCandidatesBuilt == sunCandidatesGeneration && sunCandidates)
			return;
		// A walk that changed nothing: the snapshot for the generation now in force.
		auto snapshot = std::make_shared<SunCandidates>();
		snapshot->generation = sunCandidatesGeneration;
		snapshot->entries.reserve(sunCandidateSet.size());
		std::uint32_t index = 0;
		for (const auto* root : sunCandidateSet) {
			snapshot->entries.emplace(root, index);
			if (const auto it = rootDependents.find(root); it != rootDependents.end())
				for (auto* geometry : it->second)
					if (snapshot->geometries.emplace(geometry, static_cast<std::uint32_t>(snapshot->geometryEntry.size())).second) {
						snapshot->geometryEntry.push_back(index);
						const auto entry = tracked.find(geometry);
						snapshot->primaryGeometry.push_back(entry != tracked.end() && PrimaryEntryAllows(entry->second, *geometry) ? 1 : 0);
					}
			++index;
		}
		sunCandidates = std::move(snapshot);
		sunCandidatesBuilt = sunCandidatesGeneration;
	}
}
