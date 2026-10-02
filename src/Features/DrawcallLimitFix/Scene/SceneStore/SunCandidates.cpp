#include "Internal.h"

#include <map>
#include <numeric>

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
		// What the sun's rule allows (the caster rule gives an alpha-blended or fading property no shadow pass, the paraboloid's as
		// the cascades'), and any geometry without a Lighting property. The lights' bits of every geometry are LocalLightCull's.
		if (SunEntryAllows(a_tracked, a_switchNodes))
			return true;
		return a_tracked.candidateFrame != 0 && !netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
	}

	const std::vector<RE::BSGeometry*>* SceneStore::LightDependentsOf(const RE::NiAVObject* a_root) const
	{
		const auto it = lightDependents.find(a_root);
		if (it == lightDependents.end() || it->second.empty() || IsCategoryNode(a_root))
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
			for (const auto& [root, dependents] : lightDependents)
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
		snapshot->entryNodes.reserve(lightCandidateSet.size());
		std::uint32_t index = 0;
		for (const auto* root : lightCandidateSet) {
			snapshot->entries.emplace(root, index);
			snapshot->entryNodes.push_back(root);
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
			// A property no shadow view takes, whatever the frame (refraction, for one: a fire's embers, a stream's surface).
			return a_tracked.geometry && CastsNoShadow(a_tracked.geometry->GetGeometryRuntimeData().shaderProperty.get(), a_tracked.geometry.get());
		}
	}

	std::string SceneStore::CoverageCensus() const
	{
		struct Row
		{
			std::uint32_t count = 0;
			std::string example;
		};
		auto techniqueOf = [](const RE::BSGeometry& a_geometry) -> std::string {
			const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
			if (!lighting)
				return "not lighting";
			const std::uint64_t flags = lighting->flags.underlying();
			return std::string(LightingTechniqueName((flags & 0x8004ull) ? 63u : SelectLightingTechnique(flags)));
		};
		auto describe = [&](const RE::BSGeometry& a_geometry, const Tracked& a_tracked) {
			const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
			return fmt::format("{} {} {}, shadow {}, {}", a_geometry.GetRTTI() ? a_geometry.GetRTTI()->name : "?", kIneligibleNames[static_cast<std::size_t>(a_tracked.candidateReason)], techniqueOf(a_geometry),
				ShadowRejectName(ShadowCasterReject(property, &a_geometry, false)), a_tracked.slot != kNoObjectSlot ? "record" : "no record");
		};
		auto exampleOf = [](const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_entry) {
			return fmt::format("'{}' under '{}'", a_geometry.name.c_str() ? a_geometry.name.c_str() : "", a_entry && a_entry->name.c_str() ? a_entry->name.c_str() : "");
		};
		std::map<std::string, Row> techniques, blockers;
		for (const auto& [geometry, entry] : tracked) {
			if (!geometry || entry.candidateFrame == 0 || entry.candidateReason != Ineligible::Technique)
				continue;
			auto& row = techniques[describe(*geometry, entry)];
			if (row.count++ == 0)
				row.example = exampleOf(*geometry, entry.lightRoot ? entry.lightRoot : entry.sunEntryNode);
		}
		const bool switchNodes = ActiveToggles().switchNodes;
		const auto* exclusion = lightCandidates.get();
		std::uint32_t entries = 0, kept = 0;
		for (const auto& [root, dependents] : lightDependents) {
			if (dependents.empty() || IsCategoryNode(root))
				continue;
			++entries;
			if (lightCandidateSet.contains(root))
				continue;
			++kept;
			for (auto* geometry : dependents) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end() || LightEntryAllows(it->second, *geometry, switchNodes))
					continue;
				auto& row = blockers[it == tracked.end() ? std::string("untracked") : describe(*geometry, it->second)];
				if (row.count++ == 0)
					row.example = exampleOf(*geometry, root);
				break;
			}
		}
		std::string text = fmt::format("[DCLF] coverage census: {} geometries left native for their technique, by verdict, technique, shadow verdict and record:",
			std::accumulate(techniques.begin(), techniques.end(), 0u, [](std::uint32_t a_sum, const auto& a_row) { return a_sum + a_row.second.count; }));
		for (const auto& [name, row] : techniques)
			text += fmt::format("\n    {}: {}; e.g. {}", name, row.count, row.example);
		text += fmt::format("\n[DCLF] coverage census: {} of {} light entries not candidates (the light candidates {}), by the first geometry that blocks them:", kept,
			entries, exclusion ? fmt::format("snapshot of {}", exclusion->entries.size()) : std::string("not built"));
		for (const auto& [name, row] : blockers)
			text += fmt::format("\n    {}: {}; e.g. {}", name, row.count, row.example);
		// The multi-index shapes (Ineligible::MultiIndex), by their two properties as the main registration (FUN_1414b2330)
		// takes them: the additional one's passes go into geometry group 2 with hint 12, drawing the second index list.
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		auto propertyOf = [](const RE::BSShaderProperty* a_property) -> std::string {
			if (!a_property)
				return "none";
			const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_property);
			if (!lighting)
				return fmt::format("not lighting ({})", a_property->GetRTTI() && a_property->GetRTTI()->name ? a_property->GetRTTI()->name : "?");
			const std::uint64_t flags = lighting->flags.underlying();
			auto has = [&](Flag a_flag) { return (flags & static_cast<std::uint64_t>(a_flag)) != 0; };
			return fmt::format("technique {}{}{}{}{}{}", LightingTechniqueName((flags & 0x8004ull) ? 63u : SelectLightingTechnique(flags)),
				has(Flag::kProjectedUV) ? " projectedUV" : "", has(Flag::kMultiIndexSnow) ? " multiIndexSnow" : "", has(Flag::kDecal) ? " decal" : "",
				has(Flag::kZBufferWrite) ? "" : " noZWrite", has(Flag::kSkinned) ? " skinned" : "");
		};
		std::map<std::string, Row> multiIndex;
		for (const auto& [geometry, entry] : tracked) {
			if (!geometry || entry.candidateFrame == 0 || entry.candidateReason != Ineligible::MultiIndex)
				continue;
			const auto& data = static_cast<const RE::BSMultiIndexTriShape*>(geometry)->GetMultiIndexTrishapeRuntimeData();
			const auto triangles = static_cast<const RE::BSTriShape*>(geometry)->GetTrishapeRuntimeData().triangleCount;
			const auto& runtime = geometry->GetGeometryRuntimeData();
			auto& row = multiIndex[fmt::format("main {}; additional {}; useAdditionalTriList {}, alt triangles {}, alpha property {}", propertyOf(runtime.shaderProperty.get()),
				propertyOf(data.additionalShaderProperty.get()), static_cast<std::uint32_t>(data.useAdditionalTriList),
				!data.altIndexBuffer ? "no buffer" : data.altPrimCount == 0 ? "none" : data.altPrimCount < triangles ? "fewer" : data.altPrimCount == triangles ? "as many" : "more",
				runtime.alphaProperty ? (runtime.alphaProperty->alphaFlags & 1 ? "blended" : "yes") : "no")];
			if (row.count++ == 0)
				row.example = exampleOf(*geometry, entry.lightRoot ? entry.lightRoot : entry.sunEntryNode);
		}
		text += "\n[DCLF] coverage census: multi-index shapes, by their properties:";
		for (const auto& [name, row] : multiIndex)
			text += fmt::format("\n    {}: {}; e.g. {}", name, row.count, row.example);
		return text;
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
		snapshot->entryNodes.reserve(sunCandidateSet.size());
		std::uint32_t index = 0;
		for (const auto* root : sunCandidateSet) {
			snapshot->entries.emplace(root, index);
			snapshot->entryNodes.push_back(root);
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
