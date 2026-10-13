// The shadow build (ShadowBuild.h): the shadow epochs' list, material rows and frame record (kept across builds: ShadowKept),
// and the next frame's sun and paraboloid exclusions. Snapshot builder, on DCLF's pool.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#	include "Features/DrawcallLimitFix/Engine/Views/ShadowViews.h"
#	include "Features/DrawcallLimitFix/Scene/Ports/MaterialSources.h"
#	include "Features/DrawcallLimitFix/Engine/Capture/MaterialWrites.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief In the frame's DCLF set for the mode's phase: the only objects the mode's views draw. */
		bool SetCaster(const SceneTables& a_tables, std::uint32_t a_object, std::uint32_t a_mode)
		{
			return a_object < a_tables.setPhases.size() && (a_tables.setPhases[a_object] & SetPhaseOfMode(a_mode)) != 0;
		}

		/** @brief The key slot of a_technique on a_object's geometry, or ~0u (none, or not yet). */
		std::uint32_t KeySlotOf(const SceneTables& a_tables, const Lookups& a_lookups, std::uint32_t a_object, std::uint32_t a_technique)
		{
			if (!a_technique)
				return ~0u;
			const auto& object = a_tables.objects[a_object];
			const ShadowPipelineKey key{ a_technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
			const auto it = a_lookups.shadowSlots.find(key);
			return it == a_lookups.shadowSlots.end() ? ~0u : it->second;
		}

		/**
		 * @brief An object's shadow list input: its view mask from the modes holding it (a_modes, bit m), its caster key, each
		 * occlusion view's key, and its material row. Flags keep the caster class, tested only by class-split modes' views.
		 */
		DrawInput ShadowEntry(const SceneTables& a_tables, const Lookups& a_lookups, std::uint32_t a_object, std::uint32_t a_record, std::uint8_t a_modes,
			std::uint32_t a_stream)
		{
			const auto& object = a_tables.objects[a_object];
			std::uint32_t mask = 0;
			bool caster = false;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				if ((a_modes >> m) & 1) {
					mask |= ModeViewBits(m);
					caster |= !IsOcclusionMode(m);
				}
			const std::uint32_t casterKey = caster ? KeySlotOf(a_tables, a_lookups, a_object, BaseTechnique(a_tables, kSunShadowMode, a_object)) : ~0u;
			std::uint32_t occlusionKey[kOcclusionViews];
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				occlusionKey[v] = ((a_modes >> OcclusionModeOf(v)) & 1) ? KeySlotOf(a_tables, a_lookups, a_object, BaseTechnique(a_tables, OcclusionModeOf(v), a_object)) : ~0u;
			return { casterKey == ~0u ? 0u : casterKey, a_record, object.geometryIndex, (object.flags & ~(kObjectDecal | kObjectForward)) | kInputDrawable,
				{ mask, casterKey, { occlusionKey[0], occlusionKey[1] } }, a_object, 0, PartitionsOf(a_tables, a_object), a_stream, FadeRootOf(a_tables, a_object),
				a_record, LodPartitionsOf(a_tables, a_object) };
		}
	}

	/** @brief BuildShadowPayload's kept path: material rows and inputs updated incrementally in ShadowKept. */
	void BuildKeptShadow(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept& k,
		const ShadowMaterialRow& a_plain, ResidentRegion* a_scene)
	{
		ZoneScopedN("CS.DCLF.BuildShadow.Kept");
		const std::uint32_t objects = static_cast<std::uint32_t>(a_tables.objects.size());
		++k.builds;
		const std::uint64_t build = ++k.build;
		// Only rows changed since the version the buffer holds are sent.
		k.rows.BeginBuild(a_in.materialRowsHeld);
		// Shadow words in the main build's region, or a region of its own (Policy::ReuseResidentRegion off).
		if (a_scene)
			a_scene->Begin(a_in.inputsHeld);
		else
			k.scene.inputs.BeginBuild(a_in.inputsHeld);
		// Scene buffer growth (ObjectFits moved) also rereads every object.
		bool resync = !k.cursor.Continues(a_tables.changeLog, a_in.tablesGeneration) || k.identity != a_in.addresses.identity || k.objectRecord.size() > objects ||
		              k.fit != a_in.addresses.fit;
		if (resync) {
			// Clear its words from the scene list; every held object is retaken below.
			if (a_scene) {
				auto& r = *a_scene;
				for (std::uint32_t i = static_cast<std::uint32_t>(r.sideOf.size()); i-- > 0;) {
					if (!(r.sideOf[i] & kSideShadow))
						continue;
					if (r.sideOf[i] & kSideMain) {
						DrawInput entry = r.inputs.Get()[i];
						ClearShadowPart(entry);
						r.sideOf[i] = kSideMain;
						r.inputs.Set(i, entry);
					} else {
						r.RemoveEntry(r.inputs.Get()[i].objectIndex);
					}
				}
			}
			k.Reset();
			k.cursor.Restart(a_in.tablesGeneration);
			k.identity = a_in.addresses.identity;
			k.fit = a_in.addresses.fit;
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
		// ---- Material rows: the plain one, and each material's.
		if (k.slotMaterial.empty()) {
			k.slotMaterial.push_back(nullptr);  // slot 0: the plain record
			k.slotDiffuse.push_back(nullptr);
			k.slotRefs.push_back(0);
			k.slotReady.push_back(1);
			k.slotOwner.emplace_back();
		}
		writeRow(0, a_plain);
		const std::uint32_t transformBuffer = globals::game::smState ? (globals::game::smState->textureTransformCurrentBuffer & 1) : 0u;
		auto materialRow = [&](std::uint32_t a_slot) {
			// Read the texture offset off the material now (controllers move it after the walk; see below).
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[a_slot]);
			const auto textureIt = k.slotDiffuse[a_slot] ? a_lookups.shadowTextures.find(k.slotDiffuse[a_slot]) : a_lookups.shadowTextures.end();
			const bool ready = textureIt != a_lookups.shadowTextures.end() && textureIt->second != Lookups::kNone && a_slot < a_in.addresses.recordCapacity;
			k.slotReady[a_slot] = ready ? 1 : 0;
			if (!ready)
				return false;
			ShadowMaterialRow row;
			row.texcoord = { material->texCoordOffset[transformBuffer].x, material->texCoordOffset[transformBuffer].y, material->texCoordScale[transformBuffer].x,
				material->texCoordScale[transformBuffer].y };
			row.diffuse = textureIt->second;
			writeRow(a_slot, row);
			const auto owner = a_lookups.shadowTextureOwners.find(k.slotDiffuse[a_slot]);
			std::shared_ptr<const void> held = owner != a_lookups.shadowTextureOwners.end() ? owner->second : nullptr;
			if (k.slotOwner[a_slot] != held) {
				k.slotOwner[a_slot] = std::move(held);
				k.ownersChanged = true;
			}
			return true;
		};
		auto markRow = [&](std::uint32_t a_slot) {
			if (k.rowDirtyMark.size() <= a_slot)
				k.rowDirtyMark.resize(std::size_t(a_slot) + 1, 0);
			if (!k.rowDirtyMark[a_slot]) {
				k.rowDirtyMark[a_slot] = 1;
				k.rowDirty.push_back(a_slot);
			}
		};
		// An object's record: plain (0), its material's slot (acquired), or none.
		auto recordOf = [&](std::uint32_t o) -> std::uint32_t {
			const auto& object = a_tables.objects[o];
			if (object.flags & kObjectFree)
				return ShadowKept::kNoRecord;
			const std::uint32_t occlusion = OcclusionTechniques(a_tables, a_in.modeUsed, o);
			if ((object.flags & kObjectNoShadow) && !occlusion)
				return ShadowKept::kNoRecord;
			if (!(((object.flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[o]) & 0x80) && !(occlusion & 0x80))
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
					k.slotOwner.emplace_back();
				}
				it->second = slot;
				k.slotMaterial[slot] = material;
				k.slotDiffuse[slot] = a_tables.shadowDiffuse[o];
				k.slotReady[slot] = 0;
				markRow(slot);
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
				if (k.slotOwner[slot]) {
					k.slotOwner[slot].reset();
					k.ownersChanged = true;
				}
				if (slot < k.transformWatchBuild.size())
					k.transformWatchBuild[slot] = 0;
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

		// ---- Modes: which hold each object (heldModes), and the list entry from that.
		auto evaluate = [&](std::uint32_t m, std::uint32_t o) -> int {
			// 0: not held, 1: held (region entry), 2: waiting (pipeline or texture), 3: the frame's list (a face).
			const auto& object = a_tables.objects[o];
			const bool occlusionMode = IsOcclusionMode(m);
			if (object.flags & kObjectFree)
				return 0;
			if (occlusionMode ? !ModeTechnique(a_tables, m, o) : (object.flags & kObjectNoShadow) != 0)
				return 0;
			// Views draw only the set's casters; a member that cannot be drawn is a defect, counted with what waits.
			if (!SetCaster(a_tables, o, m))
				return 0;
			const std::uint32_t record = k.objectRecord[o];
			if (record == ShadowKept::kNoRecord)
				return 0;
			auto wait = [&](const char* a_why, std::uint32_t a_value) {
				if (a_out.setWaitingFirst.empty())
					a_out.setWaitingFirst = fmt::format("object {} mode {}: {} {} (row capacity {})", o, m, a_why, a_value, a_in.addresses.recordCapacity);
				return 2;
			};
			// Past the scene buffers: waits for their growth (the resync rereads it).
			if (!ObjectFits(a_tables, o, a_in.addresses.fit))
				return wait("scene buffers", o);
			if (record != 0 && !(record < k.slotReady.size() && k.slotReady[record]))
				return wait("material row", record);
			const bool volumetricOnly = VolumetricClass(m, object.flags);
			const auto& classStates = a_in.modeRasterStates[m].Of(volumetricOnly);
			if (classStates.empty())
				return 0;
			const std::uint32_t technique = BaseTechnique(a_tables, m, o);
			const std::uint32_t slot = KeySlotOf(a_tables, a_lookups, o, technique);
			if (slot == ~0u)
				return wait("no key slot for technique", technique);
			for (const std::uint32_t state : classStates)
				if (a_lookups.ShadowMapPipeline(m, state, slot) == Lookups::kNone)
					return wait("no pipeline under state", state);
			if (IsFaceObject(a_tables, o) || (VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) & kPositionInSecondStream))
				return 3;
			return 1;
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
		if (k.heldModes.size() < objects)
			k.heldModes.resize(objects, 0);
		// Objects whose held modes or record may have moved; their entries are rewritten after the modes.
		std::vector<std::uint32_t> touched;
		auto touch = [&](std::uint32_t o) {
			if (k.touchedMark.size() <= o)
				k.touchedMark.resize(std::size_t(o) + 1, 0);
			if (!k.touchedMark[o]) {
				k.touchedMark[o] = 1;
				touched.push_back(o);
			}
		};
		auto hold = [&](std::uint32_t m, std::uint32_t o, bool a_held) {
			if (o < k.heldModes.size() && k.Holds(m, o) != a_held) {
				k.heldModes[o] ^= static_cast<std::uint8_t>(1u << m);
				k.modes[m].membership = build;
			}
			touch(o);
		};
		auto take = [&](std::uint32_t m, std::uint32_t o) {
			auto& mode = k.modes[m];
			const int result = o < objects ? evaluate(m, o) : 0;
			setMark(mode.waiting, mode.waitingMark, o, result == 2);
			const bool wasFace = o < mode.faceMark.size() && mode.faceMark[o];
			setMark(mode.faces, mode.faceMark, o, result == 3);
			if (wasFace != (result == 3))
				mode.membership = build;
			hold(m, o, result == 1);
		};
		auto writeEntry = [&](std::uint32_t o) {
			const std::uint8_t heldNow = o < k.heldModes.size() && o < objects ? k.heldModes[o] : std::uint8_t{ 0 };
			if (a_scene) {
				// Shadow words only; the main part is the main build's.
				auto& r = *a_scene;
				r.Cover(objects);
				const std::uint32_t i = r.EntryOf(o);
				if (!heldNow) {
					if (i == kNoRegion || !(r.sideOf[i] & kSideShadow))
						return;
					if (r.sideOf[i] & kSideMain) {
						DrawInput entry = r.inputs.Get()[i];
						ClearShadowPart(entry);
						r.sideOf[i] = kSideMain;
						r.inputs.Set(i, entry);
					} else {
						r.RemoveEntry(o);
					}
					++k.entriesWritten;
					return;
				}
				const DrawInput part = ShadowEntry(a_tables, a_lookups, o, k.objectRecord[o], heldNow, ~0u);
				if (i == kNoRegion) {
					DrawInput entry{};
					SetShadowPart(entry, part, false);
					r.AddEntry(o, entry, kSideShadow);
					++k.entriesWritten;
					return;
				}
				DrawInput entry = r.inputs.Get()[i];
				SetShadowPart(entry, part, (r.sideOf[i] & kSideMain) != 0);
				r.sideOf[i] |= kSideShadow;
				if (r.inputs.Set(i, entry))
					++k.entriesWritten;
				return;
			}
			auto& scene = k.scene;
			scene.Cover(objects);
			const std::uint8_t modes = o < k.heldModes.size() ? k.heldModes[o] : std::uint8_t{ 0 };
			if (!modes || o >= objects) {
				if (scene.Holds(o)) {
					scene.Remove(o);
					++k.entriesWritten;
				}
				return;
			}
			const DrawInput input = ShadowEntry(a_tables, a_lookups, o, k.objectRecord[o], modes, ~0u);
			if (const std::uint32_t i = scene.indexOf[o]; i == kNoRegion) {
				scene.Add(o, input);
				++k.entriesWritten;
			} else if (scene.inputs.Set(i, input)) {
				++k.entriesWritten;
			}
		};

		// Logged objects, or all on a resync or view-state change (a record depends on whether Skylighting's map is drawn).
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			resync |= k.modes[m].active && k.modes[m].rasterStates != (a_in.modeUsed[m] ? a_in.modeRasterStates[m] : ModeRasterStates{});
		std::vector<std::uint32_t> changed;
		if (resync) {
			changed.reserve(objects);
			for (std::uint32_t o = 0; o < objects; ++o)
				changed.push_back(o);
		} else {
			constexpr std::uint32_t kShadowCauses = kShadowChangeCauses;
			for (const auto& change : k.cursor.Unread(a_tables.changeLog))
				if ((change.causes & kShadowCauses) && change.slot < objects)
					changed.push_back(change.slot);
		}
		k.cursor.Advance(a_tables.changeLog);
		for (const std::uint32_t o : changed)
			takeRecord(o);
		// Material rows are rewritten on events (rowDirty); a lookups or capacity change marks every slot.
		if (k.lookupsGeneration != a_lookups.shadowGeneration || k.rowCapacity != a_in.addresses.recordCapacity) {
			k.lookupsGeneration = a_lookups.shadowGeneration;
			k.rowCapacity = a_in.addresses.recordCapacity;
			for (std::uint32_t slot = 1; slot < k.slotMaterial.size(); ++slot)
				if (k.slotMaterial[slot])
					markRow(slot);
		}
		// Texture-transform events watch a slot: it is reread while the two buffers (flipped each frame) differ.
		std::vector<const RE::BSShaderMaterial*> moved;
		DCLF::MaterialWrites::DrainShadowTransformChanges(moved);
		if (k.transformWatchBuild.size() < k.slotMaterial.size())
			k.transformWatchBuild.resize(k.slotMaterial.size(), 0);
		for (const auto* material : moved)
			if (const auto it = k.slotOf.find(material); it != k.slotOf.end()) {
				if (!k.transformWatchBuild[it->second])
					k.transformWatch.push_back(it->second);
				k.transformWatchBuild[it->second] = build;
			}
		for (std::size_t w = 0; w < k.transformWatch.size();) {
			const std::uint32_t slot = k.transformWatch[w];
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[slot]);
			const bool keep = material && k.transformWatchBuild[slot] &&
			                  (build - k.transformWatchBuild[slot] <= 2 || material->texCoordOffset[0] != material->texCoordOffset[1] ||
								  material->texCoordScale[0] != material->texCoordScale[1]);
			if (!keep) {
				k.transformWatchBuild[slot] = 0;
				k.transformWatch[w] = k.transformWatch.back();
				k.transformWatch.pop_back();
				continue;
			}
			markRow(slot);
			++w;
		}
		// An unwritten slot (texture unresolved, or past capacity) stays for the next build.
		for (std::size_t d = 0; d < k.rowDirty.size();) {
			const std::uint32_t slot = k.rowDirty[d];
			if (slot < k.slotMaterial.size() && k.slotMaterial[slot] && !materialRow(slot)) {
				a_out.waitingRows += slot >= a_in.addresses.recordCapacity ? 1 : 0;
				++d;
				continue;
			}
			k.rowDirtyMark[slot] = 0;
			k.rowDirty[d] = k.rowDirty.back();
			k.rowDirty.pop_back();
		}
		if (k.ownersChanged) {
			auto owners = std::make_shared<std::vector<std::shared_ptr<const void>>>();
			for (const auto& owner : k.slotOwner)
				if (owner)
					owners->push_back(owner);
			k.owners = std::move(owners);
			k.ownersChanged = false;
		}
		a_out.bindingOwners.push_back(k.owners);
		a_out.rowsWanted = static_cast<std::uint32_t>(k.slotMaterial.size());
		for (const std::uint32_t o : changed)
			touch(o);
		// The main build's resync emptied the scene list: rewrite every held object.
		if (a_scene && k.sceneResets != a_scene->resets) {
			k.sceneResets = a_scene->resets;
			for (std::uint32_t o = 0; o < k.heldModes.size() && o < objects; ++o)
				if (k.heldModes[o])
					touch(o);
		}
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			auto& mode = k.modes[m];
			const ModeRasterStates states = a_in.modeUsed[m] ? a_in.modeRasterStates[m] : ModeRasterStates{};
			if (!mode.active || mode.rasterStates != states) {
				// View states changed: every object again (none held without views).
				mode.Reset();
				mode.active = true;
				mode.rasterStates = states;
				for (std::uint32_t o = 0; o < k.heldModes.size(); ++o)
					if (k.Holds(m, o))
						hold(m, o, false);
				if (!states.Empty())
					for (std::uint32_t o = 0; o < objects; ++o)
						take(m, o);
			} else if (!states.Empty()) {
				for (const std::uint32_t o : changed)
					take(m, o);
				std::vector<std::uint32_t> waiting;
				waiting.swap(mode.waiting);
				for (const std::uint32_t o : waiting) {
					if (o >= mode.waitingMark.size() || !mode.waitingMark[o])
						continue;
					mode.waitingMark[o] = 0;
					take(m, o);
				}
			}
		}
		for (const std::uint32_t o : touched) {
			writeEntry(o);
			k.touchedMark[o] = 0;
		}
		a_out.regionInputs = a_scene ? a_scene->inputs.View() : k.scene.inputs.View();
		// The frame's list: face shapes with this walk's positions, one entry covering all their modes.
		std::vector<std::uint32_t> faceObjects;
		ankerl::unordered_dense::map<std::uint32_t, std::uint8_t> faceModes;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			auto& mode = k.modes[m];
			if (!a_in.modeUsed[m])
				continue;
			std::size_t keptFaces = 0;
			for (const std::uint32_t o : mode.faces) {
				if (o >= mode.faceMark.size() || !mode.faceMark[o])
					continue;
				mode.faces[keptFaces++] = o;
				auto [it, fresh] = faceModes.try_emplace(o, std::uint8_t{ 0 });
				if (fresh)
					faceObjects.push_back(o);
				it->second |= static_cast<std::uint8_t>(1u << m);
			}
			mode.faces.resize(keptFaces);
			a_out.membership[m] = mode.membership;
			// Skip counters: what waits (set members only: a defect).
			a_out.deferredPipelines += static_cast<std::uint32_t>(mode.waiting.size());
			a_out.skippedPipeline += static_cast<std::uint32_t>(mode.waiting.size());
			for (const std::uint32_t o : mode.waiting)
				a_out.setWaiting += o < mode.waitingMark.size() && mode.waitingMark[o] ? 1 : 0;
		}
		for (const std::uint32_t o : faceObjects) {
			const std::uint32_t streamIndex = FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions);
			if (streamIndex == ~0u)
				continue;  // no positions this walk: left to the engine
			a_out.inputList.push_back(ShadowEntry(a_tables, a_lookups, o, k.objectRecord[o], faceModes[o], streamIndex));
		}
		a_out.kept = true;
		a_out.materialRows = k.rows.View();
	}

	std::shared_ptr<SunExclusion> BuildSunExclusion(const std::shared_ptr<const SunCandidates>& a_candidates, const ShadowPayload& a_payload, std::uint32_t a_mode,
		const SceneTables& a_tables, SunExclusionCache* a_cache)
	{
		ZoneScopedN("CS.DCLF.BuildSunExclusion");
		if (!a_candidates || !a_candidates->Count())
			return nullptr;
		auto exclusion = std::make_shared<SunExclusion>();
		exclusion->candidates = a_candidates;
		exclusion->builtFrame = a_payload.inputs.frameNumber;
		const std::size_t count = a_candidates->Capacity();
		const std::uint64_t membership = a_payload.kept ? a_payload.membership[a_mode] : 0;
		bool wouldReuse = false;
		if (a_cache) {
			auto& c = *a_cache;
			++c.builds;
			bool reuse = c.valid && membership && c.candidates && c.membership == membership && c.cursor.Continues(a_tables.changeLog, a_payload.inputs.tablesGeneration);
			const std::uint32_t causes = kChangeBindings | kChangeGeometry;
			if (reuse)
				for (const auto& change : c.cursor.Unread(a_tables.changeLog))
					if (change.causes & causes) {
						reuse = false;
						break;
					}
			c.cursor.Advance(a_tables.changeLog);
			if (reuse && c.candidates != a_candidates) {
				// Only the candidates moved: keep cached verdicts for stable entries, rejudge each moved entry from its geometries.
				std::vector<std::uint32_t> changed;
				ChangedEntries(c.candidates.get(), *a_candidates, changed);
				std::vector<std::uint8_t> isInput(a_tables.objects.size(), 0);
				a_payload.ForEachInput(a_mode, [&](const DrawInput& a_input) {
					if (a_input.objectIndex < isInput.size())
						isInput[a_input.objectIndex] = 1;
				});
				auto excluded = c.excluded;
				excluded.resize(count, 0);
				bool moved = false;
				for (const std::uint32_t e : changed) {
					std::uint8_t verdict = 0;
					if (e < count && a_candidates->entryNodes[e]) {
						verdict = 1;
						for (const std::uint32_t g : a_candidates->entryGeometries[e]) {
							const std::int32_t o = a_candidates->geometrySlot[g];
							if (o >= 0 && std::size_t(o) < a_tables.objects.size() && !(a_tables.objects[o].flags & kObjectFree) && !isInput[o] &&
								(!(a_tables.objects[o].flags & kObjectNoShadow) ||
									(std::size_t(o) < a_tables.shadowReject.size() && a_tables.shadowReject[o] == static_cast<std::uint8_t>(ShadowReject::Faded)))) {
								verdict = 0;
								break;
							}
						}
					}
					moved |= e < excluded.size() && excluded[e] != verdict;
					if (e < excluded.size())
						excluded[e] = verdict;
				}
				++c.translated;
				c.candidates = a_candidates;
				c.excluded = std::move(excluded);
				c.excludedCount = static_cast<std::uint32_t>(std::count(c.excluded.begin(), c.excluded.end(), std::uint8_t(1)));
				if (moved) {
					static std::atomic<std::uint64_t> translatedVersions{ 0 };
					c.version = (1ull << 62) | (translatedVersions.fetch_add(1, std::memory_order_relaxed) + 1);
				}
			}
			// Policy::ReuseShadowExclusion may force a full build to compare against the reuse.
			wouldReuse = reuse;
			if (reuse && !Policy::ReuseShadowExclusion(a_payload.inputs.frameNumber))
				reuse = false;
			if (reuse) {
				++c.reused;
				exclusion->reused = true;
				exclusion->excluded = c.excluded;
				exclusion->excludedCount = c.excludedCount;
				exclusion->version = c.version;
				exclusion->removed = std::make_unique<std::atomic<std::uint32_t>[]>(count);
				exclusion->cleared = std::make_unique<std::atomic<std::uint32_t>[]>(a_candidates->geometryEntry.size());
				if (a_mode == kParabolicShadowMode)
					exclusion->lightReach = std::make_unique<std::atomic<std::uint64_t>[]>(count * 3);
				return exclusion;
			}
		}
		// Every live entry excluded unless a caster under it is not drawn; a free index never.
		exclusion->excluded.assign(count, 0);
		for (std::size_t e = 0; e < count; ++e)
			exclusion->excluded[e] = a_candidates->entryNodes[e] ? 1 : 0;
		std::vector<std::uint8_t> isInput(a_tables.objects.size(), 0);
		a_payload.ForEachInput(a_mode, [&](const DrawInput& a_input) {
			if (a_input.objectIndex < isInput.size())
				isInput[a_input.objectIndex] = 1;
		});
		// An object blocks its entry when the engine would cast it here and this build does not draw it (volumetric-only too: a
		// paraboloid light registers them). A point light's registration also sets its activeLightMask bit on every geometry its
		// cull reaches; LocalLightCull writes those for a skipped entry itself. A fading object (ShadowReject::Faded) counts as a
		// caster: the exclusion is used a frame later and the culls update the fade themselves (BSFadeNode::OnVisible).
		auto engineCaster = [&](std::size_t o) {
			return !(a_tables.objects[o].flags & kObjectNoShadow) ||
			       (o < a_tables.shadowReject.size() && a_tables.shadowReject[o] == static_cast<std::uint8_t>(ShadowReject::Faded));
		};
		for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
			if (a_tables.objects[o].flags & kObjectFree)
				continue;
			if (isInput[o] || !engineCaster(o))
				continue;
			if (const auto it = a_candidates->geometries.find(a_tables.objectGeometry[o]); it != a_candidates->geometries.end())
				exclusion->excluded[a_candidates->geometryEntry[it->second]] = 0;
		}
		exclusion->excludedCount = static_cast<std::uint32_t>(std::count(exclusion->excluded.begin(), exclusion->excluded.end(), std::uint8_t(1)));
		// A full build matching the cache keeps its version.
		static std::atomic<std::uint64_t> versions{ 0 };
		const bool same = a_cache && a_cache->valid && a_cache->candidates == a_candidates && a_cache->excluded == exclusion->excluded;
		exclusion->version = same ? a_cache->version : versions.fetch_add(1, std::memory_order_relaxed) + 1;
		if (a_cache) {
			auto& c = *a_cache;
			c.version = exclusion->version;
			if (wouldReuse && Diagnostics::Armed<Diagnostics::ExclusionRebuilt>())
				Diagnostics::Notify(Diagnostics::ExclusionRebuilt{ a_mode == kParabolicShadowMode, c.excluded == exclusion->excluded });
			c.candidates = a_candidates;
			c.cursor.Restart(a_payload.inputs.tablesGeneration);
			c.membership = membership;
			c.excluded = exclusion->excluded;
			c.excludedCount = exclusion->excludedCount;
			c.valid = true;
		}
		exclusion->removed = std::make_unique<std::atomic<std::uint32_t>[]>(count);
		exclusion->cleared = std::make_unique<std::atomic<std::uint32_t>[]>(a_candidates->geometryEntry.size());
		if (a_mode == kParabolicShadowMode)
			exclusion->lightReach = std::make_unique<std::atomic<std::uint64_t>[]>(count * 3);
		return exclusion;
	}

	namespace
	{
		/** @brief Per used mode, the draws its inputs can produce (a skin once per partition), total and per key slot (bucket sizes). */
		void CountModeDraws(ShadowPayload& a_out)
		{
			// One pass for every used mode (each input's mask names its modes).
			std::array<std::uint64_t, kShadowModeCount> draws{};
			std::uint32_t used = 0, usedBits = 0;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				a_out.keySlotDraws[m].clear();
				a_out.modeInputs[m] = 0;
				if (a_out.inputs.modeUsed[m]) {
					used |= 1u << m;
					usedBits |= a_out.modeBits[m];
				}
			}
			a_out.ForEachInput([&](const DrawInput& a_input) {
				if (!(a_input.view.mask & usedBits))
					return;
				const std::uint32_t inputDraws = PartitionDraws(a_input.partitions);
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (!((used >> m) & 1) || !(a_input.view.mask & a_out.modeBits[m]))
						continue;
					const std::uint32_t key = ShadowKeyOf(m, a_input);
					if (key == ~0u)
						continue;
					++a_out.modeInputs[m];
					draws[m] += inputDraws;
					auto& slots = a_out.keySlotDraws[m];
					if (key >= slots.size())
						slots.resize(std::size_t(key) + 1, 0u);
					slots[key] += inputDraws;
				}
			});
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				a_out.modeDraws[m] = static_cast<std::uint32_t>(std::min<std::uint64_t>(draws[m], UINT32_MAX));
		}
	}

	void BuildShadowPayload(const ShadowInputs& a_in, const SceneTables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
		std::shared_ptr<const StreamViews> a_streams, ShadowKept* a_kept, ResidentRegion* a_scene)
	{
		ZoneScopedN("CS.DCLF.BuildShadowPayload");
		a_out.Reset();
		a_out.inputs = a_in;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			a_out.modeBits[m] = ModeViewBits(m);
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

		// ---- Shared by every view: the geometry table and binding records (object records and bone rows are CommitSceneStreams').
		TakeGeometryDraws(a_streams.get(), a_tables, a_in.tablesGeneration, a_in.frameNumber, a_out.geometries);
		a_out.streams = std::move(a_streams);
		AppendFaceStreams(a_tables, a_in.addresses.facePositions, a_out.geometries);
		if (a_in.addresses.facePositions)
			a_out.faceStreams = a_tables.faceStreams;
		// The frame record heads the arena, then the shared blocks. No register the Utility shaders declare may be at address
		// zero (a late-compiled pipeline reading one would lose the device); unsupplied ones read zeros. CS's SharedData (b5;
		// alpha testing uses MipBias) and FeatureData (b6) sit at fixed places, filled by the commit's latched copies.
		const auto blocks = ShadowArenaBlocksOf();
		const std::uint64_t end = std::max({ blocks.zeros + kShadowZeroBlockBytes, blocks.sharedData + blocks.sharedBytes, blocks.featureData + blocks.featureBytes });
		if (block(nullptr, static_cast<std::size_t>(end)) != base)
			stl::report_and_fail("Drawcall Limit Fix: the shadow arena's blocks were not laid out from its start");
		if (a_in.sharedData.size() == blocks.sharedBytes)
			std::memcpy(arena.At(blocks.sharedData, blocks.sharedBytes).data(), a_in.sharedData.data(), blocks.sharedBytes);
		if (blocks.featureBytes && a_in.featureData.size() == blocks.featureBytes)
			std::memcpy(arena.At(blocks.featureData, blocks.featureBytes).data(), a_in.featureData.data(), blocks.featureBytes);
		// Every texture and sampler but the diffuse (the material row's).
		auto& frameRecord = a_out.frameRecord;
		const std::uint32_t nullIndex = a_lookups.nullTexture == Lookups::kNone ? 0u : a_lookups.nullTexture;
		for (auto& index : frameRecord.textures)
			index = nullIndex;
		frameRecord.textures[kObjectBufferRegister] = a_in.addresses.objectsIndex;
		frameRecord.textures[kExtrasBufferRegister] = a_in.addresses.extrasIndex;
		frameRecord.textures[kPlacementBufferRegister] = a_in.addresses.placementsIndex;
		frameRecord.textures[kPaletteBufferRegister] = a_in.addresses.palettesIndex;
		frameRecord.textures[kShadingBufferRegister] = a_in.addresses.shadingIndex;  // a tree's wind until its entry
		frameRecord.textures[kTreeWindRegister] = a_in.addresses.treeWindIndex;
		const std::uint32_t wrapAnisotropic = a_lookups.Sampler(static_cast<std::uint32_t>(RE::BSGraphics::TextureAddressMode::kWrapSWrapT),
			static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic));
		for (auto& index : frameRecord.samplers)
			index = wrapAnisotropic == Lookups::kNone ? 0u : wrapAnisotropic;
		std::memcpy(arena.At(kShadowFrameRecordOffset, sizeof(DrawBindings)).data(), &frameRecord, sizeof(DrawBindings));
		// Row 0: every caster without alpha testing (no texture offset, the null texture).
		ShadowMaterialRow plain;
		plain.diffuse = nullIndex;
		if (a_kept) {
			BuildKeptShadow(a_in, a_tables, a_lookups, a_out, *a_kept, plain, a_scene);
			CountModeDraws(a_out);
			Diagnostics::Notify(Diagnostics::ShadowBuilt{ a_in, a_tables, a_lookups, a_out, *a_kept });
			return;
		}
		std::vector<ShadowMaterialRow> rows;
		rows.push_back(plain);
		auto& objectRecord = a_out.objectRecord;
		objectRecord.assign(a_tables.objects.size(), ~0u);
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> recordByMaterial;
		const bool haveShadowMaterials = a_tables.shadowMaterial.size() == a_tables.objects.size();
		for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
			const auto& object = a_tables.objects[o];
			// A record for every caster and Skylighting occluder; alpha-tested ones name their diffuse.
			const std::uint32_t occlusion = OcclusionTechniques(a_tables, a_in.modeUsed, o);
			if ((object.flags & kObjectNoShadow) && !occlusion)
				continue;
			if (!(((object.flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[o]) & 0x80) && !(occlusion & 0x80)) {
				objectRecord[o] = 0;
				continue;
			}
			// Alpha-tested: one record per material.
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
			// Past capacity: waits for the growth (rowsWanted), the engine's meanwhile.
			++a_out.rowsWanted;
			if (rows.size() >= a_in.addresses.recordCapacity) {
				++a_out.waitingRows;
				continue;
			}
			// Read now, not from the walk: BSLightingShaderPropertyFloatController::Update moves it between Main::Draw and
			// BeforeShadowMaps. Use the buffer the frame reads (textureTransformCurrentBuffer, flipped by Main::Update), as
			// BSUtilityShader::SetupMaterial does; the controllers write the other.
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

		// ---- The modes that draw each object, then one input per object for all of them.
		std::vector<std::uint8_t> heldModes(a_tables.objects.size(), 0);
		std::vector<std::uint32_t> streams(a_tables.objects.size(), ~0u);
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_in.modeUsed[m])
				continue;
			const bool occlusionMode = IsOcclusionMode(m);
			for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
				const auto& object = a_tables.objects[o];
				if (occlusionMode ? !ModeTechnique(a_tables, m, o) : (object.flags & kObjectNoShadow) != 0)
					continue;
				// Set members of the mode's phase only; a member skipped below is a defect.
				const bool member = SetCaster(a_tables, static_cast<std::uint32_t>(o), m);
				if (!member)
					continue;
				struct MemberSkip
				{
					bool armed;
					std::uint32_t& count;
					~MemberSkip() { count += armed ? 1 : 0; }
				} memberSkip{ member, a_out.setWaiting };
				if (objectRecord[o] == ~0u || !ObjectFits(a_tables, static_cast<std::uint32_t>(o), a_in.addresses.fit)) {
					continue;
				}
				// A volumetric-only caster with no volumetric view under this mode is no input.
				const bool volumetricOnly = VolumetricClass(m, object.flags);
				const auto& classStates = a_in.modeRasterStates[m].Of(volumetricOnly);
				if (volumetricOnly && classStates.empty()) {
					memberSkip.armed = false;
					continue;
				}
				// No view of its class: no input, nothing withheld.
				if (classStates.empty())
					memberSkip.armed = false;
				const std::uint32_t technique = ModeTechnique(a_tables, m, o);
				const ShadowPipelineKey key{ BaseTechnique(a_tables, m, o), (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
					VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
				const auto slotIt = a_lookups.shadowSlots.find(key);
				if (slotIt == a_lookups.shadowSlots.end()) {
					++a_out.deferredPipelines;
					++a_out.skippedPipeline;
					continue;
				}
				// Every view of the mode must draw it: the set withholds the engine's pass from all of them (PhaseReady).
				bool deferred = false, missing = false;
				for (const std::uint32_t state : classStates) {
					if (a_lookups.ShadowMapPipeline(m, state, slotIt->second) == Lookups::kNone) {
						// shadowPipelines tells unresolved from resolved-to-nothing.
						const auto pipelineIt = a_lookups.shadowPipelines.find({ technique, key.rasterFlags, key.vertexLayout, state });
						(pipelineIt == a_lookups.shadowPipelines.end() ? deferred : missing) = true;
					}
				}
				if (deferred || missing || classStates.empty()) {
					a_out.deferredPipelines += deferred ? 1 : 0;
					++a_out.skippedPipeline;
					continue;
				}
				// Faces and second-stream-position casters need this walk's positions, else they stay the engine's.
				const std::uint32_t streamIndex = FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions);
				if (streamIndex == ~0u && (IsFaceObject(a_tables, o) || (key.vertexLayout & kPositionInSecondStream))) {
					continue;
				}
				// Sun entry rule (kCullSunEntry): BuildDraws tests the entry sphere against the latch, so inputs ignore the planes.
				heldModes[o] |= static_cast<std::uint8_t>(1u << m);
				streams[o] = streamIndex;
				memberSkip.armed = false;
			}
		}
		for (std::uint32_t o = 0; o < heldModes.size(); ++o)
			if (heldModes[o])
				a_out.inputList.push_back(ShadowEntry(a_tables, a_lookups, o, objectRecord[o], heldModes[o], streams[o]));
		CountModeDraws(a_out);
	}
}

#endif
