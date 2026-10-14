#pragma once

// Per-object helpers of the builds (DCLF's pool) and the commits (render thread): drawn geometry, partitions, fade rows,
// sequences, face streams. Included by Internal.h, in its order.

namespace DCLF::Draws
{
	/**
	 * @brief The geometry slots an object's draw writes a sequence for, in BuildDrawsCS's order: its one
	 * geometry, or for a skin of several partitions each partition its mask names, following the slots'
	 * nextPartition links from the first.
	 */
	template <class F>
	void ForEachDrawnGeometry(const SceneTables& a_tables, std::uint32_t a_firstSlot, std::uint32_t a_partitions, F&& a_draw);

	/**
	 * @brief What the versions a_sealed names of a_scene's buffers hold, with per-object buffers of a_objectBuffers objects (an epoch's
	 * visibility). A buffer the set does not name holds nothing. The payload's parts (records, geometry rows, extras) bound nothing:
	 * the payload holds every row of its publication (PayloadBuilder). Without the tree wind or fade programs no draw reads their rows
	 * (the records keep their own wind; no fade root is tested).
	 */
	inline SceneFit SceneFitOf(const NamedVersions& a_sealed, const SceneBuffers& a_scene, std::uint64_t a_objectBuffers)
	{
		const auto units = [&](const Versioned& a_buffer) { return static_cast<std::uint32_t>(std::min<std::uint64_t>(NamedUnits(a_sealed, a_buffer), UINT32_MAX)); };
		SceneFit fit;
		fit.objects = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_objectBuffers, UINT32_MAX));
		fit.faceVertices = units(a_scene.facePositions);
		if (a_scene.treeWind && a_scene.trees)
			fit.trees = std::min({ units(a_scene.trees), units(a_scene.treeWindRows[0]), units(a_scene.treeWindRows[1]) });
		if (a_scene.fadeState && a_scene.fadeRoots)
			fit.fadeRoots = std::min({ units(a_scene.fadeRoots), units(a_scene.fadeStatesOut[0]), units(a_scene.fadeStatesOut[1]) });
		return fit;
	}

	/**
	 * @brief Whether object a_object is within what the scene's buffers hold (SceneFit): its per-object rows, its tree and fade root
	 * rows and its face positions (its record, geometry rows and extras are the payload's, which holds them all). One past them waits
	 * for their growth: no input names it, so no draw or culling test reads past a buffer.
	 */
	inline bool ObjectFits(const SceneTables& a_tables, std::uint32_t a_object, const SceneFit& a_fit)
	{
		if (a_object >= a_fit.objects)
			return false;
		if (a_object < a_tables.objectTree.size() && a_tables.objectTree[a_object] < kNodelessTree && a_tables.objectTree[a_object] >= a_fit.trees)
			return false;
		if (a_object < a_tables.objectFadeRoot.size() && a_tables.objectFadeRoot[a_object] != kNoFadeRoot && a_tables.objectFadeRoot[a_object] >= a_fit.fadeRoots)
			return false;
		if (a_object < a_tables.faceStream.size() && a_tables.faceStream[a_object] != kNoFaceStream) {
			const std::uint32_t stream = a_tables.faceStream[a_object];
			if (stream < a_tables.faceStreams.size() && std::uint64_t(a_tables.faceStreams[stream].region) + a_tables.faceStreams[stream].vertexCount > a_fit.faceVertices)
				return false;
		}
		return true;
	}

	template <class F>
	void ForEachDrawnGeometry(const SceneTables& a_tables, std::uint32_t a_firstSlot, std::uint32_t a_partitions, F&& a_draw)
	{
		if (a_partitions == kNoPartitions)
			return;
		std::uint32_t slot = a_firstSlot;
		if (a_partitions & kPartitionChain) {
			for (std::uint32_t i = 0; i < (a_partitions & kPartitionChainCount) && slot < a_tables.geometries.size(); ++i) {
				a_draw(slot);
				slot = a_tables.geometries[slot].nextPartition;
			}
			return;
		}
		for (std::uint32_t i = 0; i < kMaxSkinPartitions && slot < a_tables.geometries.size(); ++i) {
			if (a_partitions == 0 || ((a_partitions >> i) & 1))
				a_draw(slot);
			if ((a_partitions >> (i + 1)) == 0)
				break;
			slot = a_tables.geometries[slot].nextPartition;
		}
	}

	inline std::uint32_t PartitionsOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		return a_object < a_tables.skinPartitions.size() ? a_tables.skinPartitions[a_object] : 0u;
	}

	/** @brief A LOD skin's partitions per LOD level (SceneTables::skinLodPartitions), 0 for any other object. */
	inline std::uint32_t LodPartitionsOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		return a_object < a_tables.skinLodPartitions.size() ? a_tables.skinLodPartitions[a_object] : 0u;
	}

	/**
	 * @brief An object's fade root slot (SceneTables::objectFadeRoot), ~0u for none: a shadow input's, whose BuildDraws drops the
	 * caster while a stood-in root fades (kFadeRootStoodIn). A member's root changes only with a noted change (ListFadeRoot).
	 */
	inline std::uint32_t FadeRootOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		return a_object < a_tables.objectFadeRoot.size() ? a_tables.objectFadeRoot[a_object] : ~0u;
	}

	/**
	 * @brief A main input's fade row: its fade root's slot (FadeStateCS's state, which its members follow), a LOD skin's
	 * partitions per level (picked by that state's level), and for the distance test (kObjectFadeTest) its fade-out
	 * distance. The node's centre is its placement row's (BindlessPlacement::lodFadeNode, FrameValues); the slot and the
	 * distance only change with its membership and bindings.
	 */
	inline void SetFadeRow(DrawInput& a_input, const SceneTables& a_tables, std::size_t a_object)
	{
		if (a_object < a_tables.objectFadeRoot.size())
			a_input.fadeRoot = a_tables.objectFadeRoot[a_object];
		a_input.lodPartitions = LodPartitionsOf(a_tables, static_cast<std::uint32_t>(a_object));
		if (!(a_input.flags & (kObjectFadeTest | kObjectHeightTest)) || a_object >= a_tables.fadeDistance.size() || a_object >= a_tables.hasFadeNode.size())
			return;
		if (!a_tables.hasFadeNode[a_object])
			return;  // no fade node: nothing to measure
		a_input.fadeDistance = a_tables.fadeDistance[a_object];
	}

	// A draw template's geometry half, from a slot (Coordinator builds the object's first the same way). The second
	// stream repeats the first unless the draw has one of its own (a face shape's positions), which a skin's
	// partitions share.
	inline void SetSequenceGeometry(DrawSequence& a_sequence, const GeometryBuffers& a_geometry, bool a_ownStream = false)
	{
		if (!a_ownStream) {
			a_sequence.streamBufferAddress = a_geometry.vertexAddress;
			a_sequence.streamBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
			a_sequence.streamStride = a_geometry.vertexStride;
		}
		a_sequence.vertexBufferAddress = a_geometry.vertexAddress;
		a_sequence.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
		a_sequence.vertexStride = a_geometry.vertexStride;
		a_sequence.indexBufferAddress = a_geometry.indexAddress;
		a_sequence.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.indexBytes, UINT32_MAX));
		a_sequence.indexCount = a_geometry.indexCount;
		a_sequence.firstIndex = a_geometry.firstIndex;
	}

	inline GeometryDraw PackGeometryDraw(const SceneTables& a_tables, std::uint32_t a_slot, std::size_t a_count)
	{
		const auto& geometry = a_tables.geometries[a_slot];
		return { geometry.vertexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.vertexBytes, UINT32_MAX)), geometry.vertexStride,
			geometry.indexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.indexBytes, UINT32_MAX)), geometry.indexCount, geometry.firstIndex,
			geometry.nextPartition < a_count ? geometry.nextPartition : kNoPartition };
	}

	/**
	 * @brief The build's geometry slots' draws: brought up to date from the geometry log in the store (its journal trimmed to
	 * a_oldestContentEpoch, the oldest a payload backing holds), or packed whole without one.
	 */
	void UpdateGeometryDraws(GeometryStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, GeometryDrawsOut& a_out);

	/**
	 * @brief The geometry buffer's uploads: the slots a buffer at a_contentEpoch lacks, then the face streams' draws after them.
	 * The bytes sent.
	 */
	template <class Emit>
	std::size_t EmitGeometryDraws(const GeometryDrawsOut& a_out, std::uint64_t a_contentEpoch, Emit&& a_emit, std::size_t a_rows = ~std::size_t(0))
	{
		// Clipped to the buffer's rows (a_rows): what is past them waits for the requirement that grows it.
		std::size_t bytes = a_out.slots.Emit(a_contentEpoch, a_emit, a_rows);
		if (!a_out.faces.empty() && a_out.SlotCount() < a_rows) {
			const std::size_t faces = std::min(a_out.faces.size(), a_rows - a_out.SlotCount());
			a_emit(a_out.faces.data(), faces * sizeof(GeometryDraw), a_out.SlotCount() * sizeof(GeometryDraw));
			bytes += faces * sizeof(GeometryDraw);
		}
		return bytes;
	}

	// ---- NPC face shapes' positions (FaceSnapshots, SceneTables::faceStreams). Each face stream has a
	// GeometryDraw after the geometry slots, whose vertex buffer view is the shape's region of the epoch's
	// positions buffer; an input names it as its second stream (DrawInput::streamIndex).

	// BSGraphics::VertexDesc: the position attribute's flag in the second stream (bit 54 + attribute 0).
	constexpr std::uint64_t kPositionInSecondStream = 1ull << 54;

	// The GeometryDraw of object a_object's positions, or ~0u: not a face shape, or no positions buffer. AppendFaceStreams
	// writes them at these indices, after the slots (the geometry table holds both: RequireSceneTables).
	inline std::uint32_t FaceStreamGeometry(const SceneTables& a_tables, std::size_t a_object, std::uint64_t a_positions)
	{
		if (!a_positions || a_object >= a_tables.faceStream.size() || a_tables.faceStream[a_object] == kNoFaceStream)
			return ~0u;
		return static_cast<std::uint32_t>(a_tables.geometries.size() + a_tables.faceStream[a_object]);
	}

	inline bool IsFaceObject(const SceneTables& a_tables, std::size_t a_object)
	{
		return a_object < a_tables.faceStream.size() && a_tables.faceStream[a_object] != kNoFaceStream;
	}

	// After PackGeometryDraws: the face streams' GeometryDraws, in faceStreams order.
	inline void AppendFaceStreams(const SceneTables& a_tables, std::uint64_t a_positions, GeometryDrawsOut& a_geometries)
	{
		if (!a_positions)
			return;
		auto& a_out = a_geometries.faces;
		for (const auto& stream : a_tables.faceStreams) {
			GeometryDraw draw{};
			// A free entry (its object's record has none) is a draw nothing names.
			if (stream.object == SceneTables::kNoFaceObject) {
				draw.nextPartition = kNoPartition;
				a_out.push_back(draw);
				continue;
			}
			draw.vertexBufferAddress = a_positions + std::uint64_t(stream.region) * 16;
			draw.vertexBufferSize = stream.vertexCount * 16;
			draw.vertexStride = 16;
			draw.nextPartition = kNoPartition;
			a_out.push_back(draw);
		}
	}

	// The second stream of a face object's draw template.
	inline void SetSequenceStream(DrawSequence& a_sequence, const SceneTables& a_tables, std::size_t a_object, std::uint64_t a_positions)
	{
		const auto& stream = a_tables.faceStreams[a_tables.faceStream[a_object]];
		a_sequence.streamBufferAddress = a_positions + std::uint64_t(stream.region) * 16;
		a_sequence.streamBufferSize = stream.vertexCount * 16;
		a_sequence.streamStride = 16;
	}

	// A commit's face uploads (render thread): a region at a time, and only when the head's snapshot is not the one the version
	// a_sealed (the adopted set) names holds (a version it did not fill holds none). The snapshot stays the walk's until its next walk,
	// which is after the commit. A region past that version's vertices waits for the requirement that grows it.
	template <class Uploads>
	std::uint32_t UploadFaceStreams(const NamedVersions& a_sealed, const std::vector<SceneTables::FaceStream>& a_streams, const Versioned& a_positions, FaceUploads& a_uploaded,
		Uploads& a_uploads)
	{
		const auto& version = a_sealed.Of(a_positions);
		if (!a_uploaded.version.Holds(version)) {
			a_uploaded.regions.clear();
			a_uploaded.version.Filled(version);
		}
		std::uint32_t count = 0;
		for (const auto& stream : a_streams) {
			if (stream.object == SceneTables::kNoFaceObject || std::uint64_t(stream.region) + stream.vertexCount > version.units)
				continue;
			auto& uploaded = a_uploaded.regions[stream.region];
			if (uploaded == stream.generation)
				continue;
			a_uploads(a_positions, stream.positions, std::size_t(stream.vertexCount) * 16, std::uint64_t(stream.region) * 16);
			uploaded = stream.generation;
			++count;
		}
		return count;
	}
}
