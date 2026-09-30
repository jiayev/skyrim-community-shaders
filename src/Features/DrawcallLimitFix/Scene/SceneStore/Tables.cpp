#include "Internal.h"

namespace DCLF
{
	void SceneStore::Tables::GrowObjects(std::size_t a_count)
	{
		if (a_count <= objects.size())
			return;
		// A draw's object word carries the index below its local shadow lights (kObjectIndexMask).
		if (a_count > std::size_t(kObjectIndexMask) + 1)
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} object slots do not fit a draw's object word ({} at most)", a_count, kObjectIndexMask + 1u));
		objects.resize(a_count, FreeObjectRecord());
		objectGeometry.resize(a_count, nullptr);
		objectIdentity.resize(a_count, 0);
		objectGroup.resize(a_count, 0);
		shading.resize(a_count, ObjectShading{});
		emissiveMult.resize(a_count, 1.0f);
		lights.resize(a_count, ObjectLights{});
		treeAnim.resize(a_count, ObjectTreeAnim{});
		skinWetness.resize(a_count, std::array<float, 4>{});
		skinPartitions.resize(a_count, 0);
		draws.resize(a_count, DrawSequence{});
		boneOffset.resize(a_count, 0);
		boneRows.resize(a_count, 0);
		extraOffset.resize(a_count, kNoExtraRows);
		shadowTechnique.resize(a_count, 0);
		shadowReject.resize(a_count, 0);
		skyTechnique.resize(a_count, 0);
		sunEntry.resize(a_count, std::array<float, 4>{});
		lodFade.resize(a_count, std::array<float, 4>{ 0.0f, 0.0f, 0.0f, -1.0f });
		fadedOut.resize(a_count, 0);
		fadeDistance.resize(a_count, 0.0f);
		residentSlot.resize(a_count, 0);
		faceStream.resize(a_count, kNoFaceStream);
		shadowDiffuse.resize(a_count, nullptr);
		shadowMaterial.resize(a_count, nullptr);
		objectSeen.resize(a_count, 0);
		sceneFlags.resize(a_count, FreeObjectRecord().flags);
	}

	SceneStore::Tables::Columns SceneStore::Tables::ColumnsOf(std::uint32_t a_slot) const
	{
		Columns columns;
		if (a_slot >= objects.size())
			return columns;
		columns.object = objects[a_slot];
		columns.draw = draws[a_slot];
		columns.shading = shading[a_slot];
		columns.lights = lights[a_slot];
		columns.tree = treeAnim[a_slot];
		columns.wetness = skinWetness[a_slot];
		columns.sunEntry = sunEntry[a_slot];
		columns.lodFade = lodFade[a_slot];
		columns.extraOffset = extraOffset[a_slot];
		if (columns.extraOffset != kNoExtraRows && (std::size_t(columns.extraOffset) + kExtraRows) * 4 <= extraRows.size())
			std::memcpy(columns.extras.data(), &extraRows[std::size_t(columns.extraOffset) * 4], sizeof(columns.extras));
		columns.geometry = objectGeometry[a_slot];
		columns.identity = objectIdentity[a_slot];
		columns.groupIdentity = objectGroup[a_slot];
		columns.shadowDiffuse = shadowDiffuse[a_slot];
		columns.shadowMaterial = shadowMaterial[a_slot];
		columns.emissiveMult = emissiveMult[a_slot];
		columns.fadeDistance = fadeDistance[a_slot];
		columns.boneOffset = boneOffset[a_slot];
		columns.boneRows = boneRows[a_slot];
		columns.shadowTechnique = shadowTechnique[a_slot];
		columns.skyTechnique = skyTechnique[a_slot];
		columns.faceStream = faceStream[a_slot];
		columns.boneCapacity = (columns.boneRows || columns.extraOffset != kNoExtraRows) ? BoneCapacity() : 0u;
		columns.sceneFlags = sceneFlags[a_slot];
		columns.skinPartitions = skinPartitions[a_slot];
		columns.shadowReject = shadowReject[a_slot];
		columns.resident = residentSlot[a_slot];
		return columns;
	}

	std::uint32_t SceneStore::Tables::CausesBetween(const Columns& a, const Columns& b)
	{
		auto same = [](const auto& a_left, const auto& a_right) { return std::memcmp(&a_left, &a_right, sizeof(a_left)) == 0; };
		std::uint32_t causes = 0;
		const auto& x = a.object;
		const auto& y = b.object;
		if (!same(x.world, y.world) || !same(x.previousWorld, y.previousWorld) || !same(x.boundCenter, y.boundCenter) || !same(x.boundRadius, y.boundRadius) ||
			!same(a.sunEntry, b.sunEntry) || !same(a.lodFade, b.lodFade))
			causes |= kChangePlacement;
		if (x.flags != y.flags || x.materialIndex != y.materialIndex || x.pipelineIndex != y.pipelineIndex || a.draw.pipelineIndex != b.draw.pipelineIndex ||
			!same(a.fadeDistance, b.fadeDistance) || a.sceneFlags != b.sceneFlags)
			causes |= kChangeBindings;
		if (!same(a.shading, b.shading) || !same(a.emissiveMult, b.emissiveMult) || !same(a.wetness, b.wetness) || !same(a.lodFade, b.lodFade))
			causes |= kChangeShading;
		if (!same(a.lights, b.lights))
			causes |= kChangeLights;
		if (!same(a.tree, b.tree))
			causes |= kChangeTree;
		if (a.skinPartitions != b.skinPartitions || a.boneOffset != b.boneOffset || a.boneRows != b.boneRows)
			causes |= kChangeSkin;
		if (a.boneCapacity != b.boneCapacity)
			causes |= (a.boneRows || b.boneRows ? kChangeSkin : 0u) | (a.extraOffset != kNoExtraRows || b.extraOffset != kNoExtraRows ? kChangeExtras : 0u);
		if (a.extraOffset != b.extraOffset || !same(a.extras, b.extras))
			causes |= kChangeExtras;
		if (a.shadowTechnique != b.shadowTechnique || a.shadowReject != b.shadowReject || a.skyTechnique != b.skyTechnique || a.shadowDiffuse != b.shadowDiffuse ||
			a.shadowMaterial != b.shadowMaterial)
			causes |= kChangeShadow;
		// The draw template's geometry half: everything but the pipeline index, which is the bindings'.
		auto geometryHalf = [](DrawSequence a_draw) {
			a_draw.pipelineIndex = 0;
			return a_draw;
		};
		if (x.geometryIndex != y.geometryIndex || !same(geometryHalf(a.draw), geometryHalf(b.draw)) || a.faceStream != b.faceStream || a.geometry != b.geometry ||
			a.identity != b.identity)
			causes |= kChangeGeometry;
		// A recycled slot is a new object even if its numeric columns happen to match.
		if (a.identity != b.identity || a.groupIdentity != b.groupIdentity)
			causes |= kChangeAll;
		if (a.resident != b.resident)
			causes |= kChangeMembership;
		return causes;
	}

	std::uint32_t SceneStore::Tables::PlaceBones(std::uint32_t a_slot, std::uint32_t a_rows)
	{
		if (a_rows && boneRows[a_slot] == a_rows)
			return boneOffset[a_slot];
		FreeBones(a_slot);
		auto& free = boneFree[std::min<std::size_t>(a_rows / 3, boneFree.size() - 1)];
		std::uint32_t offset;
		if (!free.empty()) {
			offset = free.back();
			free.pop_back();
		} else {
			offset = boneTop;
			boneTop += a_rows;
			if (boneTop > BoneCapacity()) {
				const std::size_t grown = (std::size_t(boneTop) + kBoneGrowRows - 1) / kBoneGrowRows * kBoneGrowRows;
				bones.resize(grown * 4, 0.0f);
				previousBones.resize(grown * 4, 0.0f);
				// Every record's previous palette and extras are addressed past the capacity: all of them moved.
				for (std::uint32_t slot = 0; slot < objects.size(); ++slot)
					NoteChange(slot, (boneRows[slot] ? kChangeSkin : 0u) | (extraOffset[slot] != kNoExtraRows ? kChangeExtras : 0u));
			}
		}
		boneOffset[a_slot] = offset;
		boneRows[a_slot] = a_rows;
		return offset;
	}

	void SceneStore::Tables::FreeBones(std::uint32_t a_slot)
	{
		if (a_slot >= boneRows.size())
			return;
		if (boneRows[a_slot])
			boneFree[std::min<std::size_t>(boneRows[a_slot] / 3, boneFree.size() - 1)].push_back(boneOffset[a_slot]);
		boneOffset[a_slot] = 0;
		boneRows[a_slot] = 0;
	}

	void SceneStore::Tables::ResetObject(std::uint32_t a_slot)
	{
		actorWetness.Set(a_slot, 0, 0);
		const auto before = ColumnsOf(a_slot);
		FreeExtras(a_slot);
		FreeBones(a_slot);
		objects[a_slot] = FreeObjectRecord();
		objectGeometry[a_slot] = nullptr;
		objectIdentity[a_slot] = 0;
		objectGroup[a_slot] = 0;
		shading[a_slot] = ObjectShading{};
		emissiveMult[a_slot] = 1.0f;
		lights[a_slot] = ObjectLights{};
		treeAnim[a_slot] = ObjectTreeAnim{};
		skinWetness[a_slot] = {};
		skinPartitions[a_slot] = 0;
		draws[a_slot] = DrawSequence{};
		shadowTechnique[a_slot] = 0;
		shadowReject[a_slot] = 0;
		skyTechnique[a_slot] = 0;
		sunEntry[a_slot] = {};
		lodFade[a_slot] = { 0.0f, 0.0f, 0.0f, -1.0f };
		fadedOut[a_slot] = 0;
		fadeDistance[a_slot] = 0.0f;
		residentSlot[a_slot] = 0;
		faceStream[a_slot] = kNoFaceStream;
		shadowDiffuse[a_slot] = nullptr;
		shadowMaterial[a_slot] = nullptr;
		objectSeen[a_slot] = 0;
		sceneFlags[a_slot] = FreeObjectRecord().flags;
		NoteWrite(a_slot, before);
	}

	void SceneStore::Tables::ClearFrame(bool a_keepObjects)
	{
		if (!a_keepObjects) {
			actorWetness.Clear();
			objects.clear();
			objectGeometry.clear();
			objectIdentity.clear();
			objectGroup.clear();
			shading.clear();
			emissiveMult.clear();
			lights.clear();
			treeAnim.clear();
			skinWetness.clear();
			skinPartitions.clear();
			shadingWatch.clear();
			watched.Clear();
			draws.clear();
			boneOffset.clear();
			boneRows.clear();
			bones.clear();
			previousBones.clear();
			boneFree = {};
			boneTop = 0;
			extraOffset.clear();
			extraRows.clear();
			extraFree.clear();
			shadowTechnique.clear();
			shadowReject.clear();
			skyTechnique.clear();
			sunEntry.clear();
		lodFade.clear();
			fadedOut.clear();
			fadeDistance.clear();
			residentSlot.clear();
			// Every slot is gone: the log's readers read them all again.
			InvalidateChangeLog();
			faceStream.clear();
			shadowDiffuse.clear();
			shadowMaterial.clear();
			objectSeen.clear();
			sceneFlags.clear();
			objectFree.clear();
			liveObjects = 0;
		}
		// The per-frame lists: every walk refills them, and the slots' offsets into them are rewritten with them.
		actorObjects.clear();
		decalOrdinal.clear();
		decalCount = {};
		faceStreams.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
		skyKeysUsed.clear();
	}

	void SceneStore::Tables::Clear()
	{
		actorWetness.Clear();
		auto clear = [](auto& a_column, auto&&...) { a_column.clear(); };
		GeometryColumns(clear);
		PipelineColumns(clear);
		MaterialColumns(clear);
		geometrySlots.Clear();
		pipelineSlots.Clear();
		materialSlots.Clear();
		usedMaterials.clear();
		usedPipelines.clear();
		usedMaterialBits.clear();
		usedPipelineBits.clear();
		usedMaterialsFrame = usedPipelinesFrame = 0;
		materialTextureDirty.clear();
		materialTextureQueued.clear();
		materialTextureChanges.clear();
		retiredMaterialSlots.clear();
		retiredPipelineSlots.clear();
		shadowTextureChanges.clear();
		frameSignatures.clear();
		materialSignatureListed.clear();
		materialFramePending.clear();
		transformWatch.clear();
		transformWatchFrame.clear();
		objects.clear();
		objectGeometry.clear();
		objectIdentity.clear();
		objectGroup.clear();
		geometries.clear();
		geometryLog.Invalidate();
		pipelines.clear();
		materials.clear();
		materialVersion.clear();
		materialFrameVersion.clear();
		pipelineConstantsVersion.clear();
		pipelineBindingVersion.clear();
		shading.clear();
		emissiveMult.clear();
		lights.clear();
		treeAnim.clear();
		skinWetness.clear();
		actorObjects.clear();
		skinPartitions.clear();
		shadingWatch.clear();
		watched.Clear();
		geometryConstants.clear();
		geometryConstantsValid.clear();
		geometryTemplate.clear();
		geometryTemplateNative.clear();
		techniques.clear();
		techniqueRow.clear();
		permutations.clear();
		draws.clear();
		decalOrdinal.clear();
		decalCount = {};
		bones.clear();
		previousBones.clear();
		boneOffset.clear();
		boneRows.clear();
		boneFree = {};
		boneTop = 0;
		extraRows.clear();
		extraOffset.clear();
		extraFree.clear();
		shadowTechnique.clear();
		shadowReject.clear();
		skyTechnique.clear();
		sunEntry.clear();
		lodFade.clear();
		fadedOut.clear();
		fadeDistance.clear();
		residentSlot.clear();
		InvalidateChangeLog();
		faceStreams.clear();
		faceStream.clear();
		shadowDiffuse.clear();
		shadowMaterial.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
		skyKeysUsed.clear();
		objectSeen.clear();
		sceneFlags.clear();
		objectFree.clear();
		liveObjects = 0;
	}

	void SceneStore::Tables::MarkMaterialUsed(std::uint32_t a_slot, std::uint32_t a_frame)
	{
		if (usedMaterialsFrame != a_frame) {
			usedMaterials.clear();
			std::fill(usedMaterialBits.begin(), usedMaterialBits.end(), 0);
			usedMaterialsFrame = a_frame;
		}
		if (materialLastUsed[a_slot] != a_frame) {
			materialLastUsed[a_slot] = a_frame;
			usedMaterials.push_back(a_slot);
			if (usedMaterialBits.size() <= a_slot / 64)
				usedMaterialBits.resize(a_slot / 64 + 1, 0);
			usedMaterialBits[a_slot / 64] |= 1ull << (a_slot % 64);
		}
		if (a_slot < materialTextureDirty.size() && materialTextureDirty[a_slot] && !materialTextureQueued[a_slot]) {
			materialTextureQueued[a_slot] = 1;
			materialTextureChanges.push_back(a_slot);
		}
	}

	void SceneStore::Tables::MarkPipelineUsed(std::uint32_t a_slot, std::uint32_t a_frame)
	{
		if (usedPipelinesFrame != a_frame) {
			usedPipelines.clear();
			std::fill(usedPipelineBits.begin(), usedPipelineBits.end(), 0);
			usedPipelinesFrame = a_frame;
		}
		if (pipelineLastUsed[a_slot] != a_frame) {
			pipelineLastUsed[a_slot] = a_frame;
			usedPipelines.push_back(a_slot);
			if (usedPipelineBits.size() <= a_slot / 64)
				usedPipelineBits.resize(a_slot / 64 + 1, 0);
			usedPipelineBits[a_slot / 64] |= 1ull << (a_slot % 64);
		}
	}

	void SceneStore::Tables::MarkMaterialTextureChanged(std::uint32_t a_slot, std::uint32_t a_frame)
	{
		if (a_slot >= materialTextureDirty.size()) {
			materialTextureDirty.resize(a_slot + 1, 0);
			materialTextureQueued.resize(a_slot + 1, 0);
		}
		materialTextureDirty[a_slot] = 1;
		if (materialLastUsed[a_slot] == a_frame && !materialTextureQueued[a_slot]) {
			materialTextureQueued[a_slot] = 1;
			materialTextureChanges.push_back(a_slot);
		}
	}

	void SceneStore::Tables::TakeMaterialTextureChanges(std::vector<std::uint32_t>& a_out)
	{
		a_out.clear();
		a_out.swap(materialTextureChanges);
		for (const auto slot : a_out) {
			materialTextureQueued[slot] = 0;
			materialTextureDirty[slot] = 0;
		}
	}
}
