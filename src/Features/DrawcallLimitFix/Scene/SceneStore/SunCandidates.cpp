#include "Internal.h"

#include "Features/DrawcallLimitFix/Common/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Engine/SunAccumulation.h"

#include <map>
#include <numeric>

namespace DCLF
{
	namespace
	{
		/**
		 * @brief a_verdict(i) for every dirty entry, into a_out: across the preparation pool from the scene task (the verdicts read
		 * the coordinator's state, which nothing writes meanwhile), else on the calling thread.
		 */
		template <class Verdict>
		void DirtyVerdicts(bool a_parallel, std::size_t a_count, std::vector<std::pair<bool, std::uint64_t>>& a_out, Verdict&& a_verdict)
		{
			a_out.resize(a_count);
			auto run = [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.Scene.SunCandidates.Verdicts");
				const bool marked = std::exchange(SceneStore::sceneWorkThread, true);
				const bool engineMarked = std::exchange(EngineReadWindow::sceneWork, true);
				for (std::size_t i = a_begin; i < a_end; ++i)
					a_out[i] = a_verdict(i);
				EngineReadWindow::sceneWork = engineMarked;
				SceneStore::sceneWorkThread = marked;
			};
			if (a_parallel)
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Scene.SunCandidates.Verdicts", a_count, 128, run);
			else
				run(0, a_count);
		}
	}

	bool SceneStore::OcclusionEnabled(std::uint32_t a_view)
	{
		// Skylighting's RenderOcclusion drives both maps (its hook of the engine's precipitation mask, then its own map).
		if (!globals::features::skylighting.loaded)
			return false;
		return a_view == kOcclusionSky ? ActiveToggles().skyOcclusion : a_view == kOcclusionPrecipitation && ActiveToggles().precipitationOcclusion;
	}

	void SceneStore::ResetCandidateTable(CandidateTable& a_table)
	{
		// Every index freed under a new version: the pooled snapshots drop their entries when brought up to it.
		for (std::uint32_t e = 0; e < a_table.nodes.size(); ++e)
			a_table.version[e] = ++candidateVersions;
		a_table.index.clear();
		std::fill(a_table.nodes.begin(), a_table.nodes.end(), nullptr);
		for (auto& geometries : a_table.geometries)
			geometries.clear();
		a_table.free.resize(a_table.nodes.size());
		std::iota(a_table.free.rbegin(), a_table.free.rend(), 0u);
		a_table.geometryIndex.clear();
		std::fill(a_table.geometryNodes.begin(), a_table.geometryNodes.end(), nullptr);
		std::fill(a_table.geometryEntry.begin(), a_table.geometryEntry.end(), SunCandidates::kNone);
		a_table.geometryFree.resize(a_table.geometryNodes.size());
		std::iota(a_table.geometryFree.rbegin(), a_table.geometryFree.rend(), 0u);
		a_table.changed = true;
	}

	void SceneStore::SetCandidate(CandidateTable& a_table, const RE::NiAVObject* a_root, const std::vector<RE::BSGeometry*>* a_dependents, bool a_sun)
	{
		auto& t = a_table;
		const auto found = t.index.find(a_root);
		if (!a_dependents && found == t.index.end())
			return;
		std::uint32_t e;
		if (found != t.index.end()) {
			e = found->second;
		} else if (!t.free.empty()) {
			e = t.free.back();
			t.free.pop_back();
		} else {
			e = static_cast<std::uint32_t>(t.nodes.size());
			t.nodes.push_back(nullptr);
			t.version.push_back(0);
			t.geometries.emplace_back();
		}
		// Its geometries let go (a geometry another entry lists too stays that entry's).
		for (const std::uint32_t g : t.geometries[e]) {
			if (const auto it = t.geometryIndex.find(t.geometryNodes[g]); it != t.geometryIndex.end() && it->second == g)
				t.geometryIndex.erase(it);
			t.geometryNodes[g] = nullptr;
			t.geometryEntry[g] = SunCandidates::kNone;
			t.geometryFree.push_back(g);
		}
		t.geometries[e].clear();
		t.version[e] = ++candidateVersions;
		t.changed = true;
		++t.entriesWritten;
		if (!a_dependents) {
			t.index.erase(found);
			t.nodes[e] = nullptr;
			t.free.push_back(e);
			return;
		}
		t.nodes[e] = a_root;
		t.index.insert_or_assign(a_root, e);
		for (auto* geometry : *a_dependents) {
			if (t.geometryIndex.contains(geometry))
				continue;  // listed by another entry first
			std::uint32_t g;
			if (!t.geometryFree.empty()) {
				g = t.geometryFree.back();
				t.geometryFree.pop_back();
			} else {
				g = static_cast<std::uint32_t>(t.geometryNodes.size());
				t.geometryNodes.push_back(nullptr);
				t.geometryEntry.push_back(SunCandidates::kNone);
				t.geometrySlot.push_back(-1);
				t.primaryGeometry.push_back(0);
			}
			t.geometryIndex.emplace(geometry, g);
			t.geometryNodes[g] = geometry;
			t.geometryEntry[g] = e;
			t.geometrySlot[g] = FindObject(geometry);
			t.primaryGeometry[g] = 0;
			if (a_sun)
				if (const auto entry = tracked.find(geometry); entry != tracked.end() && PrimaryEntryAllows(entry->second, *geometry))
					t.primaryGeometry[g] = 1;
			t.geometries[e].push_back(g);
		}
	}

	std::shared_ptr<const SunCandidates> SceneStore::PublishCandidates(CandidateTable& a_table, std::uint32_t& a_generation)
	{
		ZoneScopedN("CS.DCLF.Scene.SunCandidates.Snapshot");
		auto& t = a_table;
		// A pooled snapshot no reader holds (the pool's own reference only), else a new one.
		std::shared_ptr<SunCandidates> snapshot;
		for (const auto& pooled : t.pool)
			if (pooled.use_count() == 1) {
				snapshot = pooled;
				break;
			}
		if (!snapshot)
			snapshot = t.pool.emplace_back(std::make_shared<SunCandidates>());
		auto& s = *snapshot;
		const std::uint32_t capacity = static_cast<std::uint32_t>(t.nodes.size());
		const std::uint32_t geometryCapacity = static_cast<std::uint32_t>(t.geometryNodes.size());
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
		std::vector<std::uint32_t> changed;
		for (std::uint32_t e = 0; e < span; ++e)
			if (s.entryVersion[e] != (e < capacity ? t.version[e] : 0u))
				changed.push_back(e);
		for (const std::uint32_t e : changed) {
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
		for (const std::uint32_t e : changed) {
			s.entryVersion[e] = e < capacity ? t.version[e] : 0u;
			const auto* root = e < capacity ? t.nodes[e] : nullptr;
			if (!root)
				continue;
			s.entryNodes[e] = root;
			s.entries.insert_or_assign(root, e);
			s.held[e] = OwnedRoot(root);
			s.entryGeometries[e] = t.geometries[e];
			for (const std::uint32_t g : t.geometries[e]) {
				s.geometryNodes[g] = t.geometryNodes[g];
				s.geometryEntry[g] = e;
				s.geometrySlot[g] = t.geometrySlot[g];
				s.primaryGeometry[g] = t.primaryGeometry[g];
				s.geometries.insert_or_assign(t.geometryNodes[g], g);
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
		t.changed = false;
		++t.snapshots;
		return snapshot;
	}

	std::string SceneStore::CheckCandidateTable(const CandidateTable& a_table, const ankerl::unordered_dense::set<const RE::NiAVObject*>& a_set, bool a_sun) const
	{
		// CS_DCLF_PERSISTENT_PARITY: the table against the set and the dependents as they are (what a snapshot made whole would hold).
		if (a_table.index.size() != a_set.size())
			return fmt::format("{} entries, the set {}", a_table.index.size(), a_set.size());
		for (const auto* root : a_set) {
			const auto it = a_table.index.find(root);
			if (it == a_table.index.end())
				return fmt::format("a candidate without an entry ({})", static_cast<const void*>(root));
			const auto* dependents = a_sun ? (rootDependents.contains(root) ? &rootDependents.at(root) : nullptr) : LightDependentsOf(root);
			const auto& rows = a_table.geometries[it->second];
			std::size_t owned = 0;
			for (const auto* geometry : dependents ? *dependents : std::vector<RE::BSGeometry*>{}) {
				const auto g = a_table.geometryIndex.find(geometry);
				if (g == a_table.geometryIndex.end())
					return fmt::format("a dependent of {} with no geometry row", static_cast<const void*>(root));
				if (a_table.geometryEntry[g->second] != it->second)
					continue;  // another entry's (listed there first)
				++owned;
				if (a_table.geometrySlot[g->second] != FindObject(geometry))
					return fmt::format("a geometry of {} with object {}, now {}", static_cast<const void*>(root), a_table.geometrySlot[g->second], FindObject(geometry));
				if (a_sun) {
					const auto entry = tracked.find(const_cast<RE::BSGeometry*>(geometry));
					const std::uint8_t allows = entry != tracked.end() && PrimaryEntryAllows(entry->second, *geometry) ? 1 : 0;
					if (a_table.primaryGeometry[g->second] != allows)
						return fmt::format("a geometry of {} with its primary verdict {}, now {}", static_cast<const void*>(root), a_table.primaryGeometry[g->second], allows);
				}
			}
			if (owned != rows.size())
				return fmt::format("{} with {} geometry rows, {} dependents its own", static_cast<const void*>(root), rows.size(), owned);
		}
		return {};
	}

	void SceneStore::DropSunCandidates()
	{
		sunCandidateSet.clear();
		primarySignature.clear();
		sunEntriesDirty.clear();
		ResetCandidateTable(sunTable);
		sunCandidates.reset();
		++sunCandidatesGeneration;
		lightCandidateSet.clear();
		lightSignature.clear();
		lightEntriesDirty.clear();
		ResetCandidateTable(lightTable);
		lightCandidates.reset();
		++lightCandidatesGeneration;
	}

	bool SceneStore::LightEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry, bool a_switchNodes) const
	{
		// What the sun's rule allows (the caster rule gives an alpha-blended or fading property no shadow pass, the paraboloid's as
		// the cascades'), and any geometry without a Lighting property. The lights' bits of every geometry are LocalLightCull's.
		if (SunEntryAllows(a_tracked, a_switchNodes))
			return true;
		return a_tracked.candidateFrame != 0 && !SceneCapture::LeafView::Lighting(mirror.Leaf(&a_geometry).property);  // T6b1b: the mirror's
	}

	const std::vector<RE::BSGeometry*>* SceneStore::LightDependentsOf(const RE::NiAVObject* a_root) const
	{
		const auto it = lightDependents.find(a_root);
		if (it == lightDependents.end() || it->second.empty() || CategoryNodeOwn(a_root))
			return nullptr;
		return &it->second;
	}

	void SceneStore::UpdateLightCandidates(bool a_full)
	{
		bool changed = a_full;
		if (a_full) {
			ResetCandidateTable(lightTable);
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
			std::vector<std::pair<bool, std::uint64_t>> verdicts;
			DirtyVerdicts(inSceneTask, lightEntriesDirty.size(), verdicts, [&](std::size_t a_index) {
				const auto* dependents = LightDependentsOf(lightEntriesDirty[a_index]);
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
					SetCandidate(lightTable, root, candidate ? LightDependentsOf(root) : nullptr, false);
			}
			lightEntriesDirty.clear();
		}
		// A snapshot for every walk that moved an entry (and the first): the readers bring themselves up to it entry by entry.
		if (lightTable.changed || !lightCandidates)
			lightCandidates = PublishCandidates(lightTable, lightCandidatesGeneration);
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(sceneFrame, 7)) {
			++candidateParity.checks;
			if (auto why = CheckCandidateTable(lightTable, lightCandidateSet, false); !why.empty() && candidateParity.mismatches++ < 8)
				logger::warn("[DCLF] light candidates parity at frame {}: {} <- CANDIDATES", sceneFrame, why);
		}
		(void)changed;
	}

	bool SceneStore::SunEntryAllows(const Tracked& a_tracked, bool a_switchNodes) const
	{
		// A table object: its shadow is the shadow epoch's (the set decides, per frame), or the caster rule rejects it.
		if (a_tracked.slot != kNoObjectSlot)
			return true;
		if (a_tracked.candidateFrame == 0)
			return false;
		// The verdicts are the snapshot's; the exclusion is a promise about the culls that run a frame or two later (as BuildSunExclusion
		// counts a faded record a caster). A fading non-member blocks its entry: its fade advances only in the culls the exclusion would
		// skip. A hidden part of an actor blocks it too: the behaviour graph shows and hides an actor's parts during Main::Draw (a
		// shield's BShkVisibilityController, every 150-360 ms), and a shown part with no record would cast into no shadow until the next
		// candidates (seen in interiors, w71/w80: a soldier's shield symbol). Until hidden objects keep their records (T6b4's rest).
		switch (a_tracked.candidateReason) {
		case Ineligible::NotLightingShader:  // no class but Lighting casts into the cascades (measured)
		case Ineligible::AlphaBlend:         // the caster rule: no pass for an alpha-blended property
			return true;
		case Ineligible::Hidden:  // the cull skips app-culled nodes: a static's hidden bit changes by its events alone
		{
			const auto leaf = a_tracked.geometry ? mirror.Leaf(a_tracked.geometry.get()) : SceneCapture::LeafView{};
			return !(leaf.node && leaf.node->userData && leaf.node->formType == static_cast<std::uint8_t>(RE::FormType::ActorCharacter));
		}
		case Ineligible::Fading:
			return false;
		case Ineligible::Switch:  // an unselected child: the switch node culls only the selected one
			return a_switchNodes;
		default:
			// A property no shadow view takes, whatever the frame (refraction, for one: a fire's embers, a stream's surface).
			return a_tracked.geometry && CastsNoShadow(mirror.Leaf(a_tracked.geometry.get()));  // T6b1b: the mirror's
		}
	}

	std::string SceneStore::DescribeSunCandidate(const void* a_geometry) const
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

	std::string SceneStore::CoverageCensus() const
	{
		struct Row
		{
			std::uint32_t count = 0;
			std::string example;
		};
		// The mirror's records (T6b1b).
		auto techniqueOf = [&](const RE::BSGeometry& a_geometry) -> std::string {
			const auto* lighting = SceneCapture::LeafView::Lighting(mirror.Leaf(&a_geometry).property);
			if (!lighting)
				return "not lighting";
			const std::uint64_t flags = lighting->flags;
			return std::string(LightingTechniqueName((flags & 0x8004ull) ? 63u : SelectLightingTechnique(flags)));
		};
		auto describe = [&](const RE::BSGeometry& a_geometry, const Tracked& a_tracked) {
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
		const auto* exclusion = lightCandidates.get();
		std::uint32_t entries = 0, kept = 0;
		for (const auto& [root, dependents] : lightDependents) {
			if (dependents.empty() || CategoryNodeOwn(root))
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

	bool SceneStore::PrimaryEntryAllows(const Tracked& a_tracked, const RE::BSGeometry& a_geometry) const
	{
		if (a_tracked.slot == kNoObjectSlot || a_tracked.candidateFrame == 0 || a_tracked.candidateReason != Ineligible::None)
			return false;
		// The mirror's records (T6b1b).
		const auto leaf = mirror.Leaf(&a_geometry);
		if (!leaf.property)
			return false;
		// A decal is a member like any other (its group from the settled state); the blended decal group is DCLF's too.
		const bool decal = (leaf.property->flags & 0xc000000ull) != 0;
		if (decal && !ActiveToggles().decals)
			return false;
		return decal || !leaf.AlphaBlending();
	}

	void SceneStore::UpdateSunCandidates(bool a_full)
	{
		bool changed = a_full;
		if (a_full) {
			ResetCandidateTable(sunTable);
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
			std::vector<std::pair<bool, std::uint64_t>> verdicts;
			DirtyVerdicts(inSceneTask, sunEntriesDirty.size(), verdicts, [&](std::size_t a_index) {
				bool candidate = false;
				// The dependents as a set (a sum of their hashes): a geometry moved between containers is listed again at the
				// end, which changes nothing.
				std::uint64_t signature = 0;
				if (const auto it = rootDependents.find(sunEntriesDirty[a_index]); it != rootDependents.end() && !it->second.empty()) {
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
					SetCandidate(sunTable, root, dependents != rootDependents.end() ? &dependents->second : nullptr, true);
				}
			}
			sunEntriesDirty.clear();
			sunEntriesForced.clear();
		}
		// A snapshot for every walk that moved an entry (and the first): the readers bring themselves up to it entry by entry.
		if (sunTable.changed || !sunCandidates)
			sunCandidates = PublishCandidates(sunTable, sunCandidatesGeneration);
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(sceneFrame, 7)) {
			++candidateParity.checks;
			if (auto why = CheckCandidateTable(sunTable, sunCandidateSet, true); !why.empty() && candidateParity.mismatches++ < 8)
				logger::warn("[DCLF] sun candidates parity at frame {}: {} <- CANDIDATES", sceneFrame, why);
		}
		(void)changed;
	}
}
