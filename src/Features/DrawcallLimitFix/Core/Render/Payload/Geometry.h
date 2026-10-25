#pragma once

// Per-object helpers of the builds (DCLF's pool) and the commits (render thread): drawn geometry, partitions, fade rows,
// sequences, the vertex stream's rows. Included by Internal.h, in its order.

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
	 * the payload holds every row of its publication (PayloadPools). A feature's scene resources say what theirs hold
	 * (Api::SceneResources::fit): the root states' rows among them (their family's rows; without the fade program no draw reads them).
	 */
	inline SceneFit SceneFitOf(const NamedVersions& a_sealed, const SceneBuffers& a_scene, std::uint64_t a_objectBuffers)
	{
		SceneFit fit;
		fit.objects = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_objectBuffers, UINT32_MAX));
		// The vertex stream's vertices are its scene resources' (Features/Faces' face positions: SceneFit::streamVertices).
		Extensions::FitSceneResources(a_sealed, a_scene.sceneResources, fit);
		return fit;
	}

	/**
	 * @brief Whether object a_object is within what the scene's buffers hold (SceneFit): its per-object rows, its table families' rows (its
	 * root's among them: the root states' family) and its vertex stream row's vertices (its record, geometry rows and extras are the
	 * payload's, which holds them all). One past them waits for their growth: no input names it, so no draw or culling test reads past a
	 * buffer.
	 */
	inline bool ObjectFits(const SceneTables& a_tables, std::uint32_t a_object, const SceneFit& a_fit)
	{
		if (a_object >= a_fit.objects)
			return false;
		const auto families = a_tables.families.All();
		for (std::size_t f = 0; f < families.size(); ++f)
			if (!families[f].RowFits(a_object, a_fit.familyRows[f]))
				return false;
		if (a_object < a_tables.streamRow.size() && a_tables.streamRow[a_object] != kNoStreamRow) {
			const std::uint32_t row = a_tables.streamRow[a_object];
			if (row < a_tables.streamRows.size() && std::uint64_t(a_tables.streamRows[row].first) + a_tables.streamRows[row].count > a_fit.streamVertices)
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
	 * @brief Each object's root slot, by object slot (the root states' family's objectRow: kNoFadeRoot none), read as data; empty
	 * without a producer. Per build or commit, never per object.
	 */
	inline std::span<const std::uint32_t> ObjectRootsOf(const SceneTables& a_tables)
	{
		const auto* family = RootFamilyOf(a_tables);
		return family ? std::span<const std::uint32_t>(family->objectRow) : std::span<const std::uint32_t>();
	}

	/**
	 * @brief An object's fade root slot (the root states' family's objectRow), ~0u for none: a shadow input's, whose BuildDraws drops the
	 * caster while a stood-in root fades (kFadeRootStoodIn). A member's root changes only with a noted change (the family's listing).
	 */
	inline std::uint32_t FadeRootOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		const auto* family = RootFamilyOf(a_tables);
		return family ? family->RowOf(a_object) : ~0u;
	}

	/**
	 * @brief A main input's fade row: its fade root's slot (FadeStateCS's state, which its members follow: a_objectRoots, ObjectRootsOf),
	 * a LOD skin's partitions per level (picked by that state's level), and for the distance test (kObjectFadeTest) its fade-out
	 * distance. The node's centre is its placement row's (BindlessPlacement::lodFadeNode, FrameValues); the slot and the
	 * distance only change with its membership and bindings.
	 */
	inline void SetFadeRow(DrawInput& a_input, std::span<const std::uint32_t> a_objectRoots, const SceneTables& a_tables, std::size_t a_object)
	{
		if (a_object < a_objectRoots.size())
			a_input.fadeRoot = a_objectRoots[a_object];
		a_input.lodPartitions = LodPartitionsOf(a_tables, static_cast<std::uint32_t>(a_object));
		if (!(a_input.flags & (kObjectFadeTest | kObjectHeightTest)) || a_object >= a_tables.fadeDistance.size() || a_object >= a_tables.hasFadeNode.size())
			return;
		if (!a_tables.hasFadeNode[a_object])
			return;  // no fade node: nothing to measure
		a_input.fadeDistance = a_tables.fadeDistance[a_object];
	}

	// A draw template's geometry half, from a slot (Coordinator builds the object's first the same way). The second
	// stream repeats the first unless the draw has one of its own (its vertex stream row: a face shape's positions), which a skin's
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
	 * @brief The geometry buffer's uploads: the slots a buffer at a_contentEpoch lacks, then the stream rows' draws after them.
	 * The bytes sent.
	 */
	template <class Emit>
	std::size_t EmitGeometryDraws(const GeometryDrawsOut& a_out, std::uint64_t a_contentEpoch, Emit&& a_emit, std::size_t a_rows = ~std::size_t(0))
	{
		// Clipped to the buffer's rows (a_rows): what is past them waits for the requirement that grows it.
		std::size_t bytes = a_out.slots.Emit(a_contentEpoch, a_emit, a_rows);
		if (!a_out.streamRows.empty() && a_out.SlotCount() < a_rows) {
			const std::size_t rows = std::min(a_out.streamRows.size(), a_rows - a_out.SlotCount());
			a_emit(a_out.streamRows.data(), rows * sizeof(GeometryDraw), a_out.SlotCount() * sizeof(GeometryDraw));
			bytes += rows * sizeof(GeometryDraw);
		}
		return bytes;
	}

	// ---- The vertex stream (Api::VertexStreamContribution: Features/Faces' face positions; SceneTables::streamRows). Each row has a
	// GeometryDraw after the geometry slots, whose vertex buffer view is the row's range of the epoch's stream buffer (a float4 per
	// vertex); an input names it as its second stream (DrawInput::streamIndex).

	// BSGraphics::VertexDesc: the position attribute's flag in the second stream (bit 54 + attribute 0).
	constexpr std::uint64_t kPositionInSecondStream = 1ull << 54;
	// A stream vertex: kStreamVertexBytes (Resources/GpuLayouts.h).

	// The GeometryDraw of object a_object's stream row, or ~0u: no row, or no stream buffer. AppendStreamRows writes them at these
	// indices, after the slots (the geometry table holds both: RequireSceneTables).
	inline std::uint32_t StreamGeometry(const SceneTables& a_tables, std::size_t a_object, std::uint64_t a_vertices)
	{
		if (!a_vertices || a_object >= a_tables.streamRow.size() || a_tables.streamRow[a_object] == kNoStreamRow)
			return ~0u;
		return static_cast<std::uint32_t>(a_tables.geometries.size() + a_tables.streamRow[a_object]);
	}

	// Whether object a_object draws from the stream (its entry class's: it waits for its row).
	inline bool IsStreamObject(const SceneTables& a_tables, std::size_t a_object)
	{
		return a_object < a_tables.streamRow.size() && a_tables.streamRow[a_object] != kNoStreamRow;
	}

	// After PackGeometryDraws: the stream rows' GeometryDraws, in streamRows order.
	inline void AppendStreamRows(const SceneTables& a_tables, std::uint64_t a_vertices, GeometryDrawsOut& a_geometries)
	{
		if (!a_vertices)
			return;
		auto& a_out = a_geometries.streamRows;
		for (const auto& row : a_tables.streamRows) {
			GeometryDraw draw{};
			// A free row (its object's record has none) is a draw nothing names.
			if (row.object == Api::VertexStreamRow::kNoObject) {
				draw.nextPartition = kNoPartition;
				a_out.push_back(draw);
				continue;
			}
			draw.vertexBufferAddress = a_vertices + std::uint64_t(row.first) * kStreamVertexBytes;
			draw.vertexBufferSize = row.count * kStreamVertexBytes;
			draw.vertexStride = kStreamVertexBytes;
			draw.nextPartition = kNoPartition;
			a_out.push_back(draw);
		}
	}

	// The second stream of a stream object's draw template.
	inline void SetSequenceStream(DrawSequence& a_sequence, const SceneTables& a_tables, std::size_t a_object, std::uint64_t a_vertices)
	{
		const auto& row = a_tables.streamRows[a_tables.streamRow[a_object]];
		a_sequence.streamBufferAddress = a_vertices + std::uint64_t(row.first) * kStreamVertexBytes;
		a_sequence.streamBufferSize = row.count * kStreamVertexBytes;
		a_sequence.streamStride = kStreamVertexBytes;
	}
}
