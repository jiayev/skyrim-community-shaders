#include "Internal.h"

namespace DCLF
{
	void SceneStore::Tables::ListMaterialSlot(std::uint32_t a_slot, std::uint32_t a_frame)
	{
		const std::uint32_t signature = MaterialSources::Signature(materialSlotKey[a_slot].second);
		if (a_slot >= materialSignatureListed.size()) {
			materialSignatureListed.resize(materialSlotKey.size(), 0);
			transformWatchFrame.resize(materialSlotKey.size(), 0);
		}
		if (materialSignatureListed[a_slot] != signature + 1) {
			materialSignatureListed[a_slot] = signature + 1;
			frameSignatures[signature].slots.push_back(a_slot);
		}
		materialFramePending.push_back(a_slot);
		if (!transformWatchFrame[a_slot])
			transformWatch.push_back(a_slot);
		transformWatchFrame[a_slot] = a_frame;
	}

	void SceneStore::RefreshFrameMaterials()
	{
		stats.frameMaterialSamples = 0;
		auto& evaluator = ConstantEvaluator::Get();
		if (tables.materials.empty() || !evaluator.HasLightingShader())
			return;
		auto& m = materialFrameStats;
		++m.frames;
		auto apply = [&](std::uint32_t a_slot, const MaterialRecord& a_live) {
			bool floatsChanged = false;
			if (MaterialSources::ApplyFrameComponents(a_live, tables.materials[a_slot], tables.materialSlotKey[a_slot].second, &floatsChanged)) {
				tables.materialVersion[a_slot] = ++materialVersions;
				tables.MarkMaterialTextureChanged(a_slot, frame);
			}
			if (floatsChanged)
				tables.materialFrameVersion[a_slot] = tables.NextVersion();
		};
		auto keyed = [&](std::uint32_t a_slot, std::uint32_t a_signature) {
			return a_slot < tables.materials.size() && tables.materialSlots.Alive(a_slot) && tables.materialSlotKey[a_slot].first &&
			       MaterialSources::Signature(tables.materialSlotKey[a_slot].second) == a_signature;
		};
		for (auto& [signature, entry] : tables.frameSignatures) {
			// The live sample, from a slot of the signature drawn this frame (the last one's while it still is). With none
			// drawn nothing of the signature is: the slots take the sample when one is.
			auto& slots = entry.slots;
			if (!(keyed(entry.representative, signature) && tables.materialLastUsed[entry.representative] == frame)) {
				entry.representative = ~0u;
				for (const std::uint32_t slot : slots)
					if (keyed(slot, signature) && tables.materialLastUsed[slot] == frame) {
						entry.representative = slot;
						break;
					}
			}
			if (entry.representative == ~0u)
				continue;
			const auto key = tables.materialSlotKey[entry.representative];
			MaterialRecord live;
			if (!evaluator.EvaluateMaterial(key.first, key.second, live))
				continue;
			++stats.frameMaterialSamples;
			++m.samples;
			if (entry.appliedValid) {
				MaterialRecord probe = entry.applied;
				bool floatsChanged = false;
				if (!MaterialSources::ApplyFrameComponents(live, probe, key.second, &floatsChanged) && !floatsChanged)
					continue;
			}
			entry.applied = live;
			entry.appliedValid = true;
			++m.applications;
			for (std::size_t i = 0; i < slots.size();) {
				const std::uint32_t slot = slots[i];
				if (!keyed(slot, signature)) {
					if (slot < tables.materialSignatureListed.size() && tables.materialSignatureListed[slot] == signature + 1)
						tables.materialSignatureListed[slot] = 0;
					slots[i] = slots.back();
					slots.pop_back();
					continue;
				}
				apply(slot, live);
				++m.slotsApplied;
				++i;
			}
		}
		// The slots keyed or rewritten since the last application: the signature's sample regardless.
		m.pending += tables.materialFramePending.size();
		for (const std::uint32_t slot : tables.materialFramePending) {
			if (slot >= tables.materials.size() || !tables.materialSlots.Alive(slot) || !tables.materialSlotKey[slot].first)
				continue;
			const auto it = tables.frameSignatures.find(MaterialSources::Signature(tables.materialSlotKey[slot].second));
			if (it != tables.frameSignatures.end() && it->second.appliedValid)
				apply(slot, it->second.applied);
		}
		tables.materialFramePending.clear();
	}

	void SceneStore::RefreshTextureTransforms()
	{
		ZoneScopedN("CS.DCLF.Capture.TextureTransforms");
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> changed;
		MaterialSources::DrainTransformChanges(changed);
		for (const auto* material : changed) {
			if (auto it = materialDependents.find(material); it != materialDependents.end())
				for (const auto slot : it->second)
					if (slot < tables.transformWatchFrame.size() && tables.materialSlots.Alive(slot) && tables.materialSlotKey[slot].first == material) {
						if (!tables.transformWatchFrame[slot])
							tables.transformWatch.push_back(slot);
						tables.transformWatchFrame[slot] = frame;
					}
		}
		auto& list = tables.transformWatch;
		materialFrameStats.transformsWatched += list.size();
		for (std::size_t i = 0; i < list.size();) {
			const std::uint32_t slot = list[i];
			bool keep = slot < tables.materials.size() && tables.materialSlots.Alive(slot) && tables.materialSlotKey[slot].first;
			// The material is read only while a slot drawn this frame holds it.
			if (keep && tables.materialLastUsed[slot] == frame) {
				const auto* material = tables.materialSlotKey[slot].first;
				if (MaterialSources::ApplyTextureTransform(material, tables.materials[slot]))
					tables.materialFrameVersion[slot] = tables.NextVersion();
				const auto* base = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
				keep = frame - tables.transformWatchFrame[slot] <= 2 || base->texCoordOffset[0] != base->texCoordOffset[1] ||
				       base->texCoordScale[0] != base->texCoordScale[1];
			}
			if (!keep) {
				tables.transformWatchFrame[slot] = 0;
				list[i] = list.back();
				list.pop_back();
				continue;
			}
			++i;
		}
	}

	void SceneStore::CheckMaterialFrame()
	{
		// CS_DCLF_PERSISTENT_PARITY: every slot drawn this frame against its signature's sample and its material's transform,
		// as the per-frame loops applied them.
		const bool enabled = SwitchEnabled(Switch::PersistentParity);
		if (!enabled || !ParityDue(frame, 45))
			return;
		// The normal path consumes writer-produced use lists. On a parity frame compare them against the
		// old full-table discovery before trusting their absence to skip descriptor/import work.
		auto compareUsed = [&](const auto& a_lastUsed, const auto& a_emitted, const auto& a_bits, const char* a_name) {
			std::vector<std::uint32_t> discovered, emitted = a_emitted, bitmap;
			for (std::uint32_t slot = 0; slot < a_lastUsed.size(); ++slot)
				if (a_lastUsed[slot] == frame)
					discovered.push_back(slot);
			for (std::size_t word = 0; word < a_bits.size(); ++word)
				for (std::uint64_t remaining = a_bits[word]; remaining; remaining &= remaining - 1)
					bitmap.push_back(static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining)));
			std::sort(emitted.begin(), emitted.end());
			emitted.erase(std::unique(emitted.begin(), emitted.end()), emitted.end());
			if (emitted != discovered || bitmap != discovered)
				logger::warn("[DCLF] {} use journal differs from table scan at frame {}: {} emitted, {} bitmap, {} discovered", a_name, frame,
					emitted.size(), bitmap.size(), discovered.size());
		};
		compareUsed(tables.materialLastUsed, tables.usedMaterials, tables.usedMaterialBits, "material");
		compareUsed(tables.pipelineLastUsed, tables.usedPipelines, tables.usedPipelineBits, "pipeline");
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		auto& m = materialFrameStats;
		++m.checks;
		ankerl::unordered_dense::map<std::uint32_t, MaterialRecord> live;
		for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
			if (tables.materialLastUsed[slot] != frame || !tables.materialSlotKey[slot].first)
				continue;
			const auto key = tables.materialSlotKey[slot];
			const std::uint32_t signature = MaterialSources::Signature(key.second);
			auto it = live.find(signature);
			if (it == live.end()) {
				MaterialRecord record;
				if (!evaluator.EvaluateMaterial(key.first, key.second, record))
					continue;
				it = live.emplace(signature, record).first;
			}
			++m.slotsChecked;
			MaterialRecord probe = tables.materials[slot];
			bool floatsChanged = false;
			if (MaterialSources::ApplyFrameComponents(it->second, probe, key.second, &floatsChanged) || floatsChanged) {
				if (m.componentsDiffer++ == 0 && m.first.empty())
					m.first = fmt::format("slot {} (pass {:X}): frame components", slot, key.second);
			}
			if (MaterialSources::ApplyTextureTransform(key.first, probe)) {
				if (m.transformsDiffer++ == 0 && m.first.empty())
					m.first = fmt::format("slot {} (pass {:X}): texture transform", slot, key.second);
			}
		}
	}

	void SceneStore::ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (a_material)
			materialDependents[a_material].push_back(a_slot);
	}

	void SceneStore::UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (auto it = materialDependents.find(a_material); it != materialDependents.end()) {
			auto& slots = it->second;
			std::erase(slots, a_slot);
			if (slots.empty())
				materialDependents.erase(it);
		}
	}

	void SceneStore::ProcessMaterialWrites()
	{
		ZoneScopedN("CS.DCLF.Capture.MaterialWrites");
		writtenMaterials.clear();
		const bool complete = MaterialSources::Drain(writtenMaterials);
		stats.materialWrites = static_cast<std::uint32_t>(writtenMaterials.size());
		stats.materialsRewritten = stats.materialsDropped = 0;
		if (complete && writtenMaterials.empty())
			return;
		if (!complete)
			logger::warn("[DCLF] material write queue overflowed: every material record is re-evaluated");
		auto& evaluator = ConstantEvaluator::Get();
		const bool canEvaluate = evaluator.HasLightingShader();
		auto written = [&](const RE::BSShaderMaterial* a_material) { return !complete || writtenMaterials.contains(a_material); };
		std::vector<std::uint32_t> affected;
		if (complete) {
			for (const auto* material : writtenMaterials)
				if (auto it = materialDependents.find(material); it != materialDependents.end())
					affected.insert(affected.end(), it->second.begin(), it->second.end());
		} else {
			affected.reserve(tables.materials.size());
			for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot)
				affected.push_back(slot);
		}
		for (const std::uint32_t slot : affected) {
			if (!tables.materialSlots.Alive(slot))
				continue;
			const auto key = tables.materialSlotKey[slot];
			if (!key.first || !written(key.first))
				continue;
			// A referenced slot's key is a bound object's material, and so live: it is evaluated again in place.
			MaterialRecord live;
			if (tables.materialSlots.References(slot) && canEvaluate && evaluator.EvaluateMaterial(key.first, key.second, live)) {
				if (!(live == tables.materials[slot])) {
					tables.materials[slot] = live;
					tables.materialVersion[slot] = ++materialVersions;
					tables.MarkMaterialTextureChanged(slot, frame);
					++stats.materialsRewritten;
				}
				tables.ListMaterialSlot(slot, frame);
				continue;
			}
			// Unreferenced (the drain frees it) or not evaluable: dropped, so that its next use evaluates it afresh. An
			// object's cached derivation checks its slot is still allocated to the same key.
			ClearMaterialSlot(slot);
			tables.materialSlots.Free(slot);
			++stats.materialsDropped;
			slotsFreedThisFrame = true;
		}
	}

	void SceneStore::RefreshFrameConstants()
	{
		{
			RefreshFrameMaterials();
		}
		CheckMaterialFrame();
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		struct
		{
			GeometryConstants constants;
			bool valid = false;
		} frameSample, eyeSample;
		// The frame lighting, merged from the frame's evaluations (MergeFrameLighting) over what was last published (a
		// component none of them writes keeps its value) and published after them; versioned only when it differs. On a
		// parity frame each pipeline's reference is kept to compare with what was published.
		FrameLighting frameLighting;
		std::memcpy(frameLighting.data(), tables.frameLighting.data(), sizeof(frameLighting));
		std::uint32_t lightingWritten = 0;
		auto publishLighting = [&](const GeometryConstants& a_constants) { MergeFrameLighting(a_constants.ps, frameLighting, lightingWritten); };
		std::vector<std::pair<std::uint32_t, GeometryConstants>> lightingReferences;
		const bool geometryParityEnabled = SwitchEnabled(Switch::PersistentParity);
		const bool geometryParityFrame = geometryParityEnabled && ParityDue(frame, 30);
		geometryStats.checks += geometryParityFrame ? 1u : 0u;
		for (const std::uint32_t i : tables.usedPipelines) {
			if (i >= tables.pipelines.size() || i >= tables.geometryTemplate.size())
				continue;
			if (!tables.PipelineUsed(i, frame))
				continue;
			// The technique constants are the frame's (fog, settings, and the shadow mask's view), so a
			// pipeline slot that outlives the frame takes them fresh here, as it did when the pipeline
			// table was rebuilt every frame. Serving the slot's first evaluation instead was both a parity
			// regression and, at startup, a stale view pointer handed to the render graph. They are its technique
			// row's (Tables::TechniqueRow): evaluated once a frame for all the pipelines of its key, and written, and
			// versioned, only where the values differ.
			auto sameFloats = [](const ConstantBlock& a, const ConstantBlock& b) { return std::memcmp(a.floats.data(), b.floats.data(), sizeof(a.floats)) == 0; };
			auto& row = tables.techniques[tables.pipelineTechnique[i]];
			if (row.evaluated != frame) {
				row.evaluated = frame;
				TechniqueConstants now;
				EvaluateTechnique(tables.pipelines[i].passDescriptor, now);
				const bool floats = !sameFloats(now.vs, row.value.vs) || !sameFloats(now.ps, row.value.ps);
				const bool binding = now.filterModes != row.value.filterModes || now.shadowMask != row.value.shadowMask ||
				                     now.shadowMaskTexture != row.value.shadowMaskTexture;
				if (floats || binding)
					row.value = now;
				if (floats)
					row.constantsVersion = tables.NextVersion();
				if (binding)
					row.bindingVersion = tables.NextVersion();
				// CS_DCLF_PERSISTENT_PARITY: the row against a second evaluation, once a frame.
				if (geometryParityFrame) {
					TechniqueConstants reference;
					EvaluateTechnique(tables.pipelines[i].passDescriptor, reference);
					++geometryStats.techniquesChecked;
					if (!sameFloats(reference.vs, row.value.vs) || !sameFloats(reference.ps, row.value.ps) || reference.filterModes != row.value.filterModes ||
						reference.shadowMask != row.value.shadowMask || reference.shadowMaskTexture != row.value.shadowMaskTexture)
						++geometryStats.techniquesDiffer;
				}
			}
			// A pipeline's PerGeometry block is evaluated in full once (and again when the render flags change); what of it
			// changes afterwards is either overridden per object (ObjectGeometryConstants) or one of the frame's globals
			// (kPSFrameGeometry: the sun's direction and colour, the ambient terms), which every pipeline that writes them
			// shares and which are copied from the frame's one sample. SetupGeometry writes EyePosition only for Envmap, Eye
			// and technique 0x10, and the same for all three (the camera less posAdjust in world space, 0x1414dd040): their
			// pipelines take it from the frame's one evaluation of such a pipeline (eyeSample). The frame's globals are kept
			// in the block for the constant-buffer path and the parity checks, but no DCLF_BINDLESS draw reads them there
			// (frameLighting; kPSBindlessGeometryUnread), and neither the template object's own values, so only what such a
			// draw reads versions the pipeline (SameBindlessGeometry). The template's pass is looked up only to evaluate.
			auto* property = tables.geometryTemplate[i];
			auto templatePassOf = [&]() { return property ? FindLightingPass(property) : nullptr; };
			const std::uint32_t geometryTechnique = (tables.pipelines[i].passDescriptor >> 24) & 0x3f;
			const bool writesEye = geometryTechnique == 1 || geometryTechnique == 0xb || geometryTechnique == 0x10;
			const bool full = !constantsRefreshed || !tables.geometryConstantsValid[i] || (writesEye && !eyeSample.valid);
			auto& held = tables.geometryConstants[i];
			bool ownChanged = false;
			if (!full && writesEye) {
				CopyFrameGeometry(eyeSample.constants, held);
				CopyEyePosition(eyeSample.constants, held);
			} else if (full) {
				const auto* templatePass = templatePassOf();
				if (!templatePass)
					continue;  // keep what BuildFrame evaluated rather than blanking it
				GeometryConstants constants;
				if (!evaluator.EvaluateGeometry(*templatePass, tables.pipelines[i].passDescriptor, kMainPassRenderFlags, constants))
					continue;
				++geometryStats.full;
				publishLighting(constants);
				// Its own values: what a bindless draw reads from the block (PackGeometryTemplate's mask).
				ownChanged = !tables.geometryConstantsValid[i] || !SameBindlessGeometry(constants, held);
				held = constants;
				if (writesEye && !eyeSample.valid) {
					eyeSample.constants = constants;
					eyeSample.valid = true;
				}
			} else {
				if (!frameSample.valid) {
					if (const auto* templatePass = templatePassOf()) {
						frameSample.valid = evaluator.EvaluateGeometry(*templatePass, tables.pipelines[i].passDescriptor, kMainPassRenderFlags, frameSample.constants);
						++geometryStats.samples;
						if (frameSample.valid)
							publishLighting(frameSample.constants);
					}
				}
				if (!frameSample.valid)
					continue;
				CopyFrameGeometry(frameSample.constants, held);
			}
			if (ownChanged) {
				tables.pipelineConstantsVersion[i] = tables.NextVersion();
				++geometryStats.changed;
			}
			tables.geometryConstantsValid[i] = 1;
			// CS_DCLF_PERSISTENT_PARITY: the block against a full evaluation, in what no object overrides; the frame
			// lighting is checked against each reference after the loop, where the reference writes it.
			if (geometryParityFrame) {
				GeometryConstants reference;
				const auto* templatePass = templatePassOf();
				if (templatePass && evaluator.EvaluateGeometry(*templatePass, tables.pipelines[i].passDescriptor, kMainPassRenderFlags, reference)) {
					CheckFrameGeometry(static_cast<std::uint32_t>(i), reference, held);
					lightingReferences.emplace_back(static_cast<std::uint32_t>(i), reference);
				}
			}
		}
		if (lightingWritten && std::memcmp(frameLighting.data(), tables.frameLighting.data(), sizeof(frameLighting)) != 0) {
			std::memcpy(tables.frameLighting.data(), frameLighting.data(), sizeof(frameLighting));
			tables.frameLightingVersion = tables.NextVersion();
			++geometryStats.lightingVersions;
		}
		for (const auto& [pipeline, reference] : lightingReferences) {
			++geometryStats.lightingChecked;
			std::string first;
			if (MatchesFrameLighting(reference.ps, frameLighting, geometryStats.lightingFirst.empty() ? &first : nullptr))
				continue;
			++geometryStats.lightingDiffer;
			if (!first.empty())
				geometryStats.lightingFirst = fmt::format("pipeline {} (pass {:X}) {}", pipeline, tables.pipelines[pipeline].passDescriptor, first);
		}
		++geometryStats.frames;

		// Per-object shading is resampled here too, for the slots whose inputs change with no event (Tables::watched).
		// The property's alpha, emissive colour and multiplier are animated by controllers: candle and chandelier
		// emissives flicker, and sampling them at EarlyPrepass instead of here put them far enough from the draw that
		// capture parity's 0.1% tolerance on EmitColor stopped covering the difference. The LOD fades GetRenderPasses
		// leaves on the property change only when the engine registers the object, which is a patch, and the patch
		// samples them. An actor's alpha fades with the actor. ProjectedUV's and land blend's extras rows follow the
		// eye and a clock.
		auto refreshExtras = [&](std::uint32_t o) {
			if (tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) {
				const auto* geometry = tables.objectGeometry[o];
				auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				if (property && !(tables.objects[o].flags & kObjectNoBindings))
					RefreshObjectExtras(o, *static_cast<RE::BSLightingShaderProperty*>(property), *geometry);
			}
		};
		std::uint64_t resampled = 0;
		if (!constantsRefreshed || !lodFadeEventsInstalled) {
			constantsRefreshed = true;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				ResampleShading(o, true);
				refreshExtras(o);
			}
			resampled += tables.objects.size();
		} else {
			auto& watched = tables.watched;
			for (std::size_t i = 0; i < watched.Size();) {
				const std::uint32_t o = watched.list[i];
				auto& bits = tables.shadingWatch[o];
				if (o >= tables.objects.size() || (tables.objects[o].flags & kObjectFree) || !bits) {
					bits = 0;
					watched.RemoveAt(i);
					continue;
				}
				if (bits & Tables::kWatchShading)
					ResampleShading(o, true);
				if (bits & Tables::kWatchExtras)
					refreshExtras(o);
				++resampled;
				++i;
			}
			for (const std::uint32_t o : tables.actorObjects) {
				ResampleShading(o, true);
				++resampled;
			}
			lodFadeChanged.clear();
			DrainLodFadeEvents(lodFadeChanged);
			MaterialSources::DrainShadingChanges(lodFadeChanged);
			std::sort(lodFadeChanged.begin(), lodFadeChanged.end());
			lodFadeChanged.erase(std::unique(lodFadeChanged.begin(), lodFadeChanged.end()), lodFadeChanged.end());
			shadingParity.lodFadeEvents += lodFadeChanged.size();
			for (const void* key : lodFadeChanged) {
				const auto dependents = propertyDependents.find(key);
				if (dependents == propertyDependents.end())
					continue;
				for (auto* geometry : dependents->second) {
					const auto it = tracked.find(geometry);
					if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.objectStamp != objectStamp)
						continue;
					ResampleShading(it->second.slot, true);
					++resampled;
				}
			}
		}
		// The watch's completeness: every slot sampled against what the tables hold now.
		const bool shadingParityEnabled = SwitchEnabled(Switch::PersistentParity);
		auto& sp = shadingParity;
		++sp.frames;
		sp.watched += tables.watched.Size();
		sp.resampled += resampled;
		if (shadingParityEnabled && ParityDue(frame)) {
			++sp.checks;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !tables.objectGeometry[o])
					continue;
				++sp.slots;
				if (ResampleShading(o, false) && sp.missing++ == 0) {
					const auto* geometry = tables.objectGeometry[o];
					const auto& held = tables.shading[o];
					float emissiveMult = 0.0f;
					LightingDescriptors descriptors;
					descriptors.pass = tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor;
					descriptors.technique = (descriptors.pass >> 24) & 0x3f;
					const auto& lighting = *static_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
					descriptors.specularLODFade = lighting.specularLODFade;
					descriptors.envmapLODFade = lighting.envmapLODFade;
					const auto now = MakeShading(lighting, descriptors, kMainPassRenderFlags, emissiveMult, IsResidentSlot(o));
					sp.first = fmt::format("slot {} '{}' (watch {:#x}, flags {:#x}, patched {} frames ago): material data ({} {} {}) against ({} {} {}), emit ({} {} {}) against ({} {} {}), mult {} against {}",
						o, geometry->name.c_str() ? geometry->name.c_str() : "", o < tables.shadingWatch.size() ? tables.shadingWatch[o] : 0, tables.objects[o].flags,
						o < patchedFrame.size() ? frame - patchedFrame[o] : ~0u, now.materialData[0], now.materialData[1], now.materialData[2], held.materialData[0],
						held.materialData[1], held.materialData[2], now.emitColor[0], now.emitColor[1], now.emitColor[2], held.emitColor[0], held.emitColor[1],
						held.emitColor[2], emissiveMult, tables.emissiveMult[o]);
				}
			}
		}

		// Advanced Skin's wetness, per actor-owned object. Skin::GetWetness keeps each actor's fading state and
		// computes it once a frame (the first call; later ones, including its own SetupGeometry hook for the draws
		// the engine still makes this frame, return the same value), so calling it here for every actor in the
		// tables advances every actor's fade once a frame, whether or not the engine draws it.
		ZoneScopedN("CS.DCLF.Capture.Wetness");
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		const auto wetnessStats = tables.actorWetness.Update([&](std::uint32_t o) {
			auto* geometry = tables.objectGeometry[o];
			const float4 value = wetness && geometry ? skin.GetWetness(geometry) : float4{};
			return ActorValueIndex::Value{ value.x, value.y, value.z, value.w };
		}, [&](std::uint32_t o, const auto& row) {
			if (std::memcmp(row.data(), tables.skinWetness[o].data(), sizeof(row)) != 0) {
				tables.skinWetness[o] = row;
				tables.NoteChange(o, kChangeShading);
			}
		});
		TracyPlot("CS.DCLF.Wetness.Actors", static_cast<std::int64_t>(wetnessStats.actors));
		TracyPlot("CS.DCLF.Wetness.ChangedActors", static_cast<std::int64_t>(wetnessStats.changed));
		TracyPlot("CS.DCLF.Wetness.VisitedMeshes", static_cast<std::int64_t>(wetnessStats.visited));
		if (shadingParityEnabled && ParityDue(frame)) {
			// Same-frame cached Skin outputs: this neither advances fade twice nor
			// compares against a later engine update.
			std::uint32_t mismatches = wetnessStats.members != tables.actorObjects.size();
			for (const auto o : tables.actorObjects) {
				mismatches += !tables.actorWetness.Contains(o, tables.objectGroup[o], tables.objectIdentity[o]);
				const float4 value = wetness && tables.objectGeometry[o] ? skin.GetWetness(tables.objectGeometry[o]) : float4{};
				const ActorValueIndex::Value reference{ value.x, value.y, value.z, value.w };
				mismatches += std::memcmp(reference.data(), tables.skinWetness[o].data(), sizeof(reference)) != 0;
			}
			logger::info("[DCLF] actor wetness index parity: {} meshes, {} actors, {} propagated, {} differ",
				tables.actorObjects.size(), wetnessStats.actors, wetnessStats.visited, mismatches);
		}
	}

	void SceneStore::CheckFrameGeometry(std::uint32_t a_pipeline, const GeometryConstants& a_reference, const GeometryConstants& a_held)
	{
		auto& g = geometryStats;
		++g.pipelinesChecked;
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& layout = stage ? LightingPSLayout() : LightingVSLayout();
			const auto& a = stage ? a_reference.ps : a_reference.vs;
			const auto& b = stage ? a_held.ps : a_held.vs;
			const std::uint32_t technique = (tables.pipelines[a_pipeline].passDescriptor >> 24) & 0x3f;
			const bool writesEye = technique == 1 || technique == 0xb || technique == 0x10;
			const std::uint64_t skip = stage ? kObjectGeometryPS : (kObjectGeometryVS | (writesEye ? 0ull : 1ull << kVSEyePosition));
			const std::uint64_t perGeometry = stage ? kPSGroups[kPerGeometry] : kVSGroups[kPerGeometry];
			for (std::uint32_t v = 0; v < layout.count; ++v) {
				if ((skip & (1ull << v)) || !(perGeometry & (1ull << v)))
					continue;
				if (std::memcmp(&a.floats[layout.offset[v]], &b.floats[layout.offset[v]], layout.size[v] * sizeof(float)) == 0)
					continue;
				++g.differ[stage][v];
				if (g.first.empty())
					g.first = fmt::format("pipeline {} (pass {:X}) {}{}: {} against {}", a_pipeline, tables.pipelines[a_pipeline].passDescriptor, stage ? "PS" : "VS", v,
						a.floats[layout.offset[v]], b.floats[layout.offset[v]]);
			}
		}
	}

	bool SceneStore::ResampleShading(std::uint32_t a_slot, bool a_write)
	{
		if (a_slot >= tables.objects.size() || a_slot >= tables.objectGeometry.size())
			return false;
		const auto* geometry = tables.objectGeometry[a_slot];
		auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
		if (!property || (tables.objects[a_slot].flags & (kObjectNoBindings | kObjectFree)))
			return false;
		const std::uint32_t passDescriptor = tables.pipelines[tables.objects[a_slot].pipelineIndex].passDescriptor;
		LightingDescriptors descriptors;
		descriptors.pass = passDescriptor;
		descriptors.technique = (passDescriptor >> 24) & 0x3f;
		const auto& lighting = *static_cast<RE::BSLightingShaderProperty*>(property);
		descriptors.specularLODFade = lighting.specularLODFade;
		descriptors.envmapLODFade = lighting.envmapLODFade;
		float emissiveMult = tables.emissiveMult[a_slot];
		const auto shading = MakeShading(lighting, descriptors, kMainPassRenderFlags, emissiveMult, IsResidentSlot(a_slot));
		if (std::memcmp(&shading, &tables.shading[a_slot], sizeof(shading)) == 0 && std::bit_cast<std::uint32_t>(emissiveMult) == std::bit_cast<std::uint32_t>(tables.emissiveMult[a_slot]))
			return false;
		if (a_write) {
			tables.shading[a_slot] = shading;
			tables.emissiveMult[a_slot] = emissiveMult;
			tables.NoteChange(a_slot, kChangeShading);
		}
		return true;
	}

	void SceneStore::RefreshObjectExtras(std::size_t a_object, const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry)
	{
		if (a_object >= tables.extraOffset.size() || tables.extraOffset[a_object] == kNoExtraRows)
			return;
		float* rows = &tables.extraRows[std::size_t(tables.extraOffset[a_object]) * 4];
		std::array<float, kExtraRows * 4> before;
		std::memcpy(before.data(), rows, sizeof(before));
		// Noted when the rows come out different (on every return below).
		struct Note
		{
			Tables& tables;
			std::uint32_t slot;
			const float* rows;
			const std::array<float, kExtraRows * 4>& before;
			~Note()
			{
				if (std::memcmp(before.data(), rows, sizeof(before)) != 0)
					tables.NoteChange(slot, kChangeExtras);
			}
		} note{ tables, static_cast<std::uint32_t>(a_object), rows, before };
		const auto& object = tables.objects[a_object];
		auto& state = globals::game::shadowState->GetRuntimeData();
		const auto eye = state.posAdjust.getEye();

		if (object.flags & kObjectLandBlend) {
			// BSLightingShader::SetupGeometry, techniques 8 and 19 (engine notes: per-object constants):
			// xy from the landscape material, zw a blend between two BSShaderManager::State positions by
			// a clock the same state holds, minus the geometry's world translation. Module-relative reads.
			static const REL::Relocation<std::uintptr_t> blendClock{ REL::Offset(0x2033080) };
			static const REL::Relocation<std::uintptr_t> blendClockStart{ REL::Offset(0x2033118) };
			static const REL::Relocation<std::uintptr_t> blendDuration{ REL::Offset(0x20330f8) };
			static const REL::Relocation<std::uintptr_t> blendRate{ REL::Offset(0x1ad2840) };
			static const REL::Relocation<std::uintptr_t> blendFromX{ REL::Offset(0x2033108) };
			static const REL::Relocation<std::uintptr_t> blendFromY{ REL::Offset(0x203310c) };
			static const REL::Relocation<std::uintptr_t> blendToX{ REL::Offset(0x2033110) };
			static const REL::Relocation<std::uintptr_t> blendToY{ REL::Offset(0x2033114) };
			float t = (GlobalFloatAt(blendClock) - GlobalFloatAt(blendClockStart)) * (GlobalFloatAt(blendRate) / GlobalFloatAt(blendDuration));
			if (t <= 0.0f)
				t = 0.0f;
			if (1.0f <= t)
				t = 1.0f;
			const float x = (GlobalFloatAt(blendToX) - GlobalFloatAt(blendFromX)) * t + GlobalFloatAt(blendFromX);
			const float y = (GlobalFloatAt(blendToY) - GlobalFloatAt(blendFromY)) * t + GlobalFloatAt(blendFromY);
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(a_property.material);
			float* land = rows + kExtraRowLandBlend * 4;
			land[0] = material ? material->landBlendParams.red : 0.0f;
			land[1] = material ? material->landBlendParams.green : 0.0f;
			land[2] = x - a_geometry.world.translate.x;
			land[3] = y - a_geometry.world.translate.y;
		}

		if (object.flags & kObjectProjectedUV) {
			// The texture matrix, as SetupGeometry builds it for a ProjectedUV pass (engine notes): the
			// projection is a fixed rotation about Z placed at posAdjust, converted with the engine's own
			// NiTransform-to-matrix routine (which subtracts posAdjust, so its translation is zero); for
			// every technique but Envmap it is multiplied onto the geometry's world matrix, converted the
			// same way and with posAdjust added back. The engine's two routines are called so that the
			// result is the native one to the bit, and this runs at Prepass so posAdjust is the main
			// camera's. TextureProj's rows are the product's columns.
			using ToMatrix = void (*)(float*, const RE::NiTransform*);
			using Multiply = void* (*)(float*, const float*, const float*);
			static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(0x14aaf10) };
			static const REL::Relocation<Multiply> multiply{ REL::Offset(0x153d3c8) };
			RE::NiTransform projection;
			projection.rotate.entry[0][0] = 0.0f;
			projection.rotate.entry[0][1] = 1.0f;
			projection.rotate.entry[0][2] = 0.0f;
			projection.rotate.entry[1][0] = -1.0f;
			projection.rotate.entry[1][1] = 0.0f;
			projection.rotate.entry[1][2] = 0.0f;
			projection.rotate.entry[2][0] = 0.0f;
			projection.rotate.entry[2][1] = 0.0f;
			projection.rotate.entry[2][2] = 1.0f;
			projection.translate = eye;
			projection.scale = 1.0f;
			float p[16], m[16];
			toMatrix(p, &projection);
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (technique == 1) {
				std::memcpy(m, p, sizeof(m));
			} else {
				float w[16];
				toMatrix(w, &a_geometry.world);
				w[12] += eye.x;
				w[13] += eye.y;
				w[14] += eye.z;
				multiply(m, w, p);
			}
			float* proj = rows + kExtraRowTextureProj * 4;
			for (std::uint32_t r = 0; r < 3; ++r) {
				proj[r * 4 + 0] = m[0 + r];
				proj[r * 4 + 1] = m[4 + r];
				proj[r * 4 + 2] = m[8 + r];
				proj[r * 4 + 3] = m[12 + r];
			}
			// The pixel parameters (FUN_1414e00c0): the property's projectedUVParams folded by its w, its
			// projectedUVColor, and the two tiling globals with the projected-normals switch.
			static const REL::Relocation<std::uintptr_t> tilingDiffuse{ REL::Offset(0x2035560) };
			static const REL::Relocation<std::uintptr_t> tilingDetail{ REL::Offset(0x2035578) };
			static const REL::Relocation<std::uintptr_t> projectedNormals{ REL::Offset(0x2035518) };
			const auto& params = a_property.projectedUVParams;
			const auto& colour = a_property.projectedUVColor;
			float* out = rows + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params.alpha;
			out[0] = fade * params.red;
			out[1] = 0.0f;  // never written by the engine
			out[2] = params.blue;
			out[3] = fade * params.green + params.alpha;
			out[4] = colour.red;
			out[5] = colour.green;
			out[6] = colour.blue;
			out[7] = colour.alpha;
			out[8] = GlobalFloatAt(tilingDiffuse);
			out[9] = GlobalFloatAt(tilingDetail);
			out[10] = 0.0f;
			out[11] = *reinterpret_cast<const std::uint8_t*>(projectedNormals.address()) ? 1.0f : 0.0f;
		}
	}

	void SceneStore::NoteProjectedTextures()
	{
		auto& state = globals::game::shadowState->GetRuntimeData();
		ProjectedTextures seen{};
		for (std::size_t i = 0; i < ProjectedTextures::kSlots.size(); ++i) {
			seen.views[i] = reinterpret_cast<ID3D11ShaderResourceView*>(state.PSTexture[ProjectedTextures::kSlots[i]]);
			if (!seen.views[i])
				return;
		}
		seen.valid = true;
		projectedTextures = seen;
	}

	void SceneStore::MaterialReference::reset(RE::BSShaderMaterial* a_material)
	{
		// BSIntrusiveRefCounted's count (+0x8), incremented as the engine takes a reference on a property's material
		// (FUN_1414ac820); a material being read off a live property has one already, so it cannot be at zero here.
		if (a_material)
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<std::byte*>(a_material) + 0x8));
		if (material) {
			using Release = void(void*, RE::BSShaderMaterial*);
			static REL::Relocation<Release*> release{ REL::Offset(0x14f7a40) };  // AE ID 107720
			static REL::Relocation<void**> manager{ REL::Offset(0x3187758) };     // AE ID 403555
			release(*manager, material);
		}
		material = a_material;
	}

	bool SceneStore::ProfileEnabled()
	{
		return SwitchEnabled(Switch::Profile);
	}

	bool SceneStore::EvaluateMaterialForSlot(const RE::BSShaderMaterial* a_material, std::uint32_t a_pass, MaterialRecord& a_record)
	{
		// Called only on a materialIndex miss. The persistent slot holds the result;
		// writer events update that record and frame captures patch shared inputs.
		if (!ConstantEvaluator::Get().EvaluateMaterial(a_material, a_pass, a_record)) {
			++stats.ineligible[static_cast<std::size_t>(Ineligible::NotLightingShader)];
			--stats.ineligible[static_cast<std::size_t>(Ineligible::None)];
			return false;
		}
		++stats.materialsEvaluated;
		return true;
	}

	void SceneStore::NoteStaleMaterial(std::uint32_t a_slot, const std::pair<const RE::BSShaderMaterial*, std::uint32_t>& a_key,
		const MaterialRecord& a_served, const MaterialRecord& a_live)
	{
		++stats.materialCacheStale;
		if (!a_served.vs.SameBits(a_live.vs))
			stats.materialDiffMask |= 1u << 0;
		if (!a_served.ps.SameBits(a_live.ps))
			stats.materialDiffMask |= 1u << 1;
		if (a_served.textures != a_live.textures)
			stats.materialDiffMask |= 1u << 2;
		if (a_served.addressModes != a_live.addressModes)
			stats.materialDiffMask |= 1u << 3;
		if (a_served.filterModes != a_live.filterModes)
			stats.materialDiffMask |= 1u << 4;
		if (a_served.textureWritten != a_live.textureWritten)
			stats.materialDiffMask |= 1u << 5;
		static std::uint32_t logged = 0;
		if (logged++ >= 16)
			return;
		std::string what;
		for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f) {
			if (!a_served.vs.SameBits(a_live.vs, f))
				what += fmt::format(" vs[{}] {}->{}", f, a_served.vs.floats[f], a_live.vs.floats[f]);
			if (!a_served.ps.SameBits(a_live.ps, f))
				what += fmt::format(" ps[{}] {}->{}", f, a_served.ps.floats[f], a_live.ps.floats[f]);
		}
		for (std::size_t t = 0; t < a_served.textures.size(); ++t)
			if (a_served.textures[t] != a_live.textures[t] || a_served.addressModes[t] != a_live.addressModes[t] || a_served.filterModes[t] != a_live.filterModes[t])
				what += fmt::format(" texture[{}] {}/{}/{} -> {}/{}/{}", t, static_cast<const void*>(a_served.textures[t]), a_served.addressModes[t],
					a_served.filterModes[t], static_cast<const void*>(a_live.textures[t]), a_live.addressModes[t], a_live.filterModes[t]);
		if (a_served.textureWritten != a_live.textureWritten)
			what += fmt::format(" written {:X}->{:X}", a_served.textureWritten, a_live.textureWritten);
		logger::warn("[DCLF] STALE material record{} (material {}, pass {:X}): a writer the material events do not cover:{}",
			a_slot == ~0u ? std::string() : fmt::format(" in slot {}", a_slot), fmt::ptr(a_key.first), a_key.second, what.substr(0, 600));
	}

	std::uint32_t SceneStore::TechniqueRowFor(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t key = TechniqueKey(a_passDescriptor);
		const auto [it, fresh] = tables.techniqueRow.try_emplace(key, static_cast<std::uint32_t>(tables.techniques.size()));
		if (fresh) {
			auto& row = tables.techniques.emplace_back();
			row.key = key;
			EvaluateTechnique(a_passDescriptor, row.value);
			row.evaluated = frame;
			row.constantsVersion = tables.NextVersion();
			row.bindingVersion = tables.NextVersion();
		}
		return it->second;
	}

	void SceneStore::ValidateMaterialSlice()
	{
		// The standing alarm: a few records drawn this frame, re-evaluated live and compared outside their
		// frame-sourced components. A difference is a material writer the events do not cover; it is
		// reported, not repaired, because repairing it here is what hid the missing events before.
		const std::string& mode = SwitchValue(Switch::MaterialCache);
		if (mode == "off" || tables.materials.empty())
			return;
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		auto validate = [&](std::uint32_t slot) {
			const auto key = tables.materialSlotKey[slot];
			MaterialRecord live;
			if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
				return;
			++stats.materialsValidated;
			MaterialRecord served = tables.materials[slot];
			MaterialSources::CopyFrameComponents(live, served, key.second);
			if (!(served == live))
				NoteStaleMaterial(slot, key, served, live);
		};
		if (mode == "probe") {
			for (const auto slot : tables.usedMaterials)
				if (slot < tables.materials.size() && tables.materialSlots.Alive(slot) && tables.materialLastUsed[slot] == frame)
					validate(slot);
			return;
		}
		std::uint32_t looked = 0;
		for (std::uint32_t n = 0; n < kMaterialValidationsPerFrame * kMaterialValidationStride && looked < kMaterialValidationsPerFrame; ++n) {
			const std::uint32_t slot = materialValidationCursor++ % static_cast<std::uint32_t>(tables.materials.size());
			if (tables.materialLastUsed[slot] != frame)
				continue;
			++looked;
			validate(slot);
		}
	}
}
