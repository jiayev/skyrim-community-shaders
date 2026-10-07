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
		lights.resize(a_count, ObjectLights{});
		treeAnim.resize(a_count, ObjectTreeAnim{});
		skinPartitions.resize(a_count, 0);
		draws.resize(a_count, DrawSequence{});
		boneOffset.resize(a_count, 0);
		boneRows.resize(a_count, 0);
		extraOffset.resize(a_count, kNoExtraRows);
		shadowTechnique.resize(a_count, 0);
		shadowReject.resize(a_count, 0);
		for (auto& column : occlusionTechnique)
			column.resize(a_count, 0);
		sunEntryNode.resize(a_count, nullptr);
		hasFadeNode.resize(a_count, 0);
		fadeDistance.resize(a_count, 0.0f);
		residentSlot.resize(a_count, 0);
		faceStream.resize(a_count, kNoFaceStream);
		shadowDiffuse.resize(a_count, nullptr);
		shadowMaterial.resize(a_count, nullptr);
		objectSeen.resize(a_count, 0);
		sceneFlags.resize(a_count, FreeObjectRecord().flags);
		layerBase.resize(a_count, kNoObjectSlot);
		layerOf.resize(a_count, kNoObjectSlot);
	}

	SceneStore::Tables::Columns SceneStore::Tables::ColumnsOf(std::uint32_t a_slot) const
	{
		Columns columns;
		if (a_slot >= objects.size())
			return columns;
		columns.object = objects[a_slot];
		columns.draw = draws[a_slot];
		columns.lights = lights[a_slot];
		columns.tree = treeAnim[a_slot];
		columns.sunEntryNode = sunEntryNode[a_slot];
		columns.hasFadeNode = hasFadeNode[a_slot];
		columns.extraOffset = extraOffset[a_slot];
		if (columns.extraOffset != kNoExtraRows && (std::size_t(columns.extraOffset) + kExtraRows) * 4 <= extraRows.size())
			std::memcpy(columns.extras.data(), &extraRows[std::size_t(columns.extraOffset) * 4], sizeof(columns.extras));
		columns.geometry = objectGeometry[a_slot];
		columns.identity = objectIdentity[a_slot];
		columns.groupIdentity = objectGroup[a_slot];
		columns.shadowDiffuse = shadowDiffuse[a_slot];
		columns.shadowMaterial = shadowMaterial[a_slot];
		columns.fadeDistance = fadeDistance[a_slot];
		columns.boneOffset = boneOffset[a_slot];
		columns.boneRows = boneRows[a_slot];
		columns.shadowTechnique = shadowTechnique[a_slot];
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			columns.occlusionTechnique[v] = occlusionTechnique[v][a_slot];
		columns.faceStream = faceStream[a_slot];
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
		// The placement (and the sun entry node's bound, the fade node's centre) is FrameValues': no column of the tables.
		if (x.flags != y.flags || x.materialIndex != y.materialIndex || x.pipelineIndex != y.pipelineIndex || a.draw.pipelineIndex != b.draw.pipelineIndex ||
			!same(a.fadeDistance, b.fadeDistance) || a.sceneFlags != b.sceneFlags || a.hasFadeNode != b.hasFadeNode)
			causes |= kChangeBindings;
		if (!same(a.lights, b.lights))
			causes |= kChangeLights;
		if (!same(a.tree, b.tree))
			causes |= kChangeTree;
		if (a.skinPartitions != b.skinPartitions || a.boneOffset != b.boneOffset || a.boneRows != b.boneRows)
			causes |= kChangeSkin;
		if (a.extraOffset != b.extraOffset || !same(a.extras, b.extras))
			causes |= kChangeExtras;
		if (a.shadowTechnique != b.shadowTechnique || a.shadowReject != b.shadowReject || a.occlusionTechnique != b.occlusionTechnique || a.shadowDiffuse != b.shadowDiffuse ||
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
		if (x.pipelineIndex != y.pipelineIndex || a.draw.pipelineIndex != b.draw.pipelineIndex || x.geometryIndex != y.geometryIndex ||
			!same(geometryHalf(a.draw), geometryHalf(b.draw)) || a.skinPartitions != b.skinPartitions || a.shadowTechnique != b.shadowTechnique ||
			a.shadowReject != b.shadowReject || a.occlusionTechnique != b.occlusionTechnique)
			causes |= kChangeStructure;
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
			// FrameValues' palettes grow with it; no block moves.
			if (boneTop > boneCapacity)
				boneCapacity = (boneTop + kBoneGrowRows - 1) / kBoneGrowRows * kBoneGrowRows;
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
		// Out of the frame's set with its record: the set is applied once a frame (ApplySet), and a slot freed after it must draw
		// in no phase (the shadow builds read setPhases).
		if (a_slot < setPhases.size())
			setPhases[a_slot] = 0;
		objectIdentity[a_slot] = 0;
		objectGroup[a_slot] = 0;
		lights[a_slot] = ObjectLights{};
		treeAnim[a_slot] = ObjectTreeAnim{};
		skinPartitions[a_slot] = 0;
		draws[a_slot] = DrawSequence{};
		shadowTechnique[a_slot] = 0;
		shadowReject[a_slot] = 0;
		for (auto& column : occlusionTechnique)
			column[a_slot] = 0;
		sunEntryNode[a_slot] = nullptr;
		hasFadeNode[a_slot] = 0;
		fadeDistance[a_slot] = 0.0f;
		residentSlot[a_slot] = 0;
		ClearFaceStream(a_slot);
		shadowDiffuse[a_slot] = nullptr;
		shadowMaterial[a_slot] = nullptr;
		objectSeen[a_slot] = 0;
		sceneFlags[a_slot] = FreeObjectRecord().flags;
		// The link between a layer and its base goes with either.
		if (const auto base = layerBase[a_slot]; base < layerOf.size() && layerOf[base] == a_slot)
			layerOf[base] = kNoObjectSlot;
		if (const auto layer = layerOf[a_slot]; layer < layerBase.size() && layerBase[layer] == a_slot)
			layerBase[layer] = kNoObjectSlot;
		layerBase[a_slot] = kNoObjectSlot;
		layerOf[a_slot] = kNoObjectSlot;
		NoteWrite(a_slot, before);
	}

	void SceneStore::Tables::SetFaceStream(std::uint32_t a_slot, FaceStream a_stream)
	{
		a_stream.object = a_slot;
		auto& index = faceStream[a_slot];
		if (index == kNoFaceStream) {
			if (!faceStreamFree.empty()) {
				index = faceStreamFree.back();
				faceStreamFree.pop_back();
			} else {
				index = static_cast<std::uint32_t>(faceStreams.size());
				faceStreams.emplace_back();
			}
		} else if (const auto& held = faceStreams[index]; held.geometry != a_stream.geometry || held.region != a_stream.region) {
			faceStreamsReleased.emplace_back(held.geometry, held.region);
		}
		faceStreams[index] = std::move(a_stream);
	}

	void SceneStore::Tables::ClearFaceStream(std::uint32_t a_slot)
	{
		auto& index = faceStream[a_slot];
		if (index == kNoFaceStream)
			return;
		auto& stream = faceStreams[index];
		faceStreamsReleased.emplace_back(stream.geometry, stream.region);
		stream = {};
		faceStreamFree.push_back(index);
		index = kNoFaceStream;
	}

	void SceneStore::Tables::ClearFrame(bool a_keepObjects)
	{
		if (!a_keepObjects) {
			actorWetness.Clear();
			objects.clear();
			objectGeometry.clear();
			objectIdentity.clear();
			objectGroup.clear();
			lights.clear();
			treeAnim.clear();
			skinPartitions.clear();
			draws.clear();
			boneOffset.clear();
			boneRows.clear();
			boneFree = {};
			boneTop = 0;
			boneCapacity = 0;
			extraOffset.clear();
			extraRows.clear();
			extraFree.clear();
			shadowTechnique.clear();
			shadowReject.clear();
			for (auto& column : occlusionTechnique)
				column.clear();
			sunEntryNode.clear();
			hasFadeNode.clear();
			fadeDistance.clear();
			residentSlot.clear();
			// Every slot is gone: the log's readers read them all again.
			InvalidateChangeLog();
			for (const auto& stream : faceStreams)
				if (stream.object != kNoFaceObject)
					faceStreamsReleased.emplace_back(stream.geometry, stream.region);
			faceStreams.clear();
			faceStreamFree.clear();
			faceStream.clear();
			shadowDiffuse.clear();
			shadowMaterial.clear();
			objectSeen.clear();
			sceneFlags.clear();
			layerBase.clear();
			layerOf.clear();
			objectFree.clear();
			liveObjects = 0;
			// The decals' ordinals are the member decals' (SceneStore::OrderDecals), kept with the objects.
			decalOrdinal.clear();
			decalCount = {};
		}
		// The per-frame lists: every walk refills them, and the slots' offsets into them are rewritten with them.
		actorObjects.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
		for (auto& keys : occlusionKeysUsed)
			keys.clear();
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
		usedMaterialBits.clear();
		usedPipelineBits.clear();
		retiredMaterialSlots.clear();
		retiredPipelineSlots.clear();
		shadowTextureChanges.clear();
		objects.clear();
		setPhases.clear();
		objectGeometry.clear();
		objectIdentity.clear();
		objectGroup.clear();
		geometries.clear();
		geometryLog.Invalidate();
		pipelines.clear();
		materials.clear();
		materialVersion.clear();
		pipelineBindingVersion.clear();
		lights.clear();
		treeAnim.clear();
		trees.clear();
		treeRefs.clear();
		treeFree.clear();
		treeNode.clear();
		treeIndex.clear();
		objectTree.clear();
		treeObjects.clear();
		++treesVersion;
		++treeObjectsVersion;
		fadeRoots.clear();
		fadeRootRefs.clear();
		fadeRootFree.clear();
		fadeRootNode.clear();
		fadeRootIndex.clear();
		fadeRootSwitch.clear();
		objectFadeRoot.clear();
		fadeRootsJournal.Resync();
		actorObjects.clear();
		skinPartitions.clear();
		geometryTemplate.clear();
		techniqueKeys.clear();
		techniqueRow.clear();
		permutations.clear();
		draws.clear();
		decalOrdinal.clear();
		decalCount = {};
		boneOffset.clear();
		boneRows.clear();
		boneFree = {};
		boneTop = 0;
		boneCapacity = 0;
		extraRows.clear();
		extraOffset.clear();
		extraFree.clear();
		shadowTechnique.clear();
		shadowReject.clear();
		for (auto& column : occlusionTechnique)
			column.clear();
		sunEntryNode.clear();
		hasFadeNode.clear();
		fadeDistance.clear();
		residentSlot.clear();
		InvalidateChangeLog();
		faceStreams.clear();
		faceStreamFree.clear();
		faceStreamsReleased.clear();
		faceStream.clear();
		shadowDiffuse.clear();
		shadowMaterial.clear();
		shadowTextureSet.clear();
		shadowTextureSeen.clear();
		shadowKeysUsed.clear();
		for (auto& keys : occlusionKeysUsed)
			keys.clear();
		objectSeen.clear();
		sceneFlags.clear();
		layerBase.clear();
		layerOf.clear();
		objectFree.clear();
		liveObjects = 0;
	}

	void SceneStore::Tables::MarkMaterialUsed(std::uint32_t a_slot)
	{
		if (usedMaterialBits.size() <= a_slot / 64)
			usedMaterialBits.resize(a_slot / 64 + 1, 0);
		usedMaterialBits[a_slot / 64] |= 1ull << (a_slot % 64);
	}

	void SceneStore::Tables::MarkPipelineUsed(std::uint32_t a_slot)
	{
		if (usedPipelineBits.size() <= a_slot / 64)
			usedPipelineBits.resize(a_slot / 64 + 1, 0);
		usedPipelineBits[a_slot / 64] |= 1ull << (a_slot % 64);
	}
}
