#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	/** @brief The kept path of BuildShadowPayload: the material rows and the inputs from the kept state (ShadowKept). */
	void BuildKeptShadow(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept& k,
		const ShadowMaterialRow& a_plain)
	{
		ZoneScopedN("CS.DCLF.BuildShadow.Kept");
		const std::uint32_t objects = static_cast<std::uint32_t>(std::min<std::size_t>(a_tables.objects.size(), kMaxObjects));
		++k.builds;
		// Membership changes of this build carry this stamp.
		const std::uint64_t build = ++k.build;
		// What the buffers hold is the version they were sent: what changes from here on is sent alone.
		k.rows.BeginBuild(a_in.materialRowsHeld);
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			k.modes[m].inputs.BeginBuild(a_in.inputsHeld[m]);
		bool resync = !k.cursor.Continues(a_tables.changeLog, a_in.tablesGeneration) || k.identity != a_in.addresses.identity || k.objectRecord.size() > objects;
		if (resync) {
			k.Reset();
			k.cursor.Restart(a_in.tablesGeneration);
			k.identity = a_in.addresses.identity;
			++k.resyncs;
		}
		if (k.objectRecord.size() < objects) {
			k.objectRecord.resize(objects, ShadowKept::kNoRecord);
			k.objectMaterial.resize(objects, nullptr);
		}
		auto writeRow = [&](std::uint32_t a_slot, const ShadowMaterialRow& a_row) {
			if (a_slot >= k.rows.Size()) {
				auto& rows = k.rows.Mutable();
				rows.resize(std::size_t(a_slot) + 1);
				rows[a_slot] = a_row;
				k.rows.Mark(a_slot);
				++k.rowsWritten;
			} else if (k.rows.Set(a_slot, a_row)) {
				++k.rowsWritten;
			}
		};
		// ---- The material rows: the plain one, and each material's.
		if (k.slotMaterial.empty()) {
			k.slotMaterial.push_back(nullptr);  // slot 0: the plain record
			k.slotDiffuse.push_back(nullptr);
			k.slotRefs.push_back(0);
			k.slotReady.push_back(1);
		}
		writeRow(0, a_plain);
		const std::uint32_t transformBuffer = globals::game::smState ? (globals::game::smState->textureTransformCurrentBuffer & 1) : 0u;
		auto materialRow = [&](std::uint32_t a_slot) {
			// The material's diffuse and texture offset (read off the material now: shader-property controllers move it
			// between the walk and this build). A slot past the table's capacity waits for the next frame's growth.
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[a_slot]);
			const auto textureIt = k.slotDiffuse[a_slot] ? a_lookups.shadowTextures.find(k.slotDiffuse[a_slot]) : a_lookups.shadowTextures.end();
			const bool ready = textureIt != a_lookups.shadowTextures.end() && textureIt->second != Lookups::kNone && a_slot < a_in.addresses.recordCapacity;
			k.slotReady[a_slot] = ready ? 1 : 0;
			if (!ready)
				return;
			ShadowMaterialRow row;
			row.texcoord = { material->texCoordOffset[transformBuffer].x, material->texCoordOffset[transformBuffer].y, material->texCoordScale[transformBuffer].x,
				material->texCoordScale[transformBuffer].y };
			row.diffuse = textureIt->second;
			writeRow(a_slot, row);
			if (const auto owner = a_lookups.shadowTextureOwners.find(k.slotDiffuse[a_slot]); owner != a_lookups.shadowTextureOwners.end())
				a_out.bindingOwners.push_back(owner->second);
		};
		// An object's record: the plain one, its material's (acquired), or none.
		auto recordOf = [&](std::uint32_t o) -> std::uint32_t {
			const auto& object = a_tables.objects[o];
			if (object.flags & kObjectFree)
				return ShadowKept::kNoRecord;
			const std::uint32_t sky = o < a_tables.skyTechnique.size() && a_in.modeUsed[kSkyMode] ? a_tables.skyTechnique[o] : 0u;
			if ((object.flags & kObjectNoShadow) && !sky)
				return ShadowKept::kNoRecord;
			if (!(((object.flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[o]) & 0x80) && !(sky & 0x80))
				return 0;
			const auto* material = o < a_tables.shadowMaterial.size() ? a_tables.shadowMaterial[o] : nullptr;
			if (!material)
				return ShadowKept::kNoRecord;
			auto [it, fresh] = k.slotOf.try_emplace(material, 0u);
			if (fresh) {
				std::uint32_t slot;
				if (!k.freeSlots.empty()) {
					std::pop_heap(k.freeSlots.begin(), k.freeSlots.end(), std::greater<>());
					slot = k.freeSlots.back();
					k.freeSlots.pop_back();
				} else {
					slot = static_cast<std::uint32_t>(k.slotMaterial.size());
					k.slotMaterial.push_back(nullptr);
					k.slotDiffuse.push_back(nullptr);
					k.slotRefs.push_back(0);
					k.slotReady.push_back(0);
				}
				it->second = slot;
				k.slotMaterial[slot] = material;
				k.slotDiffuse[slot] = a_tables.shadowDiffuse[o];
				k.slotReady[slot] = 0;
			}
			return it->second;
		};
		auto releaseRecord = [&](std::uint32_t o) {
			const std::uint32_t slot = k.objectRecord[o];
			if (slot != ShadowKept::kNoRecord && slot != 0 && slot < k.slotRefs.size() && k.objectMaterial[o] && --k.slotRefs[slot] == 0) {
				k.slotOf.erase(k.slotMaterial[slot]);
				k.slotMaterial[slot] = nullptr;
				k.slotDiffuse[slot] = nullptr;
				k.slotReady[slot] = 0;
				k.freeSlots.push_back(slot);
				std::push_heap(k.freeSlots.begin(), k.freeSlots.end(), std::greater<>());
			}
			k.objectRecord[o] = ShadowKept::kNoRecord;
			k.objectMaterial[o] = nullptr;
		};
		auto takeRecord = [&](std::uint32_t o) {
			releaseRecord(o);
			const std::uint32_t slot = recordOf(o);
			k.objectRecord[o] = slot;
			if (slot != ShadowKept::kNoRecord && slot != 0) {
				++k.slotRefs[slot];
				k.objectMaterial[o] = k.slotMaterial[slot];
			}
		};

		// ---- The modes: a mode whose views' states changed is read again whole.
		const bool entryOnGpu = true;  // the kept path runs only when the latch holds the sun's processes
		(void)entryOnGpu;
		auto evaluate = [&](std::uint32_t m, std::uint32_t o, DrawInput& a_input) -> int {
			// 0: no input, 1: an input (a_input), 2: waiting (a pipeline or a texture), 3: the frame's list (a face).
			const auto& object = a_tables.objects[o];
			const bool skyMode = m == kSkyMode;
			if (object.flags & kObjectFree)
				return 0;
			if (skyMode ? (o >= a_tables.skyTechnique.size() || !a_tables.skyTechnique[o]) : (object.flags & kObjectNoShadow) != 0)
				return 0;
			const std::uint32_t record = k.objectRecord[o];
			if (record == ShadowKept::kNoRecord)
				return 0;
			if (record != 0 && !(record < k.slotReady.size() && k.slotReady[record]))
				return 2;
			const bool volumetricOnly = !skyMode && (object.flags & kObjectVolumetricOnly) != 0;
			const std::uint32_t classStates = volumetricOnly ? (a_in.modeRasterStates[m] >> 16) : (a_in.modeRasterStates[m] & 0xFFFFu);
			if (classStates == 0)
				return 0;
			const std::uint32_t technique = skyMode ? a_tables.skyTechnique[o] : (a_tables.shadowTechnique[o] | ModeBitsOf(m));
			const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
			const auto slotIt = a_lookups.shadowSlots.find(key);
			if (slotIt == a_lookups.shadowSlots.end())
				return 2;
			for (std::uint32_t states = classStates; states; states &= states - 1) {
				const auto& row = a_lookups.shadowMapRows[std::countr_zero(states)];
				if ((slotIt->second < row.size() ? row[slotIt->second] : Lookups::kNone) == Lookups::kNone)
					return 2;
			}
			if (IsFaceObject(a_tables, o) || (key.vertexLayout & kPositionInSecondStream))
				return 3;
			a_input = { slotIt->second, record, object.geometryIndex, (object.flags & ~kObjectDecal) | kInputDrawable,
				{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, o, 0, PartitionsOf(a_tables, o), ~0u };
			SetSunEntryRow(a_input, a_tables, o);
			return 1;
		};
		auto removeEntry = [&](ShadowKept::Mode& a_mode, std::uint32_t o) {
			if (!a_mode.Holds(o))
				return;
			if (o < a_mode.claimedOf.size() && a_mode.claimedOf[o]) {
				if (const auto held = a_mode.claims.find(a_mode.claimedOf[o]); held != a_mode.claims.end() && held->second == o)
					a_mode.claims.erase(held);
				a_mode.claimedOf[o] = nullptr;
				a_mode.claimsChanged = true;
			}
			a_mode.Remove(o);
			a_mode.membership = build;
			++k.entriesWritten;
		};
		auto setMark = [](std::vector<std::uint32_t>& a_list, std::vector<std::uint8_t>& a_mark, std::uint32_t o, bool a_on) {
			if (a_mark.size() <= o)
				a_mark.resize(std::size_t(o) + 1, 0);
			if (a_on && !a_mark[o]) {
				a_mark[o] = 1;
				a_list.push_back(o);
			} else if (!a_on && a_mark[o]) {
				a_mark[o] = 0;  // dropped from the list at its next pass
			}
		};
		// Claims hold a geometry: the one a slot draws now (a slot reused by another object removed its entry first).
		auto take = [&](std::uint32_t m, std::uint32_t o) {
			auto& mode = k.modes[m];
			mode.Cover(objects);
			if (mode.claimedOf.size() < objects)
				mode.claimedOf.resize(objects, nullptr);
			DrawInput input{};
			const int result = o < a_tables.objects.size() ? evaluate(m, o, input) : 0;
			setMark(mode.waiting, mode.waitingMark, o, result == 2);
			const bool wasFace = o < mode.faceMark.size() && mode.faceMark[o];
			setMark(mode.faces, mode.faceMark, o, result == 3);
			if (wasFace != (result == 3))
				mode.membership = build;
			if (result != 1) {
				removeEntry(mode, o);
				return;
			}
			std::uint32_t i = mode.indexOf[o];
			if (i == kNoRegion) {
				if (mode.inputs.Size() >= kMaxInputs - kLoopReserve)
					return;
				mode.Add(o, input);
				mode.membership = build;
				++k.entriesWritten;
			} else if (mode.inputs.Set(i, input)) {
				++k.entriesWritten;
			}
			// Its claim: the geometry the slot draws now.
			const auto* geometry = o < a_tables.objectGeometry.size() ? a_tables.objectGeometry[o] : nullptr;
			if (mode.claimedOf[o] != geometry) {
				if (mode.claimedOf[o])
					if (const auto held = mode.claims.find(mode.claimedOf[o]); held != mode.claims.end() && held->second == o)
						mode.claims.erase(held);
				if (geometry)
					mode.claims[geometry] = o;
				mode.claimedOf[o] = geometry;
				mode.claimsChanged = true;
			}
			return;
		};

		// The objects the log names (or every object on a resync, or when a mode's views changed their states - a record
		// depends on whether Skylighting's map is drawn): their record, then their entry in each mode.
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			resync |= k.modes[m].active && k.modes[m].rasterStates != (a_in.modeUsed[m] ? a_in.modeRasterStates[m] : 0u);
		std::vector<std::uint32_t> changed;
		if (resync) {
			changed.reserve(objects);
			for (std::uint32_t o = 0; o < objects; ++o)
				changed.push_back(o);
		} else {
			constexpr std::uint32_t kShadowCauses = kChangeShadow | kChangeBindings | kChangePlacement | kChangeGeometry | kChangeSkin | kChangeMembership;
			for (const auto& change : k.cursor.Unread(a_tables.changeLog))
				if ((change.causes & kShadowCauses) && change.slot < objects)
					changed.push_back(change.slot);
		}
		k.cursor.Advance(a_tables.changeLog);
		for (const std::uint32_t o : changed)
			takeRecord(o);
		// The rows of every material slot held (its texture may have been resolved since, its texcoord moves, the table may
		// have grown to hold it).
		for (std::uint32_t slot = 1; slot < k.slotMaterial.size(); ++slot) {
			if (!k.slotMaterial[slot])
				continue;
			materialRow(slot);
			a_out.waitingRows += slot >= a_in.addresses.recordCapacity ? 1 : 0;
		}
		a_out.rowsWanted = static_cast<std::uint32_t>(k.slotMaterial.size());
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			auto& mode = k.modes[m];
			const std::uint32_t states = a_in.modeUsed[m] ? a_in.modeRasterStates[m] : 0u;
			if (!mode.active || mode.rasterStates != states) {
				// A mode's views changed their states: every object again for it.
				mode.Reset();
				mode.active = true;
				mode.rasterStates = states;
				if (states)
					for (std::uint32_t o = 0; o < objects; ++o)
						take(m, o);
			} else if (states) {
				for (const std::uint32_t o : changed)
					take(m, o);
				// What waited: again, while it still waits.
				std::vector<std::uint32_t> waiting;
				waiting.swap(mode.waiting);
				for (const std::uint32_t o : waiting) {
					if (o >= mode.waitingMark.size() || !mode.waitingMark[o])
						continue;
					mode.waitingMark[o] = 0;
					take(m, o);
				}
			}
			if (!a_in.modeUsed[m])
				continue;
			// The frame's list: the face shapes, with their positions of this walk.
			auto& list = a_out.inputList[m];
			std::vector<const RE::BSGeometry*> faceGeometries;
			std::size_t keptFaces = 0;
			for (const std::uint32_t o : mode.faces) {
				if (o >= mode.faceMark.size() || !mode.faceMark[o])
					continue;
				mode.faces[keptFaces++] = o;
				const auto& object = a_tables.objects[o];
				const std::uint32_t streamIndex = FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions);
				if (streamIndex == ~0u)
					continue;  // no positions this walk: no input, the engine's
				const bool skyMode = m == kSkyMode;
				const std::uint32_t technique = skyMode ? a_tables.skyTechnique[o] : (a_tables.shadowTechnique[o] | ModeBitsOf(m));
				const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
					VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
				const auto slotIt = a_lookups.shadowSlots.find(key);
				if (slotIt == a_lookups.shadowSlots.end())
					continue;
				list.push_back({ slotIt->second, k.objectRecord[o], object.geometryIndex, (object.flags & ~kObjectDecal) | kInputDrawable,
					{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, o, 0, PartitionsOf(a_tables, o), streamIndex });
				SetSunEntryRow(list.back(), a_tables, o);
				if (o < a_tables.objectGeometry.size() && a_tables.objectGeometry[o])
					faceGeometries.push_back(a_tables.objectGeometry[o]);
			}
			mode.faces.resize(keptFaces);
			// The claims: the region's, and the frame's list's; a new set only when either changed.
			std::sort(faceGeometries.begin(), faceGeometries.end());
			if (mode.claimsChanged || faceGeometries != mode.lastFaces || !mode.published) {
				auto claims = std::make_shared<PassCapture::ClaimSet>();
				claims->reserve(mode.claims.size() + faceGeometries.size());
				for (const auto& [geometry, owner] : mode.claims)
					claims->insert(geometry);
				claims->insert(faceGeometries.begin(), faceGeometries.end());
				mode.published = std::move(claims);
				mode.lastFaces = std::move(faceGeometries);
				mode.claimsChanged = false;
			}
			if (m != kSkyMode)
				a_out.claims[m] = mode.published;
			a_out.regionInputs[m] = mode.inputs.View();
			a_out.membership[m] = mode.membership;
			// The skip counters, as the loop kept them: what waits.
			a_out.deferredPipelines += static_cast<std::uint32_t>(mode.waiting.size());
			a_out.skippedPipeline += static_cast<std::uint32_t>(mode.waiting.size());
		}
		// Skylighting's map is DCLF's only when every occluder is drawn: what waits leaves it to the engine this frame.
		if (a_in.modeUsed[kSkyMode])
			for (const std::uint32_t o : k.modes[kSkyMode].waiting)
				a_out.skySkipped += o < k.modes[kSkyMode].waitingMark.size() && k.modes[kSkyMode].waitingMark[o] ? 1 : 0;
		a_out.kept = true;
		a_out.materialRows = k.rows.View();
	}

	std::shared_ptr<SunExclusion> BuildSunExclusion(const std::shared_ptr<const SunCandidates>& a_candidates, const ShadowPayload& a_payload, std::uint32_t a_mode,
		const SceneStore::Tables& a_tables, SunExclusionCache* a_cache)
	{
		ZoneScopedN("CS.DCLF.BuildSunExclusion");
		if (!a_candidates || a_candidates->entries.empty())
			return nullptr;
		auto exclusion = std::make_shared<SunExclusion>();
		exclusion->candidates = a_candidates;
		const std::size_t count = a_candidates->entries.size();
		const std::uint64_t membership = a_payload.kept ? a_payload.membership[a_mode] : 0;
		bool wouldReuse = false;
		if (a_cache) {
			auto& c = *a_cache;
			++c.builds;
			bool reuse = c.valid && membership && c.candidates == a_candidates && c.membership == membership &&
			             c.cursor.Continues(a_tables.changeLog, a_payload.inputs.tablesGeneration);
			if (reuse)
				for (const auto& change : c.cursor.Unread(a_tables.changeLog))
					if (change.causes & (kChangeBindings | kChangeGeometry)) {
						reuse = false;
						break;
					}
			c.cursor.Advance(a_tables.changeLog);
			// CS_DCLF_PERSISTENT_PARITY: a reuse is built in full every 60 frames and compared.
			wouldReuse = reuse;
			if (reuse && PersistentParityEnabled() && ParityDue(a_payload.inputs.frameNumber))
				reuse = false;
			if (reuse) {
				++c.reused;
				exclusion->excluded = c.excluded;
				exclusion->excludedCount = c.excludedCount;
				exclusion->removed = std::make_unique<std::atomic<std::uint32_t>[]>(count);
				exclusion->cleared = std::make_unique<std::atomic<std::uint32_t>[]>(a_candidates->geometryEntry.size());
				return exclusion;
			}
		}
		exclusion->excluded.assign(count, 1);
		std::vector<std::uint8_t> isInput(a_tables.objects.size(), 0);
		a_payload.ForEachInput(a_mode, [&](const DrawInput& a_input) {
			if (a_input.objectIndex < isInput.size())
				isInput[a_input.objectIndex] = 1;
		});
		for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
			if ((a_tables.objects[o].flags & (kObjectFree | kObjectNoShadow)) || isInput[o])
				continue;
			if (const auto it = a_candidates->geometries.find(a_tables.objectGeometry[o]); it != a_candidates->geometries.end())
				exclusion->excluded[a_candidates->geometryEntry[it->second]] = 0;
		}
		exclusion->excludedCount = static_cast<std::uint32_t>(std::count(exclusion->excluded.begin(), exclusion->excluded.end(), std::uint8_t(1)));
		if (a_cache) {
			auto& c = *a_cache;
			if (wouldReuse) {
				c.parity.Check(c.excluded == exclusion->excluded);
			}
			c.candidates = a_candidates;
			c.cursor.Restart(a_payload.inputs.tablesGeneration);
			c.membership = membership;
			c.excluded = exclusion->excluded;
			c.excludedCount = exclusion->excludedCount;
			c.valid = true;
		}
		exclusion->removed = std::make_unique<std::atomic<std::uint32_t>[]>(count);
		exclusion->cleared = std::make_unique<std::atomic<std::uint32_t>[]>(a_candidates->geometryEntry.size());
		return exclusion;
	}

	void CheckKeptShadow(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, const ShadowPayload& a_kept, ShadowKept& k)
	{
		ShadowPayload reference;
		BuildShadowPayload(a_in, a_tables, a_lookups, reference);
		auto fail = [&](std::uint32_t a_object, const std::string& a_what) {
			if (k.parity.mismatches++ == 0)
				k.parity.first = fmt::format("object {}: {}", a_object, a_what);
		};
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_in.modeUsed[m])
				continue;
			ankerl::unordered_dense::map<std::uint32_t, DrawInput> keptInputs;
			a_kept.ForEachInput(m, [&](const DrawInput& a_input) { keptInputs.emplace(a_input.objectIndex, a_input); });
			std::size_t matched = 0;
			for (const auto& input : reference.inputList[m]) {
				++k.parity.checks;
				const auto it = keptInputs.find(input.objectIndex);
				if (it == keptInputs.end()) {
					// Why the kept build lacks it: no record, waiting on a pipeline or texture, in the frame's face list, or
					// held nowhere (an object no change named since it became a caster).
					const std::uint32_t o = input.objectIndex;
					const auto& mode = k.modes[m];
					const std::uint32_t record = o < k.objectRecord.size() ? k.objectRecord[o] : ShadowKept::kNoRecord;
					const char* why = record == ShadowKept::kNoRecord                        ? "no record" :
					                  o < mode.waitingMark.size() && mode.waitingMark[o]      ? "waiting" :
					                  o < mode.faceMark.size() && mode.faceMark[o]            ? "a face" :
					                  mode.Holds(o)                                           ? "held" :
					                                                                            "not held";
					auto& count = k.missingBy[why];
					if (!count++) {
						const auto* geometry = o < a_tables.objectGeometry.size() ? a_tables.objectGeometry[o] : nullptr;
						k.missingFirst[why] = fmt::format("'{}' (object {}, flags {:#x}, technique {:#x}, record {} against {})",
							geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", o, a_tables.objects[o].flags, a_tables.shadowTechnique[o], record,
							input.recordIndex);
					}
					fail(o, fmt::format("mode {}: an input of the per-frame build only ({})", m, why));
					continue;
				}
				++matched;
				DrawInput x = it->second, y = input;
				const std::uint32_t xr = x.recordIndex, yr = y.recordIndex;
				x.recordIndex = y.recordIndex = 0;
				if (std::memcmp(&x, &y, sizeof(DrawInput)) != 0) {
					fail(input.objectIndex, fmt::format("mode {}: the input differs", m));
					continue;
				}
				// The rows by value: the two builds number them differently.
				const auto* a = a_kept.materialRows.At(xr);
				const auto* b = reference.materialRows.At(yr);
				if (!a || !b) {
					fail(input.objectIndex, "a material row out of range");
					continue;
				}
				if (!(*a == *b))
					fail(input.objectIndex, fmt::format("mode {}: its material row differs (kept {} against {})", m, xr, yr));
			}
			if (matched != keptInputs.size())
				fail(~0u, fmt::format("mode {}: {} inputs of the kept build only", m, keptInputs.size() - matched));
			if (m != kSkyMode && a_kept.claims[m]) {
				const auto claims = ShadowClaimSet(reference.inputList[m], a_tables);
				if (*claims != *a_kept.claims[m])
					fail(~0u, fmt::format("mode {}: the claims differ ({} against {})", m, a_kept.claims[m]->size(), claims->size()));
			}
		}
	}

	namespace
	{
		/** @brief Per used mode, the draws its inputs can produce (a skin once per partition): its views' max count. */
		void CountModeDraws(ShadowPayload& a_out)
		{
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!a_out.inputs.modeUsed[m])
					continue;
				std::uint64_t draws = 0;
				a_out.ForEachInput(m, [&](const DrawInput& a_input) { draws += a_input.partitions ? std::popcount(a_input.partitions) : 1; });
				a_out.modeDraws[m] = static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX));
			}
		}
	}

	void BuildShadowPayload(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		ObjectRecordStore* a_objects, BonesStore* a_bones, ShadowKept* a_kept, GeometryStore* a_geometries)
	{
		ZoneScopedN("CS.DCLF.BuildShadowPayload");
		a_out.Reset();
		a_out.inputs = a_in;
		if (a_lookups.sharedBindingBlock)
			a_out.bindingOwners.push_back(a_lookups.sharedBindingBlock);
		const std::uint64_t base = a_in.addresses.constants;
		auto& arena = a_out.arena;
		auto block = [&](const void* a_data, std::size_t a_size) -> std::uint64_t {
			const auto offset = arena.Allocate(a_size);
			if (offset == ~0ull)
				return 0;
			if (a_data)
				std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
			return base + offset;
		};

		// ---- What every view shares: the object records, bone rows, geometry table, binding records.
		UpdateObjectRecords(a_objects, a_in.tablesHeld.objects, a_tables, a_in.tablesGeneration, a_in.frameNumber, a_out.objects);
		UpdateBones(a_bones, a_in.tablesHeld.bones, a_tables, a_in.tablesGeneration, a_out.bones);
		UpdateGeometryDraws(a_geometries, a_in.tablesHeld.geometries, a_tables, a_in.tablesGeneration, a_in.frameNumber, a_out.geometries);
		AppendFaceStreams(a_tables, a_in.addresses.facePositions, a_out.geometries);
		if (a_in.addresses.facePositions)
			a_out.faceStreams = a_tables.faceStreams;
		// The views' blocks at the head of the arena (the commit's), then the frame record, then the blocks every view's push
		// data names. No register the Utility shaders declare may be left at address zero: a pipeline that reads one arrives
		// from the background compiler seconds after the first epoch, and a null read is a device loss. Everything not
		// supplied below reads zeros.
		(void)arena.Allocate(kShadowArenaBlocksOffset);
		static constexpr std::size_t kZeroBlockBytes = 1024;
		a_out.zerosAddress = block(nullptr, kZeroBlockBytes);
		// Community Shaders' SharedData (b5) and FeatureData (b6): the alpha-tested pixel stage samples
		// its diffuse with SharedData::MipBias. Packed from the copies the inputs carry, as the main epochs do.
		a_out.sharedDataAddress = a_in.sharedData.empty() ? a_out.zerosAddress : block(a_in.sharedData.data(), a_in.sharedData.size());
		a_out.featureDataAddress = a_in.featureData.empty() ? a_out.zerosAddress : block(a_in.featureData.data(), a_in.featureData.size());
		// The frame record: every texture and sampler but the diffuse, which is the draw's material row's.
		auto& frameRecord = a_out.frameRecord;
		const std::uint32_t nullIndex = a_lookups.nullTexture == Lookups::kNone ? 0u : a_lookups.nullTexture;
		for (auto& index : frameRecord.textures)
			index = nullIndex;
		frameRecord.textures[kObjectBufferRegister] = a_in.addresses.objectsIndex;
		frameRecord.textures[kBonesBufferRegister] = a_in.addresses.bonesIndex;
		const std::uint32_t wrapAnisotropic = a_lookups.Sampler(static_cast<std::uint32_t>(RE::BSGraphics::TextureAddressMode::kWrapSWrapT),
			static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic));
		for (auto& index : frameRecord.samplers)
			index = wrapAnisotropic == Lookups::kNone ? 0u : wrapAnisotropic;
		std::memcpy(arena.At(kShadowFrameRecordOffset, sizeof(DrawBindings)).data(), &frameRecord, sizeof(DrawBindings));
		// Row 0: every caster without alpha testing (no texture offset, the null texture).
		ShadowMaterialRow plain;
		plain.diffuse = nullIndex;
		if (a_kept && a_in.sunEntryPlaneMasks.size() <= kMaxSunEntryProcesses) {
			BuildKeptShadow(a_in, a_tables, a_lookups, a_out, *a_kept, plain);
			CountModeDraws(a_out);
			if (PersistentParityEnabled() && ParityDue(a_in.frameNumber))
				CheckKeptShadow(a_in, a_tables, a_lookups, a_out, *a_kept);
			return;
		}
		std::vector<ShadowMaterialRow> rows;
		rows.push_back(plain);
		auto& objectRecord = a_out.objectRecord;
		objectRecord.assign(a_tables.objects.size(), ~0u);
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> recordByMaterial;
		const bool haveShadowMaterials = a_tables.shadowMaterial.size() == a_tables.objects.size();
		for (std::size_t o = 0; o < a_tables.objects.size() && o < kMaxObjects; ++o) {
			const auto& object = a_tables.objects[o];
			// A record for every caster and every occluder of Skylighting's map; the alpha-tested ones name their diffuse.
			const std::uint32_t sky = o < a_tables.skyTechnique.size() && a_in.modeUsed[kSkyMode] ? a_tables.skyTechnique[o] : 0u;
			if ((object.flags & kObjectNoShadow) && !sky)
				continue;
			if (!(((object.flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[o]) & 0x80) && !(sky & 0x80)) {
				objectRecord[o] = 0;
				continue;
			}
			// Alpha-tested: the material's diffuse and texture offset, one record per material.
			const auto* material = haveShadowMaterials ? a_tables.shadowMaterial[o] : nullptr;
			if (!material) {
				++a_out.skippedTexture;
				continue;
			}
			if (auto it = recordByMaterial.find(material); it != recordByMaterial.end()) {
				objectRecord[o] = it->second;
				continue;
			}
			auto* srv = a_tables.shadowDiffuse[o];
			const auto textureIt = srv ? a_lookups.shadowTextures.find(srv) : a_lookups.shadowTextures.end();
			if (textureIt == a_lookups.shadowTextures.end()) {
				if (srv)
					++a_out.deferredTextures;
				++a_out.skippedTexture;
				continue;
			}
			if (textureIt->second == Lookups::kNone) {
				++a_out.skippedTexture;
				continue;
			}
			// Past the table's capacity: it waits for the next frame's growth (rowsWanted), and stays the engine's meanwhile.
			++a_out.rowsWanted;
			if (rows.size() >= a_in.addresses.recordCapacity) {
				++a_out.waitingRows;
				continue;
			}
			// The texture transform off the material now, not from the walk: shader-property controllers
			// (BSLightingShaderPropertyFloatController::Update) move it between Main::Draw, where the walk
			// starts, and BeforeShadowMaps, where this build is kicked. Of the two buffers, the one the frame
			// reads (BSShaderManager::State::textureTransformCurrentBuffer, flipped by Main::Update), as
			// BSUtilityShader::SetupMaterial does; the controllers write the other one.
			const std::uint32_t transformBuffer = globals::game::smState ? (globals::game::smState->textureTransformCurrentBuffer & 1) : 0u;
			ShadowMaterialRow row;
			row.texcoord = { material->texCoordOffset[transformBuffer].x, material->texCoordOffset[transformBuffer].y, material->texCoordScale[transformBuffer].x,
				material->texCoordScale[transformBuffer].y };
			row.diffuse = textureIt->second;
			if (const auto owner = a_lookups.shadowTextureOwners.find(srv); owner != a_lookups.shadowTextureOwners.end())
				a_out.bindingOwners.push_back(owner->second);
			const auto index = static_cast<std::uint32_t>(rows.size());
			rows.push_back(row);
			recordByMaterial.emplace(material, index);
			objectRecord[o] = index;
		}
		a_out.rowsWanted += 1;  // row 0
		a_out.materialRows = { std::make_shared<const std::vector<ShadowMaterialRow>>(std::move(rows)), {} };

		// ---- The inputs per render mode among the captured views: every caster with a ready pipeline
		// for its technique under that mode. The cascades share one set; a spot light has its own.
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			auto& inputs = a_out.inputList[m];
			if (!a_in.modeUsed[m])
				continue;
			const bool skyMode = m == kSkyMode;
			const std::uint32_t modeBits = ModeBitsOf(m);
			for (std::size_t o = 0; o < a_tables.objects.size() && o < kMaxObjects && inputs.size() < kMaxInputs; ++o) {
				const auto& object = a_tables.objects[o];
				if (skyMode ? (o >= a_tables.skyTechnique.size() || !a_tables.skyTechnique[o]) : (object.flags & kObjectNoShadow) != 0)
					continue;
				if (objectRecord[o] == ~0u) {
					a_out.skySkipped += skyMode ? 1 : 0;
					continue;
				}
				// The states of the views that draw this caster's class. A volumetric-only caster with no view
				// of the copy under this mode is no input at all: it is then the engine's, unclaimed.
				const bool volumetricOnly = !skyMode && (object.flags & kObjectVolumetricOnly) != 0;
				const std::uint32_t classStates = volumetricOnly ? (a_in.modeRasterStates[m] >> 16) : (a_in.modeRasterStates[m] & 0xFFFFu);
				if (volumetricOnly && classStates == 0)
					continue;
				const std::uint32_t technique = skyMode ? a_tables.skyTechnique[o] : (a_tables.shadowTechnique[o] | modeBits);
				const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
					VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
				const auto slotIt = a_lookups.shadowSlots.find(key);
				if (slotIt == a_lookups.shadowSlots.end()) {
					++a_out.deferredPipelines;
					++a_out.skippedPipeline;
					a_out.skySkipped += skyMode ? 1 : 0;
					continue;
				}
				// Every view of the mode has to be able to draw it: the claim withholds the engine's pass
				// from all of them, so a view without the pipeline would leave the caster to nobody.
				bool deferred = false, missing = false;
				for (std::uint32_t states = classStates; states; states &= states - 1) {
					const auto& row = a_lookups.shadowMapRows[std::countr_zero(states)];
					const std::uint32_t pipeline = slotIt->second < row.size() ? row[slotIt->second] : Lookups::kNone;
					if (pipeline == Lookups::kNone) {
						// Not resolved yet, or resolved to nothing: which of the two is in shadowPipelines.
						const auto pipelineIt = a_lookups.shadowPipelines.find({ technique, WithShadowState(key.rasterFlags, std::countr_zero(states)), key.vertexLayout });
						(pipelineIt == a_lookups.shadowPipelines.end() ? deferred : missing) = true;
					}
				}
				if (deferred || missing || classStates == 0) {
					a_out.deferredPipelines += deferred ? 1 : 0;
					++a_out.skippedPipeline;
					a_out.skySkipped += skyMode ? 1 : 0;
					continue;
				}
				// A face shape draws only with its positions: without them (no buffer, the geometry table full) it
				// is no input, and stays the engine's. And a caster whose layout reads its position from the second
				// stream (a dynamic shape's) is never an input without one: the slot would fall back to the
				// geometry's own buffer and draw its other attributes as positions.
				const std::uint32_t streamIndex = FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions);
				if (streamIndex == ~0u && (IsFaceObject(a_tables, o) || (key.vertexLayout & kPositionInSecondStream))) {
					a_out.skySkipped += skyMode ? 1 : 0;
					continue;
				}
				// The sun's entry rule (kCullSunEntry): BuildDraws tests the entry's sphere, carried in the fade row, against the
				// frame's full-frustum processes in the view's latch - so the input does not change with the frame's planes.
				// With more processes than the latch holds, the verdict is taken here, on the CPU.
				const bool entryOnGpu = a_in.sunEntryPlaneMasks.size() <= kMaxSunEntryProcesses;
				inputs.push_back({ slotIt->second, objectRecord[o], object.geometryIndex,
					(object.flags & ~kObjectDecal) | kInputDrawable | (!entryOnGpu && OutsideSunEntry(a_in, a_tables, o) ? kInputOutsideSunEntry : 0u),
					{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, static_cast<std::uint32_t>(o), 0,
					PartitionsOf(a_tables, static_cast<std::uint32_t>(o)), streamIndex });
				if (entryOnGpu)
					SetSunEntryRow(inputs.back(), a_tables, o);
			}
		}
		CountModeDraws(a_out);
	}
}

#endif
