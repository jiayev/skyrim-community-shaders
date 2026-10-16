// CS_DCLF_WALK_PARITY: every 60 passes, right after the walk, the slot tables against a dense rebuild (Coordinator's reference walk,
// which leaves the coordinator as it found it), object by object, and the kept verdicts and traits against a classification from scratch.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		void CheckWalkParity(Scene::Coordinator& store)
		{
			using Tables = SceneTables;
			// What the delta walk evaluated (the reference walk's order overwrites the entries' scheduledWalk), for the stale verdicts'
			// report.
			const auto& tracked = V::tracked(store);
			ankerl::unordered_dense::set<const RE::BSGeometry*> evaluated;
			for (const auto& [geometry, entry] : tracked)
				if (entry.scheduledWalk == V::walkSerial(store))
					evaluated.insert(geometry);
			SceneTables dense;
			V::ReferenceReasons referenceReasons;
			V::DenseReference(store, dense, referenceReasons);
			const auto& slots = V::tables(store);
			const auto& tables = slots;
			const auto& perFrameSet = V::perFrameSet(store);
			auto& walkParity = SceneDiagnostics().walk;

			++walkParity.checks;
			// A slot is its geometry's base record or its layer's (Tables::layerBase): the two walks pair them by both.
			auto keyOf = [](const Tables& a_tables, std::uint32_t a_slot) {
				return std::pair{ a_tables.objectGeometry[a_slot], a_tables.IsLayer(a_slot) };
			};
			std::map<std::pair<const RE::BSGeometry*, bool>, std::uint32_t> denseIndex;
			for (std::uint32_t d = 0; d < dense.objects.size(); ++d)
				denseIndex.emplace(keyOf(dense, d), d);
			auto note = [&](const char* a_what, const RE::BSGeometry* a_geometry) {
				++walkParity.byWhat[a_what];
				if (walkParity.first.empty())
					walkParity.first = fmt::format("{} on '{}'", a_what, a_geometry && a_geometry->name.c_str() ? a_geometry->name.c_str() : "?");
			};
			auto same = [](const auto& a_left, const auto& a_right) { return std::memcmp(&a_left, &a_right, sizeof(a_left)) == 0; };
			std::uint32_t liveSeen = 0;
			for (std::uint32_t s = 0; s < slots.objects.size(); ++s) {
				const auto* geometry = slots.objectGeometry[s];
				if (!geometry) {
					if (!(slots.objects[s].flags & kObjectFree)) {
						++walkParity.differ;
						note("a free slot without kObjectFree", nullptr);
					}
					continue;
				}
				++liveSeen;
				++walkParity.objects;
				const auto it = denseIndex.find(keyOf(slots, s));
				if (it == denseIndex.end()) {
					++walkParity.extra;
					note("a slot the dense walk has no object for", geometry);
					continue;
				}
				const std::uint32_t d = it->second;
				denseIndex.erase(it);
				const char* what = nullptr;
				// A member keeps its accumulated half across walks (keepMember): compared as the walk left it, which
				// is the scene half alone (the accumulated half's columns are the accumulate phase's, checked by the change log).
				auto objectRecord = slots.objects[s];
				auto drawRecord = slots.draws[s];
				objectRecord.flags = slots.sceneFlags[s];
				objectRecord.materialIndex = objectRecord.pipelineIndex = 0;
				drawRecord.pipelineIndex = 0;
				if (!same(objectRecord, dense.objects[d]) || slots.sceneFlags[s] != dense.sceneFlags[d]) {
					what = "the object record";
					if (walkParity.first.empty()) {
						const auto entryIt = tracked.find(const_cast<RE::BSGeometry*>(geometry));
						const auto& a = slots.objects[s];
						const auto& b = dense.objects[d];
						walkParity.first = fmt::format("the object record on '{}' (per-frame {}): flags {:X}/{:X} scene {:X}/{:X} geometry {}/{}",
							geometry->name.c_str() ? geometry->name.c_str() : "?", entryIt != tracked.end() && entryIt->second.perFrame, a.flags, b.flags, slots.sceneFlags[s], dense.sceneFlags[d],
							a.geometryIndex, b.geometryIndex);
					}
				}
				else if (!same(drawRecord, dense.draws[d]))
					what = "the draw";
				else if (slots.skinPartitions[s] != dense.skinPartitions[d] || slots.skinLodPartitions[s] != dense.skinLodPartitions[d] || slots.boneRows[s] != dense.boneRows[d])
					what = "the skin";
				else if (slots.shadowTechnique[s] != dense.shadowTechnique[d] || slots.shadowReject[s] != dense.shadowReject[d])
					what = "the shadow verdict";
				else if (slots.sunEntryNode[s] != dense.sunEntryNode[d])
					what = "the sun entry node";
				else if (slots.hasFadeNode[s] != dense.hasFadeNode[d])
					what = "the fade node";
				else if (slots.shadowDiffuse[s] != dense.shadowDiffuse[d] || slots.shadowMaterial[s] != dense.shadowMaterial[d])
					what = "the shadow material";
				else if ((slots.streamRow[s] == kNoStreamRow) != (dense.streamRow[d] == kNoStreamRow))
					what = "the face stream";
				else if (slots.streamRow[s] != kNoStreamRow) {
					const auto& a = slots.streamRows[slots.streamRow[s]];
					const auto& b = dense.streamRows[dense.streamRow[d]];
					if (a.object != s || b.object != d || a.first != b.first || a.count != b.count || a.generation != b.generation || a.vertices != b.vertices)
						what = "the face stream";
				}
				if (what) {
					++walkParity.differ;
					note(what, geometry);
				}
			}
			walkParity.missing += static_cast<std::uint32_t>(denseIndex.size());
			// The classifications themselves, records or not: a kept verdict the reference no longer reaches is stale
			// whether or not it decides a record (a negative one leaves the object to the engine).
			for (const auto& [geometry, entry] : tracked) {
				if (entry.candidateFrame == 0)
					continue;
				const auto it = referenceReasons.find(geometry);
				if (it == referenceReasons.end() || it->second == entry.candidateReason)
					continue;
				++walkParity.staleVerdicts;
				if (walkParity.firstStale.empty()) {
					// The chain as the two walks read it: each node's live hidden bit and the walk's (HiddenForWalk).
					std::string chain;
					for (const RE::NiAVObject* object = geometry; object && chain.size() < 600; object = object->parent) {
						chain += fmt::format(" > {}{}{}", object->name.c_str() ? object->name.c_str() : "?", IsHidden(object) ? "[H]" : "",
							V::HiddenForWalk(store, object) != IsHidden(object) ? "[walk differs]" : "");
						if (object == entry.categoryNode)
							break;
					}
					const auto copies = std::count_if(perFrameSet.begin(), perFrameSet.end(), [&](const auto& a_item) { return a_item.geometry == geometry; });
					walkParity.firstStale = fmt::format("'{}' kept {} ({} passes old, per-frame {} (listed {}, {} copies in the set), traits {:X}, light {:X}, evaluated this walk {}) now {}; chain:{}",
						geometry->name.c_str() ? geometry->name.c_str() : "?", kIneligibleNames[static_cast<std::size_t>(entry.candidateReason)], store.PassStamp() - entry.candidateFrame,
						entry.perFrame, entry.perFrameListed, copies, V::PerFrameTraits(store, entry, *geometry), entry.lightTraits, evaluated.contains(geometry),
						kIneligibleNames[static_cast<std::size_t>(it->second)], chain);
				}
			}
			// The traits: an entry a fresh classification would evaluate every frame, or on a heavier path, is one whose event
			// was missed, even while its record still matches.
			{
				ankerl::unordered_dense::map<const RE::NiAVObject*, bool> freshMotion;
				for (auto& [geometry, entry] : tracked) {
					if (entry.candidateFrame == 0)
						continue;
					const auto it = referenceReasons.find(geometry);
					if (it == referenceReasons.end() || it->second != entry.candidateReason)
						continue;
					std::uint32_t traits = 0;
					const auto [perFrame, light] = V::PerFrameOfFresh(store, entry, *geometry, it->second, traits, freshMotion);
					// An actor's light path places its record every frame (its owner class's own motion): motion it gained since is served
					// already.
					if ((entry.lightTraits & V::kTraitOwnerClass) && entry.owner.ownMotion)
						traits &= ~(V::kTraitMoves | V::kTraitRootMoves);
					if (!perFrame || (entry.perFrame && (!entry.lightTraits || (light && !(traits & ~entry.lightTraits)))))
						continue;
					++walkParity.staleTraits;
					if (walkParity.firstStaleTraits.empty())
						walkParity.firstStaleTraits = fmt::format("'{}' {} traits {:X} now {:X} ({} passes since classified)", geometry->name.c_str() ? geometry->name.c_str() : "?",
							entry.perFrame ? "per-frame with" : "kept, no", entry.lightTraits, traits, store.PassStamp() - entry.candidateFrame);
				}
			}
			if (!denseIndex.empty())
				note("an object with no slot", denseIndex.begin()->first.first);
			if (liveSeen != slots.liveObjects || slots.liveObjects + slots.objectFree.size() + slots.objectsRetiring != slots.objects.size()) {
				++walkParity.differ;
				note("the live count", nullptr);
			}
			// The per-frame lists, as sets: the rows were compared per object above (the slot tables keep each palette's
			// block, so their palette arrays have holes the dense walk's have not), and the actors are mapped to geometries
			// below.
			auto sorted = [](auto a_list) {
				std::sort(a_list.begin(), a_list.end());
				return a_list;
			};
			if (slots.shadowKeysUsed.size() != dense.shadowKeysUsed.size() || sorted(slots.shadowTextureSet) != sorted(dense.shadowTextureSet) ||
				slots.actorObjects.size() != dense.actorObjects.size()) {
				++walkParity.differ;
				note("a per-frame list", nullptr);
			} else {
				std::vector<const RE::BSGeometry*> slotActors, denseActors;
				for (const auto o : slots.actorObjects)
					slotActors.push_back(slots.objectGeometry[o]);
				for (const auto o : dense.actorObjects)
					denseActors.push_back(dense.objectGeometry[o]);
				std::sort(slotActors.begin(), slotActors.end());
				std::sort(denseActors.begin(), denseActors.end());
				if (slotActors != denseActors || !std::is_sorted(slots.actorObjects.begin(), slots.actorObjects.end())) {
					++walkParity.differ;
					note("the actor list", nullptr);
				}
			}
			if (walkParity.checks % 5 == 0) {
				const bool ok = !walkParity.differ && !walkParity.missing && !walkParity.extra && !walkParity.staleVerdicts && !walkParity.staleTraits;
				std::string byWhat;
				for (const auto& [what, count] : walkParity.byWhat)
					byWhat += fmt::format("{}{} {}", byWhat.empty() ? "" : ", ", what, count);
				if (!byWhat.empty())
					logger::info("[DCLF] walk parity differences by what: {}", byWhat);
				logger::info("[DCLF] walk parity: {} checks, {} objects compared, {} differ, {} missing, {} extra, {} stale verdicts, {} stale traits ({} slots, {} free){}{}{}{}{}{}",
					walkParity.checks, walkParity.objects, walkParity.differ, walkParity.missing, walkParity.extra, walkParity.staleVerdicts, walkParity.staleTraits,
					tables.objects.size(), tables.objectFree.size(), ok ? " <- OK" : "; first: ", ok ? "" : walkParity.first,
					walkParity.firstStale.empty() ? "" : "; first stale verdict: ", walkParity.firstStale,
					walkParity.firstStaleTraits.empty() ? "" : "; first stale traits: ", walkParity.firstStaleTraits);
				walkParity = {};
			}
		}

		void OnWalkDone(const On<ScenePoint::WalkDone>&)
		{
			auto& store = Scene::Coordinator::Get();
			if (!ParityDue(V::passSerial(store)))
				return;
			ZoneScopedN("CS.DCLF.Scene.WalkParity");
			auto& stats = V::stats(store);
			ScenePartScope scope(stats.scenePartMs[static_cast<std::size_t>(ScenePart::WalkReference)],
				stats.scenePartFrameMs[static_cast<std::size_t>(ScenePart::WalkReference)]);
			CheckWalkParity(store);
		}
	}

	void InstallWalkParity()
	{
		Subscribe<ScenePoint::WalkDone>(OnWalkDone);
	}
}
