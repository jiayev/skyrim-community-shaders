#include "Internal.h"

namespace DCLF
{
	void SceneStore::ValidateSlice()
	{
		// Safety net for missed detaches: a tracked geometry must still hang under the category node it
		// was found under. Checks a slice per frame so the cost stays flat.
		if (tracked.empty())
			return;
		std::vector<RE::BSGeometry*> stale;
		const std::size_t size = tracked.size();
		const std::size_t count = std::min(kValidationSlice, size);
		for (std::size_t i = 0; i < count; ++i) {
			validationCursor = (validationCursor + 1) % size;
			// The map's storage is a dense vector, so the cursor walks it without hashing.
			auto it = tracked.begin() + static_cast<std::ptrdiff_t>(validationCursor);
			Ineligible reason = Ineligible::None;
			if (FindCategoryNode(it->first, &reason) != it->second.categoryNode) {
				stale.push_back(it->first);
			} else if (it->second.parentReason != reason) {
				it->second.parentReason = reason;
				it->second.candidateFrame = 0;
				pendingEvaluation.push_back(it->first);
			}
		}
		for (auto* geometry : stale) {
			EraseTracked(geometry);
			++stats.validationDrops;
		}
	}

	std::string SceneStore::SceneReport()
	{
		std::string text;
		// The change log (Tables::changeLog): notes per frame by cause, and its completeness check.
		if (const double n = delta.walks) {
			std::uint64_t total = 0;
			std::string causes;
			for (std::uint32_t i = 0; i < kChangeCauseCount; ++i) {
				const auto notes = tables.changeCounts[i] - reportedChangeCounts[i];
				total += notes;
				if (notes)
					causes += fmt::format("{}{} {:.1f}", causes.empty() ? "" : ", ", kChangeCauseNames[i], notes / n);
			}
			reportedChangeCounts = tables.changeCounts;
			text += fmt::format("[DCLF] change log: {:.1f} changes a frame, by column: {} ({} entries held)\n", static_cast<double>(total) / n,
				causes.empty() ? "-" : causes, tables.changeLog.Size());
		}
		if (auto& m = materialFrameStats; m.frames) {
			const double n = static_cast<double>(m.frames);
			const auto differ = m.componentsDiffer + m.transformsDiffer;
			text += fmt::format("[DCLF] material frame components: {:.1f} samples, {:.2f} applications to {:.1f} slots, {:.1f} pending slots and {:.1f} watched transforms a frame; parity {} checks, {} slots, {} frame components and {} transforms differ{}{}\n",
				m.samples / n, m.applications / n, m.slotsApplied / n, m.pending / n, m.transformsWatched / n, m.checks, m.slotsChecked, m.componentsDiffer, m.transformsDiffer,
				m.checks ? (differ ? " <- DIFFER; first: " : " <- OK") : "", differ ? m.first : std::string());
			m = {};
		}
		if (auto& g = geometryStats; g.frames) {
			std::string differ;
			std::uint64_t total = 0;
			for (std::uint32_t stage = 0; stage < 2; ++stage)
				for (std::uint32_t v = 0; v < 64; ++v)
					if (g.differ[stage][v]) {
						total += g.differ[stage][v];
						differ += fmt::format(" {}{}={}", stage ? "PS" : "VS", v, g.differ[stage][v]);
					}
			text += fmt::format("[DCLF] pipeline constants: {:.2f} full geometry evaluations and {:.2f} frame samples a frame, {:.1f} pipelines changed, frame lighting changed {:.2f}; parity {} checks, {} pipelines, {} geometry variables, {} of {} technique blocks and {} of {} frame lightings differ{}{}{}\n",
				static_cast<double>(g.full) / g.frames, static_cast<double>(g.samples) / g.frames, static_cast<double>(g.changed) / g.frames,
				static_cast<double>(g.lightingVersions) / g.frames, g.checks, g.pipelinesChecked, total, g.techniquesDiffer, g.techniquesChecked, g.lightingDiffer, g.lightingChecked,
				g.checks ? (total || g.techniquesDiffer || g.lightingDiffer ? " <- DIFFER:" : " <- OK") : "", differ,
				(total ? "; first: " + g.first : std::string()) + (g.lightingDiffer ? "; lighting: " + g.lightingFirst : std::string()));
			g = {};
		}
		if (auto& sp = shadingParity; sp.frames) {
			text += fmt::format("[DCLF] shading resample: {:.1f} slots watched, {:.1f} LOD fade events, {:.1f} resampled a frame; parity {} checks, {} slots compared, {} changed unsampled{}{}\n",
				static_cast<double>(sp.watched) / sp.frames, static_cast<double>(sp.lodFadeEvents) / sp.frames, static_cast<double>(sp.resampled) / sp.frames, sp.checks, sp.slots, sp.missing,
				sp.checks ? (sp.missing ? " <- MISSED; first: " : " <- OK") : "", sp.first);
			sp = {};
		}
		if (auto& c = changeParity; c.checks || c.skipped) {
			text += fmt::format("[DCLF] change log parity: {} checks ({} skipped), {} slots compared, {} changed, {} changed with no log entry{}{}\n", c.checks, c.skipped, c.slots,
				c.changed, c.missing, c.missing ? " <- MISSING; first: " : " <- OK", c.first);
			c = { std::move(c.snapshot), c.cursor };
		}
		if (auto& t = delta; t.walks) {
			const double n = t.walks;
			text += fmt::format("[DCLF] scene delta: {} walks ({} full), evaluated {:.0f}/frame (max {}; per-frame {:.0f}, of them {:.0f} kept by the light path and {:.0f} moved by it; pending {:.0f}, fade {:.1f}, property {:.1f}, node {:.1f}, sun entry node {:.1f}, geometry {:.1f}), settling {:.1f}, lapsed {:.0f}, {:.0f} live slots; events per frame: {:.1f} property, {:.1f} node; {:.2f} inputs re-read changed; switch events {:.2f}/frame: {:.2f} changed, {:.2f} caught up ({:.2f} at attach), {:.1f} entries classified again\n",
				t.walks, t.full, t.evaluated / n, t.evaluatedMax, t.perFrame / n, t.kept / n, t.moved / n, t.pending / n, t.fade / n, t.property / n, t.node / n, t.roots / n,
				t.geometryDirty / n, t.settling / n, t.restored / n, t.live / n, t.propertyEvents / n, t.nodeEvents / n, t.reread / n,
				t.switchEvents / n, t.switchChanges / n, t.switchCatchUps / n, t.attachCatchUps / n, t.switchReclassified / n);
			if (rootMotion.size() > (1u << 16))
				rootMotion.clear();
			t = {};
		}
		return text;
	}

	void SceneStore::CheckWalkParity()
	{
		// The slot walk has just run. Everything a second walk overwrites is kept and restored, so the frame goes on
		// with the slot tables exactly as they were.
		const Tables slots = tables;
		const auto savedStamp = objectStamp;
		const auto savedStats = stats;
		const auto savedSkinned = skinnedObjects;
		const auto savedDecalOrder = decalOrder;
		// The dense walk may resolve a geometry slot the delta walk did not (an object only it writes, which is a
		// difference): the index map is not part of the tables, so it is kept too.
		const auto savedGeometryIndex = geometryIndex;
		const auto savedRefreshed = refreshedGeometry;
		// What the delta walk evaluated (BuildFullOrder below overwrites scheduledWalk), for the stale verdicts' report.
		ankerl::unordered_dense::set<const RE::BSGeometry*> evaluated;
		for (const auto& [geometry, entry] : tracked)
			if (entry.scheduledWalk == walkSerial)
				evaluated.insert(geometry);
		// The delta walk's `order` holds only what it evaluated; the reference is the whole tracked set.
		BuildFullOrder();
		referenceReasons.clear();
		denseWalk = true;
		DenseWalk();
		denseWalk = false;
		geometryIndex = savedGeometryIndex;
		refreshedGeometry = savedRefreshed;
		const Tables dense = std::move(tables);
		tables = slots;
		objectStamp = savedStamp;
		stats = savedStats;
		skinnedObjects = savedSkinned;
		decalOrder = savedDecalOrder;

		++walkParity.checks;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> denseIndex;
		for (std::uint32_t d = 0; d < dense.objects.size(); ++d)
			denseIndex.emplace(dense.objectGeometry[d], d);
		auto note = [&](const char* a_what, const RE::BSGeometry* a_geometry) {
			if (walkParity.first.empty())
				walkParity.first = fmt::format("{} on '{}'", a_what, a_geometry && a_geometry->name.c_str() ? a_geometry->name.c_str() : "?");
		};
		auto same = [](const auto& a_left, const auto& a_right) { return std::memcmp(&a_left, &a_right, sizeof(a_left)) == 0; };
		// A skinned object's rows, by content: the two walks lay the palettes out in their own visiting orders.
		auto sameRows = [&](std::uint32_t a_slot, std::uint32_t a_dense) {
			const std::size_t rows = slots.boneRows[a_slot];
			if (!rows)
				return true;
			const std::size_t at = std::size_t(slots.boneOffset[a_slot]) * 4, denseAt = std::size_t(dense.boneOffset[a_dense]) * 4, floats = rows * 4;
			if (at + floats > slots.bones.size() || denseAt + floats > dense.bones.size() || at + floats > slots.previousBones.size() ||
				denseAt + floats > dense.previousBones.size())
				return false;
			return std::memcmp(&slots.bones[at], &dense.bones[denseAt], floats * sizeof(float)) == 0 &&
			       std::memcmp(&slots.previousBones[at], &dense.previousBones[denseAt], floats * sizeof(float)) == 0;
		};
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
			const auto it = denseIndex.find(geometry);
			if (it == denseIndex.end()) {
				++walkParity.extra;
				note("a slot the dense walk has no object for", geometry);
				continue;
			}
			const std::uint32_t d = it->second;
			denseIndex.erase(it);
			const char* what = nullptr;
			// Every record keeps its accumulated half across walks (LapseAccumulated): compared as the walk left it, which
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
					walkParity.first = fmt::format("the object record on '{}' (per-frame {}): flags {:X}/{:X} scene {:X}/{:X} geometry {}/{} world {} previous {} bound {}",
						geometry->name.c_str() ? geometry->name.c_str() : "?", entryIt != tracked.end() && entryIt->second.perFrame, a.flags, b.flags, slots.sceneFlags[s], dense.sceneFlags[d],
						a.geometryIndex, b.geometryIndex, std::memcmp(a.world, b.world, sizeof(a.world)) == 0, std::memcmp(a.previousWorld, b.previousWorld, sizeof(a.previousWorld)) == 0,
						a.boundRadius == b.boundRadius && std::memcmp(a.boundCenter, b.boundCenter, sizeof(a.boundCenter)) == 0);
				}
			}
			else if (!same(drawRecord, dense.draws[d]))
				what = "the draw";
			else if (slots.skinPartitions[s] != dense.skinPartitions[d] || slots.boneRows[s] != dense.boneRows[d] || !sameRows(s, d))
				what = "the skin rows";
			else if (slots.shadowTechnique[s] != dense.shadowTechnique[d] || slots.shadowReject[s] != dense.shadowReject[d])
				what = "the shadow verdict";
			else if (slots.sunEntry[s] != dense.sunEntry[d]) {
				what = "the sun entry";
				if (walkParity.first.empty()) {
					const auto entryIt = tracked.find(const_cast<RE::BSGeometry*>(geometry));
					const auto& a = slots.sunEntry[s];
					const auto& b = dense.sunEntry[d];
					walkParity.first = fmt::format("the sun entry on '{}' (per-frame {}, traits {:X}, entry node '{}'): kept ({:.2f} {:.2f} {:.2f} r {:.2f}) now ({:.2f} {:.2f} {:.2f} r {:.2f})",
						geometry->name.c_str() ? geometry->name.c_str() : "?", entryIt != tracked.end() && entryIt->second.perFrame, entryIt != tracked.end() ? PerFrameTraits(entryIt->second, *geometry) : 999u,
						entryIt != tracked.end() && entryIt->second.sunEntryNode && entryIt->second.sunEntryNode->name.c_str() ? entryIt->second.sunEntryNode->name.c_str() : "?",
						a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
				}
			} else if (slots.shadowDiffuse[s] != dense.shadowDiffuse[d] || slots.shadowMaterial[s] != dense.shadowMaterial[d])
				what = "the shadow material";
			else if ((slots.faceStream[s] == kNoFaceStream) != (dense.faceStream[d] == kNoFaceStream))
				what = "the face stream";
			else if (slots.faceStream[s] != kNoFaceStream) {
				const auto& a = slots.faceStreams[slots.faceStream[s]];
				const auto& b = dense.faceStreams[dense.faceStream[d]];
				if (a.object != s || b.object != d || a.region != b.region || a.vertexCount != b.vertexCount || a.generation != b.generation || a.positions != b.positions)
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
						HiddenForWalk(object) != IsHidden(object) ? "[walk differs]" : "");
					if (object == entry.categoryNode)
						break;
				}
				const auto copies = std::count(perFrameSet.begin(), perFrameSet.end(), geometry);
				walkParity.firstStale = fmt::format("'{}' kept {} ({} frames old, per-frame {} (listed {}, {} copies in the set), traits {:X}, light {:X}, evaluated this walk {}) now {}; chain:{}",
					geometry->name.c_str() ? geometry->name.c_str() : "?", kIneligibleNames[static_cast<std::size_t>(entry.candidateReason)], frame - entry.candidateFrame,
					entry.perFrame, entry.perFrameListed, copies, PerFrameTraits(entry, *geometry), entry.lightTraits, evaluated.contains(geometry),
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
				const auto [perFrame, light] = PerFrameOf(entry, *geometry, it->second, traits, &freshMotion);
				// An actor's light path places its record every frame: motion it gained since is served already.
				if (entry.lightTraits & kTraitActor)
					traits &= ~(kTraitMoves | kTraitRootMoves);
				if (!perFrame || (entry.perFrame && (!entry.lightTraits || (light && !(traits & ~entry.lightTraits)))))
					continue;
				++walkParity.staleTraits;
				if (walkParity.firstStaleTraits.empty())
					walkParity.firstStaleTraits = fmt::format("'{}' {} traits {:X} now {:X} ({} frames since classified)", geometry->name.c_str() ? geometry->name.c_str() : "?",
						entry.perFrame ? "per-frame with" : "kept, no", entry.lightTraits, traits, frame - entry.candidateFrame);
			}
		}
		if (!denseIndex.empty())
			note("an object with no slot", denseIndex.begin()->first);
		if (liveSeen != slots.liveObjects || slots.liveObjects + slots.objectFree.size() != slots.objects.size()) {
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
			logger::info("[DCLF] walk parity: {} checks, {} objects compared, {} differ, {} missing, {} extra, {} stale verdicts, {} stale traits ({} slots, {} free){}{}{}{}{}{}",
				walkParity.checks, walkParity.objects, walkParity.differ, walkParity.missing, walkParity.extra, walkParity.staleVerdicts, walkParity.staleTraits,
				tables.objects.size(), tables.objectFree.size(), ok ? " <- OK" : "; first: ", ok ? "" : walkParity.first,
				walkParity.firstStale.empty() ? "" : "; first stale verdict: ", walkParity.firstStale,
				walkParity.firstStaleTraits.empty() ? "" : "; first stale traits: ", walkParity.firstStaleTraits);
			walkParity = {};
		}
	}

	void SceneStore::CheckChangeLog()
	{
		const bool enabled = SwitchEnabled(Switch::ChangeLogParity);
		if (!enabled)
			return;
		auto& c = changeParity;
		if (c.cursor.active) {
			const bool continues = c.cursor.Continues(tables.changeLog, tablesGeneration);
			c.cursor.active = false;
			if (!continues) {
				++c.skipped;  // the log started again (a load, a reset): every reader resyncs anyway
			} else {
				++c.checks;
				// What the log says changed since the snapshot, per slot.
				ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> logged;
				for (const auto& change : tables.changeLog.From(c.cursor.position))
					logged[change.slot] |= change.causes;
				for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot) {
					++c.slots;
					const auto now = tables.ColumnsOf(slot);
					const bool grown = slot >= c.snapshot.size();
					// A slot grown since is new: a live one must be in the log at all.
					const std::uint32_t causes = grown ? ((tables.objects[slot].flags & kObjectFree) ? 0u : kChangeAll) : Tables::CausesBetween(c.snapshot[slot], now);
					if (!causes)
						continue;
					++c.changed;
					const auto it = logged.find(slot);
					const std::uint32_t missing = grown ? (it == logged.end() ? causes : 0u) : causes & ~(it == logged.end() ? 0u : it->second);
					if (!missing)
						continue;
					++c.missing;
					if (c.first.empty()) {
						std::string names;
						for (std::uint32_t bits = missing; bits; bits &= bits - 1)
							names += fmt::format("{}{}", names.empty() ? "" : "+", kChangeCauseNames[std::countr_zero(bits)]);
						const auto* geometry = tables.objectGeometry[slot];
						c.first = fmt::format("frame {}: slot {} '{}' changed {} with no log entry for it", frame, slot,
							geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", names);
					}
				}
			}
		}
		if (ParityDue(frame)) {
			c.snapshot.resize(tables.objects.size());
			for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
				c.snapshot[slot] = tables.ColumnsOf(slot);
			c.cursor.Restart(tablesGeneration);
			c.cursor.Advance(tables.changeLog);
		}
	}
}
