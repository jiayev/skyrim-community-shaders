// The shadow build (ShadowBuild.h): the shadow epochs' list, material rows and frame record (kept across builds: ShadowKept),
// and the next frame's sun and paraboloid exclusions. Snapshot builder, on DCLF's pool.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief In the frame's DCLF set for the mode's phase (Extensions::ShadowViewSet::phaseOfMode): the only objects the mode's views draw. */
		bool SetCaster(const SceneTables& a_tables, std::uint32_t a_object, std::uint32_t a_mode)
		{
			return a_object < a_tables.setPhases.size() && (a_tables.setPhases[a_object] & Extensions::ShadowViewsSet().phaseOfMode[a_mode]) != 0;
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

		/** @brief The texture-transform buffer the frame reads (textureTransformCurrentBuffer, flipped by Main::Update): ShadowTexcoord's. */
		std::uint32_t ShadowTransformBuffer()
		{
			return globals::game::smState ? (globals::game::smState->textureTransformCurrentBuffer & 1) : 0u;
		}

		/**
		 * @brief A shadow material row's texcoord (offset xy, scale zw): a_material's texture transform in a_buffer (ShadowTransformBuffer).
		 * Read now, not from the walk: BSLightingShaderPropertyFloatController::Update moves it between Main::Draw and BeforeShadowMaps. Use
		 * the buffer the frame reads, as BSUtilityShader::SetupMaterial does; the controllers write the other.
		 */
		std::array<float, 4> ShadowTexcoord(const RE::BSShaderMaterial& a_material, std::uint32_t a_buffer)
		{
			return { a_material.texCoordOffset[a_buffer].x, a_material.texCoordOffset[a_buffer].y, a_material.texCoordScale[a_buffer].x,
				a_material.texCoordScale[a_buffer].y };
		}

		/**
		 * @brief The kept build's transform watch: the transform moves since the last build (Extensions::DrainTransformMoves) watch their
		 * material's slot, and a watched slot's row is marked (a_markRow) while its two buffers (flipped each frame) differ or for 2 builds
		 * after its last move, so both buffers' values reach the row.
		 */
		template <class MarkRow>
		void WatchTransformMoves(ShadowKept& k, std::uint64_t a_build, MarkRow&& a_markRow)
		{
			std::vector<const RE::BSShaderMaterial*> moved;
			Extensions::DrainTransformMoves(moved);
			if (k.transformWatchBuild.size() < k.slotMaterial.size())
				k.transformWatchBuild.resize(k.slotMaterial.size(), 0);
			for (const auto* material : moved)
				if (const auto it = k.slotOf.find(material); it != k.slotOf.end()) {
					if (!k.transformWatchBuild[it->second])
						k.transformWatch.push_back(it->second);
					k.transformWatchBuild[it->second] = a_build;
				}
			for (std::size_t w = 0; w < k.transformWatch.size();) {
				const std::uint32_t slot = k.transformWatch[w];
				const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[slot]);
				const bool keep = material && k.transformWatchBuild[slot] &&
				                  (a_build - k.transformWatchBuild[slot] <= 2 || material->texCoordOffset[0] != material->texCoordOffset[1] ||
									  material->texCoordScale[0] != material->texCoordScale[1]);
				if (!keep) {
					k.transformWatchBuild[slot] = 0;
					k.transformWatch[w] = k.transformWatch.back();
					k.transformWatch.pop_back();
					continue;
				}
				a_markRow(slot);
				++w;
			}
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
		// The journal keeps what the oldest payload backing lacks; each backing is sent the rows changed since its content epoch.
		k.rows.BeginBuild(a_in.materialRowsOldestContentEpoch);
		// Shadow words in the main build's region, or a region of its own (Policy::ReuseResidentRegion off).
		if (a_scene)
			a_scene->Begin(a_in.inputsOldestContentEpoch);
		else
			k.scene.inputs.BeginBuild(a_in.inputsOldestContentEpoch);
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
		const std::uint32_t transformBuffer = ShadowTransformBuffer();
		auto materialRow = [&](std::uint32_t a_slot) {
			// Read the texture offset off the material now (controllers move it after the walk: ShadowTexcoord).
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[a_slot]);
			const auto textureIt = k.slotDiffuse[a_slot] ? a_lookups.shadowTextures.find(k.slotDiffuse[a_slot]) : a_lookups.shadowTextures.end();
			const bool ready = textureIt != a_lookups.shadowTextures.end() && textureIt->second != Lookups::kNone;
			k.slotReady[a_slot] = ready ? 1 : 0;
			if (!ready)
				return false;
			ShadowMaterialRow row;
			row.texcoord = ShadowTexcoord(*material, transformBuffer);
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
					a_out.setWaitingFirst = fmt::format("object {} mode {}: {} {}", o, m, a_why, a_value);
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
			if (IsStreamObject(a_tables, o) || (VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) & kPositionInSecondStream))
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
		// Material rows are rewritten on events (rowDirty); a lookups change marks every slot.
		if (k.lookupsGeneration != a_lookups.shadowGeneration) {
			k.lookupsGeneration = a_lookups.shadowGeneration;
			for (std::uint32_t slot = 1; slot < k.slotMaterial.size(); ++slot)
				if (k.slotMaterial[slot])
					markRow(slot);
		}
		// Texture-transform events watch a slot: it is reread while the two buffers (flipped each frame) differ.
		WatchTransformMoves(k, build, markRow);
		// An unwritten slot (texture unresolved) stays for the next build.
		for (std::size_t d = 0; d < k.rowDirty.size();) {
			const std::uint32_t slot = k.rowDirty[d];
			if (slot < k.slotMaterial.size() && k.slotMaterial[slot] && !materialRow(slot)) {
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
			const std::uint32_t streamIndex = StreamGeometry(a_tables, o, a_in.addresses.streamBuffer);
			if (streamIndex == ~0u)
				continue;  // no positions this walk: left to the engine
			a_out.inputList.push_back(ShadowEntry(a_tables, a_lookups, o, k.objectRecord[o], faceModes[o], streamIndex));
		}
		a_out.kept = true;
		a_out.materialRows = k.rows.View();
	}

	/**
	 * @brief What BuildSunExclusion keeps in a SunExclusionCache (its index) to update its verdicts by what moved instead of judging every
	 * object again: the objects indexed by their geometry (an entry's verdict is read from the objects of its geometries), the mode's
	 * inputs, the exclusion the verdicts were published in (published again while nothing moved), the stamped words every version
	 * shares (SunExclusion), and the ends of the forward chains (ForwardExclusion). The snapshot builder's thread only (one build
	 * at a time).
	 */
	struct SunExclusionIndex
	{
		static constexpr std::uint32_t kNone = ~0u;
		std::shared_ptr<SunExclusion> last;
		// Per object: the geometry it is indexed under (null: free, or none) and the next object indexed under it.
		std::vector<const RE::BSGeometry*> objectGeometry;
		std::vector<std::uint32_t> nextObject;
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> firstObject;
		// The mode's inputs as of the last build: per object bit 0 (bit 1: a build's scratch), and the list of them.
		std::vector<std::uint8_t> inputMark;
		std::vector<std::uint32_t> inputs;
		std::uint32_t modeBits = 0;
		// A build's scratch: the objects it looks at again, the entries it judges again.
		std::vector<std::uint8_t> objectMark, entryMark;
		std::vector<std::uint32_t> objects, entries, changed;
		// The stamped words, grown by replacing them (the versions made before hold theirs).
		std::shared_ptr<std::atomic<std::uint32_t>[]> removed, cleared;
		std::shared_ptr<std::atomic<std::uint64_t>[]> lightReach;
		std::uint32_t removedCount = 0, clearedCount = 0;
		// Every exclusion made for the last candidates the mode was built for whose chain is still held (each link is held by the one
		// before it; the first by whoever holds the built exclusion): translated to the next candidates when they move.
		std::vector<std::weak_ptr<SunExclusion>> ends;
		bool valid = false;

		void Link(std::uint32_t a_object, const RE::BSGeometry* a_geometry)
		{
			objectGeometry[a_object] = a_geometry;
			if (!a_geometry)
				return;
			auto [it, fresh] = firstObject.try_emplace(a_geometry, a_object);
			nextObject[a_object] = fresh ? kNone : std::exchange(it->second, a_object);
		}
		void Unlink(std::uint32_t a_object)
		{
			const auto* geometry = std::exchange(objectGeometry[a_object], nullptr);
			if (!geometry)
				return;
			const auto it = firstObject.find(geometry);
			if (it == firstObject.end())
				return;
			if (it->second == a_object) {
				if (nextObject[a_object] == kNone)
					firstObject.erase(it);
				else
					it->second = nextObject[a_object];
				return;
			}
			for (std::uint32_t o = it->second; nextObject[o] != kNone; o = nextObject[o])
				if (nextObject[o] == a_object) {
					nextObject[o] = nextObject[a_object];
					return;
				}
		}
		/** @brief Every object indexed again from the tables. */
		void Reindex(const SceneTables& a_tables)
		{
			const std::size_t count = a_tables.objects.size();
			firstObject.clear();
			objectGeometry.assign(count, nullptr);
			nextObject.assign(count, kNone);
			for (std::uint32_t o = 0; o < count; ++o)
				if (!(a_tables.objects[o].flags & kObjectFree))
					Link(o, a_tables.objectGeometry[o]);
		}
		void Grow(std::size_t a_objects, std::size_t a_entries)
		{
			if (objectGeometry.size() < a_objects) {
				objectGeometry.resize(a_objects, nullptr);
				nextObject.resize(a_objects, kNone);
			}
			if (inputMark.size() < a_objects)
				inputMark.resize(a_objects, 0);
			if (objectMark.size() < a_objects)
				objectMark.resize(a_objects, 0);
			if (entryMark.size() < a_entries)
				entryMark.resize(a_entries, 0);
		}
		/** @brief The stamped words for a_entries entries and a_geometries geometries (a_reach: with the reach words). */
		void EnsureStamps(std::uint32_t a_entries, std::uint32_t a_geometries, bool a_reach)
		{
			if (!removed || removedCount < a_entries || (a_reach && !lightReach)) {
				const std::uint32_t n = std::max(a_entries, removedCount + removedCount / 2);
				removed = std::make_shared<std::atomic<std::uint32_t>[]>(n);
				lightReach = a_reach ? std::make_shared<std::atomic<std::uint64_t>[]>(std::size_t(n) * 3) : nullptr;
				removedCount = n;
			}
			if (!cleared || clearedCount < a_geometries) {
				const std::uint32_t n = std::max(a_geometries, clearedCount + clearedCount / 2);
				cleared = std::make_shared<std::atomic<std::uint32_t>[]>(n);
				clearedCount = n;
			}
		}
		void GiveStamps(SunExclusion& a_exclusion) const
		{
			a_exclusion.removed = removed;
			a_exclusion.cleared = cleared;
			a_exclusion.lightReach = lightReach;
			a_exclusion.removedCount = removedCount;
			a_exclusion.clearedCount = clearedCount;
		}
	};

	namespace
	{
		/** @brief The cache's index, made with its first build. */
		SunExclusionIndex& IndexOf(SunExclusionCache& a_cache)
		{
			if (!a_cache.index)
				a_cache.index = std::make_shared<SunExclusionIndex>();
			return *a_cache.index;
		}

		/**
		 * @brief An object blocks its entry when the engine would cast it here and this build does not draw it (volumetric-only too: a
		 * paraboloid light registers them). A point light's registration also sets its activeLightMask bit on every geometry its
		 * cull reaches; LocalLightCull writes those for a skipped entry itself. A fading object (ShadowReject::Faded) counts as a
		 * caster: the exclusion is used a frame later and the culls update the fade themselves (BSFadeNode::OnVisible).
		 */
		bool EngineCaster(const SceneTables& a_tables, std::size_t a_object)
		{
			return !(a_tables.objects[a_object].flags & kObjectNoShadow) ||
			       (a_object < a_tables.shadowReject.size() && a_tables.shadowReject[a_object] == static_cast<std::uint8_t>(ShadowReject::Faded));
		}

		/** @brief The mode's inputs: a_mark |= a_bit for each (a_list: each once). */
		void CollectInputs(const ShadowPayload& a_payload, std::uint32_t a_mode, std::vector<std::uint8_t>& a_mark, std::uint8_t a_bit, std::vector<std::uint32_t>& a_list)
		{
			a_payload.ForEachInput(a_mode, [&](const DrawInput& a_input) {
				const std::uint32_t o = a_input.objectIndex;
				if (o < a_mark.size() && !(a_mark[o] & a_bit)) {
					a_mark[o] |= a_bit;
					a_list.push_back(o);
				}
			});
		}

		/** @brief Every entry judged from every object (the reference the kept verdicts are checked against). a_isInput: per object. */
		void JudgeAll(const EntrySpace& a_candidates, const SceneTables& a_tables, const std::vector<std::uint8_t>& a_isInput, std::uint8_t a_bit,
			std::vector<std::uint8_t>& a_out)
		{
			// Every live entry excluded unless a caster under it is not drawn; a free index never.
			const std::size_t count = a_candidates.Capacity();
			a_out.assign(count, 0);
			for (std::size_t e = 0; e < count; ++e)
				a_out[e] = a_candidates.entryNodes[e] ? 1 : 0;
			for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
				if (a_tables.objects[o].flags & kObjectFree)
					continue;
				if ((o < a_isInput.size() && (a_isInput[o] & a_bit)) || !EngineCaster(a_tables, o))
					continue;
				if (const auto it = a_candidates.geometries.find(a_tables.objectGeometry[o]); it != a_candidates.geometries.end())
					a_out[a_candidates.geometryEntry[it->second]] = 0;
			}
		}

		/**
		 * @brief The forward links: every chain end made for other candidates translated to a_built's (TranslateExclusion, sharing its
		 * stamped words), so a reader holding an older verdict takes the one for its frame's candidates by following links
		 * (ForwardExclusion) instead of translating. The ends whose chain nobody holds any more are dropped.
		 */
		void ForwardEnds(SunExclusionIndex& a_index, const std::shared_ptr<SunExclusion>& a_built, bool a_paraboloid)
		{
			std::vector<std::weak_ptr<SunExclusion>> ends;
			ends.reserve(a_index.ends.size() + 1);
			bool listed = false;
			for (const auto& weak : a_index.ends) {
				auto end = weak.lock();
				if (!end || end->forward.load(std::memory_order_relaxed))
					continue;
				listed |= end == a_built;
				if (end->candidates == a_built->candidates) {
					ends.push_back(end);
					continue;
				}
				// The paraboloid's translation keeps the version (LocalLightCull's filter stands); the sun's takes a new one when an
				// entry leaves (the scene lists' filter is judged again), as the frame's translation did.
				auto next = TranslateExclusion(end, a_built->candidates, !a_paraboloid, a_paraboloid, &a_index.changed, a_built.get());
				next->forwardChanged = a_index.changed;
				end->forwardOwned = next;
				end->forward.store(next.get(), std::memory_order_release);
				ends.push_back(next);
			}
			if (!listed)
				ends.push_back(a_built);
			a_index.ends = std::move(ends);
		}

		/** @brief The builds' versions (translated copies count apart: TranslateExclusion). */
		std::uint64_t NextExclusionVersion()
		{
			static std::atomic<std::uint64_t> versions{ 0 };
			return versions.fetch_add(1, std::memory_order_relaxed) + 1;
		}
	}

	std::shared_ptr<SunExclusion> BuildSunExclusion(const std::shared_ptr<const EntrySpace>& a_candidates, const ShadowPayload& a_payload, std::uint32_t a_mode,
		const SceneTables& a_tables, SunExclusionCache* a_cache)
	{
		ZoneScopedN("CS.DCLF.BuildSunExclusion");
		if (!a_candidates || !a_candidates->Count())
			return nullptr;
		const auto& candidates = *a_candidates;
		const std::uint32_t count = candidates.Capacity();
		const std::uint32_t geometryCount = candidates.GeometryCapacity();
		const bool paraboloid = a_mode == kParabolicShadowMode;
		const std::size_t objects = a_tables.objects.size();
		if (!a_cache) {
			// No cache: judged whole, with words of its own.
			auto exclusion = std::make_shared<SunExclusion>();
			exclusion->candidates = a_candidates;
			exclusion->builtFrame = a_payload.inputs.frameNumber;
			std::vector<std::uint8_t> isInput(objects, 0);
			std::vector<std::uint32_t> list;
			CollectInputs(a_payload, a_mode, isInput, 1, list);
			JudgeAll(candidates, a_tables, isInput, 1, exclusion->excluded);
			exclusion->excludedCount = static_cast<std::uint32_t>(std::count(exclusion->excluded.begin(), exclusion->excluded.end(), std::uint8_t(1)));
			exclusion->version = NextExclusionVersion();
			exclusion->removed = std::make_shared<std::atomic<std::uint32_t>[]>(count);
			exclusion->cleared = std::make_shared<std::atomic<std::uint32_t>[]>(geometryCount);
			if (paraboloid)
				exclusion->lightReach = std::make_shared<std::atomic<std::uint64_t>[]>(std::size_t(count) * 3);
			exclusion->removedCount = count;
			exclusion->clearedCount = geometryCount;
			return exclusion;
		}
		auto& c = *a_cache;
		auto& x = IndexOf(c);
		++c.builds;
		const auto buildStart = std::chrono::steady_clock::now();
		const std::uint64_t membership = a_payload.kept ? a_payload.membership[a_mode] : 0;
		const std::uint32_t modeBits = a_payload.modeBits[a_mode];
		const auto isBlocking = [&](std::uint32_t a_object) {
			return a_object < objects && !(a_tables.objects[a_object].flags & kObjectFree) && !(x.inputMark[a_object] & 1) && EngineCaster(a_tables, a_object);
		};
		// The kept verdicts follow what moved: the tables' change log (an object's flags, shadow reject or geometry: kChangeBindings,
		// kChangeShadow, kChangeGeometry), the mode's inputs (the objects that joined or left them) and the candidates (the entries
		// whose versions moved: ChangedEntriesByLog). Each entry any of them touches is judged again from the objects of its
		// geometries; the rest keep theirs. Whole only for a new cache or tables generation, or a reader behind the log's trim.
		bool kept = c.valid && x.valid && c.candidates && c.cursor.Continues(a_tables.changeLog, a_payload.inputs.tablesGeneration);
		bool flipped = false;
		const bool candidatesMoved = c.candidates != a_candidates;
		if (kept) {
			x.Grow(objects, count);
			const auto noteEntry = [&](std::uint32_t a_entry) {
				if (a_entry < count && !x.entryMark[a_entry]) {
					x.entryMark[a_entry] = 1;
					x.entries.push_back(a_entry);
				}
			};
			const auto noteGeometry = [&](const RE::BSGeometry* a_geometry) {
				if (!a_geometry)
					return;
				if (const auto it = candidates.geometries.find(a_geometry); it != candidates.geometries.end())
					noteEntry(candidates.geometryEntry[it->second]);
			};
			constexpr std::uint32_t causes = kChangeBindings | kChangeShadow | kChangeGeometry;
			for (const auto& change : c.cursor.Unread(a_tables.changeLog))
				if ((change.causes & causes) && change.slot < objects && !x.objectMark[change.slot]) {
					x.objectMark[change.slot] = 1;
					x.objects.push_back(change.slot);
				}
			for (const std::uint32_t o : x.objects) {
				x.objectMark[o] = 0;
				const RE::BSGeometry* geometry = (a_tables.objects[o].flags & kObjectFree) ? nullptr : a_tables.objectGeometry[o];
				noteGeometry(x.objectGeometry[o]);
				if (geometry != x.objectGeometry[o]) {
					x.Unlink(o);
					x.Link(o, geometry);
				}
				noteGeometry(geometry);
			}
			x.objects.clear();
			// The inputs, when an object may have joined or left them (the mode's membership version moved, or none is kept).
			if (!membership || membership != c.membership || modeBits != x.modeBits) {
				std::vector<std::uint32_t> inputs;
				inputs.reserve(x.inputs.size());
				CollectInputs(a_payload, a_mode, x.inputMark, 2, inputs);
				for (const std::uint32_t o : x.inputs)
					if (!(x.inputMark[o] & 2)) {
						x.inputMark[o] = 0;  // left
						noteGeometry(x.objectGeometry[o]);
					}
				for (const std::uint32_t o : inputs) {
					if (!(x.inputMark[o] & 1))
						noteGeometry(x.objectGeometry[o]);  // joined
					x.inputMark[o] = 1;
				}
				x.inputs = std::move(inputs);
				x.modeBits = modeBits;
			}
			if (candidatesMoved) {
				x.changed.clear();
				ChangedEntriesByLog(c.candidates.get(), candidates, x.changed);
				for (const std::uint32_t e : x.changed)
					noteEntry(e);
				if (c.excluded.size() > count)
					c.excludedCount -= static_cast<std::uint32_t>(std::count(c.excluded.begin() + count, c.excluded.end(), std::uint8_t(1)));
				c.excluded.resize(count, 0);
			}
			for (const std::uint32_t e : x.entries) {
				x.entryMark[e] = 0;
				std::uint8_t verdict = candidates.entryNodes[e] ? 1 : 0;
				for (std::size_t i = 0; verdict && i < candidates.entryGeometries[e].size(); ++i) {
					const auto* geometry = candidates.geometryNodes[candidates.entryGeometries[e][i]];
					const auto it = geometry ? x.firstObject.find(geometry) : x.firstObject.end();
					for (std::uint32_t o = it != x.firstObject.end() ? it->second : SunExclusionIndex::kNone; o != SunExclusionIndex::kNone; o = x.nextObject[o])
						if (isBlocking(o)) {
							verdict = 0;
							break;
						}
				}
				if (c.excluded[e] != verdict) {
					c.excluded[e] = verdict;
					c.excludedCount = verdict ? c.excludedCount + 1 : c.excludedCount - 1;
					flipped = true;
				}
			}
			x.entries.clear();
		}
		c.cursor.Advance(a_tables.changeLog);
		c.membership = membership;
		// Policy::ReuseShadowExclusion may judge every object to compare against the kept verdicts.
		std::vector<std::uint8_t> whole;
		bool judgedWhole = false;
		if (kept && !Policy::ReuseShadowExclusion(a_payload.inputs.frameNumber)) {
			std::vector<std::uint8_t> isInput(objects, 0);
			std::vector<std::uint32_t> list;
			CollectInputs(a_payload, a_mode, isInput, 1, list);
			JudgeAll(candidates, a_tables, isInput, 1, whole);
			judgedWhole = true;
			const bool same = whole == c.excluded;
			if (Diagnostics::Armed<Diagnostics::ExclusionRebuilt>())
				Diagnostics::Notify(Diagnostics::ExclusionRebuilt{ paraboloid, same });
			kept = same;  // a difference is judged whole from here (and reported)
		}
		std::shared_ptr<SunExclusion> exclusion;
		if (kept) {
			++c.reused;
			c.translated += candidatesMoved ? 1 : 0;
			if (!flipped && !candidatesMoved && x.last && x.last->candidates == a_candidates) {
				// Nothing it was judged from moved: the same exclusion again.
				exclusion = x.last;
				++c.same;
			} else {
				exclusion = std::make_shared<SunExclusion>();
				exclusion->candidates = a_candidates;
				exclusion->builtFrame = a_payload.inputs.frameNumber;
				exclusion->reused = true;
				exclusion->excluded = c.excluded;
				exclusion->excludedCount = c.excludedCount;
				exclusion->version = flipped ? NextExclusionVersion() : c.version;
			}
		} else {
			// Whole: the index, the inputs and every verdict again.
			x.Grow(objects, count);
			x.Reindex(a_tables);
			for (const std::uint32_t o : x.inputs)
				if (o < x.inputMark.size())
					x.inputMark[o] = 0;
			x.inputs.clear();
			CollectInputs(a_payload, a_mode, x.inputMark, 1, x.inputs);
			x.modeBits = modeBits;
			if (!judgedWhole)
				JudgeAll(candidates, a_tables, x.inputMark, 1, whole);
			exclusion = std::make_shared<SunExclusion>();
			exclusion->candidates = a_candidates;
			exclusion->builtFrame = a_payload.inputs.frameNumber;
			exclusion->excludedCount = static_cast<std::uint32_t>(std::count(whole.begin(), whole.end(), std::uint8_t(1)));
			// A whole build matching the cache keeps its version.
			const bool same = c.valid && c.candidates == a_candidates && c.excluded == whole;
			exclusion->version = same ? c.version : NextExclusionVersion();
			exclusion->excluded = whole;
			c.excluded = std::move(whole);
			c.excludedCount = exclusion->excludedCount;
			c.cursor.Restart(a_payload.inputs.tablesGeneration);
			c.valid = true;
			x.valid = true;
		}
		if (exclusion != x.last) {
			x.EnsureStamps(count, geometryCount, paraboloid);
			x.GiveStamps(*exclusion);
		}
		c.candidates = a_candidates;
		c.version = exclusion->version;
		x.last = exclusion;
		ForwardEnds(x, exclusion, paraboloid);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
		c.buildMs += ms;
		c.buildMaxMs = std::max(c.buildMaxMs, ms);
		return exclusion;
	}

	namespace
	{
		// CountModeDraws goes to the pool past this many inputs, in chunks of this many.
		constexpr std::size_t kCountFanoutMin = 8192;
		constexpr std::size_t kCountGrain = 4096;

		/** @brief Per used mode, the draws its inputs can produce (a skin once per partition), total and per key slot (bucket sizes). */
		void CountModeDraws(ShadowPayload& a_out)
		{
			ZoneScopedN("CS.DCLF.BuildShadow.CountModeDraws");
			// One pass for every used mode (each input's mask names its modes): a reduce, each chunk counting into its own totals, summed
			// in chunk order.
			std::uint32_t used = 0, usedBits = 0;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				a_out.keySlotDraws[m].clear();
				a_out.modeInputs[m] = 0;
				if (a_out.inputs.modeUsed[m]) {
					used |= 1u << m;
					usedBits |= a_out.modeBits[m];
				}
			}
			struct Counts
			{
				std::array<std::uint64_t, kShadowModeCount> draws{};
				std::array<std::uint32_t, kShadowModeCount> inputs{};
				std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlots;
			};
			const std::size_t regionCount = a_out.regionInputs.elements ? a_out.regionInputs.elements->size() : 0;
			const std::size_t count = regionCount + a_out.inputList.size();
			auto countRange = [&](std::size_t a_begin, std::size_t a_end, Counts& a_counts) {
				for (std::size_t n = a_begin; n < a_end; ++n) {
					const DrawInput& input = n < regionCount ? (*a_out.regionInputs.elements)[n] : a_out.inputList[n - regionCount];
					if (!(input.view.mask & usedBits))
						continue;
					const std::uint32_t inputDraws = PartitionDraws(input.partitions);
					for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
						if (!((used >> m) & 1) || !(input.view.mask & a_out.modeBits[m]))
							continue;
						const std::uint32_t key = ShadowKeyOf(m, input);
						if (key == ~0u)
							continue;
						++a_counts.inputs[m];
						a_counts.draws[m] += inputDraws;
						auto& slots = a_counts.keySlots[m];
						if (key >= slots.size())
							slots.resize(std::size_t(key) + 1, 0u);
						slots[key] += inputDraws;
					}
				}
			};
			std::vector<Counts> chunks((std::max<std::size_t>(count, 1) + kCountGrain - 1) / kCountGrain);
			if (count >= kCountFanoutMin && !Policy::SerialFanout()) {
				SceneScheduler::Executor().ParallelFor("CS.DCLF.BuildShadow.CountModeDraws", count, kCountGrain, [&](std::size_t a_begin, std::size_t a_end) {
					ZoneScopedN("CS.DCLF.BuildShadow.CountModeDrawsShards");
					countRange(a_begin, a_end, chunks[a_begin / kCountGrain]);
				});
			} else {
				chunks.resize(1);
				countRange(0, count, chunks[0]);
			}
			std::array<std::uint64_t, kShadowModeCount> draws{};
			for (const auto& chunk : chunks)
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					draws[m] += chunk.draws[m];
					a_out.modeInputs[m] += chunk.inputs[m];
					auto& slots = a_out.keySlotDraws[m];
					if (slots.size() < chunk.keySlots[m].size())
						slots.resize(chunk.keySlots[m].size(), 0u);
					for (std::size_t key = 0; key < chunk.keySlots[m].size(); ++key)
						slots[key] += chunk.keySlots[m][key];
				}
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

		// ---- Shared by every view: the geometry table and binding records (object records are the stream views', published
		// in the payload by PayloadBuilder; palettes are FrameValues').
		TakeGeometryDraws(a_streams.get(), a_tables, a_in.tablesGeneration, a_in.frameNumber, a_out.geometries);
		a_out.streams = std::move(a_streams);
		AppendStreamRows(a_tables, a_in.addresses.streamBuffer, a_out.geometries);
		if (a_in.addresses.streamBuffer)
			a_out.streamRows = a_tables.streamRows;
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
		// The objects' and extras' registers are the payload's: the commit fills them from the installed payload.
		frameRecord.textures[kPlacementBufferRegister] = a_in.addresses.placementsIndex;
		frameRecord.textures[kPaletteBufferRegister] = a_in.addresses.palettesIndex;
		frameRecord.textures[kShadingBufferRegister] = a_in.addresses.shadingIndex;  // a tree's wind until its entry
		Extensions::WriteFrameBindings(a_in.addresses.frameBindings, frameRecord.textures);
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
			// Read now, not from the walk (ShadowTexcoord).
			ShadowMaterialRow row;
			row.texcoord = ShadowTexcoord(*material, ShadowTransformBuffer());
			row.diffuse = textureIt->second;
			if (const auto owner = a_lookups.shadowTextureOwners.find(srv); owner != a_lookups.shadowTextureOwners.end())
				a_out.bindingOwners.push_back(owner->second);
			const auto index = static_cast<std::uint32_t>(rows.size());
			rows.push_back(row);
			recordByMaterial.emplace(material, index);
			objectRecord[o] = index;
		}
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
				const std::uint32_t streamIndex = StreamGeometry(a_tables, o, a_in.addresses.streamBuffer);
				if (streamIndex == ~0u && (IsStreamObject(a_tables, o) || (key.vertexLayout & kPositionInSecondStream))) {
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
