#include "Internal.h"

namespace DCLF
{
	bool SceneStore::SkyOcclusionEnabled()
	{
		return ActiveToggles().skyOcclusion && globals::features::skylighting.loaded;
	}

	void SceneStore::DropSunCandidates()
	{
		sunCandidateSet.clear();
		primarySignature.clear();
		sunEntriesDirty.clear();
		sunCandidates.reset();
		++sunCandidatesGeneration;
	}

	bool SceneStore::SunEntryAllows(const Tracked& a_tracked, bool a_switchNodes)
	{
		// A table object: its shadow is the shadow epoch's (the claims decide, per frame), or the caster rule rejects it.
		if (a_tracked.slot != kNoObjectSlot)
			return true;
		if (a_tracked.candidateFrame == 0)
			return false;
		switch (a_tracked.candidateReason) {
		case Ineligible::NotLightingShader:  // no class but Lighting casts into the cascades (CS_DCLF_CASCADE_PROBE)
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

	bool SceneStore::PrimaryEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry)
	{
		if (a_tracked.slot == kNoObjectSlot || a_tracked.candidateFrame == 0 || a_tracked.candidateReason != Ineligible::None)
			return false;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		if (!property || (property->flags.underlying() & 0xc000000ull))  // the decal flags: their depth is the native depth pass's
			return false;
		const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		return !(alpha && (alpha->alphaFlags & 1));
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
				std::uint64_t signature = 1469598103934665603ull;
				if (const auto it = rootDependents.find(root); it != rootDependents.end() && !it->second.empty()) {
					candidate = true;
					for (auto* geometry : it->second) {
						const auto entry = tracked.find(geometry);
						if (entry == tracked.end() || !SunEntryAllows(entry->second, switchNodes)) {
							candidate = false;
							break;
						}
						const std::uint64_t allows = PrimaryEntryAllows(entry->second, *geometry) ? 1 : 0;
						signature = (signature ^ (reinterpret_cast<std::uintptr_t>(geometry) * 2 + allows)) * 1099511628211ull;
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
