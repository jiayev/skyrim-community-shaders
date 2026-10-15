#pragma once

// The main rows and the main payload a build makes for a main epoch: written by the main build (DCLF's pool), read by the
// commit on the render thread. Included by Internal.h, in its order.

#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"

namespace DCLF::Draws
{
	/**
	 * @brief A main-pass row's bytes (DrawPipelines.h: kMaterialRowBytes, kPipelineRowBytes). Its header's addresses hold the
	 * blocks' offsets in the row until the upload adds the row's own address (EmitMainRows): a table that grows is sent again
	 * whole, with the new addresses.
	 */
	template <std::size_t Bytes>
	struct RowBytes
	{
		std::array<std::byte, Bytes> bytes{};
	};
	using MaterialRow = RowBytes<kMaterialRowBytes>;
	using PipelineRow = RowBytes<kPipelineRowBytes>;
	inline MaterialRowHeader& HeaderOf(MaterialRow& a_row) { return *reinterpret_cast<MaterialRowHeader*>(a_row.bytes.data() + kMaterialRowHeader); }
	inline PipelineRowHeader& HeaderOf(PipelineRow& a_row) { return *reinterpret_cast<PipelineRowHeader*>(a_row.bytes.data() + kPipelineRowHeader); }

	/**
	 * @brief The main pass's rows, kept across frames and shared by the Z-prepass and colour segments (one build makes both):
	 * a material row per material slot and a pipeline row per pipeline slot, each
	 * written again only when what it is written from changed (its key), and sent as what changed since the content epoch the
	 * table holds. What a pair of them can draw is checked per build (MainBuild::AssembleRecord), from what each row holds.
	 */
	struct MainRows
	{
		KeptArray<MaterialRow> material;
		KeptArray<PipelineRow> pipeline;
		struct MaterialState
		{
			std::array<std::uint32_t, 8> key{};
			bool written = false;
			bool texturesOk = false;  // every texture the material sets resolved (else deferred: waiting for them)
			bool deferred = false;
			std::uint32_t missingTexture = 0;
			std::uint32_t textures = 0;  // t0-t15 the row gives a descriptor (bit per register)
			std::uint32_t samplers = 0;  // s0-s15 likewise
			std::uint32_t features = 0;  // kFeatureMaterialRegisters likewise
			std::uint64_t tables = 0;    // the constant tables its blocks were packed with (a hash)
			bool blocksOk = false;       // both blocks fit their places
			// Where its VS block placed TexcoordOffset (the frame's, which a feature patches on the GPU from a payload product): the packed
			// dword, ~0 none, and the components the record wrote.
			std::uint32_t texcoordDword = ~0u;
			std::uint8_t texcoordMask = 0;
		};
		struct PipelineState
		{
			std::array<std::uint32_t, 8> key{};
			bool written = false;
			bool blocksOk = false;
			bool shadowMask = false, shadowMaskSampler = false;  // t14, s14 given
		};
		std::vector<MaterialState> materials;
		std::vector<PipelineState> pipelines;
		const void* identity = nullptr;
		std::uint32_t generation = 0;  // the tables generation its rows were written for
		bool active = false;
		// Since the last report.
		std::uint64_t builds = 0, materialsWritten = 0, pipelinesWritten = 0, resyncs = 0;
		// Material rows written again, by the key part that moved (WriteMaterialRow): record, frame values, lookup, shared lookups,
		// technique bindings, projected, constant tables (two words).
		std::array<std::uint64_t, 8> keyMoved{};
		std::atomic<std::uint32_t> busy{ 0 };
		// Per pipeline slot: its constant tables' hash (TablesHash) and the lookup entry it was taken from (the lookups' instance and the
		// entry's version, which moves with the tables), so a build hashes only the entries that changed.
		struct TablesHashOf
		{
			std::uint64_t instance = 0;
			std::uint32_t version = 0;
			std::uint64_t hash = 0;
		};
		std::vector<TablesHashOf> tablesHashes;
		// The region's pair work and the builder's stages (MainBuild::UpdateRegionPairs, RunAhead), since the last report (MainPairsReport).
		struct PairStats
		{
			std::uint64_t builds = 0;          // builds that updated the region's pairs
			std::uint64_t checked = 0;         // pairs the events named (the check list)
			std::uint64_t resolved = 0;        // of those, resolved again (their witness moved)
			std::uint64_t everyPair = 0;       // builds that checked every pair (frame witness moved, a log lost)
			std::uint64_t pipelinesMoved = 0;  // pipelines whose witness moved (their pairs checked)
			std::uint64_t fannedOut = 0, chunks = 0;  // builds that resolved on the pool, and their chunks
			std::uint64_t entriesRewritten = 0;       // region entries Pairs.Inputs rewrote
			std::uint64_t bundles = 0;                // owner bundles made again
			std::uint64_t resyncs = 0, resyncsFannedOut = 0;
			double resolveMs = 0, resolveMaxMs = 0, inputsMs = 0, bundleMs = 0;
			// RunAhead: the stages, and the builds whose stream views and main build overlapped on the pool.
			std::uint64_t aheads = 0, overlapped = 0;
			double streamsMs = 0, mainMs = 0, stagesMs = 0, shadowMs = 0, exclusionsMs = 0;
			// Parity observers (Policy::CheckBuilderPairs, Policy::CheckRowPacking).
			std::uint64_t parityChecks = 0, parityMismatches = 0, packingChecks = 0, packingConflicts = 0;
		} pairStats;
	};
	/** @brief The `[DCLF] main pairs` report line from a_rows.pairStats, which it resets (the snapshot builder's, with StoresReport). */
	std::string MainPairsReport(MainRows& a_rows);

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
		// The PerMaterial floats that are the frame's rather than the material's (Coordinator::
		// GetMaterialPatchedFloats / GetMaterialPatchedVSFloats, MaterialSources): the build cache leaves them
		// out of a pair's signature and repacks the group, so a drifting frame component or a scrolling
		// TexcoordOffset does not rebuild every pair every frame.
		std::vector<std::uint32_t> materialPatchedFloats;
		std::vector<std::uint32_t> materialPatchedVSFloats;
		ResourceAddresses addresses{};
		std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
		// What the region's, the kept tables' and the rows' journals keep changes back to: the oldest content epoch a backing of
		// the payload's pools holds of each (PayloadBuilder::OldestContentEpoch; ~0: none, every change may go).
		std::uint64_t inputsOldestContentEpoch = 0;
		TablesContentEpochs tablesOldestContentEpochs;
		std::uint64_t materialRowsOldestContentEpoch = 0, pipelineRowsOldestContentEpoch = 0;
	};

	struct MainPayload
	{
		MainInputs inputs;
		std::vector<std::shared_ptr<const void>> bindingOwners;  // exact roots used by this payload, including clean kept rows
		std::vector<DrawSequence> sequences;  // CPU templates of BuildDraws' output
		std::array<std::vector<DrawSequence>, kDecalGroups> decalTemplates;  // by group and slot
		std::vector<DrawInput> inputList;
		GeometryDrawsOut geometryDraws;
		std::vector<SceneTables::FaceStream> faceStreams;  // the tables', for the commit's uploads
		// The rows (MainRows), kept across frames and shared by both segments: uploaded as the rows changed since the content epoch
		// the tables hold.
		KeptView<MaterialRow> materialRows;
		KeptView<PipelineRow> pipelineRows;
		std::uint64_t materialRowsWritten = 0, pipelineRowsWritten = 0;
		// The features' payload products (Api::PayloadProduct, by registration), built at MainBuild::Finish.
		Extensions::PayloadProducts products;
		// The frame's stream views the build read (its geometry slots), held with the payload.
		std::shared_ptr<const StreamViews> streams;
		// The frame's textures (t16 and up) the drawn pipelines read, which only the commit can resolve (into the frame
		// record): the commit counts the ones it could not.
		std::array<std::uint64_t, 2> frameRegisters{};
		// Per object in the tables: what this build did with it - kObjectStateDrawable, kObjectStateDecal,
		// a Skip reason, or kObjectStateAbsent when it never reached the inputs (the set parity explains its mismatches
		// with this).
		std::vector<std::uint8_t> objectState;
		// The culling's ViewProj (the latch's, the eye folded in) when the commit culled: set parity tests the live bounds with it.
		std::array<float, 16> cullViewProj{};
		bool culled = false;
		std::array<std::uint32_t, kDecalGroups> decalCount{};
		// The build's stats, merged into IndirectDraws::Stats by the commit.
		std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipped{};
		std::array<std::uint32_t, 4> missingTextures{};
		std::uint32_t missingNext = 0;
		std::uint32_t missingVertexConstants = 0, missingPixelConstants = 0;
		std::uint32_t decalsDrawn = 0, shortBuffers = 0, deferredTextures = 0;
		std::uint32_t rowTableConflicts = 0;  // pairs whose pipeline's constant tables are not the ones its material row was packed with
		std::array<double, 7> partMs{};
		// The first draw past its buffers, for the commit to log with the geometry's name.
		struct ShortBuffer
		{
			std::uint32_t object = ~0u;
			std::uint64_t vertexNeeded = 0, indexNeeded = 0;
		} shortBuffer;
		// The segment's resident region (ResidentRegion): its inputs lead the payload's scene list (caught up from what its backing
		// holds); inputList follows them.
		KeptView<DrawInput> resident;
		std::uint32_t residentDraws = 0, residentPairs = 0, residentUndrawable = 0, residentResyncs = 0;
		std::uint32_t residentResyncReasons = 0;  // bit k: kResyncReasons' k
		std::uint32_t residentDecalRetakes = 0;   // builds whose decal groups' counts moved (no resync: UpdateRegionEntries)

		void Reset()
		{
			resident.Reset();
			residentDraws = residentPairs = residentUndrawable = residentResyncs = residentResyncReasons = residentDecalRetakes = 0;
			bindingOwners.clear();
			sequences.clear();
			inputList.clear();
			geometryDraws.Reset();
			streams.reset();
			faceStreams.clear();
			frameRegisters = {};
			objectState.clear();
			materialRows.Reset();
			pipelineRows.Reset();
			materialRowsWritten = pipelineRowsWritten = 0;
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
			rowTableConflicts = 0;
			partMs = {};
			shortBuffer = {};
		}
	};
}
