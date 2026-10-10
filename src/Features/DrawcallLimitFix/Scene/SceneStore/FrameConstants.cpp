#include "Internal.h"

#include "Features/DrawcallLimitFix/Draws/FrameValues.h"

#include <cmath>

namespace DCLF
{
	void SceneStore::ListFrameMaterial(std::uint32_t a_slot)
	{
		auto& f = frameTables;
		const std::uint32_t signature = MaterialSources::Signature(f.materialKeys[a_slot].second);
		if (f.materialSignatureListed[a_slot] != signature + 1) {
			f.materialSignatureListed[a_slot] = signature + 1;
			f.frameSignatures[signature].slots.push_back(a_slot);
		}
		f.materialFramePending.push_back(a_slot);
		if (!f.transformWatchFrame[a_slot])
			f.transformWatch.push_back(a_slot);
		f.transformWatchFrame[a_slot] = frame;
	}

	void SceneStore::SyncFrameMaterials()
	{
		const Tables& view = FrameView();
		// The frame's copy of each slot's record: taken again from the tables when the slot's key or record moved (a join keyed
		// it, a slot was freed and keyed again); the frame's writes stay on it otherwise. Held while the frame keys it.
		auto& f = frameTables;
		if (f.materialsGeneration != tablesGeneration) {
			f.ResetMaterials();
			frameMaterialOwners.clear();
			f.materialsGeneration = tablesGeneration;
		}
		f.materialLog.Trim(1u << 16);
		const std::size_t count = view.materials.size();
		f.ResizeMaterials(count);
		frameMaterialOwners.resize(count);
		for (std::uint32_t slot = 0; slot < count; ++slot) {
			const auto key = view.materialSlotKey[slot];
			const std::uint32_t source = view.materialVersion[slot];
			if (f.materialKeys[slot] == key && f.materialSource[slot] == source)
				continue;
			if (f.materialKeys[slot].first != key.first) {
				if (f.materialKeys[slot].first)
					f.UnlistMaterialDependent(f.materialKeys[slot].first, slot);
				if (key.first)
					f.materialDependents[key.first].push_back(slot);
				frameMaterialOwners[slot].reset(const_cast<RE::BSShaderMaterial*>(key.first));
			}
			f.materialKeys[slot] = key;
			f.materialSource[slot] = source;
			if (!key.first)
				continue;
			f.materials[slot] = view.materials[slot];
			f.materialVersion[slot] = f.NextVersion();
			f.materialLog.Push(slot);
			ListFrameMaterial(slot);
		}
	}

	void SceneStore::RefreshFrameMaterials()
	{
		const Tables& view = FrameView();
		stats.frameMaterialSamples = 0;
		auto& evaluator = ConstantEvaluator::Get();
		auto& f = frameTables;
		if (f.materials.empty() || !evaluator.HasLightingShader())
			return;
		auto& m = materialFrameStats;
		++m.frames;
		auto apply = [&](std::uint32_t a_slot, const MaterialRecord& a_live) {
			bool floatsChanged = false;
			const bool recordChanged = MaterialSources::ApplyFrameComponents(a_live, f.materials[a_slot], f.materialKeys[a_slot].second, &floatsChanged);
			if (recordChanged) {
				f.materialVersion[a_slot] = f.NextVersion();
				f.materialLog.Push(a_slot);
			}
			if (floatsChanged) {
				f.materialFrameVersion[a_slot] = f.NextVersion();
				f.materialLog.Push(a_slot);
			}
			if (recordChanged || floatsChanged)
				NoteFrameFloats(a_slot);
		};
		auto keyed = [&](std::uint32_t a_slot, std::uint32_t a_signature) {
			return a_slot < f.materials.size() && view.materialSlots.Alive(a_slot) && f.materialKeys[a_slot].first &&
			       MaterialSources::Signature(f.materialKeys[a_slot].second) == a_signature;
		};
		for (auto& [signature, entry] : f.frameSignatures) {
			// The live sample, from a slot of the signature drawn this frame (the last one's while it still is). With none
			// drawn nothing of the signature is: the slots take the sample when one is.
			auto& slots = entry.slots;
			if (!(keyed(entry.representative, signature) && view.MaterialUsed(entry.representative))) {
				entry.representative = ~0u;
				for (const std::uint32_t slot : slots)
					if (keyed(slot, signature) && view.MaterialUsed(slot)) {
						entry.representative = slot;
						break;
					}
			}
			if (entry.representative == ~0u)
				continue;
			const auto key = f.materialKeys[entry.representative];
			MaterialRecord live;
			if (!evaluator.EvaluateMaterial(key.first, key.second, live))
				continue;
			++stats.frameMaterialSamples;
			++m.samples;
			if (auto* lightView = MaterialSources::CharacterLightView(live, key.second))
				frameCapture.characterLightView = lightView;
			if (entry.appliedValid) {
				MaterialRecord probe = entry.applied;
				bool floatsChanged = false;
				if (!MaterialSources::ApplyFrameComponents(live, probe, key.second, &floatsChanged) && !floatsChanged)
					continue;
			}
			entry.applied = live;
			MaterialSources::ApplyFrameComponents(live, entry.applied, key.second);
			entry.appliedValid = true;
			++m.applications;
			for (std::size_t i = 0; i < slots.size();) {
				const std::uint32_t slot = slots[i];
				if (!keyed(slot, signature)) {
					if (slot < f.materialSignatureListed.size() && f.materialSignatureListed[slot] == signature + 1)
						f.materialSignatureListed[slot] = 0;
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
		m.pending += f.materialFramePending.size();
		for (const std::uint32_t slot : f.materialFramePending) {
			if (slot >= f.materials.size() || !view.materialSlots.Alive(slot) || !f.materialKeys[slot].first)
				continue;
			const auto it = f.frameSignatures.find(MaterialSources::Signature(f.materialKeys[slot].second));
			if (it != f.frameSignatures.end() && it->second.appliedValid)
				apply(slot, it->second.applied);
		}
		f.materialFramePending.clear();
	}

	void SceneStore::RefreshTextureTransforms()
	{
		const Tables& view = FrameView();
		ZoneScopedN("CS.DCLF.Capture.TextureTransforms");
		auto& f = frameTables;
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> changed;
		MaterialSources::DrainTransformChanges(changed);
		for (const auto* material : changed) {
			if (auto it = f.materialDependents.find(material); it != f.materialDependents.end())
				for (const auto slot : it->second)
					if (slot < f.transformWatchFrame.size() && view.materialSlots.Alive(slot) && f.materialKeys[slot].first == material) {
						if (!f.transformWatchFrame[slot])
							f.transformWatch.push_back(slot);
						f.transformWatchFrame[slot] = frame;
					}
		}
		auto& list = f.transformWatch;
		materialFrameStats.transformsWatched += list.size();
		for (std::size_t i = 0; i < list.size();) {
			const std::uint32_t slot = list[i];
			bool keep = slot < f.materials.size() && view.materialSlots.Alive(slot) && f.materialKeys[slot].first;
			// The material is read only while a member's slot holds it.
			if (keep && view.MaterialUsed(slot)) {
				const auto* material = f.materialKeys[slot].first;
				if (MaterialSources::ApplyTextureTransform(material, f.materials[slot])) {
					f.materialFrameVersion[slot] = f.NextVersion();
					f.materialLog.Push(slot);
					NoteFrameFloats(slot);
				}
				const auto* base = static_cast<const RE::BSLightingShaderMaterialBase*>(material);
				keep = frame - f.transformWatchFrame[slot] <= 2 || base->texCoordOffset[0] != base->texCoordOffset[1] ||
				       base->texCoordScale[0] != base->texCoordScale[1];
			}
			if (!keep) {
				if (slot < f.transformWatchFrame.size())
					f.transformWatchFrame[slot] = 0;
				list[i] = list.back();
				list.pop_back();
				continue;
			}
			++i;
		}
	}

	void SceneStore::CheckMaterialFrame()
	{
		const Tables& view = FrameView();
		// CS_DCLF_PERSISTENT_PARITY: every slot drawn this frame against its signature's sample and its material's transform,
		// as the per-frame loops applied them.
		const bool enabled = SwitchEnabled(Switch::PersistentParity);
		if (!enabled || !ParityDue(frame, 45))
			return;
		// The used sets are kept by membership changes. On a parity frame compare them against the slots the bound records
		// name, before trusting their absence to skip descriptor/import work.
		{
			std::vector<std::uint64_t> pipelines(view.usedPipelineBits.size()), materials(view.usedMaterialBits.size());
			auto set = [](std::vector<std::uint64_t>& a_bits, std::uint32_t a_slot) {
				if (a_bits.size() <= a_slot / 64)
					a_bits.resize(a_slot / 64 + 1, 0);
				a_bits[a_slot / 64] |= 1ull << (a_slot % 64);
			};
			for (std::uint32_t o = 0; o < view.objects.size(); ++o)
				if (!(view.objects[o].flags & (kObjectNoBindings | kObjectFree))) {
					set(pipelines, view.objects[o].pipelineIndex);
					set(materials, view.objects[o].materialIndex);
				}
			auto compare = [&](std::vector<std::uint64_t> a_named, const std::vector<std::uint64_t>& a_used, const char* a_name) {
				a_named.resize(std::max(a_named.size(), a_used.size()), 0);
				std::uint32_t missing = 0, extra = 0;
				for (std::size_t word = 0; word < a_named.size(); ++word) {
					const std::uint64_t used = word < a_used.size() ? a_used[word] : 0;
					missing += static_cast<std::uint32_t>(std::popcount(a_named[word] & ~used));
					extra += static_cast<std::uint32_t>(std::popcount(used & ~a_named[word]));
				}
				if (missing || extra)
					logger::warn("[DCLF] {} used set differs from the bound records at frame {}: {} named but not used, {} used but not named", a_name, frame,
						missing, extra);
			};
			compare(std::move(pipelines), view.usedPipelineBits, "pipeline");
			compare(std::move(materials), view.usedMaterialBits, "material");
		}
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		auto& m = materialFrameStats;
		++m.checks;
		ankerl::unordered_dense::map<std::uint32_t, MaterialRecord> live;
		const auto& f = frameTables;
		for (std::uint32_t slot = 0; slot < f.materials.size(); ++slot) {
			if (!view.MaterialUsed(slot) || !f.materialKeys[slot].first)
				continue;
			const auto key = f.materialKeys[slot];
			const std::uint32_t signature = MaterialSources::Signature(key.second);
			auto it = live.find(signature);
			if (it == live.end()) {
				MaterialRecord record;
				if (!evaluator.EvaluateMaterial(key.first, key.second, record))
					continue;
				it = live.emplace(signature, record).first;
			}
			++m.slotsChecked;
			MaterialRecord probe = f.materials[slot];
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
		const Tables& view = FrameView();
		ZoneScopedN("CS.DCLF.Capture.MaterialWrites");
		writtenMaterials.clear();
		const bool complete = MaterialSources::Drain(writtenMaterials);
		stats.materialWrites = static_cast<std::uint32_t>(writtenMaterials.size());
		stats.materialsRewritten = stats.materialsHeld = 0;
		auto& f = frameTables;
		// The slots a write could not be evaluated for yet (the Lighting shader not found, the material not ready): asked again, as
		// if written again, until they are.
		std::vector<std::uint32_t> retry = std::move(f.materialEvaluationsPending);
		f.materialEvaluationsPending.clear();
		if (complete && writtenMaterials.empty() && retry.empty())
			return;
		if (!complete)
			logger::warn("[DCLF] material write queue overflowed: every material record is re-evaluated");
		// The slots no object references are the coordinator's to drop (their references are its): posted for its next work.
		if (!complete)
			materialWritesAll = true;
		else
			materialWritesPosted.insert(materialWritesPosted.end(), writtenMaterials.begin(), writtenMaterials.end());
		auto& evaluator = ConstantEvaluator::Get();
		const bool canEvaluate = evaluator.HasLightingShader();
		auto written = [&](const RE::BSShaderMaterial* a_material) { return !complete || writtenMaterials.contains(a_material); };
		std::vector<std::uint32_t> affected;
		if (complete) {
			for (const auto* material : writtenMaterials)
				if (auto it = f.materialDependents.find(material); it != f.materialDependents.end())
					affected.insert(affected.end(), it->second.begin(), it->second.end());
		} else {
			affected.reserve(f.materials.size());
			for (std::uint32_t slot = 0; slot < f.materials.size(); ++slot)
				affected.push_back(slot);
		}
		const std::size_t written_ = affected.size();
		affected.insert(affected.end(), retry.begin(), retry.end());
		for (std::size_t i = 0; i < affected.size(); ++i) {
			const std::uint32_t slot = affected[i];
			if (slot >= f.materials.size() || !view.materialSlots.Alive(slot))
				continue;
			const auto key = f.materialKeys[slot];
			if (!key.first || (i < written_ && !written(key.first)))
				continue;
			// The frame's record, evaluated again in place (the frame holds the material: frameMaterialOwners). One that cannot be
			// evaluated now keeps its last record, which its objects go on drawing with, and is asked again next frame.
			MaterialRecord live;
			if (canEvaluate && evaluator.EvaluateMaterial(key.first, key.second, live)) {
				// Its frame-sourced components (and the character light's t11, which records hold no view of) are the frame's,
				// kept by RefreshFrameMaterials: only the material's own values are compared and rewritten.
				MaterialSources::CopyFrameComponents(f.materials[slot], live, key.second);
				if (!(live == f.materials[slot])) {
					f.materials[slot] = live;
					f.materialVersion[slot] = f.NextVersion();
					f.materialLog.Push(slot);
					++stats.materialsRewritten;
					PostMaterialRecord(slot);
				}
			} else if (std::find(f.materialEvaluationsPending.begin(), f.materialEvaluationsPending.end(), slot) == f.materialEvaluationsPending.end()) {
				f.materialEvaluationsPending.push_back(slot);
				++stats.materialsHeld;
			}
			ListFrameMaterial(slot);
		}
	}

	void SceneStore::DropWrittenMaterials()
	{
		// The coordinator: a written material's slots no object references are dropped, so that their next use evaluates them
		// afresh (the frame re-evaluated the referenced ones in its copies). An object's cached derivation checks its slot is
		// still allocated to the same key.
		if (materialWritesPosted.empty() && !materialWritesAll)
			return;
		stats.materialsDropped = 0;
		// The references are counted from the tables' logs, which this frame's bindings are in but not counted yet.
		UpdateSlotReferences();
		std::vector<std::uint32_t> affected;
		if (materialWritesAll) {
			for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot)
				affected.push_back(slot);
		} else {
			std::sort(materialWritesPosted.begin(), materialWritesPosted.end());
			materialWritesPosted.erase(std::unique(materialWritesPosted.begin(), materialWritesPosted.end()), materialWritesPosted.end());
			for (const auto* material : materialWritesPosted)
				if (auto it = materialDependents.find(material); it != materialDependents.end())
					affected.insert(affected.end(), it->second.begin(), it->second.end());
		}
		materialWritesPosted.clear();
		materialWritesAll = false;
		for (const std::uint32_t slot : affected) {
			if (!tables.materialSlots.Alive(slot) || !tables.materialSlotKey[slot].first || tables.materialSlots.References(slot))
				continue;
			ClearMaterialSlot(slot);
			tables.materialSlots.Free(slot);
			++stats.materialsDropped;
			slotsFreedThisFrame = true;
		}
	}

	void SceneStore::RefreshFrameConstants()
	{
		const Tables& view = FrameView();
		ZoneNamedN(frameConstantsZone, "CS.DCLF.FrameConstants", true);
		SyncFrameMaterials();
		{
			ZoneScopedN("CS.DCLF.FrameConstants.Materials");
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
		std::memcpy(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting));
		std::uint32_t lightingWritten = 0;
		FrameFog frameFog = frameCapture.fog;
		std::uint32_t fogWritten = 0;
		auto publishLighting = [&](const GeometryConstants& a_constants) { MergeFrameLighting(a_constants.ps, frameLighting, lightingWritten); };
		std::vector<std::pair<std::uint32_t, GeometryConstants>> lightingReferences;
		const bool geometryParityEnabled = SwitchEnabled(Switch::PersistentParity);
		const bool geometryParityFrame = geometryParityEnabled && ParityDue(frame, 30);
		geometryStats.checks += geometryParityFrame ? 1u : 0u;
		frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, tablesGeneration);
		frameTables.SyncTechniques(view.techniqueKeys.size());
		TracyCZoneN(pipelinesZone, "CS.DCLF.FrameConstants.Pipelines", true);
		std::int64_t pipelinesVisited = 0, techniquesEvaluated = 0;
		for (std::size_t word = 0; word < view.usedPipelineBits.size(); ++word) {
			for (std::uint64_t remaining = view.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t i = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (i >= view.pipelines.size() || i >= view.geometryTemplate.size())
					continue;
				++pipelinesVisited;
				// The technique constants are the frame's (fog, settings, and the shadow mask's view), so a
				// pipeline slot that outlives the frame takes them fresh here, as it did when the pipeline
				// table was rebuilt every frame. Serving the slot's first evaluation instead was both a parity
				// regression and, at startup, a stale view pointer handed to the render graph. They are its technique
				// row's (Tables::TechniqueRow): evaluated once a frame for all the pipelines of its key, and written, and
				// versioned, only where the values differ.
				auto sameFloats = [](const ConstantBlock& a, const ConstantBlock& b) { return std::memcmp(a.floats.data(), b.floats.data(), sizeof(a.floats)) == 0; };
				auto& row = frameTables.techniques[view.pipelineTechnique[i]];
				if (row.evaluated != frame) {
					row.evaluated = frame;
					++techniquesEvaluated;
					TechniqueConstants now;
					EvaluateTechnique(view.pipelines[i].passDescriptor, now);
					// The fog is the frame's (FrameFog): taken for the frame, and the row keeps its own, so the time of
					// day versions no technique row (nor every pipeline and pair with it).
					MergeFrameFog(now, frameFog, fogWritten);
					KeepTechniqueFog(row.value, now);
					const bool floats = !sameFloats(now.vs, row.value.vs) || !sameFloats(now.ps, row.value.ps);
					const bool binding = now.filterModes != row.value.filterModes || now.shadowMask != row.value.shadowMask ||
					                     now.shadowMaskTexture != row.value.shadowMaskTexture;
					if (floats || binding || !row.valid)
						row.value = now;
					if (floats || !row.valid)
						row.constantsVersion = frameTables.NextVersion();
					if (binding || !row.valid)
						row.bindingVersion = frameTables.NextVersion();
					if (floats || binding || !row.valid)
						PostTechniqueConstants(view.pipelineTechnique[i]);
					row.valid = true;
					// CS_DCLF_PERSISTENT_PARITY: the row against a second evaluation, once a frame.
					if (geometryParityFrame) {
						TechniqueConstants reference;
						EvaluateTechnique(view.pipelines[i].passDescriptor, reference);
						KeepTechniqueFog(row.value, reference);
						++geometryStats.techniquesChecked;
						if (!sameFloats(reference.vs, row.value.vs) || !sameFloats(reference.ps, row.value.ps) || reference.filterModes != row.value.filterModes ||
							reference.shadowMask != row.value.shadowMask || reference.shadowMaskTexture != row.value.shadowMaskTexture)
							++geometryStats.techniquesDiffer;
					}
				}
				// A pipeline's PerGeometry block is evaluated in full once (and again when the render flags change); what of it
				// changes afterwards is either overridden per object (ObjectGeometryConstants) or one of the frame's globals
				// (kPSFrameGeometry: the sun's direction and colour, the ambient terms), which every pipeline that writes them
				// shares and which are copied from the frame's one sample. SetupGeometry writes EyePosition only for some passes
				// (WritesEyePosition), the same for all of them (the camera less posAdjust in world space, 0x1414dd040): their
				// pipelines take it from the frame's one evaluation of such a pipeline (eyeSample). The frame's globals are kept
				// in the block for the constant-buffer path and the parity checks, but no DCLF_BINDLESS draw reads them there
				// (frameLighting; kPSBindlessGeometryUnread), and neither the template object's own values, so only what such a
				// draw reads versions the pipeline (SameBindlessGeometry). The template's pass is looked up only to evaluate.
				auto templatePassOf = [&]() { return TemplatePassOf(view, static_cast<std::uint32_t>(i)); };
				const bool writesEye = WritesEyePosition(view.pipelines[i].passDescriptor);
				const bool full = !constantsRefreshed || !frameTables.geometryConstantsValid[i] || (writesEye && !eyeSample.valid);
				auto& held = frameTables.geometryConstants[i];
				bool ownChanged = false;
				if (!full && writesEye) {
					CopyFrameGeometry(eyeSample.constants, held);
					CopyEyePosition(eyeSample.constants, held);
				} else if (full) {
					const auto* templatePass = templatePassOf();
					if (!templatePass)
						continue;  // keep what BuildFrame evaluated rather than blanking it
					GeometryConstants constants;
					if (!evaluator.EvaluateGeometry(*templatePass, view.pipelines[i].passDescriptor, kMainPassRenderFlags, constants))
						continue;
					++geometryStats.full;
					publishLighting(constants);
					// Its own values: what a bindless draw reads from the block (PackGeometryTemplate's mask).
					ownChanged = !frameTables.geometryConstantsValid[i] || !SameBindlessGeometry(constants, held);
					held = constants;
					if (writesEye && !eyeSample.valid) {
						eyeSample.constants = constants;
						eyeSample.valid = true;
					}
				} else {
					if (!frameSample.valid) {
						if (const auto* templatePass = templatePassOf()) {
							frameSample.valid = evaluator.EvaluateGeometry(*templatePass, view.pipelines[i].passDescriptor, kMainPassRenderFlags, frameSample.constants);
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
					frameTables.pipelineConstantsVersion[i] = frameTables.NextVersion();
					++geometryStats.changed;
					PostPipelineConstants(i);
				}
				frameTables.geometryConstantsValid[i] = 1;
				// CS_DCLF_PERSISTENT_PARITY: the block against a full evaluation, in what no object overrides; the frame
				// lighting is checked against each reference after the loop, where the reference writes it.
				if (geometryParityFrame) {
					GeometryConstants reference;
					const auto* templatePass = templatePassOf();
					if (templatePass && evaluator.EvaluateGeometry(*templatePass, view.pipelines[i].passDescriptor, kMainPassRenderFlags, reference)) {
						CheckFrameGeometry(static_cast<std::uint32_t>(i), reference, held);
						lightingReferences.emplace_back(static_cast<std::uint32_t>(i), reference);
						// T6: against the evaluation from a pass the engine registered for the template's property (or the frame's).
						if (const auto* registered = RegisteredTemplatePassOf(view.geometryTemplate[i])) {
							GeometryConstants engine;
							if (evaluator.EvaluateGeometry(*registered, view.pipelines[i].passDescriptor, kMainPassRenderFlags, engine)) {
								++geometryStats.templateChecked;
								// PS NumLightNumShadowLight.x is the registered pass's own point lights (its object's, the sun not counted):
								// per object, and unread with Light Limit Fix (only .y, the shadow lights, is).
								const std::uint32_t lightCount = LightingPSLayout().offset[kPSNumLights];
								engine.ps.floats[lightCount] = reference.ps.floats[lightCount];
								const bool own = SameBindlessGeometry(engine, reference);
								FrameLighting engineLighting{};
								std::uint32_t engineWritten = 0;
								MergeFrameLighting(engine.ps, engineLighting, engineWritten);
								const bool lighting = MatchesFrameLighting(reference.ps, engineLighting, nullptr);
								if (!own || !lighting) {
									++(own ? geometryStats.templateLightingDiffer : geometryStats.templateDiffer);
									if (geometryStats.templateFirst.empty())
										geometryStats.templateFirst = fmt::format("pipeline {} (descriptor {:#x}): {}", i, view.pipelines[i].passDescriptor,
											own ? std::string("frame lighting") : BindlessGeometryDifferences(engine, reference));
								}
							}
						} else {
							++geometryStats.templateMissing;
						}
					}
				}
			}
		}
		TracyCZoneEnd(pipelinesZone);
		TracyPlot("CS.DCLF.FrameConstants.Pipelines", pipelinesVisited);
		TracyPlot("CS.DCLF.FrameConstants.Techniques", techniquesEvaluated);
		// The new pipelines' evaluations, where nothing above wrote the component (a later pipeline's own frame variables).
		for (const auto& seed : lightingSeeds)
			MergeFrameLighting(seed, frameLighting, lightingWritten);
		lightingSeeds.clear();
		if (fogWritten)
			frameCapture.fog = frameFog;
		if (lightingWritten && std::memcmp(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting)) != 0) {
			std::memcpy(frameCapture.lighting.data(), frameLighting.data(), sizeof(frameLighting));
			++geometryStats.lightingVersions;
		}
		for (const auto& [pipeline, reference] : lightingReferences) {
			++geometryStats.lightingChecked;
			std::string first;
			if (MatchesFrameLighting(reference.ps, frameLighting, geometryStats.lightingFirst.empty() ? &first : nullptr))
				continue;
			++geometryStats.lightingDiffer;
			if (!first.empty())
				geometryStats.lightingFirst = fmt::format("pipeline {} (pass {:X}) {}", pipeline, view.pipelines[pipeline].passDescriptor, first);
		}
		++geometryStats.frames;

		// The objects' shading is FrameValues' (BindlessShading), sampled at the frame's start for the slots the walk names by their
		// events (NameShadingEvents), and their extras' static rows are written by the same events: nothing of either is sampled here.
		// Every pipeline was evaluated in full once: from the next frame, the frame's one sample.
		constantsRefreshed = true;
		++shadingParity.frames;
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(frame) && lodFadeEventsInstalled) {
			auto& ep = extrasParity;
			const auto frameInputs = SampleExtrasFrame();
			std::array<float, kExtraRows * 4> reference{}, completed{};
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !(tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) ||
					!tables.objectGeometry[o] || o >= tables.extraOffset.size() || tables.extraOffset[o] == kNoExtraRows)
					continue;
				float* held = &tables.extraRows[std::size_t(tables.extraOffset[o]) * 4];
				const std::array<float, kExtraRows * 4> before = [&] { std::array<float, kExtraRows * 4> copy; std::memcpy(copy.data(), held, sizeof(copy)); return copy; }();
				if (WriteObjectExtras(o)) {
					++ep.staleStatic;
					std::memcpy(held, before.data(), sizeof(before));
				}
				if (!ReferenceExtras(o, reference.data()))
					continue;
				BindlessPlacement placement;
				if (!FrameValues::SampleSlot(tables, o, placement))
					continue;
				CompleteExtras(held, tables.objects[o].flags, placement.world, frameInputs, completed.data());
				++ep.objects;
				float worst = 0.0f;
				for (std::size_t c = 0; c < completed.size(); ++c)
					worst = std::max(worst, std::abs(completed[c] - reference[c]));
				ep.maxDifference = std::max(ep.maxDifference, worst);
				// TextureProj's translation column: the engine subtracts the eye and adds it back, the draw does not (an ulp of the
				// world position).
				if (worst > 0.05f && ep.differ++ == 0) {
					const auto* geometry = tables.objectGeometry[o];
					ep.first = fmt::format("slot {} '{}' (flags {:#x}): differs by {}", o, geometry->name.c_str() ? geometry->name.c_str() : "?", tables.objects[o].flags, worst);
				}
			}
		}
		if (SwitchEnabled(Switch::PersistentParity) && ParityDue(frame))
			CheckShadingParity();
	}

	void SceneStore::CheckFrameGeometry(std::uint32_t a_pipeline, const GeometryConstants& a_reference, const GeometryConstants& a_held)
	{
		auto& g = geometryStats;
		++g.pipelinesChecked;
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& layout = stage ? LightingPSLayout() : LightingVSLayout();
			const auto& a = stage ? a_reference.ps : a_reference.vs;
			const auto& b = stage ? a_held.ps : a_held.vs;
			const bool writesEye = WritesEyePosition(tables.pipelines[a_pipeline].passDescriptor);
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

	void SceneStore::NameShading(std::uint32_t a_slot, bool a_member)
	{
		if (a_slot >= tables.objects.size() || a_slot >= tables.objectGeometry.size() || (tables.objects[a_slot].flags & (kObjectNoBindings | kObjectFree)))
			return;
		auto* property = SlotProperty(a_slot);
		if (!property)
			return;
		auto& item = shadingNamed.emplace_back();
		item.property.reset(property);
		item.slot = a_slot;
		item.pass = tables.pipelines[tables.objects[a_slot].pipelineIndex].passDescriptor;
		item.member = a_member;
		item.actor = tables.actorWetness.Grouped(a_slot);
	}

	void SceneStore::NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots)
	{
		for (const void* key : a_keys) {
			const auto dependents = propertyDependents.find(key);
			if (dependents == propertyDependents.end())
				continue;
			for (auto* geometry : dependents->second) {
				const auto it = tracked.find(geometry);
				if (it == tracked.end() || it->second.slot == kNoObjectSlot || it->second.objectStamp != objectStamp)
					continue;
				for (const std::uint32_t slot : { it->second.slot, it->second.layerSlot }) {
					if (slot == kNoObjectSlot || slot >= tables.objects.size())
						continue;
					NameShading(slot, IsResidentSlot(slot));
					// The extras' static rows read the same property fields (ProjectedUV's parameters and colour).
					if ((tables.objects[slot].flags & (kObjectProjectedUV | kObjectLandBlend)) && WriteObjectExtras(slot))
						tables.NoteChange(slot, kChangeExtras);
					++shadingParity.resampled;
					if (a_slots)
						a_slots->push_back(slot);
				}
			}
		}
	}

	void SceneStore::NameShadingEvents(std::vector<std::uint32_t>* a_slots)
	{
		// The events: the controllers' writes of the emissive colour and multiplier and of material fields (MaterialSources), the
		// LOD fades and the alpha GetRenderPasses leaves on the property (an actor's fade among them; lodFadeEvents), and external
		// emittance's shared colour (emittanceEvents). Their dependents are sampled at the next frame's start: a value that changes
		// during this frame (a controller on the animation job, a cull's GetRenderPasses) is drawn from the next frame on, a frame
		// after the engine's own draw would show it (dclf-async-publication.md, "Accepted differences").
		lodFadeChanged.clear();
		DrainLodFadeEvents(lodFadeChanged);
		const std::size_t lodFades = lodFadeChanged.size();
		DrainEmittanceEvents(lodFadeChanged);
		shadingParity.emittanceEvents += lodFadeChanged.size() - lodFades;
		MaterialSources::DrainShadingChanges(lodFadeChanged);
		if (!shadingNamedAll || !lodFadeEventsInstalled) {
			// Every slot (the events are not installed: every walk).
			shadingNamedAll = true;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				NameShading(o, IsResidentSlot(o));
				if ((tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) && WriteObjectExtras(o))
					tables.NoteChange(o, kChangeExtras);
			}
			shadingParity.resampled += tables.objects.size();
			return;
		}
		std::sort(lodFadeChanged.begin(), lodFadeChanged.end());
		lodFadeChanged.erase(std::unique(lodFadeChanged.begin(), lodFadeChanged.end()), lodFadeChanged.end());
		shadingParity.lodFadeEvents += lodFadeChanged.size();
		NameShadingKeys(lodFadeChanged, a_slots);
	}

	std::vector<SceneStore::WetnessValue> SceneStore::CaptureWetness()
	{
		// Advanced Skin's wetness, per actor-owned object. Skin::GetWetness keeps each actor's fading state and computes it once a
		// frame (the first call; later ones, including its own SetupGeometry hook for the draws the engine still makes this frame,
		// return the same value), so calling it here for every actor in the tables advances every actor's fade once a frame,
		// whether or not the engine draws it. Its cache is not thread-safe: the render thread's, at the frame's start.
		ZoneScopedN("CS.DCLF.Capture.Wetness");
		std::vector<WetnessValue> values;
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		const auto wetnessStats = tables.actorWetness.Update([&](std::uint32_t o) {
			auto* geometry = o < tables.objectGeometry.size() ? tables.objectGeometry[o] : nullptr;
			const float4 value = wetness && geometry ? skin.GetWetness(geometry) : float4{};
			return ActorValueIndex::Value{ value.x, value.y, value.z, value.w };
		}, [&](std::uint32_t o, const auto& row) { values.push_back({ o, row }); });
		TracyPlot("CS.DCLF.Wetness.Actors", static_cast<std::int64_t>(wetnessStats.actors));
		TracyPlot("CS.DCLF.Wetness.ChangedActors", static_cast<std::int64_t>(wetnessStats.changed));
		TracyPlot("CS.DCLF.Wetness.VisitedMeshes", static_cast<std::int64_t>(wetnessStats.visited));
		return values;
	}

	void SceneStore::CheckShadingParity()
	{
		auto& sp = shadingParity;
		// A diagnostics frame: waits for the frame's values.
		const auto* rows = FrameValues::Get().ShadingIfDone(true);
		if (!rows || !lodFadeEventsInstalled)
			return;
		++sp.checks;
		// Named for the next frame: by the walk (its events) and the accumulate phase, this frame.
		std::vector<std::uint8_t> named(tables.objects.size(), 0);
		for (const auto& item : shadingNamed)
			if (item.slot < named.size())
				named[item.slot] = 1;
		std::vector<std::uint32_t> changed;
		for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size() && o < rows->size(); ++o) {
			if ((tables.objects[o].flags & (kObjectFree | kObjectNoBindings)) || !tables.objectGeometry[o])
				continue;
			const auto* property = static_cast<const RE::BSLightingShaderProperty*>(SlotProperty(o));
			if (!property)
				continue;
			++sp.slots;
			BindlessShading now = (*rows)[o];
			SampleShading(*property, tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor, IsResidentSlot(o), now);
			// The tree's wind is the row's as the slot was named (a member's until its tree's entry: T6b1a), not kept current: not compared.
			if (std::memcmp(&now, &(*rows)[o], offsetof(BindlessShading, treeParams)) != 0)
				changed.push_back(o);
		}
		// A write after the walk took the events (the animation job runs the controllers alongside the render thread; the culls'
		// GetRenderPasses) is not missed: its event is queued. Those events are taken now and their dependents named for the next
		// frame, so each changed slot is named, queued or missed.
		std::vector<const void*> late;
		DrainLodFadeEvents(late);
		DrainEmittanceEvents(late);
		MaterialSources::DrainShadingChanges(late);
		std::sort(late.begin(), late.end());
		late.erase(std::unique(late.begin(), late.end()), late.end());
		std::vector<std::uint32_t> covered;
		NameShadingKeys(late, &covered);
		std::sort(covered.begin(), covered.end());
		for (const std::uint32_t o : changed) {
			if (named[o]) {
				++sp.named;
				continue;
			}
			if (std::binary_search(covered.begin(), covered.end(), o)) {
				++sp.late;
				continue;
			}
			if (sp.missing++ == 0) {
				const auto* geometry = tables.objectGeometry[o];
				const auto& held = (*rows)[o];
				BindlessShading now = held;
				SampleShading(*static_cast<const RE::BSLightingShaderProperty*>(SlotProperty(o)), tables.pipelines[tables.objects[o].pipelineIndex].passDescriptor,
					IsResidentSlot(o), now);
				sp.first = fmt::format("slot {} '{}' (flags {:#x}): material data ({} {} {}) against ({} {} {}), emit ({} {} {}) against ({} {} {}), mult {} against {}",
					o, geometry->name.c_str() ? geometry->name.c_str() : "", tables.objects[o].flags, now.shading.materialData[0], now.shading.materialData[1],
					now.shading.materialData[2], held.shading.materialData[0], held.shading.materialData[1], held.shading.materialData[2], now.shading.emitColor[0],
					now.shading.emitColor[1], now.shading.emitColor[2], held.shading.emitColor[0], held.shading.emitColor[1], held.shading.emitColor[2], now.emissiveMult,
					held.emissiveMult);
			}
		}
		// The wetness, as the frame's start captured it (Skin::GetWetness's same-frame value: no second advance): a mesh named this
		// frame (joined, or its record rewritten) takes its value with the next frame's capture.
		auto& skin = globals::features::skin;
		const bool wetness = skin.loaded && skin.settings.EnableSkin;
		for (const auto o : tables.actorObjects) {
			if (o >= rows->size() || o >= named.size() || named[o] || !tables.objectGeometry[o])
				continue;
			++sp.wetness;
			const float4 value = wetness ? skin.GetWetness(tables.objectGeometry[o]) : float4{};
			const float expected[4] = { value.x, value.y, value.z, value.w };
			sp.wetnessDiffer += std::memcmp(expected, (*rows)[o].skinPerGeometry, sizeof(expected)) != 0;
		}
	}

	bool SceneStore::WriteObjectExtras(std::uint32_t a_object)
	{
		if (a_object >= tables.extraOffset.size() || tables.extraOffset[a_object] == kNoExtraRows || a_object >= tables.objectGeometry.size())
			return false;
		const auto& object = tables.objects[a_object];
		const auto* geometry = tables.objectGeometry[a_object];
		const auto* property = static_cast<const RE::BSLightingShaderProperty*>(SlotProperty(a_object));
		if (!geometry || !property || (object.flags & kObjectNoBindings))
			return false;
		// What of the rows is the object's alone (CompleteExtras adds the frame's): the land blend's material offset; how its
		// TextureProj is made, and a multi-index shape's own; the ProjectedUV parameters.
		std::array<float, kExtraRows * 4> rows{};
		if (object.flags & kObjectLandBlend) {
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(property->material);
			rows[kExtraRowLandBlend * 4 + 0] = material ? material->landBlendParams.red : 0.0f;
			rows[kExtraRowLandBlend * 4 + 1] = material ? material->landBlendParams.green : 0.0f;
		}
		if (object.flags & kObjectProjectedUV) {
			const auto* multiIndex = const_cast<RE::BSGeometry*>(geometry)->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape ?
			                             &static_cast<const RE::BSMultiIndexTriShape*>(geometry)->GetMultiIndexTrishapeRuntimeData() :
			                             nullptr;
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (!(object.flags & kObjectLandBlend))
				rows[kExtraRowLandBlend * 4] = multiIndex ? kTextureProjShape : technique == 1 ? kTextureProjProjection : kTextureProjWorld;
			// A multi-index shape's passes take the shape's own projection (SetupGeometry's ProjectedUV block, engine notes):
			// materialProjection's columns, as stored.
			if (multiIndex) {
				float* proj = rows.data() + kExtraRowTextureProj * 4;
				const auto& shapeProjection = multiIndex->materialProjection;
				for (std::uint32_t r = 0; r < 3; ++r) {
					proj[r * 4 + 0] = shapeProjection.m[0][r];
					proj[r * 4 + 1] = shapeProjection.m[1][r];
					proj[r * 4 + 2] = shapeProjection.m[2][r];
					proj[r * 4 + 3] = shapeProjection.m[3][r];
				}
			}
			// The pixel parameters (FUN_1414e00c0): the property's projectedUVParams folded by its w, its projectedUVColor (a
			// multi-index shape's materialParams, normalDampener and materialScale); the globals are the frame's (ExtrasFrame).
			const auto& params = multiIndex ? multiIndex->materialParams : property->projectedUVParams;
			const auto& colour = property->projectedUVColor;
			float* out = rows.data() + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params.alpha;
			out[0] = fade * params.red;
			out[1] = 0.0f;  // never written by the engine
			out[2] = params.blue;
			out[3] = fade * params.green + params.alpha;
			if (multiIndex) {
				out[4] = multiIndex->normalDampener;
				out[5] = multiIndex->materialScale;
			} else {
				out[4] = colour.red;
				out[5] = colour.green;
				out[6] = colour.blue;
				out[7] = colour.alpha;
			}
		}
		float* held = &tables.extraRows[std::size_t(tables.extraOffset[a_object]) * 4];
		if (std::memcmp(held, rows.data(), sizeof(rows)) == 0)
			return false;
		std::memcpy(held, rows.data(), sizeof(rows));
		tables.NoteExtrasBlock(tables.extraOffset[a_object]);
		return true;
	}

	bool SceneStore::ReferenceExtras(std::uint32_t a_object, float* a_out)
	{
		const auto& object = tables.objects[a_object];
		const auto* geometry = a_object < tables.objectGeometry.size() ? tables.objectGeometry[a_object] : nullptr;
		const auto* property = static_cast<const RE::BSLightingShaderProperty*>(SlotProperty(a_object));
		if (!geometry || !property || (object.flags & kObjectNoBindings))
			return false;
		std::fill_n(a_out, kExtraRows * 4, 0.0f);
		const auto eye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
		const auto frameInputs = SampleExtrasFrame();
		if (object.flags & kObjectLandBlend) {
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(property->material);
			float* land = a_out + kExtraRowLandBlend * 4;
			land[0] = material ? material->landBlendParams.red : 0.0f;
			land[1] = material ? material->landBlendParams.green : 0.0f;
			land[2] = frameInputs.landBlend[0] - geometry->world.translate.x;
			land[3] = frameInputs.landBlend[1] - geometry->world.translate.y;
		}
		if (object.flags & kObjectProjectedUV) {
			// The texture matrix as SetupGeometry builds it for a ProjectedUV pass (engine notes), through the engine's own routines:
			// for every technique but Envmap the projection multiplied onto the geometry's world matrix, converted the same way and with
			// posAdjust added back.
			using ToMatrix = void (*)(float*, const RE::NiTransform*);
			using Multiply = void* (*)(float*, const float*, const float*);
			static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(0x14aaf10) };
			static const REL::Relocation<Multiply> multiply{ REL::Offset(0x153d3c8) };
			const float* p = frameInputs.projection;
			float m[16];
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (technique == 1) {
				std::memcpy(m, p, sizeof(m));
			} else {
				float w[16];
				toMatrix(w, &geometry->world);
				w[12] += eye.x;
				w[13] += eye.y;
				w[14] += eye.z;
				multiply(m, w, p);
			}
			float* proj = a_out + kExtraRowTextureProj * 4;
			const auto* multiIndex = const_cast<RE::BSGeometry*>(geometry)->GetType().get() == RE::BSGeometry::Type::kMultiIndexTriShape ?
			                             &static_cast<const RE::BSMultiIndexTriShape*>(geometry)->GetMultiIndexTrishapeRuntimeData() :
			                             nullptr;
			for (std::uint32_t r = 0; r < 3; ++r)
				for (std::uint32_t c = 0; c < 4; ++c)
					proj[r * 4 + c] = multiIndex ? multiIndex->materialProjection.m[c][r] : m[c * 4 + r];
			const auto& params = multiIndex ? multiIndex->materialParams : property->projectedUVParams;
			const auto& colour = property->projectedUVColor;
			float* out = a_out + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params.alpha;
			out[0] = fade * params.red;
			out[2] = params.blue;
			out[3] = fade * params.green + params.alpha;
			if (multiIndex) {
				out[4] = multiIndex->normalDampener;
				out[5] = multiIndex->materialScale;
			} else {
				out[4] = colour.red;
				out[5] = colour.green;
				out[6] = colour.blue;
				out[7] = colour.alpha;
			}
			std::memcpy(out + 8, frameInputs.projectedGlobals, sizeof(frameInputs.projectedGlobals));
		}
		return true;
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
		// A record holds no view of the character light's t11 (the frame's: MaterialSources::ApplyFrameComponents).
		MaterialSources::StripFrameViews(a_record, a_pass);
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

	void SceneStore::RefreshLodTechniqueRanges()
	{
		const Tables& view = FrameView();
		HoldLodHighDetailRange();
		float range[4];
		LodHighDetailRange(range);
		const auto offset = LightingVSLayout().offset[kVSHighDetailRange];
		frameTables.SyncTechniques(view.techniqueKeys.size());
		for (std::size_t r = 0; r < view.techniqueKeys.size(); ++r) {
			const std::uint32_t technique = view.techniqueKeys[r] >> 1;  // TechniqueKey
			auto& row = frameTables.techniques[r];
			if ((technique != 9 && technique != 18) || !row.valid)
				continue;
			float* out = &row.value.vs.floats[offset];
			if (std::memcmp(out, range, sizeof(range)) == 0)
				continue;
			std::memcpy(out, range, sizeof(range));
			row.constantsVersion = frameTables.NextVersion();
			PostTechniqueConstants(static_cast<std::uint32_t>(r));
		}
	}

	std::uint32_t SceneStore::TechniqueRowFor(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t key = TechniqueKey(a_passDescriptor);
		const auto [it, fresh] = tables.techniqueRow.try_emplace(key, static_cast<std::uint32_t>(tables.techniqueKeys.size()));
		if (fresh) {
			tables.techniqueKeys.push_back(key);
			tables.techniqueConstants.emplace_back();
			tables.NoteConstantsWrite();
		}
		return it->second;
	}

	void SceneStore::PostPipelineConstants(std::uint32_t a_slot)
	{
		auto& post = pipelineConstantsPosted.emplace_back();
		post.slot = a_slot;
		post.key = frameTables.pipelineKeys[a_slot];
		post.binding = frameTables.pipelineBindings[a_slot];
		post.generation = frameTables.tablesGeneration;
		post.constants = frameTables.geometryConstants[a_slot];
		++constantsPostStats.pipelinesPosted;
	}

	void SceneStore::PostTechniqueConstants(std::uint32_t a_row)
	{
		const Tables& view = FrameView();
		if (a_row >= view.techniqueKeys.size())
			return;
		auto& post = techniqueConstantsPosted.emplace_back();
		post.row = a_row;
		post.key = view.techniqueKeys[a_row];
		post.generation = frameTables.tablesGeneration;
		post.value = frameTables.techniques[a_row].value;
		++constantsPostStats.techniquesPosted;
	}

	void SceneStore::PostMaterialRecord(std::uint32_t a_slot)
	{
		auto& post = materialPosted.emplace_back();
		post.slot = a_slot;
		post.generation = tablesGeneration;
		post.key = frameTables.materialKeys[a_slot];
		post.record = frameTables.materials[a_slot];
		++constantsPostStats.recordsPosted;
	}

	void SceneStore::NoteFrameFloats(std::uint32_t a_slot)
	{
		if (frameFloatsDirtyMark.size() <= a_slot)
			frameFloatsDirtyMark.resize(std::size_t(a_slot) + 1, 0);
		if (!std::exchange(frameFloatsDirtyMark[a_slot], std::uint8_t{ 1 }))
			frameFloatsDirty.push_back(a_slot);
	}

	void SceneStore::PostFrameFloats()
	{
		// The frame floats as the frame left them, once a slot: what the coordinator's record takes (CopyFrameComponents).
		auto& f = frameTables;
		for (const std::uint32_t slot : frameFloatsDirty) {
			frameFloatsDirtyMark[slot] = 0;
			if (slot >= f.materials.size() || !f.materialKeys[slot].first)
				continue;
			auto& post = materialPosted.emplace_back();
			post.slot = slot;
			post.generation = tablesGeneration;
			post.frameFloats = true;
			post.key = f.materialKeys[slot];
			post.record = f.materials[slot];
			++constantsPostStats.framesPosted;
		}
		frameFloatsDirty.clear();
	}

	void SceneStore::ApplyMaterialPosts()
	{
		// The coordinator's records (step 6e B), in the frame's order: a writer event's record whole, a slot's frame floats onto its
		// record. A slot that holds another key now (freed, keyed again) takes nothing: its new tenant's record is evaluated afresh.
		for (auto& post : materialInbox) {
			const std::uint32_t slot = post.slot;
			if (post.generation != tablesGeneration || slot >= tables.materials.size() || !tables.materialSlots.Alive(slot) || tables.materialSlotKey[slot] != post.key) {
				++constantsPostStats.materialsStale;
				continue;
			}
			auto& record = tables.materials[slot];
			if (post.frameFloats) {
				const MaterialRecord before = record;
				MaterialSources::CopyFrameComponents(post.record, record, post.key.second);
				if (record == before)
					continue;
				tables.materialFrameVersion[slot] = ++materialVersions;
				++constantsPostStats.framesApplied;
			} else {
				if (record == post.record)
					continue;
				record = post.record;
				tables.materialVersion[slot] = ++materialVersions;
				++constantsPostStats.recordsApplied;
			}
			tables.NoteMaterial(slot);
		}
		materialInbox.clear();
	}

	void SceneStore::ApplyConstantsPosts()
	{
		// The coordinator's tables: a post whose slot holds another key now (a slot reused, the tables made again) is dropped; the
		// render thread evaluates the new tenant when the frame's snapshot shows it.
		bool wrote = false;
		for (auto& post : pipelineConstantsInbox) {
			const std::uint32_t p = post.slot;
			if (post.generation != tablesGeneration || p >= tables.pipelines.size() || p >= tables.pipelineConstants.size() || !(tables.pipelines[p] == post.key) ||
				tables.pipelineBindingVersion[p] != post.binding) {
				++constantsPostStats.stale;
				continue;
			}
			auto& row = tables.pipelineConstants[p];
			row.constants = post.constants;
			row.key = post.key;
			row.binding = post.binding;
			row.version = tables.NextVersion();
			row.valid = true;
			wrote = true;
			++constantsPostStats.pipelinesApplied;
		}
		pipelineConstantsInbox.clear();
		auto sameFloats = [](const ConstantBlock& a, const ConstantBlock& b) { return std::memcmp(a.floats.data(), b.floats.data(), sizeof(a.floats)) == 0; };
		for (auto& post : techniqueConstantsInbox) {
			if (post.generation != tablesGeneration || post.row >= tables.techniqueKeys.size() || post.row >= tables.techniqueConstants.size() ||
				tables.techniqueKeys[post.row] != post.key) {
				++constantsPostStats.stale;
				continue;
			}
			auto& row = tables.techniqueConstants[post.row];
			const bool floats = !row.valid || !sameFloats(post.value.vs, row.value.vs) || !sameFloats(post.value.ps, row.value.ps);
			const bool binding = !row.valid || post.value.filterModes != row.value.filterModes || post.value.shadowMask != row.value.shadowMask ||
			                     post.value.shadowMaskTexture != row.value.shadowMaskTexture;
			if (!floats && !binding)
				continue;
			row.value = post.value;
			if (floats)
				row.constantsVersion = tables.NextVersion();
			if (binding)
				row.bindingVersion = tables.NextVersion();
			row.valid = true;
			wrote = true;
			++constantsPostStats.techniquesApplied;
		}
		techniqueConstantsInbox.clear();
		if (wrote)
			tables.NoteConstantsWrite();
	}

	void SceneStore::ValidateMaterialSlice()
	{
		const Tables& view = FrameView();
		// CS_DCLF_PERSISTENT_PARITY: a few records drawn this frame (every one on a parity frame), re-evaluated live and compared
		// outside their frame-sourced components. A difference is a material writer the events do not cover; it is reported, not
		// repaired, because repairing it here is what hid the missing events before.
		const auto& f = frameTables;
		if (!SwitchEnabled(Switch::PersistentParity) || f.materials.empty())
			return;
		auto& evaluator = ConstantEvaluator::Get();
		if (!evaluator.HasLightingShader())
			return;
		auto validate = [&](std::uint32_t slot) {
			if (slot >= f.materials.size())
				return;
			const auto key = f.materialKeys[slot];
			MaterialRecord live;
			if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
				return;
			++stats.materialsValidated;
			MaterialRecord served = f.materials[slot];
			MaterialSources::CopyFrameComponents(live, served, key.second);
			if (!(served == live))
				NoteStaleMaterial(slot, key, served, live);
		};
		if (ParityDue(frame, 23)) {
			Tables::ForEachBit(view.usedMaterialBits, [&](const std::uint32_t slot) {
				if (slot < view.materials.size() && view.materialSlots.Alive(slot))
					validate(slot);
			});
			return;
		}
		std::uint32_t looked = 0;
		for (std::uint32_t n = 0; n < kMaterialValidationsPerFrame * kMaterialValidationStride && looked < kMaterialValidationsPerFrame; ++n) {
			const std::uint32_t slot = materialValidationCursor++ % static_cast<std::uint32_t>(f.materials.size());
			if (!view.MaterialUsed(slot))
				continue;
			++looked;
			validate(slot);
		}
	}
}
