#include "Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/MirrorWatch.h"

namespace DCLF
{
	void SceneStore::ValidateSlice()
	{
		// Safety net for missed detaches: a tracked geometry must still hang under the category node it
		// was found under. Checks a slice per frame so the cost stays flat.
		if (tracked.empty())
			return;
		std::vector<RE::BSGeometry*> stale;
		// The entries are the mirror's (the scene as the applied batch left it), the check live: a geometry detached since the batch's
		// ingestion differs until the next batch's detach erases it. A suspect still tracked and still differing a batch later is a
		// detach the events missed (T6b1b).
		for (auto* geometry : std::exchange(validationSuspects, {}))
			if (const auto it = tracked.find(geometry); it != tracked.end() && FindCategoryNodeLive(geometry, nullptr) != it->second.categoryNode)
				stale.push_back(geometry);
		const std::size_t size = tracked.size();
		const std::size_t count = std::min(kValidationSlice, size);
		for (std::size_t i = 0; i < count; ++i) {
			validationCursor = (validationCursor + 1) % size;
			// The map's storage is a dense vector, so the cursor walks it without hashing.
			auto it = tracked.begin() + static_cast<std::ptrdiff_t>(validationCursor);
			Ineligible reason = Ineligible::None;
			if (FindCategoryNodeLive(it->first, &reason) != it->second.categoryNode) {
				validationSuspects.push_back(it->first);
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
			if (g.templateChecked || g.templateMissing)
				text += fmt::format("[DCLF] pipeline templates (T6: the synthetic pass against a registered one's evaluation): {} checked ({} with none registered), {} differ "
									"in the bindless draw's values, {} in the frame lighting{}{}\n",
					g.templateChecked, g.templateMissing, g.templateDiffer, g.templateLightingDiffer, g.templateDiffer || g.templateLightingDiffer ? " <- TEMPLATE" : " <- OK",
					g.templateFirst.empty() ? "" : "; first: " + g.templateFirst);
			g = {};
		}
		if (auto& sp = shadingParity; sp.frames) {
			text += fmt::format("[DCLF] shading (sampled at the frame's start for the slots its events name): {:.1f} LOD fade events, {:.1f} emittance events, {:.1f} "
								"named a frame; parity at Prepass against the frame's rows: {} checks, {} slots compared, changed since the frame's start: {} named for "
								"the next frame, {} queued, {} missed{}{}; wetness {} meshes, {} differ{}\n",
				static_cast<double>(sp.lodFadeEvents) / sp.frames, static_cast<double>(sp.emittanceEvents) / sp.frames, static_cast<double>(sp.resampled) / sp.frames, sp.checks,
				sp.slots, sp.named, sp.late, sp.missing, sp.checks ? (sp.missing ? " <- MISSED; first: " : " <- OK") : "", sp.first, sp.wetness, sp.wetnessDiffer,
				sp.wetness ? (sp.wetnessDiffer ? " <- DIFFER" : " <- OK") : "");
			sp = {};
		}
		if (auto& dp = decalOrderParity; dp.checks) {
			text += fmt::format("[DCLF] decal order parity (the kept order against one made whole): {} checks, {} decals, {} differ{}{}; {} kept keys stale, "
								"{} neighbours the whole order does not hold{}{}\n",
				dp.checks, dp.decals, dp.differ, dp.differ ? " <- DIFFER; first: " : " <- OK", dp.first, dp.staleKeys, dp.unordered,
				dp.staleKeys ? "; first stale: " : "", dp.staleFirst);
			dp = {};
		}
		if (auto& ep = extrasParity; ep.objects) {
			text += fmt::format("[DCLF] extras parity (the draw's completion against the engine's routines): {} objects, {} differ, largest difference {}, {} static rows "
								"stale with no event{}{}\n",
				ep.objects, ep.differ, ep.maxDifference, ep.staleStatic, ep.differ || ep.staleStatic ? " <- DIFFER" : " <- OK", ep.differ ? "; first: " + ep.first : std::string());
			ep = {};
		}
		if (auto& l = lodSegmentStats; l.events || l.checks || !lodRanges.empty()) {
			std::size_t partial = 0;
			for (const auto& [shape, ranges] : lodRanges) {
				const std::uint32_t whole = LodSegments::At<std::uint16_t>(shape, LodSegments::kTriangleCount) * 3u;
				partial += ranges.size() != 1 || ranges[0].firstIndex != 0 || ranges[0].indexCount != whole ? 1 : 0;
			}
			text += fmt::format("[DCLF] object LOD segments: {} shapes ({} partly hidden){}; {} segment events, {} drawn-range changes; parity {} checks, {} shapes compared, {} differ{}{}\n",
				lodRanges.size(), partial, lodSegmentEventsInstalled ? "" : ", events not installed", l.events, l.changed, l.checks, l.shapes, l.differ,
				l.checks ? (l.differ ? " <- LOD SEGMENTS; first: " : " <- OK") : "", l.first);
			l = {};
		}
		if (treeLod.Size() || TreeLod::installed)
			text += treeLod.Report();
		if (auto& c = changeParity; c.checks || c.skipped) {
			text += fmt::format("[DCLF] change log parity: {} checks ({} skipped), {} slots compared, {} changed, {} changed with no log entry{}{}\n", c.checks, c.skipped, c.slots,
				c.changed, c.missing, c.missing ? " <- MISSING; first: " : " <- OK", c.first);
			c = { std::move(c.snapshot), c.cursor };
		}
		text += mirror.Report();
		text += MirrorReadReport();
		text += MirrorWatch::TakeReport(frame);
		if (const auto c = SceneCapture::TakeCounters(); c.attaches || c.outOfWorld)
			text += fmt::format("[DCLF] scene capture (6e F3): {} attaches in the world captured ({} records, {:.1f} us in all; {} on the main thread, {:.1f} us), {} out of the "
								"world left to their world attach\n",
				c.attaches, c.records, static_cast<double>(c.ns) / 1000.0, c.mainThread, static_cast<double>(c.mainThreadNs) / 1000.0, c.outOfWorld);
		if (captureFrames) {
			const auto unscoped = FrameGlobals::TakeUnscopedReads();
			text += fmt::format("[DCLF] frame capture (6e F2): render thread {:.1f} us/frame for the globals, {:.1f} us/frame for the categories ({} captures made; {} subtrees the mirror lacked captured, {} records; "
								"{} subtrees waited for the mirror, {} dropped with no chain after); {} reads off the render thread with no frame's capture bound{}\n",
				static_cast<double>(captureNs) / 1000.0 / captureFrames, static_cast<double>(categoryCaptureNs) / 1000.0 / captureFrames, categoryCapturesMade, categoryMirrorCaptures, categoryMirrorRecords,
				subtreesPended, subtreesDropped, unscoped, unscoped ? " <- UNSCOPED GLOBALS" : " <- OK");
			captureNs = categoryCaptureNs = categoryCapturesMade = captureFrames = 0; categoryMirrorCaptures = categoryMirrorRecords = 0;
			subtreesPended = subtreesDropped = 0;
		}
		if (auto& t = delta; t.walks) {
			const double n = t.walks;
			// The catch-ups are the render thread's, at ingestion (CatchUpSwitches).
			t.switchCatchUps = catchUpsBySwitch.exchange(0, std::memory_order_relaxed);
			t.attachCatchUps = catchUpsByAttach.exchange(0, std::memory_order_relaxed);
			const double catchUpUs = static_cast<double>(catchUpNs.exchange(0, std::memory_order_relaxed)) / 1000.0 / n;
			text += fmt::format("[DCLF] candidates: sun {} entries ({} indices, {} geometry rows), {} written in {} snapshots; light {} entries ({} indices), "
								"{} written in {} snapshots; parity {} checks, {} differ{}\n",
				sunTable.index.size(), sunTable.nodes.size(), sunTable.geometryIndex.size(), std::exchange(sunTable.entriesWritten, 0), std::exchange(sunTable.snapshots, 0),
				lightTable.index.size(), lightTable.nodes.size(), std::exchange(lightTable.entriesWritten, 0), std::exchange(lightTable.snapshots, 0),
				candidateParity.checks, candidateParity.mismatches, candidateParity.checks ? (candidateParity.mismatches ? " <- CANDIDATES" : " <- OK") : "");
			candidateParity = {};
			text += fmt::format("[DCLF] scene delta: {} walks ({} full), evaluated {:.0f}/frame (max {}; per-frame {:.0f}, of them {:.0f} kept by the light path and {:.0f} moved by it; pending {:.0f}, fade {:.1f}, property {:.1f}, node {:.1f}, sun entry node {:.1f}, geometry {:.1f}), lapsed {:.0f}, {:.0f} live slots; events per frame: {:.1f} property, {:.1f} node; {:.2f} inputs re-read changed; switch events {:.2f}/frame: {:.2f} changed, {:.2f} caught up by the render thread at ingestion ({:.2f} under attached subtrees or the world after a load; {:.2f} us/frame), {:.1f} entries classified again; face publications {:.1f}/frame: {:.1f} streams updated in place, {:.2f} shapes written\n",
				t.walks, t.full, t.evaluated / n, t.evaluatedMax, t.perFrame / n, t.kept / n, t.moved / n, t.pending / n, t.fade / n, t.property / n, t.node / n, t.roots / n,
				t.geometryDirty / n, t.restored / n, t.live / n, t.propertyEvents / n, t.nodeEvents / n, t.reread / n,
				t.switchEvents / n, t.switchChanges / n, t.switchCatchUps / n, t.attachCatchUps / n, catchUpUs, t.switchReclassified / n, t.facePublished / n, t.faceUpdated / n,
				t.faceWritten / n);
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
		// The dense walk may resolve a geometry slot the delta walk did not (an object only it writes, which is a
		// difference): the index map is not part of the tables, so it is kept too.
		const auto savedGeometryIndex = geometryIndex;
		const auto savedRefreshed = refreshedGeometry;
		// The face regions likewise: the streams that hold them are the slot tables'.
		const auto savedFaceRegions = faceRegions;
		const auto savedFaceRegionFree = faceRegionFree;
		const auto savedFaceRegionTop = faceRegionTop;
		const auto savedTreeOwners = treeOwners;
		const auto savedFadeRootOwners = fadeRootOwners;
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
		faceRegions = savedFaceRegions;
		faceRegionFree = savedFaceRegionFree;
		faceRegionTop = savedFaceRegionTop;
		treeOwners = savedTreeOwners;
		fadeRootOwners = savedFadeRootOwners;
		const Tables dense = std::move(tables);
		tables = slots;
		objectStamp = savedStamp;
		stats = savedStats;
		skinnedObjects = savedSkinned;

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
				const auto copies = std::count_if(perFrameSet.begin(), perFrameSet.end(), [&](const PerFrameItem& a_item) { return a_item.geometry == geometry; });
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
