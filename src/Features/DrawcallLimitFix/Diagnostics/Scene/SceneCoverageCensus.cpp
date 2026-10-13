#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

#include <numeric>

// CS_DCLF_COVERAGE_PROBE, the report's census: every tracked geometry the main pass leaves to the engine for its technique, by technique,
// shadow verdict and record; the light entries kept in the point lights' culls, by what blocks them; the multi-index shapes.

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;
	}

	std::string CoverageCensus(const Scene::Coordinator& a_store)
	{
		const auto& mirror = V::mirror(a_store);
		const auto& tracked = V::tracked(a_store);
		struct Row
		{
			std::uint32_t count = 0;
			std::string example;
		};
		// The mirror's records.
		auto techniqueOf = [&](const RE::BSGeometry& a_geometry) -> std::string {
			const auto* lighting = SceneCapture::LeafView::Lighting(mirror.Leaf(&a_geometry).property);
			if (!lighting)
				return "not lighting";
			const std::uint64_t flags = lighting->flags;
			return std::string(LightingTechniqueName((flags & 0x8004ull) ? 63u : SelectLightingTechnique(flags)));
		};
		auto describe = [&](const RE::BSGeometry& a_geometry, const V::Tracked& a_tracked) {
			const auto leaf = mirror.Leaf(&a_geometry);
			const auto* rtti = leaf.node ? static_cast<const RE::NiRTTI*>(leaf.node->rtti) : nullptr;
			return fmt::format("{} {} {}, shadow {}, {}", rtti ? rtti->name : "?", kIneligibleNames[static_cast<std::size_t>(a_tracked.candidateReason)], techniqueOf(a_geometry),
				ShadowRejectName(ShadowCasterReject(leaf)), a_tracked.slot != kNoObjectSlot ? "record" : "no record");
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
		const auto* exclusion = V::lightCandidates(a_store).get();
		std::uint32_t entries = 0, kept = 0;
		for (const auto& [root, dependents] : V::lightDependents(a_store)) {
			if (dependents.empty() || V::CategoryNodeOwn(a_store, root))
				continue;
			++entries;
			if (V::lightCandidateSet(a_store).contains(root))
				continue;
			++kept;
			for (auto* geometry : dependents) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end() || V::LightEntryAllows(a_store, it->second, *geometry, switchNodes))
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
}
