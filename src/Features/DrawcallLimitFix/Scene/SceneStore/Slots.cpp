#include "Internal.h"

namespace DCLF
{
	std::uint32_t SceneStore::AllocateGeometrySlot()
	{
		const auto allocation = tables.geometrySlots.Allocate();
		if (allocation.grown) {
			tables.GeometryColumns(kGrowColumn);
			tables.NoteGeometry(allocation.slot);
		}
		return allocation.slot;
	}

	std::uint32_t SceneStore::AllocatePipelineSlot()
	{
		const auto allocation = tables.pipelineSlots.Allocate();
		if (allocation.grown)
			tables.PipelineColumns(kGrowColumn);
		return allocation.slot;
	}

	std::uint32_t SceneStore::AllocateMaterialSlot()
	{
		const auto allocation = tables.materialSlots.Allocate();
		if (allocation.grown)
			tables.MaterialColumns(kGrowColumn);
		return allocation.slot;
	}

	void SceneStore::InvalidateVerdicts()
	{
		// Every cached negative, candidate verdict and positive derivation: they witness the object, not
		// the toggles, so a toggle that enters the classification leaves all of them stale at once.
		for (auto& [geometry, entry] : tracked) {
			entry.verdict.cached = false;
			entry.candidateFrame = 0;
			entry.derived.valid = false;
		}
		++tablesGeneration;
		fullEvaluation = true;
	}

	void SceneStore::ResetSlotTables()
	{
		residents.clear();
		residentPatches.clear();
		residentPos.clear();
		residentMaintenanceDirty = true;
		tables.Clear();
		ClearFaceRegions();
		// Drop material binding owners on world/device reset without permitting
		// an old worker snapshot to match a newly empty lookup generation.
		const auto lookupGeneration = lookups.generation;
		const auto shadowGeneration = lookups.shadowGeneration;
		const auto lookupVersion = lookups.versionCounter;
		lookups = Lookups{};
		lookups.generation = lookupGeneration + 1;
		lookups.shadowGeneration = shadowGeneration + 1;
		lookups.versionCounter = lookupVersion;
		for (auto& [geometry, entry] : tracked)
			entry.slot = kNoObjectSlot;
		geometryIndex.clear();
		pipelineIndex.clear();
		materialIndex.clear();
		materialDependents.clear();
		materialOwners.clear();
		bindQueue.clear();
		membershipWitness = ~0u;
		materialMember.clear();
		// The lookup import owners were reset above. The next shadow-index
		// reconstruction must publish additions even if an SRV address recurs.
		shadowTextureMembers.clear();
		shadowKeyMembers.clear();
		for (auto& members : occlusionKeyMembers)
			members.clear();
		shadowIndexNeedsRebuild = true;
		shadowDirtySlots.clear();
		++tablesGeneration;
		fullEvaluation = true;
		memberDecals.clear();
		memberDecalsChanged = true;
	}

	void SceneStore::CheckObjectSlots(bool a_resolveBuffers)
	{
		stats.slotViolations = 0;
		for (std::size_t o = 0; o < tables.objects.size(); ++o) {
			auto& object = tables.objects[o];
			if (object.flags & kObjectNoBindings)
				continue;
			const char* what = nullptr;
			if (!tables.geometrySlots.Alive(object.geometryIndex))
				what = "geometry slot free";
			else if (a_resolveBuffers && (!tables.geometries[object.geometryIndex].vertexAddress || !tables.geometries[object.geometryIndex].indexAddress))
				what = "geometry slot unresolved";
			else if (object.pipelineIndex >= tables.pipelines.size() || !tables.PipelineUsed(object.pipelineIndex))
				what = "pipeline slot not a member's";
			else if (object.materialIndex >= tables.materials.size() || !tables.MaterialUsed(object.materialIndex))
				what = "material slot not a member's";
			else if (!tables.geometryTemplate[object.pipelineIndex])
				what = "pipeline without a template";
			if (!what)
				continue;
			if (stats.slotViolations++ == 0) {
				const auto* geometry = tables.objectGeometry[o];
				logger::error("[DCLF] slot check: object {} '{}' {} (geometry {} written {}, pipeline {} used {}, material {} used {}, member {}, frame {})",
					o, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", what,
					object.geometryIndex, object.geometryIndex < tables.geometryLastUsed.size() ? tables.geometryLastUsed[object.geometryIndex] : ~0u,
					object.pipelineIndex, tables.PipelineUsed(object.pipelineIndex), object.materialIndex, tables.MaterialUsed(object.materialIndex),
					IsResidentSlot(static_cast<std::uint32_t>(o)), frame);
			}
			// Neutralised: nothing downstream may draw from slots no member holds. Its record is no
			// longer the scene phase's, so the next delta walk writes it again.
			pendingEvaluation.push_back(tables.objectGeometry[o]);
			DropResidentSlot(static_cast<std::uint32_t>(o), false);
			object.flags = (object.flags & ~(kObjectMember | kObjectSunTest)) | kObjectNoBindings;
			object.geometryIndex = object.pipelineIndex = object.materialIndex = 0;
			tables.NoteChange(static_cast<std::uint32_t>(o), kChangeBindings | kChangeGeometry);
		}
	}

	void SceneStore::ProbeSlots(bool a_resolveBuffers)
	{
		static std::uint32_t logged = 0;
		if (logged >= 40)
			return;
		auto& evaluator = ConstantEvaluator::Get();
		auto& gpu = GpuResources::Get();
		std::uint32_t materialDiffers = 0, geometryDiffers = 0, materialsProbed = 0, geometriesProbed = 0;
		std::string first;
		if (evaluator.HasLightingShader()) {
			for (std::uint32_t slot = 0; slot < tables.materials.size(); ++slot) {
				if (!tables.MaterialUsed(slot))
					continue;
				const auto key = tables.materialSlotKey[slot];
				MaterialRecord live;
				if (!key.first || !evaluator.EvaluateMaterial(key.first, key.second, live))
					continue;
				++materialsProbed;
				const auto& served = tables.materials[slot];
				for (std::size_t t = 0; t < served.textures.size(); ++t) {
					if (served.textures[t] != live.textures[t]) {
						if (materialDiffers++ == 0)
							first = fmt::format("material slot {} (pass {:X}) texture[{}] {} -> {}", slot, key.second, t,
								static_cast<const void*>(served.textures[t]), static_cast<const void*>(live.textures[t]));
						break;
					}
				}
			}
		}
		if (a_resolveBuffers) {
			for (std::uint32_t slot = 0; slot < tables.geometries.size(); ++slot) {
				if (tables.geometryLastUsed[slot] != frame)
					continue;
				++geometriesProbed;
				const auto& record = tables.geometries[slot];
				const auto* triShape = tables.geometrySlotKey[slot];
				const auto vertexLease = gpu.Acquire(record.vertexBuffer);
				const auto indexLease = gpu.Acquire(record.indexBuffer);
				const auto* vertex = vertexLease ? &vertexLease->buffer : nullptr;
				const auto* index = indexLease ? &indexLease->buffer : nullptr;
				const bool same = triShape && vertex && index && vertex->address == record.vertexAddress && index->address == record.indexAddress &&
				                  reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer) == record.vertexBuffer &&
				                  reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer) == record.indexBuffer;
				if (!same && geometryDiffers++ == 0 && first.empty())
					first = fmt::format("geometry slot {} vb {:#x} -> {:#x} ib {:#x} -> {:#x} (trishape {} buffers {}/{} vs {}/{})", slot,
						record.vertexAddress, vertex ? vertex->address : 0ull, record.indexAddress, index ? index->address : 0ull,
						static_cast<const void*>(triShape), static_cast<const void*>(record.vertexBuffer), static_cast<const void*>(record.indexBuffer),
						triShape ? static_cast<const void*>(triShape->vertexBuffer) : nullptr, triShape ? static_cast<const void*>(triShape->indexBuffer) : nullptr);
			}
		}
		if (materialDiffers || geometryDiffers || frame < 12) {
			++logged;
			logger::info("[DCLF] slot probe frame {}: {} of {} used materials differ, {} of {} used geometries differ{}{}", frame, materialDiffers, materialsProbed,
				geometryDiffers, geometriesProbed, first.empty() ? "" : "; first: ", first);
		}
	}

	void SceneStore::ClearGeometrySlot(std::uint32_t a_slot)
	{
		auto& link = slotReferences.link;
		if (a_slot < link.size()) {
			tables.geometrySlots.Release(link[a_slot].slot, link[a_slot].generation);
			link[a_slot] = {};
		}
		geometryIndex.erase(tables.geometrySlotKey[a_slot]);
		tables.geometrySlotKey[a_slot] = nullptr;
		tables.geometryImports[a_slot] = {};
		tables.geometryLastUsed[a_slot] = Tables::kSlotFree;
	}

	void SceneStore::FreeGeometrySlot(std::uint32_t a_slot)
	{
		ClearGeometrySlot(a_slot);
		tables.geometrySlots.Free(a_slot);
	}

	void SceneStore::ClearMaterialSlot(std::uint32_t a_slot)
	{
		const auto key = tables.materialSlotKey[a_slot];
		materialIndex.erase(key);
		UnlistMaterialDependent(key.first, a_slot);
		materialOwners[a_slot].reset();
		tables.materialSlotKey[a_slot] = { nullptr, 0u };
		tables.UnmarkMaterial(a_slot);
		tables.retiredMaterialSlots.push_back(a_slot);
	}

	void SceneStore::UpdateSlotReferences()
	{
		auto& r = slotReferences;
		using Reference = SlotReferences::Reference;
		auto referenceTo = [](const SlotTable& a_table, std::uint32_t a_slot) {
			return a_table.Alive(a_slot) ? Reference{ a_slot, a_table.Generation(a_slot) } : Reference{};
		};
		auto move = [&](SlotTable& a_table, Reference& a_counted, const Reference& a_now) {
			if (a_counted == a_now)
				return;
			if (a_counted.slot != SlotReferences::kNone)
				a_table.Release(a_counted.slot, a_counted.generation);
			if (a_now.slot != SlotReferences::kNone)
				a_table.AddRef(a_now.slot, a_now.generation);
			a_counted = a_now;
		};
		// An object references its geometry while it has a record, and its pipeline and material while it is bound.
		auto countObject = [&](std::uint32_t o) {
			if (r.object.size() <= o)
				r.object.resize(std::size_t(o) + 1);
			auto& counted = r.object[o];
			const bool live = o < tables.objects.size() && !(tables.objects[o].flags & kObjectFree);
			const bool bound = live && !(tables.objects[o].flags & kObjectNoBindings);
			const auto& object = live ? tables.objects[o] : ObjectRecord{};
			move(tables.geometrySlots, counted.geometry, live ? referenceTo(tables.geometrySlots, object.geometryIndex) : Reference{});
			move(tables.pipelineSlots, counted.pipeline, bound ? referenceTo(tables.pipelineSlots, object.pipelineIndex) : Reference{});
			move(tables.materialSlots, counted.material, bound ? referenceTo(tables.materialSlots, object.materialIndex) : Reference{});
		};
		// A geometry slot references the next partition of its skin.
		auto countLink = [&](std::uint32_t g) {
			if (r.link.size() <= g)
				r.link.resize(std::size_t(g) + 1);
			const bool linked = tables.geometrySlots.Alive(g) && tables.geometries[g].nextPartition != kNoPartition;
			move(tables.geometrySlots, r.link[g], linked ? referenceTo(tables.geometrySlots, tables.geometries[g].nextPartition) : Reference{});
		};
		if (!r.objects.Continues(tables.changeLog, tablesGeneration) || !r.links.Continues(tables.geometryLog, tablesGeneration)) {
			// Every reference counted again: the first frame, new tables, or a log this reader fell behind.
			tables.geometrySlots.ResetReferences();
			tables.pipelineSlots.ResetReferences();
			tables.materialSlots.ResetReferences();
			r.object.assign(tables.objects.size(), {});
			r.link.assign(tables.geometries.size(), {});
			for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
				countObject(o);
			for (std::uint32_t g = 0; g < tables.geometries.size(); ++g)
				countLink(g);
			r.objects.Restart(tablesGeneration);
			r.links.Restart(tablesGeneration);
		} else {
			for (const auto& change : r.objects.Unread(tables.changeLog))
				countObject(change.slot);
			for (const std::uint32_t g : r.links.Unread(tables.geometryLog))
				countLink(g);
		}
		r.objects.Advance(tables.changeLog);
		r.links.Advance(tables.geometryLog);
	}

	void SceneStore::SweepSlots()
	{
		UpdateSlotReferences();
		// Every slot has explicit users (the object records, a skin's chain): its last reference going frees it, after the
		// whole journal batch, with no grace period.
		std::uint32_t freed = tables.geometrySlots.DrainUnreferenced([&](std::uint32_t a_slot) { ClearGeometrySlot(a_slot); });
		freed += tables.pipelineSlots.DrainUnreferenced([&](std::uint32_t a_slot) {
			pipelineIndex.erase(tables.pipelines[a_slot]);
			tables.geometryTemplate[a_slot] = nullptr;
			tables.geometryConstantsValid[a_slot] = 0;
			tables.UnmarkPipeline(a_slot);
			tables.retiredPipelineSlots.push_back(a_slot);
		});
		const bool slotParity = SwitchEnabled(Switch::PersistentParity);
		freed += tables.materialSlots.DrainUnreferenced([&](std::uint32_t a_slot) {
			// CS_DCLF_PERSISTENT_PARITY: a bound record still naming the slot is a reference the counts missed.
			if (slotParity)
				for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
					if (!(tables.objects[o].flags & (kObjectNoBindings | kObjectFree)) && tables.objects[o].materialIndex == a_slot) {
						static std::uint32_t logged = 0;
						if (logged++ < 10)
							logger::error("[DCLF] material slot {} drained while object {} '{}' (flags {:#x}, member {}) is bound to it, frame {}", a_slot, o,
								tables.objectGeometry[o] && tables.objectGeometry[o]->name.c_str() ? tables.objectGeometry[o]->name.c_str() : "", tables.objects[o].flags,
								IsResidentSlot(o), frame);
					}
			ClearMaterialSlot(a_slot);
			++stats.materialCacheEvicted;
			if (a_slot < materialMember.size() && std::exchange(materialMember[a_slot], 0))
				++stats.materialEvictedMember;
		});
		stats.slotsSwept += freed;
		if (freed)
			slotsFreedThisFrame = true;
	}

	std::uint32_t SceneStore::AcquireObjectSlot(Tracked& a_tracked, RE::BSGeometry* a_geometry)
	{
		if (denseWalk) {
			const auto slot = static_cast<std::uint32_t>(tables.objects.size());
			tables.GrowObjects(std::size_t(slot) + 1);
			tables.objectGeometry[slot] = a_geometry;
			tables.objectIdentity[slot] = a_tracked.identity;
			tables.objectGroup[slot] = a_tracked.groupIdentity;
			return slot;
		}
		if (a_tracked.slot != kNoObjectSlot && a_tracked.slot < tables.objectGeometry.size() && tables.objectGeometry[a_tracked.slot] == a_geometry &&
			tables.objectIdentity[a_tracked.slot] == a_tracked.identity && tables.objectGroup[a_tracked.slot] == a_tracked.groupIdentity)
			return a_tracked.slot;
		std::uint32_t slot;
		if (!tables.objectFree.empty()) {
			slot = tables.objectFree.back();
			tables.objectFree.pop_back();
		} else {
			slot = static_cast<std::uint32_t>(tables.objects.size());
			tables.GrowObjects(std::size_t(slot) + 1);
		}
		tables.objectGeometry[slot] = a_geometry;
		tables.objectIdentity[slot] = a_tracked.identity;
		tables.objectGroup[slot] = a_tracked.groupIdentity;
		a_tracked.slot = slot;
		return slot;
	}

	void SceneStore::ReleaseObjectSlot(Tracked& a_entry)
	{
		const auto slot = a_entry.slot;
		a_entry.slot = kNoObjectSlot;
		a_entry.objectStamp = 0;
		shadowSetsDirty = true;
		if (slot != kNoObjectSlot)
			shadowDirtySlots.push_back(slot);
		if (slot == kNoObjectSlot || slot >= tables.objects.size() || tables.objectGeometry[slot] != a_entry.geometry.get())
			return;
		if (IsResidentSlot(slot)) {
			DropResidentSlot(slot, false);
			++residentStats.released;
		}
		tables.ResetObject(slot);
		tables.objectFree.push_back(slot);
		if (tables.liveObjects)
			--tables.liveObjects;
	}

	void SceneStore::EraseTracked(RE::BSGeometry* a_geometry)
	{
		if (const auto it = tracked.find(a_geometry); it != tracked.end()) {
			ReleaseObjectSlot(it->second);
			MoveBucket(it->second, Ineligible::Count);
			UnlistFadeDependent(it->first, it->second);
			UnlistDependents(it->first, it->second, true);
			UnlistFaceShape(it->first, it->second);
			sceneIdentity.Detach(a_geometry);
			tracked.erase(it);
			++trackedLayout;
		}
	}

	void SceneStore::SweepObjectSlots()
	{
		// Consumers look objects up in the per-frame index lists by binary search (CaptureParity's actor check).
		std::sort(tables.actorObjects.begin(), tables.actorObjects.end());
		if (denseWalk) {
			tables.liveObjects = static_cast<std::uint32_t>(tables.objects.size());
			return;
		}
		// A slot this walk did not write belongs to an object that has no record this frame (ineligible now, or
		// no longer tracked): it is freed, and its Tracked entry, if it still exists, forgets it. The pointer is
		// only a key here; the geometry may already be gone.
		for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot) {
			auto* geometry = tables.objectGeometry[slot];
			if (!geometry || tables.objectSeen[slot] == walkSerial)
				continue;
			if (const auto it = tracked.find(geometry); it != tracked.end() && it->second.slot == slot)
				it->second.slot = kNoObjectSlot;
			tables.ResetObject(slot);
			tables.objectFree.push_back(slot);
		}
		tables.liveObjects = static_cast<std::uint32_t>(tables.objects.size() - tables.objectFree.size());
	}
}
