#include "Internal.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"

#include "Features/DrawcallLimitFix/Draws/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Scene/Ports/EngineShaders.h"

#include <cmath>

namespace DCLF::Scene
{
	void Coordinator::RefreshMaterialRecords()
	{
		// T6b2c step 7: the scene work, its frame's FrameGlobals bound, the captures drained (ApplyEvents): the written materials' records,
		// the frame components, the texture transforms, in that order (a rewrite keeps the slot's frame parts, which the two after bring
		// to this frame's sources). No engine read: the captures and the frame's sample are the inputs.
		ZoneScopedN("CS.DCLF.Scene.MaterialRecords");
		++materialRecordStats.passes;
		stats.materialWrites = stats.materialsRewritten = stats.materialsHeld = stats.frameMaterialSamples = 0;
		RewriteCapturedMaterials();
		RefreshMaterialSignatures();
		RefreshMaterialTransforms();
	}

	void Coordinator::RewriteCapturedMaterials()
	{
		auto& r = materialRecordStats;
		std::vector<const RE::BSShaderMaterial*> materials = std::exchange(materialsCaptured, {});
		const std::size_t captured = materials.size();
		materials.insert(materials.end(), materialRewritesPending.begin(), materialRewritesPending.end());
		materialRewritesPending.clear();
		if (materials.empty())
			return;
		std::sort(materials.begin(), materials.end());
		materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
		r.captured += captured;
		stats.materialWrites = static_cast<std::uint32_t>(captured);
		const auto& sources = FrameGlobals::Current().material;
		for (const auto* material : materials) {
			const auto dependents = materialDependents.find(material);
			if (dependents == materialDependents.end())
				continue;  // no slot holds it: a join makes its record from the capture
			const auto held = materialSnapshots.find(material);
			if (held == materialSnapshots.end() || !held->second.held)
				continue;  // let go: no slot has used it for kMaterialSnapshotFrames, and its next capture comes with its next write
			held->second.frame = sceneFrame;
			const auto& snapshot = held->second.held->snapshot;
			WatchMaterialTransforms(material, snapshot);
			bool retry = false;
			for (const std::uint32_t slot : dependents->second) {
				if (slot >= tables.materials.size() || !tables.materialSlots.Alive(slot) || tables.materialSlotKey[slot].first != material)
					continue;
				const std::uint32_t pass = tables.materialSlotKey[slot].second;
				MaterialRecord record;
				if (!MaterialPort::Evaluate(snapshot, pass, sources, record)) {
					// An input the frame's sources do not have yet (Advanced Skin's textures for a key it has not set up): again with a
					// later pass's, the slot keeping its record meanwhile. Else a class the port does not cover (counted).
					if (!MaterialPort::FeatureHooksCovered(snapshot, sources.feature))
						retry = true;
					else
						++r.uncovered;
					continue;
				}
				// A record holds no view of the character light's t11; its frame parts (the frame components, TexcoordOffset) are kept
				// by the signature's and the transform watch's writes: only the material's own values are compared and rewritten.
				MaterialSources::StripFrameViews(record, pass);
				MaterialSources::CopyFrameComponents(tables.materials[slot], record, pass);
				MaterialSources::KeepUnreadFloats(tables.materials[slot], record);
				NoteMaterialRecord(slot, snapshot);
				if (record == tables.materials[slot])
					continue;
				tables.materials[slot] = record;
				tables.materialVersion[slot] = ++materialVersions;
				tables.NoteMaterial(slot);
				++r.rewritten;
				++stats.materialsRewritten;
			}
			if (retry) {
				materialRewritesPending.push_back(material);
				++r.retried;
				++stats.materialsHeld;
			}
		}
	}

	void Coordinator::RefreshMaterialSignatures()
	{
		// The frame-sourced components (MaterialSources): the same for every record of a signature, so one port evaluation of the
		// signature's probe against the frame's sources gives them; written into every slot of the signature only when they moved
		// since the last application. A slot keyed since was made from the same sources (the joins evaluate with them).
		auto& r = materialRecordStats;
		const auto& sources = FrameGlobals::Current().material;
		auto keyed = [&](std::uint32_t a_slot, std::uint32_t a_signature) {
			return a_slot < tables.materials.size() && tables.materialSlots.Alive(a_slot) && tables.materialSlotKey[a_slot].first &&
			       MaterialSources::Signature(tables.materialSlotKey[a_slot].second) == a_signature;
		};
		for (auto& [signature, entry] : materialSignatures) {
			if (!entry.probeValid)
				continue;
			MaterialRecord live;
			if (!MaterialPort::Evaluate(entry.probe, entry.probePass, sources, live)) {
				// Not evaluable with these sources (an Advanced Skin key not set up yet): tried again next pass, unless a slot noted
				// meanwhile gives another probe.
				entry.probeFailed = true;
				continue;
			}
			entry.probeFailed = false;
			++r.samples;
			++stats.frameMaterialSamples;
			if (entry.appliedValid) {
				MaterialRecord probe = entry.applied;
				bool floatsChanged = false;
				if (!MaterialSources::ApplyFrameComponents(live, probe, entry.probePass, &floatsChanged) && !floatsChanged)
					continue;
			}
			entry.applied = live;
			entry.appliedValid = true;
			++r.applications;
			auto& slots = entry.slots;
			for (std::size_t i = 0; i < slots.size();) {
				const std::uint32_t slot = slots[i];
				if (!keyed(slot, signature)) {
					if (slot < materialSignatureListed.size() && materialSignatureListed[slot] == signature + 1)
						materialSignatureListed[slot] = 0;
					slots[i] = slots.back();
					slots.pop_back();
					continue;
				}
				bool floatsChanged = false;
				const bool recordChanged = MaterialSources::ApplyFrameComponents(live, tables.materials[slot], tables.materialSlotKey[slot].second, &floatsChanged);
				if (recordChanged)
					tables.materialVersion[slot] = ++materialVersions;
				if (floatsChanged)
					tables.materialFrameVersion[slot] = ++materialVersions;
				if (recordChanged || floatsChanged)
					tables.NoteMaterial(slot);
				++r.slotsApplied;
				++i;
			}
		}
	}

	void Coordinator::RefreshMaterialTransforms()
	{
		// TexcoordOffset: the frame reads one of a material's two buffers (FrameGlobals' textureTransformBuffer, flipped every frame by
		// Main::Update), and a controller's write captures it (MaterialSources). A material is watched from its capture or keying until two
		// frames have gone (T6b3d: frames, not passes - the buffer flips once a frame) and both buffers agree; meanwhile its slots take the
		// frame's buffer.
		auto& r = materialRecordStats;
		const std::uint32_t buffer = FrameGlobals::Current().material.vanilla.textureTransformBuffer & 1;
		r.transformsWatched += transformWatch.size();
		for (auto it = transformWatch.begin(); it != transformWatch.end();) {
			const auto* material = it->first;
			const auto& watch = it->second;
			const auto dependents = materialDependents.find(material);
			if (dependents == materialDependents.end()) {
				it = transformWatch.erase(it);
				continue;
			}
			for (const std::uint32_t slot : dependents->second) {
				if (slot >= tables.materials.size() || !tables.materialSlots.Alive(slot) || tables.materialSlotKey[slot].first != material)
					continue;
				if (MaterialSources::ApplyTextureTransform(watch.transforms, buffer, tables.materials[slot])) {
					tables.materialFrameVersion[slot] = ++materialVersions;
					tables.NoteMaterial(slot);
					++r.transformsWritten;
				}
			}
			if (sceneFrame - watch.frame > 2 && watch.transforms.Agree())
				it = transformWatch.erase(it);
			else
				++it;
		}
	}

	void Coordinator::NoteMaterialRecord(std::uint32_t a_slot, const MaterialPort::MaterialSnapshot& a_snapshot)
	{
		if (a_slot >= tables.materialSlotKey.size())
			return;
		const auto [material, pass] = tables.materialSlotKey[a_slot];
		if (!material)
			return;
		// T7d's Effect port: an effect record has no signature-sourced component (its pass descriptor's bits are not Lighting's), only its
		// texture transforms.
		if (a_slot < tables.materials.size() && tables.materials[a_slot].kind == kMaterialKindEffect) {
			WatchMaterialTransforms(material, a_snapshot);
			return;
		}
		const std::uint32_t signature = MaterialSources::Signature(pass);
		auto& entry = materialSignatures[signature];
		if (materialSignatureListed.size() <= a_slot)
			materialSignatureListed.resize(std::size_t(a_slot) + 1, 0);
		if (materialSignatureListed[a_slot] != signature + 1) {
			materialSignatureListed[a_slot] = signature + 1;
			entry.slots.push_back(a_slot);
		}
		if (!entry.probeValid || entry.probeFailed) {
			entry.probe = a_snapshot;
			entry.probePass = pass;
			entry.probeValid = true;
			entry.probeFailed = false;
		}
		WatchMaterialTransforms(material, a_snapshot);
	}

	void Coordinator::WatchMaterialTransforms(const RE::BSShaderMaterial* a_material, const MaterialPort::MaterialSnapshot& a_snapshot)
	{
		auto& watch = transformWatch[a_material];
		watch.transforms = MaterialSources::TextureTransformsOf(a_snapshot);
		watch.frame = sceneFrame;
	}

	void Coordinator::ResetMaterialRecords()
	{
		for (auto& item : materialSignatures) {
			item.second.slots.clear();
			item.second.appliedValid = false;
		}
		materialSignatureListed.clear();
		transformWatch.clear();
		materialRewritesPending.clear();
	}

	void Coordinator::ListMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (a_material)
			materialDependents[a_material].push_back(a_slot);
	}

	void Coordinator::UnlistMaterialDependent(const RE::BSShaderMaterial* a_material, std::uint32_t a_slot)
	{
		if (auto it = materialDependents.find(a_material); it != materialDependents.end()) {
			auto& slots = it->second;
			std::erase(slots, a_slot);
			if (slots.empty())
				materialDependents.erase(it);
		}
	}

	void Coordinator::NameShading(std::uint32_t a_slot, bool a_member)
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

	void Coordinator::NameShadingKeys(const std::vector<const void*>& a_keys, std::vector<std::uint32_t>* a_slots)
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
					++shadingStats.resampled;
					if (a_slots)
						a_slots->push_back(slot);
				}
			}
		}
	}

	void Coordinator::NameShadingEvents(std::vector<std::uint32_t>* a_slots)
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
		shadingStats.emittanceEvents += lodFadeChanged.size() - lodFades;
		MaterialWrites::DrainShadingChanges(lodFadeChanged);
		if (!shadingNamedAll || !lodFadeEventsInstalled) {
			// Every slot (the events are not installed: every walk).
			shadingNamedAll = true;
			for (std::uint32_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				NameShading(o, IsResidentSlot(o));
				if ((tables.objects[o].flags & (kObjectProjectedUV | kObjectLandBlend)) && WriteObjectExtras(o))
					tables.NoteChange(o, kChangeExtras);
			}
			shadingStats.resampled += tables.objects.size();
			return;
		}
		std::sort(lodFadeChanged.begin(), lodFadeChanged.end());
		lodFadeChanged.erase(std::unique(lodFadeChanged.begin(), lodFadeChanged.end()), lodFadeChanged.end());
		shadingStats.lodFadeEvents += lodFadeChanged.size();
		NameShadingKeys(lodFadeChanged, a_slots);
	}

	bool Coordinator::WriteObjectExtras(std::uint32_t a_object)
	{
		if (a_object >= tables.extraOffset.size() || tables.extraOffset[a_object] == kNoExtraRows || a_object >= tables.objectGeometry.size())
			return false;
		const auto& object = tables.objects[a_object];
		const auto* geometry = tables.objectGeometry[a_object];
		// The records (T6b1b): the slot's property's (its layer's for a layer slot) and the geometry's.
		const auto leaf = geometry ? mirror.Leaf(geometry) : SceneCapture::LeafView{};
		const auto* property = tables.IsLayer(a_object) ? leaf.layer : leaf.property;
		if (!geometry || !leaf || !property || (object.flags & kObjectNoBindings))
			return false;
		// What of the rows is the object's alone (CompleteExtras adds the frame's): the land blend's material offset; how its
		// TextureProj is made, and a multi-index shape's own; the ProjectedUV parameters.
		std::array<float, kExtraRows * 4> rows{};
		if (object.flags & kObjectLandBlend) {
			rows[kExtraRowLandBlend * 4 + 0] = property->landBlend[0];  // the landscape material's landBlendParams
			rows[kExtraRowLandBlend * 4 + 1] = property->landBlend[1];
		}
		if (object.flags & kObjectProjectedUV) {
			// A multi-index shape's parameters (GeometryRecord::multiParams: materialProjection, materialParams, materialScale,
			// normalDampener).
			const bool multiIndex = leaf.Type() == static_cast<std::uint8_t>(RE::BSGeometry::Type::kMultiIndexTriShape);
			const auto& m = leaf.geometry->multiParams;
			const std::uint32_t technique = (tables.pipelines[object.pipelineIndex].passDescriptor >> 24) & 0x3f;
			if (!(object.flags & kObjectLandBlend))
				rows[kExtraRowLandBlend * 4] = multiIndex ? kTextureProjShape : technique == 1 ? kTextureProjProjection : kTextureProjWorld;
			// A multi-index shape's passes take the shape's own projection (SetupGeometry's ProjectedUV block, engine notes):
			// materialProjection's columns, as stored.
			if (multiIndex) {
				float* proj = rows.data() + kExtraRowTextureProj * 4;
				for (std::uint32_t r = 0; r < 3; ++r) {
					proj[r * 4 + 0] = m[0 * 4 + r];  // materialProjection.m[column][row], as stored
					proj[r * 4 + 1] = m[1 * 4 + r];
					proj[r * 4 + 2] = m[2 * 4 + r];
					proj[r * 4 + 3] = m[3 * 4 + r];
				}
			}
			// The pixel parameters (FUN_1414e00c0): the property's projectedUVParams folded by its w, its projectedUVColor (a
			// multi-index shape's materialParams, normalDampener and materialScale); the globals are the frame's (ExtrasFrame).
			const std::array<float, 4> params = multiIndex ? std::array<float, 4>{ m[16], m[17], m[18], m[19] } :
			                                                 std::array<float, 4>{ property->projected[0], property->projected[1], property->projected[2], property->projected[3] };
			float* out = rows.data() + kExtraRowProjectedParams * 4;
			const float fade = 1.0f - params[3];
			out[0] = fade * params[0];
			out[1] = 0.0f;  // never written by the engine
			out[2] = params[2];
			out[3] = fade * params[1] + params[3];
			if (multiIndex) {
				out[4] = m[21];  // normalDampener
				out[5] = m[20];  // materialScale
			} else {
				out[4] = property->projected[4];  // projectedUVColor
				out[5] = property->projected[5];
				out[6] = property->projected[6];
				out[7] = property->projected[7];
			}
		}
		float* held = &tables.extraRows[std::size_t(tables.extraOffset[a_object]) * 4];
		if (std::memcmp(held, rows.data(), sizeof(rows)) == 0)
			return false;
		std::memcpy(held, rows.data(), sizeof(rows));
		tables.NoteExtrasBlock(tables.extraOffset[a_object]);
		return true;
	}

	void Coordinator::MaterialReference::reset(RE::BSShaderMaterial* a_material)
	{
		// BSIntrusiveRefCounted's count (+0x8), incremented as the engine takes a reference on a property's material
		// (FUN_1414ac820); a material being read off a live property has one already, so it cannot be at zero here.
		if (a_material)
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(reinterpret_cast<std::byte*>(a_material) + 0x8));
		if (material) {
			using Release = void(void*, RE::BSShaderMaterial*);
			static REL::Relocation<Release*> release{ REL::Offset(Engine::Address::kMaterialRelease) };
			static REL::Relocation<void**> manager{ REL::Offset(Engine::Address::kMaterialManager) };
			release(*manager, material);
		}
		material = a_material;
	}

	std::uint32_t Coordinator::TechniqueRowFor(std::uint32_t a_passDescriptor)
	{
		const std::uint32_t key = TechniqueKey(a_passDescriptor);
		const auto [it, fresh] = tables.techniqueRow.try_emplace(key, static_cast<std::uint32_t>(tables.techniqueKeys.size()));
		if (fresh) {
			tables.techniqueKeys.push_back(key);
			tables.techniqueConstants.emplace_back().passDescriptor = a_passDescriptor;
			// Evaluated now, from the frame's sample (the witness RefreshTechniqueRows took this pass): drawable with this publication.
			EvaluateTechniqueRow(it->second);
			tables.NoteConstantsWrite();
		}
		return it->second;
	}

	void Coordinator::RefreshTechniqueRows()
	{
		// T6b2c: the coordinator's technique rows follow its frame's sample (FrameGlobals, bound to the scene work). The witness is what
		// the rows read of it but the fog (SameTechniqueWitness) and SetupTechniqueDescriptor's bytes; while it holds, every row is
		// already this sample's. It moves with the loaded grid (terrain LOD's HighDetailRange), the shadow mask target, the INI clamps.
		const auto& g = FrameGlobals::Current();
		auto& w = techniqueWitness;
		if (w.valid && w.byte12 == g.techniqueByte12 && w.byte7 == g.techniqueByte7 && SameTechniqueWitness(w.inputs, g.technique))
			return;
		w.inputs = g.technique;
		w.byte12 = g.techniqueByte12;
		w.byte7 = g.techniqueByte7;
		w.valid = true;
		++constantsPostStats.techniqueInputsMoved;
		for (std::uint32_t r = 0; r < tables.techniqueConstants.size(); ++r)
			EvaluateTechniqueRow(r);
	}

	void Coordinator::EvaluateTechniqueRow(std::uint32_t a_row)
	{
		if (a_row >= tables.techniqueConstants.size())
			return;
		auto& row = tables.techniqueConstants[a_row];
		const auto& inputs = FrameGlobals::Current().technique;
		TechniqueConstants now;
		EvaluateTechnique(row.passDescriptor, inputs, now);
		++constantsPostStats.techniquesEvaluated;
		// The row keeps the fog it was made with (the draws read the frame's, FrameCapture::fog): the time of day versions no row.
		if (row.valid)
			KeepTechniqueFog(row.value, now);
		const bool floats = !row.valid || !SameTechniqueFloats(now, row.value);
		const bool binding = !row.valid || !SameTechniqueBindings(now, row.value);
		if (!floats && !binding)
			return;
		row.value = now;
		row.inputs = inputs;
		if (floats)
			row.constantsVersion = tables.NextVersion();
		if (binding)
			row.bindingVersion = tables.NextVersion();
		row.valid = true;
		tables.NoteConstantsWrite();
		++constantsPostStats.techniquesWritten;
	}

	void Coordinator::ApplyConstantsPosts()
	{
		// The coordinator's tables: a post whose slot holds another key now (a slot reused, the tables made again) is dropped; the
		// render thread evaluates the new tenant when the frame's snapshot shows it.
		bool wrote = false;
		channels.pipelineConstantsPosted.Drain([&](std::unique_ptr<PipelineConstantsPost>&& a_post) {
			if (!a_post)
				return;
			const auto& post = *a_post;
			const std::uint32_t p = post.slot;
			if (post.generation != tablesGeneration || p >= tables.pipelines.size() || p >= tables.pipelineConstants.size() || !(tables.pipelines[p] == post.key) ||
				tables.pipelineBindingVersion[p] != post.binding) {
				++constantsPostStats.stale;
				return;
			}
			auto& row = tables.pipelineConstants[p];
			row.constants = post.constants;
			row.key = post.key;
			row.binding = post.binding;
			row.version = tables.NextVersion();
			row.valid = true;
			wrote = true;
			++constantsPostStats.pipelinesApplied;
		});
		if (wrote)
			tables.NoteConstantsWrite();
	}
}
