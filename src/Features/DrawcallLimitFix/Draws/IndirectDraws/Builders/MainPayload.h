#pragma once

// The main rows and the main payload a build makes for a main epoch: written by the main build (DCLF's pool), read by the
// commit on the render thread. Included by Internal.h, in its order.

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
	/** @brief The main rows a row table's capture lays out (Resources::rowCaptures): immutable kept views, the producer's newest. */
	struct MainRowCaptures
	{
		std::atomic<std::shared_ptr<const KeptView<MaterialRow>>> materials;
		std::atomic<std::shared_ptr<const KeptView<PipelineRow>>> pipelines;
	};
	inline MaterialRowHeader& HeaderOf(MaterialRow& a_row) { return *reinterpret_cast<MaterialRowHeader*>(a_row.bytes.data() + kMaterialRowHeader); }
	inline PipelineRowHeader& HeaderOf(PipelineRow& a_row) { return *reinterpret_cast<PipelineRowHeader*>(a_row.bytes.data() + kPipelineRowHeader); }

	/**
	 * @brief The main pass's rows, kept across frames and shared by the Z-prepass and colour segments (one build makes both):
	 * a material row per material slot and a pipeline row per pipeline slot, each
	 * written again only when what it is written from changed (its key), and sent as what changed since the version the
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
	};

	/**
	 * @brief Rows a table at a_held lacks, each with its header's addresses made absolute (a_base plus the row's offset):
	 * a_emit(data, bytes, offset). a_patch(row, rowAddress) fixes one row's header. Rows past a_capacity wait for the table's
	 * growth: a captured version is filled with them, and the table then holds what it was filled with.
	 */
	template <class Row, class Patch, class Emit>
	std::size_t EmitMainRows(const KeptView<Row>& a_rows, std::uint64_t a_held, std::uint64_t a_base, std::uint64_t a_capacity, Patch&& a_patch, Emit&& a_emit)
	{
		if (!a_rows.elements)
			return 0;
		const auto& rows = *a_rows.elements;
		std::vector<Row> run;
		std::size_t sent = 0;
		a_rows.changes.ForEachRun(a_held, rows.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
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
	 * @brief A build's inputs against its input buffer's named version, which the revision producer required to hold them (the
	 * snapshot builder's input demand, Impl::InputDemand): past it is a defect, never data to drop.
	 */
	inline void CheckInputCapacity(std::size_t a_inputs, std::uint64_t a_capacity, const char* a_what)
	{
		if (a_inputs <= a_capacity)
			return;
		stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s build is past its tables: {} inputs of {}", a_what, a_inputs, a_capacity));
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
	/** @brief Every row of a_rows, laid out for a table at a_address (a row table's capture: SceneBufferSet). */
	template <class Row>
	std::vector<std::byte> CapturedMainRows(const KeptView<Row>& a_rows, std::uint64_t a_address)
	{
		std::vector<std::byte> bytes;
		if (!a_rows.elements)
			return bytes;
		const auto& rows = *a_rows.elements;
		bytes.resize(rows.size() * sizeof(Row));
		for (std::size_t i = 0; i < rows.size(); ++i) {
			Row row = rows[i];
			PatchRowAddresses(row, a_address + i * sizeof(Row));
			std::memcpy(bytes.data() + i * sizeof(Row), &row, sizeof(Row));
		}
		return bytes;
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
		// The resident region version the segment's input buffer holds (Resources::ResidentHeld): the region
		// uploads only the entries changed since that one.
		std::uint64_t residentUploaded = 0;
		TablesHeld tablesHeld;  // likewise the kept tables (SceneBuffers::Held)
		// The versions of the kept rows the tables hold (Resources::materialRowsFilled, pipelineRowsFilled): what the rows'
		// journals keep changes for.
		std::uint64_t materialRowsHeld = 0, pipelineRowsHeld = 0;
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
		// The rows (MainRows), kept across frames and shared by both segments: uploaded as the rows changed since the version
		// the tables hold.
		KeptView<MaterialRow> materialRows;
		KeptView<PipelineRow> pipelineRows;
		std::uint64_t materialRowsWritten = 0, pipelineRowsWritten = 0;
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
		// The segment's resident region (ResidentRegion): its inputs lead the input buffer, uploaded when residentVersion
		// is not the one the buffer holds (Resources::ResidentHeld); inputList follows them.
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
