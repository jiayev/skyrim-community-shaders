#pragma once

// The main payload a build makes for a main epoch, with its rows (MainRows.h): written by the main build (DCLF's pool), read by the
// commit on the render thread. Included by Internal.h, in its order.

#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/MainRows.h"

#include <span>

namespace DCLF::Draws
{
	/**
	 * @brief Rows a table at a_contentEpoch lacks, each with its header's addresses made absolute (a_base plus the row's offset):
	 * a_emit(data, bytes, offset). a_patch(row, rowAddress) fixes one row's header. Rows past a_capacity wait for the table's
	 * growth: a captured version is filled with them, and the table then holds what it was filled with.
	 */
	template <class Row, class Patch, class Emit>
	std::size_t EmitMainRows(const KeptView<Row>& a_rows, std::uint64_t a_contentEpoch, std::uint64_t a_base, std::uint64_t a_capacity, Patch&& a_patch, Emit&& a_emit)
	{
		if (!a_rows.elements)
			return 0;
		const auto& rows = *a_rows.elements;
		std::vector<Row> run;
		std::size_t sent = 0;
		a_rows.changes.ForEachRun(a_contentEpoch, rows.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
			if (a_first >= a_capacity)
				return;
			a_count = std::min(a_count, a_capacity - a_first);
			run.assign(rows.begin() + static_cast<std::ptrdiff_t>(a_first), rows.begin() + static_cast<std::ptrdiff_t>(a_first + a_count));
			for (std::size_t i = 0; i < run.size(); ++i)
				a_patch(run[i], a_base + (a_first + i) * sizeof(Row));
			a_emit(run.data(), run.size() * sizeof(Row), static_cast<std::size_t>(a_first) * sizeof(Row));
			sent += run.size();
		});
		return sent;
	}
	/**
	 * @brief The parts a material row is journalled in (KeptArray::SetParts): its VS blocks, its PS blocks' two halves and its header,
	 * 256 bytes each. An animated material (an effect's TexcoordOffset, falloff or base colour, which the engine's controllers move every
	 * frame) moves one part, and a backing catches up that part alone.
	 */
	inline constexpr std::size_t kMaterialRowParts = 4;
	static_assert(kMaterialRowBytes % kMaterialRowParts == 0 && kMaterialRowVS % (kMaterialRowBytes / kMaterialRowParts) == 0 &&
				  kMaterialRowPS % (kMaterialRowBytes / kMaterialRowParts) == 0 && kMaterialRowHeader % (kMaterialRowBytes / kMaterialRowParts) == 0);

	/**
	 * @brief EmitMainRows for rows journalled by part (KeptArray::SetParts, a_parts a row): the parts a table at a_contentEpoch lacks, each
	 * cut from its row with the header's addresses made absolute (a_base plus the row's offset): a_emit(data, bytes, offset). Rows past
	 * a_capacity wait for the table's growth.
	 */
	template <class Row, class Patch, class Emit>
	std::size_t EmitMainRowParts(const KeptView<Row>& a_rows, std::size_t a_parts, std::uint64_t a_contentEpoch, std::uint64_t a_base, std::uint64_t a_capacity, Patch&& a_patch,
		Emit&& a_emit)
	{
		if (!a_rows.elements)
			return 0;
		const auto& rows = *a_rows.elements;
		const std::size_t partBytes = sizeof(Row) / a_parts;
		const std::uint64_t capacity = a_capacity * a_parts;
		std::vector<std::byte> run;
		std::size_t sent = 0;
		a_rows.changes.ForEachRun(a_contentEpoch, rows.size() * a_parts, [&](std::uint64_t a_first, std::uint64_t a_count) {
			if (a_first >= capacity)
				return;
			a_count = std::min(a_count, capacity - a_first);
			run.resize(static_cast<std::size_t>(a_count * partBytes));
			Row row;
			std::uint64_t patched = ~std::uint64_t{ 0 };
			for (std::uint64_t i = 0; i < a_count; ++i) {
				const std::uint64_t part = a_first + i, r = part / a_parts;
				if (r != patched) {
					row = rows[static_cast<std::size_t>(r)];
					a_patch(row, a_base + r * sizeof(Row));
					patched = r;
				}
				std::memcpy(run.data() + i * partBytes, reinterpret_cast<const std::byte*>(&row) + (part % a_parts) * partBytes, partBytes);
			}
			a_emit(run.data(), run.size(), static_cast<std::size_t>(a_first * partBytes));
			sent += static_cast<std::size_t>(a_count);
		});
		return sent;
	}
	inline void PatchRowAddresses(MaterialRow& a_row, std::uint64_t a_address)
	{
		auto& header = HeaderOf(a_row);
		header.vsMaterial += a_address;
		header.psMaterial += a_address;
	}
	inline void PatchRowAddresses(PipelineRow& a_row, std::uint64_t a_address)
	{
		auto& header = HeaderOf(a_row);
		for (auto* address : { &header.vsTechnique, &header.psTechnique, &header.vsGeometry, &header.psGeometry, &header.vsPermutation, &header.psPermutation })
			*address += a_address;
	}
	/** @brief An input's rows (BuildDrawsCS.hlsl, RowsOf): the pipeline slot in the high 12 bits, the material slot in the low 20. */
	constexpr std::uint32_t kRowPipelineShift = 20;
	constexpr std::uint32_t kRowMaterialMask = (1u << kRowPipelineShift) - 1;
	constexpr std::uint32_t RowsOf(std::uint32_t a_pipeline, std::uint32_t a_material) { return (a_pipeline << kRowPipelineShift) | a_material; }

	struct MainInputs
	{
		std::uint32_t frameNumber = 0;
		bool depthOnly = false;
		std::uint32_t vsFrameMask = 0, psFrameMask = 0;  // the frame slots the commit supplies
		std::array<std::uint32_t, kDecalGroups> decalCount{};
		ResourceAddresses addresses{};
		std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
		// What the region's, the kept tables' and the rows' journals keep changes back to: the oldest content epoch a backing of
		// the payload's pools holds of each (PayloadBuilder::OldestContentEpoch; ~0: none, every change may go).
		std::uint64_t inputsOldestContentEpoch = 0;
		TablesContentEpochs tablesOldestContentEpochs;
		std::uint64_t materialRowsOldestContentEpoch = 0, pipelineRowsOldestContentEpoch = 0;
	};

	/**
	 * @brief What a main build makes for both segments alike: one per build, shared by its segments' payloads (MainPayload::body),
	 * immutable once the build's Finish hands it over. Pooled by the build cache (BuildCache::bodies): a body no payload holds is reset
	 * and written again, its storage kept.
	 */
	struct MainBody
	{
		std::vector<std::shared_ptr<const void>> bindingOwners;  // exact roots used by this payload, including clean kept rows
		std::vector<DrawSequence> sequences;  // CPU templates of BuildDraws' output
		std::array<std::vector<DrawSequence>, kDecalGroups> decalTemplates;  // by group and slot
		std::vector<DrawInput> inputList;
		GeometryDrawsOut geometryDraws;
		// The tables' vertex stream rows, for the commit's uploads (empty without a stream buffer): the publication's tables' own, which
		// the draws holding this payload hold (ImplSnapshotBuilder::DrawPublication::tables), not a copy.
		std::span<const Api::VertexStreamRow> streamRows;
		// The rows (MainRows), kept across frames and shared by both segments: uploaded as the rows changed since the content epoch
		// the tables hold.
		KeptView<MaterialRow> materialRows;
		KeptView<PipelineRow> pipelineRows;
		// The features' payload products (Api::PayloadProduct, by registration), built at MainBuild::Finish.
		Extensions::PayloadProducts products;
		// The frame's stream views the build read (its geometry slots), held with the payload.
		std::shared_ptr<const StreamViews> streams;
		// Per object in the tables: what this build did with it - kObjectStateDrawable, kObjectStateDecal,
		// a Skip reason, or kObjectStateAbsent when it never reached the inputs (the set parity explains its mismatches
		// with this).
		std::vector<std::uint8_t> objectState;
		std::array<std::uint32_t, kDecalGroups> decalCount{};
		// The build's stats, merged into IndirectDraws::Stats by the commit.
		std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipped{};
		std::array<std::uint32_t, 4> missingTextures{};
		std::uint32_t missingNext = 0;
		std::uint32_t missingVertexConstants = 0, missingPixelConstants = 0;
		std::uint32_t decalsDrawn = 0, shortBuffers = 0, deferredTextures = 0;
		// The first draw past its buffers, for the commit to log with the geometry's name.
		struct ShortBuffer
		{
			std::uint32_t object = ~0u;
			std::uint64_t vertexNeeded = 0, indexNeeded = 0;
		} shortBuffer;
		// The resident region's draws (ResidentRegion), the inputs of which lead the payload's scene list (MainPayload::resident).
		std::uint32_t residentDraws = 0, residentPairs = 0, residentUndrawable = 0;

		void Reset()
		{
			residentDraws = residentPairs = residentUndrawable = 0;
			bindingOwners.clear();
			sequences.clear();
			inputList.clear();
			geometryDraws.Reset();
			streams.reset();
			streamRows = {};
			objectState.clear();
			materialRows.Reset();
			pipelineRows.Reset();
			for (auto& product : products)
				product.reset();
			for (auto& templates : decalTemplates)
				templates.clear();
			decalCount = {};
			skipped = {};
			missingTextures = {};
			missingNext = 0;
			missingVertexConstants = missingPixelConstants = 0;
			decalsDrawn = shortBuffers = deferredTextures = 0;
			shortBuffer = {};
		}
	};
	/** @brief The body of a payload no build wrote (MainPayload::Reset): empty, never written. */
	inline const std::shared_ptr<const MainBody>& EmptyMainBody()
	{
		static const std::shared_ptr<const MainBody> kEmpty = std::make_shared<MainBody>();
		return kEmpty;
	}

	/**
	 * @brief One segment's payload: its own inputs, frame registers and one-off counts, and the build's body both segments share (one
	 * build makes both: MainBuild::Finish).
	 */
	struct MainPayload
	{
		MainInputs inputs;
		std::shared_ptr<const MainBody> body = EmptyMainBody();  // never null
		// The frame's textures (t16 and up) the drawn pipelines read, which only the commit can resolve (into the frame
		// record): the commit counts the ones it could not. The depth segment's resolves none.
		std::array<std::uint64_t, 2> frameRegisters{};
		// The culling's ViewProj (the latch's, the eye folded in) when the commit culled: set parity tests the live bounds with it.
		std::array<float, 16> cullViewProj{};
		bool culled = false;
		// The build's one-off counts, the primary segment's alone (the other's are zero), merged into IndirectDraws::Stats by the commit.
		std::uint64_t materialRowsWritten = 0, pipelineRowsWritten = 0;
		std::uint32_t rowTableConflicts = 0;  // pairs whose pipeline's constant tables are not the ones its material row was packed with
		std::array<double, 7> partMs{};
		// The segment's resident region (ResidentRegion): its inputs lead the payload's scene list (caught up from what its backing
		// holds); the body's inputList follows them.
		KeptView<DrawInput> resident;
		std::uint32_t residentResyncs = 0;
		std::uint32_t residentResyncReasons = 0;  // bit k: kResyncReasons' k
		std::uint32_t residentDecalRetakes = 0;   // builds whose decal groups' counts moved (no resync: UpdateRegionEntries)

		void Reset()
		{
			body = EmptyMainBody();
			resident.Reset();
			residentResyncs = residentResyncReasons = residentDecalRetakes = 0;
			frameRegisters = {};
			materialRowsWritten = pipelineRowsWritten = 0;
			rowTableConflicts = 0;
			partMs = {};
		}
	};
}
