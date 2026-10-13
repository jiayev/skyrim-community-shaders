// The main build (MainBuild.h): both main segments' payloads from a publication's tables and lookups (pipeline and material rows,
// the resident region, the object loop). Pure; run by the snapshot builder on DCLF's pool, one build at a time.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Draws
{
	namespace
	{
		using Skip = IndirectDraws::Skip;

		// Blocks shared by many objects.
		struct PipelineBlocks
		{
			std::uint32_t setIndex = Lookups::kNone;
			std::span<const std::uint8_t> vsTable, psTable;
			std::array<const Lookups::RegisterUsageBits*, 2> usage{};  // per segment (kAsyncColour, kAsyncZPrepass): its variant's
			std::uint32_t shadowMaskIndex = Lookups::kNone;
			std::uint64_t tables = 0;  // TablesHash of its constant tables
			bool rowOk = false;        // its pipeline row's blocks fit (MainRows)
		};

		/** @brief A pipeline's constant tables, hashed (FNV-1a): what a material row was packed with. */
		std::uint64_t TablesHash(std::span<const std::uint8_t> a_vs, std::span<const std::uint8_t> a_ps)
		{
			std::uint64_t hash = 14695981039346656037ull;
			auto mix = [&](std::span<const std::uint8_t> a_table) {
				hash = (hash ^ a_table.size()) * 1099511628211ull;
				for (const auto byte : a_table)
					hash = (hash ^ byte) * 1099511628211ull;
			};
			mix(a_vs);
			mix(a_ps);
			return hash;
		}

		/** @brief The projected texture a ProjectedUV pipeline binds at a_slot, or -1. */
		std::int32_t ProjectedSlot(bool a_projectedPipeline, std::uint32_t a_slot)
		{
			if (!a_projectedPipeline)
				return -1;
			for (std::size_t i = 0; i < Scene::FrameView::ProjectedTextures::kSlots.size(); ++i) {
				if (Scene::FrameView::ProjectedTextures::kSlots[i] == a_slot)
					return static_cast<std::int32_t>(i);
			}
			return -1;
		}

		/** @brief The segment built first: colour when built (its skips are the report's). */
		std::size_t PrimarySegment(const std::array<const MainInputs*, 2>& a_in) { return a_in[kAsyncColour] ? kAsyncColour : kAsyncZPrepass; }

		/**
		 * @brief One build of the main segments' payloads (BuildMainPayloads); Run calls the parts in order. Both segments draw the
		 * frame's DCLF set and nothing else. They read one list: each main candidate once, its view mask choosing the segments
		 * (MainMaskOf), drawable only when every built segment can draw its pair, so a member is drawn by both or neither. The
		 * other segment's payload is a copy of the primary's with its own inputs (Finish).
		 */
		class MainBuild final
		{
		public:
			MainBuild(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
				const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams) :
				segmentIn(a_in), segmentOut(a_out), primary(PrimarySegment(a_in)), in(*a_in[primary]), tables(a_tables), frame(a_frame), lookups(a_lookups),
				out(*a_out[primary]), rows(a_rows), cache(a_cache), streams(std::move(a_streams))
			{}

			void Run();

		private:
			const std::array<const MainInputs*, 2> segmentIn;  // per segment; none when not built
			const std::array<MainPayload*, 2> segmentOut;
			const std::size_t primary;
			const MainInputs& in;  // the primary segment's
			const SceneTables& tables;
			const FrameTables& frame;  // the frame's PerGeometry blocks
			const Lookups& lookups;
			MainPayload& out;  // the primary segment's
			MainRows& rows;
			BuildCache* cache;
			std::shared_ptr<const StreamViews> streams;  // the frame's, or none: the build packs its own geometry slots

			decltype(MainPayload::sequences)& sequences = out.sequences;
			decltype(MainPayload::inputList)& drawInputs = out.inputList;
			decltype(MainPayload::decalCount)& decalCount = out.decalCount;
			decltype(MainPayload::decalTemplates)& decalTemplates = out.decalTemplates;
			const bool colourBuilt = segmentIn[kAsyncColour] != nullptr;
			const std::uint32_t frameNumber = in.frameNumber;
			static constexpr auto renderFlags = GeometryPort::kMainPassRenderFlags;

			// The frame registers' blocks (FrameRegisters), per segment.
			std::array<std::array<std::uint64_t, kConstantBufferRegisters>, 2> frameVS{}, framePS{};
			std::uint64_t sharedLightBlock = 0, frameLightingBlock = 0;
			std::vector<PipelineBlocks> pipelineBlocks;
			// Per (material, pipeline) pair: its verdict this build.
			ankerl::unordered_dense::map<std::uint64_t, ResolvedBindings> resolvedBindings;
			std::uint32_t currentObject = ~0u;
			std::chrono::steady_clock::time_point partStart;
			// The resident region and its share of the buffers' capacity.
			ResidentRegion* region = nullptr;
			bool wholeScene = false;
			std::size_t regionInputs = 0, regionDraws = 0;

			void Reset();
			void BeginRows();
			void FrameRegisters();
			void PackPipelines();
			void BeginStates();
			void PrepareObjects();
			void Skipped(Skip a_reason);
			void Mark(std::size_t a_part);
			/** @brief The built segments as bits (1 << kAsyncColour, 1 << kAsyncZPrepass). */
			std::uint8_t Segments() const
			{
				return static_cast<std::uint8_t>((segmentIn[kAsyncColour] ? 1u << kAsyncColour : 0u) | (segmentIn[kAsyncZPrepass] ? 1u << kAsyncZPrepass : 0u));
			}
			// In the frame's set: the only objects either segment draws. First-person members (kSetFirstPerson, no kObjectMember)
			// are drawable, their input the first-person view's alone.
			bool InSet(std::uint32_t o) const
			{
				return o < tables.objects.size() && ((tables.objects[o].flags & kObjectMember) || (o < tables.setPhases.size() && (tables.setPhases[o] & kSetFirstPerson)));
			}

			// The pair's rows (RowsOf), checked once per build, or kNoRecord after Skipped() recorded why.
			std::uint32_t AssembleRecord(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks);
			/** @brief The pair's verdict: what its rows give against what its pipeline reads. */
			void ResolvePair(const ObjectRecord& object, const PipelineBlocks& blocks, ResolvedBindings& resolved);
			/** @brief Rewrites the pipeline's row when its sources changed. */
			void WritePipelineRow(std::uint32_t p, const PipelineBlocks& blocks);
			/** @brief Rewrites the material's row when its sources changed. */
			void WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks);

			void BuildResidentRegion();
			static std::uint64_t PairKeyOf(const ObjectRecord& a_object) { return (std::uint64_t(a_object.materialIndex) << 32) | a_object.pipelineIndex; }
			bool ResidentAt(std::uint32_t o) const { return o < tables.residentSlot.size() && tables.residentSlot[o] != 0; }
			// What the region can hold: a resident's (with the whole scene, anyone's) single-input record, plus with the whole
			// scene the depth segment's cull-only candidates.
			bool RegionEligible(std::uint32_t o) const;
			// The input the loop would write, drawable while in the set with its pair's record built.
			std::uint8_t RegionEntry(std::uint32_t o, DrawInput& a_input) const;
			void RegionAcquire(std::uint64_t a_key);
			void RegionRelease(std::uint64_t a_key);
			// An entry's draws, with the region's totals.
			void RegionSetDraws(std::uint32_t i, std::uint8_t a_draws);
			void RegionRemove(std::uint32_t o);
			/** @brief Writes entry i whole (the caller merged its main part). */
			void RegionWrite(std::uint32_t i, const DrawInput& a_entry);
			/** @brief A decal's blank input: BuildDrawsCS writes a zero-count draw; it names no row past the scene's buffers. */
			DrawInput DecalBlank(std::uint32_t o) const;
			void RegionUpsert(std::uint32_t o);
			void RegionTake(std::uint32_t o);
			/** @brief Updates the entries the change log named since the last build, or every slot on a resync. */
			void UpdateRegionEntries();
			/** @brief Updates entries whose pipeline's set index changed or whose pair's record became (un)buildable. */
			void UpdateRegionPairs();
			std::uint64_t PipelineWitness(std::uint32_t p) const;
			/** @brief Routes every touched slot to the region, the loop (loopList) or nobody, and notes candidate-only ones. */
			void RouteTouched();
			/** @brief Publishes the region's draws into the payload. */
			void PublishRegion();

			void RunObjectLoop();
			/** @brief One loop object: its draw input, and its sequence and record when drawn. */
			void LoopObject(std::uint32_t o);
			void Finish();
		};
	}

	void MainBuild::Run()
	{
		ZoneScopedN("CS.DCLF.BuildMainPayload");
		Reset();
		FrameRegisters();
		PackPipelines();
		BeginStates();
		partStart = std::chrono::steady_clock::now();
		PrepareObjects();
		BuildResidentRegion();
		RunObjectLoop();
		Finish();
	}

	void MainBuild::Reset()
	{
		TracyCZoneN(resetMainZone, "CS.DCLF.BuildMain.Reset", true);
		out.Reset();
		out.inputs = in;
		if (lookups.sharedBindingBlock)
			out.bindingOwners.push_back(lookups.sharedBindingBlock);
		BeginRows();
		TracyCZoneEnd(resetMainZone);
	}

	void MainBuild::BeginRows()
	{
		// The tables hold the version last sent; only what is written from here on is sent.
		rows.material.BeginBuild(in.materialRowsHeld);
		rows.pipeline.BeginBuild(in.pipelineRowsHeld);
		if (!rows.active || rows.identity != in.addresses.identity || rows.generation != in.tablesGeneration) {
			// New resources or tables (slots mean other things): every row again.
			rows.material.Clear();
			rows.pipeline.Clear();
			rows.materials.clear();
			rows.pipelines.clear();
			rows.active = true;
			rows.identity = in.addresses.identity;
			rows.generation = in.tablesGeneration;
			++rows.resyncs;
		}
		++rows.builds;
		if (rows.material.Size() < tables.materials.size())
			rows.material.Mutable().resize(tables.materials.size());
		if (rows.pipeline.Size() < tables.pipelines.size())
			rows.pipeline.Mutable().resize(tables.pipelines.size());
		rows.materials.resize(std::max(rows.materials.size(), tables.materials.size()));
		rows.pipelines.resize(std::max(rows.pipelines.size(), tables.pipelines.size()));
	}

	void MainBuild::FrameRegisters()
	{
		// A frame slot the commit does not supply stays zero, which the constants check rejects for a pipeline reading it.
		frameVS = {};
		framePS = {};
		for (std::size_t j = 0; j < segmentIn.size(); ++j) {
			if (!segmentIn[j])
				continue;
			const auto& segment = *segmentIn[j];
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((segment.vsFrameMask >> slot) & 1)
					frameVS[j][slot] = segment.addresses.frameConstants + FrameSlotOffset(false, slot);
				if ((segment.psFrameMask >> slot) & 1)
					framePS[j][slot] = segment.addresses.frameConstants + FrameSlotOffset(true, slot);
			}
		}
		sharedLightBlock = in.addresses.frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
		frameLightingBlock = in.addresses.frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
	}

	void MainBuild::PackPipelines()
	{
		pipelineBlocks.assign(tables.pipelines.size(), {});
		TracyCZoneN(pipelinesZone, "CS.DCLF.BuildMain.Pipelines", true);
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			auto& blocks = pipelineBlocks[p];
			// Only used pipeline slots get rows.
			if (!tables.PipelineUsed(p) || p >= lookups.pipelines.size())
				continue;
			const auto& entry = lookups.pipelines[p];
			if (entry.setIndex == Lookups::kNone || !(entry.key == tables.pipelines[p]))
				continue;
			// No published constants yet: no row (its members wait).
			if (!tables.PipelineConstantsCurrent(p) || !tables.TechniqueConstantsValid(p))
				continue;
			blocks.setIndex = entry.setIndex;
			blocks.vsTable = entry.vsTable;
			blocks.psTable = entry.psTable;
			blocks.usage[kAsyncColour] = &entry.usage[kColorVariant];
			blocks.usage[kAsyncZPrepass] = &entry.usage[kDepthVariant];
			blocks.shadowMaskIndex = entry.shadowMaskIndex;
			blocks.tables = TablesHash(blocks.vsTable, blocks.psTable);
			WritePipelineRow(static_cast<std::uint32_t>(p), blocks);
			blocks.rowOk = rows.pipelines[p].blocksOk;
		}
		TracyCZoneEnd(pipelinesZone);
	}

	void MainBuild::WritePipelineRow(std::uint32_t p, const PipelineBlocks& blocks)
	{
		// The row's sources: the pipeline's constants and permutation, its technique's constants and bindings, its lookup entry
		// and the shared lookups.
		auto& state = rows.pipelines[p];
		const auto& techniqueRow = tables.TechniqueOf(p);
		const std::uint64_t vertexLayout = tables.pipelines[p].vertexLayout;
		const std::array<std::uint32_t, 8> key{ tables.pipelineConstants[p].version, techniqueRow.constantsVersion,
			techniqueRow.bindingVersion, lookups.pipelines[p].version, lookups.sharedVersion, 1u, static_cast<std::uint32_t>(vertexLayout),
			static_cast<std::uint32_t>(vertexLayout >> 32) };
		if (state.written && state.key == key)
			return;
		const auto& technique = techniqueRow.value;
		PipelineRow row;
		bool ok = true;
		auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
						std::uint32_t a_offset, std::uint32_t a_room) {
			if (ConstantGroupSize(a_layout, a_table, a_variables, a_first) > a_room) {
				ok = false;
				return;
			}
			PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, std::span(row.bytes.data() + a_offset, a_room));
		};
		// An effect key's technique and PerGeometry blocks stay zero: its variants read the frame's (fog VS b13, camera planes PS
		// b13 c17: Effect.hlsl) or the object's (World, PropertyColor, alpha test reference).
		const bool effect = RasterEffect(tables.pipelines[p].rasterFlags);
		if (!effect) {
			pack(technique.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique], kPipelineRowTechniqueVS, kPipelineRowTechniqueBytes);
			pack(technique.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique], kPipelineRowTechniquePS, kPipelineRowTechniqueBytes);
			// The PerGeometry template: what objects do not override from their records.
			GeometryTemplate geometry;
			PackGeometryTemplate(tables.pipelineConstants[p].constants, blocks.vsTable, blocks.psTable, true, geometry);
			if (geometry.vs.size() > kPipelineRowGeometryVSBytes || geometry.ps.size() > kPipelineRowGeometryPSBytes)
				ok = false;
			else {
				std::memcpy(row.bytes.data() + kPipelineRowGeometryVS, geometry.vs.data(), geometry.vs.size());
				std::memcpy(row.bytes.data() + kPipelineRowGeometryPS, geometry.ps.data(), geometry.ps.size());
			}
		}
		// The permutation is per pipeline: its one per-object bit, SuppressExternalEmittance, is read only by Effect.hlsl's
		// GetLightingColor, never by Lighting.hlsl.
		const auto& permutation = tables.permutations[p];
		const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, permutation.extraShaderDescriptor,
			permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
		std::memcpy(row.bytes.data() + kPipelineRowPermutation, data, sizeof(data));
		auto& header = HeaderOf(row);
		header.vsTechnique = kPipelineRowTechniqueVS;
		header.psTechnique = kPipelineRowTechniquePS;
		header.vsGeometry = kPipelineRowGeometryVS;
		header.psGeometry = kPipelineRowGeometryPS;
		header.vsPermutation = header.psPermutation = kPipelineRowPermutation;
		// The technique's shadow mask (t14, s14), or the null texture where the technique binds none.
		const std::uint32_t shadowMask = technique.shadowMask ? blocks.shadowMaskIndex : lookups.nullTexture;
		header.shadowMask = shadowMask == Lookups::kNone ? 0u : shadowMask;
		const std::uint32_t filter = technique.shadowMask && technique.filterModes[kShadowMaskSlot] != kUnwrittenFilterMode ? technique.filterModes[kShadowMaskSlot] : 0u;
		const std::uint32_t sampler = lookups.Sampler(0, filter);
		header.shadowMaskSampler = sampler == Lookups::kNone ? 0u : sampler;
		header.vertexLayout = vertexLayout;
		state.shadowMask = shadowMask != Lookups::kNone;
		state.shadowMaskSampler = sampler != Lookups::kNone;
		state.blocksOk = ok;
		state.key = key;
		state.written = true;
		if (rows.pipeline.Set(p, row))
			++out.pipelineRowsWritten;
		++rows.pipelinesWritten;
	}

	void MainBuild::BeginStates()
	{
		// Per-object states are the diagnostics' alone.
		if (Diagnostics::Observe().claims)
			out.objectState.assign(tables.objects.size(), kObjectStateAbsent);
	}

	void MainBuild::PrepareObjects()
	{
		// The per-object records and palettes change every frame: the commit's (CommitSceneStreams), not the build's.
		Mark(6);
		if (colourBuilt) {
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				// The decal ranges hold every decal except while their growth is outstanding: the rest wait.
				decalCount[group] = std::min(tables.decalCount[group], in.addresses.sequenceDecals);
			}
			// A decal past the scene's buffers keeps its slot with a blank input: cutting the range would drop every later
			// decal while the set claims them.
			for (std::uint32_t group = 0; group < kDecalGroups; ++group)
				decalTemplates[group].resize(decalCount[group]);
		}
	}

	void MainBuild::Skipped(Skip a_reason)
	{
		++out.skipped[static_cast<std::size_t>(a_reason)];
		if (currentObject < out.objectState.size())
			out.objectState[currentObject] = static_cast<std::uint8_t>(a_reason);
	}

	void MainBuild::Mark(std::size_t a_part)
	{
		const auto now = std::chrono::steady_clock::now();
		out.partMs[a_part] += std::chrono::duration<double, std::milli>(now - partStart).count();
		partStart = now;
	}

	std::uint32_t MainBuild::AssembleRecord(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks)
	{
		(void)o;
		auto [resolvedIt, newResolved] = resolvedBindings.try_emplace(PairKeyOf(object));
		auto& resolved = resolvedIt->second;
		if (newResolved) {
			if (object.materialIndex < lookups.materials.size() && lookups.materials[object.materialIndex].bindingBlock)
				out.bindingOwners.push_back(lookups.materials[object.materialIndex].bindingBlock);
			if (object.pipelineIndex < lookups.pipelines.size() && lookups.pipelines[object.pipelineIndex].shadowMaskOwner)
				out.bindingOwners.push_back(lookups.pipelines[object.pipelineIndex].shadowMaskOwner);
			ResolvePair(object, blocks, resolved);
		}
		Mark(4);
		if (resolved.skipReason != kNoSkip) {
			if (resolved.skipReason == static_cast<std::uint32_t>(Skip::Texture) && out.missingNext < out.missingTextures.size())
				out.missingTextures[out.missingNext++] = resolved.missingTexture;
			if (resolved.deferred)
				++out.deferredTextures;
			Skipped(static_cast<Skip>(resolved.skipReason));  // per draw, not once per pair
			return kNoRecord;
		}
		return resolved.recordIndex;
	}

	void MainBuild::ResolvePair(const ObjectRecord& object, const PipelineBlocks& blocks, ResolvedBindings& resolved)
	{
		const std::uint32_t m = object.materialIndex, p = object.pipelineIndex;
		// An input names its rows in one word (RowsOf): past its fields is a hard failure, like the sequence limits.
		if (m > kRowMaterialMask || p >= (1u << (32 - kRowPipelineShift)))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: material slot {} or pipeline slot {} past an input's row fields", m, p));
		auto fail = [&](Skip a_reason) { resolved.skipReason = static_cast<std::uint32_t>(a_reason); };
		// A row past its table waits for the growth adopted with the revision claiming its draws; it is still written (kept),
		// so the growth's fill holds it.
		const bool rowsFit = m < in.addresses.recordCapacity && p < in.addresses.pipelineCapacity;
		Mark(0);
		WriteMaterialRow(m, p, blocks);
		const auto& material = rows.materials[m];
		const auto& pipeline = rows.pipelines[p];
		Mark(1);
		// The row was packed with its technique's constant tables; a pipeline of the same slot with others would read it wrong.
		if (material.tables != blocks.tables) {
			if (out.rowTableConflicts++ == 0)
				logger::warn("[DCLF] material slot {} was packed with other constant tables than pipeline slot {} has; its draws stay native", m, p);
			return fail(Skip::Constants);
		}
		if (!material.texturesOk) {
			resolved.deferred = material.deferred;
			resolved.missingTexture = material.missingTexture;
			return fail(Skip::Texture);
		}
		// Whether segment j's variant gets what it reads from the rows and its frame; false after fail() recorded why.
		auto reads = [&](std::size_t j) {
			const auto& usage = *blocks.usage[j];
			const bool depthOnly = j == kAsyncZPrepass;
			// Textures: the material row's (t0-t15 but t14, and the features'), the pipeline row's (t14), the frame record's (rest).
			for (std::uint32_t t = 0; t < kTextureRegisters; ++t) {
				if (!usage.UsesTexture(t))
					continue;
				bool given = true;
				if (t == kShadowMaskSlot)
					given = pipeline.shadowMask;
				else if (t < kPixelTextureSlots)
					given = (material.textures >> t) & 1;
				else if (const int f = FeatureMaterialSlot(t); f >= 0)
					given = (material.features >> f) & 1;
				else if (t == kObjectBufferRegister || t == kExtrasBufferRegister || t == kTreeWindRegister || t == kPlacementBufferRegister || t == kPaletteBufferRegister ||
						 t == kShadingBufferRegister || t == kFadeStatesRegister || t == kFadeRootsRegister)
					given = true;
				else if (depthOnly)
					// The Z-prepass frame record binds no frame textures but the character light's noise, which depth never reads.
					given = t == kCharacterLightRegister;
				else
					out.frameRegisters[t / 64] |= 1ull << (t % 64);  // the commit resolves it into the frame record
				if (!given) {
					resolved.missingTexture = t;
					fail(Skip::Texture);
					return false;
				}
			}
			for (std::uint32_t sampler = 0; sampler < kSamplerRegisters; ++sampler) {
				if (!((usage.samplers >> sampler) & 1))
					continue;
				const bool given = sampler == kShadowMaskSlot ? pipeline.shadowMaskSampler : ((material.samplers >> sampler) & 1) != 0;
				if (!given) {
					fail(Skip::Sampler);
					return false;
				}
			}
			// Constant buffers: the rows' (b0, b1, b2, b4), the frame slots', and the pass's (PS b3, b13).
			bool constantsOk = true;
			for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
				const bool rowRegister = r == kPerTechnique || r == kPerMaterial || r == kPerGeometry || r == 4;
				const bool rowsOk = r == kPerMaterial ? material.blocksOk : pipeline.blocksOk;
				if ((usage.vertexConstants >> r) & 1) {
					if (rowRegister ? !rowsOk : !frameVS[j][r]) {
						constantsOk = false;
						out.missingVertexConstants |= 1u << r;
					}
				}
				if ((usage.pixelConstants >> r) & 1) {
					const bool pass = r == 3 || r == kFrameLightingRegister;
					if (rowRegister ? !rowsOk : !(pass || framePS[j][r])) {
						constantsOk = false;
						out.missingPixelConstants |= 1u << r;
					}
				}
			}
			if (!constantsOk)
				fail(Skip::Constants);
			return constantsOk;
		};
		// Both segments or neither: every built segment's variant must get what it reads, colour first (its skip is the report's).
		if (colourBuilt && !reads(kAsyncColour))
			return;
		// Forward keys (blended Lighting, refraction, effects) are no depth segment input, so their depth variant is never drawn. An
		// effect's depth variant is its colour stages (BuildEffect), drawn only by the colour segment's decal depth pass with the
		// colour frame record, which the check above covered.
		const auto rasterFlags = tables.pipelines[p].rasterFlags;
		if (segmentIn[kAsyncZPrepass] && !RasterForward(rasterFlags) && !RasterEffect(rasterFlags) && !reads(kAsyncZPrepass)) {
			// Colour alone would test EQUAL against depth nobody wrote: neither segment draws it.
			static std::atomic<std::uint32_t> logged{ 0 };
			if (colourBuilt && logged.fetch_add(1, std::memory_order_relaxed) < 8)
				logger::info("[DCLF] main pair (material {}, pipeline {}): its colour variant is given what it reads, its depth variant not ({}): neither segment "
							 "draws it <- SEGMENT SPLIT",
					m, p, kSkipNames[resolved.skipReason]);
			return;
		}
		if (!rowsFit)
			return fail(Skip::Capacity);
		resolved.recordIndex = RowsOf(p, m);
	}

	void MainBuild::WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks)
	{
		// The row's sources: the material record and frame values, its lookup entry, the shared lookups, its technique's bindings
		// (filter modes a material leaves unwritten) and the constant tables it is packed with.
		auto& state = rows.materials[m];
		const Lookups::Material* lookup = m < lookups.materials.size() ? &lookups.materials[m] : nullptr;
		// The Lighting pass descriptor's ProjectedUV (an effect key's bit 15 is Membrane's, not ported).
		const bool projected = (tables.pipelines[p].passDescriptor & 0x8000u) != 0 && !RasterEffect(tables.pipelines[p].rasterFlags);
		const std::array<std::uint32_t, 8> key{ m < tables.materialVersion.size() ? tables.materialVersion[m] : 0u,
			m < tables.materialFrameVersion.size() ? tables.materialFrameVersion[m] : 0u, lookup ? lookup->version : ~0u, lookups.sharedVersion,
			tables.TechniqueOf(p).bindingVersion, projected ? 1u : 0u, static_cast<std::uint32_t>(blocks.tables), static_cast<std::uint32_t>(blocks.tables >> 32) };
		if (state.written && state.key == key)
			return;
		// Which of the key's parts moved (the report's).
		if (state.written)
			for (std::size_t k = 0; k < key.size(); ++k)
				rows.keyMoved[k] += state.key[k] != key[k] ? 1u : 0u;
		const auto& material = tables.materials[m];
		const auto& technique = tables.TechniqueOf(p).value;
		MaterialRow row;
		state = {};
		// PerMaterial blocks, packed through the technique's tables in the record's shader's variable space (an effect record is
		// BSEffectShader's, its tables Effect.hlsl's). A record of another shader than its key's gets no row.
		const bool effect = RasterEffect(tables.pipelines[p].rasterFlags);
		const auto& vsLayout = MaterialVSLayout(material.kind);
		const auto& psLayout = MaterialPSLayout(material.kind);
		const std::uint64_t vsGroup = effect ? kEffectVSGroups[kPerMaterial] : kVSGroups[kPerMaterial];
		const std::uint64_t psGroup = effect ? kEffectPSGroups[kPerMaterial] : kPSGroups[kPerMaterial];
		const std::uint32_t vsFirst = effect ? kEffectVSFirstVariable[kPerMaterial] : kVSFirstVariable[kPerMaterial];
		const std::uint32_t psFirst = effect ? kEffectPSFirstVariable[kPerMaterial] : kPSFirstVariable[kPerMaterial];
		const auto vsSize = ConstantGroupSize(vsLayout, blocks.vsTable, vsGroup, vsFirst);
		const auto psSize = ConstantGroupSize(psLayout, blocks.psTable, psGroup, psFirst);
		state.blocksOk = vsSize <= kMaterialRowVSBytes && psSize <= kMaterialRowPSBytes && effect == (material.kind == kMaterialKindEffect);
		if (state.blocksOk) {
			PackConstantGroup(material.vs, vsLayout, blocks.vsTable, vsGroup, vsFirst, std::span(row.bytes.data() + kMaterialRowVS, kMaterialRowVSBytes));
			PackConstantGroup(material.ps, psLayout, blocks.psTable, psGroup, psFirst, std::span(row.bytes.data() + kMaterialRowPS, kMaterialRowPSBytes));
		}
		auto& header = HeaderOf(row);
		header.vsMaterial = kMaterialRowVS;
		header.psMaterial = kMaterialRowPS;
		// Textures: the material's, else a projected technique's, else the null texture. t14 is the pipeline row's.
		const bool resolved = lookup && lookup->resolved && lookup->key.first == tables.materialSlotKey[m].first && lookup->key.second == tables.materialSlotKey[m].second;
		// A character-light pass reads the frame's kCharacterLightRegister instead of t11: null texture there.
		const std::uint32_t textureWritten = material.textureWritten &
		                                     ~(MaterialSources::FrameCharacterLight(tables.materialSlotKey[m].second) ? 1u << kCharacterLightMaterialRegister : 0u);
		state.texturesOk = true;
		for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
			std::uint32_t index = lookups.nullTexture;
			if ((textureWritten >> t) & 1) {
				if (!resolved) {
					state.texturesOk = false;
					state.deferred = true;
					state.missingTexture = t;
					break;
				}
				index = lookup->textureIndex[t];
			} else if (const auto slot = ProjectedSlot(projected, t); slot >= 0) {
				index = lookups.projectedTextures[slot];
			}
			if (t == kShadowMaskSlot)
				continue;
			header.textures[t] = index == kInvalidIndex ? 0u : index;
			state.textures |= index == kInvalidIndex ? 0u : (1u << t);
		}
		for (std::uint32_t f = 0; f < kFeatureMaterialTextures && state.texturesOk; ++f) {
			std::uint32_t index = lookups.nullTexture;
			if (material.featureTextures[f]) {
				if (!resolved) {
					state.texturesOk = false;
					state.deferred = true;
					state.missingTexture = kFeatureMaterialRegisters[f];
					break;
				}
				index = lookup->featureIndex[f];
			}
			header.features[f] = index == kInvalidIndex ? 0u : index;
			state.features |= index == kInvalidIndex ? 0u : (1u << f);
		}
		// Samplers: the material's modes (its technique's filter where unwritten), or a projected technique's.
		for (std::uint32_t sampler = 0; sampler < kSamplerRegisters; ++sampler) {
			if (sampler == kShadowMaskSlot)
				continue;
			std::uint32_t address = 0, filter = 0;
			if ((material.textureWritten >> sampler) & 1) {
				address = material.addressModes[sampler];
				filter = material.filterModes[sampler] != kUnwrittenFilterMode ? material.filterModes[sampler] : technique.filterModes[sampler];
			} else if (ProjectedSlot(projected, sampler) >= 0) {
				address = 3;  // SetupGeometry: wrap, anisotropic (engine notes: samplers)
				filter = 1;
			}
			if (filter == kUnwrittenFilterMode)
				filter = 0;
			const auto index = lookups.Sampler(address, filter);
			header.samplers[sampler] = index == kInvalidIndex ? 0u : index;
			state.samplers |= index == kInvalidIndex ? 0u : (1u << sampler);
		}
		state.tables = blocks.tables;
		state.key = key;
		state.written = true;
		if (rows.material.Set(m, row))
			++out.materialRowsWritten;
		++rows.materialsWritten;
	}

	// The resident region (drawcall-limit-fix.md, "Persistent resident draws"): draw inputs kept across frames, changed only by
	// the tables' change log, a pipeline's set index or a pair's record failing. The loop skips its objects; the commit uploads
	// the inputs only on a new version.
	void MainBuild::BuildResidentRegion()
	{
		region = cache && Policy::ReuseResidentRegion() ? &cache->region : nullptr;
		if (!region && cache && cache->region.cursor.active)
			cache->region.Reset();
		// The whole scene, in both segments.
		wholeScene = region != nullptr;
		TracyCZoneN(residentRegionZone, "CS.DCLF.BuildMain.ResidentRegion", true);
		if (region) {
			{
				ZoneScopedN("CS.DCLF.BuildMain.Region.Entries");
				UpdateRegionEntries();
			}
			{
				ZoneScopedN("CS.DCLF.BuildMain.Region.Pairs");
				UpdateRegionPairs();
			}
			{
				ZoneScopedN("CS.DCLF.BuildMain.Region.Route");
				RouteTouched();
			}
			{
				ZoneScopedN("CS.DCLF.BuildMain.Region.Publish");
				PublishRegion();
			}
		}
		TracyCZoneEnd(residentRegionZone);
	}

	bool MainBuild::RegionEligible(std::uint32_t o) const
	{
		if (o >= tables.objects.size())
			return false;
		const bool resident = ResidentAt(o);
		if (!resident && !wholeScene)
			return false;
		const auto& object = tables.objects[o];
		if (object.flags & (kObjectFree | kObjectShadowOnly))
			return false;
		// A decal past the buffers stays a blank entry, so its group's range keeps every slot written.
		if (!ObjectFits(tables, o, in.addresses.fit) && !ObjectDecalGroup(object.flags))
			return false;
		// A cull-only candidate (depth segment).
		if (object.flags & kObjectNoBindings)
			return wholeScene && object.geometryIndex < tables.geometries.size();
		if (object.pipelineIndex >= pipelineBlocks.size() || object.pipelineIndex >= tables.pipelines.size() || object.geometryIndex >= tables.geometries.size())
			return false;
		const auto& geometry = tables.geometries[object.geometryIndex];
		if (!geometry.vertexAddress || !geometry.indexAddress)
			return false;
		// A decal (colour segment only) needs its ordinal within its group's range (OrderDecals logs a change for every move).
		if (const std::uint32_t group = ObjectDecalGroup(object.flags))
			if (o >= tables.decalOrdinal.size() || tables.decalOrdinal[o] >= decalCount[group - 1])
				return false;
		// A face shape, or a second-stream position, needs its positions' stream.
		const bool needsStream = IsFaceObject(tables, o) || (tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream);
		return !needsStream || FaceStreamGeometry(tables, o, in.addresses.facePositions) != ~0u;
	}

	std::uint8_t MainBuild::RegionEntry(std::uint32_t o, DrawInput& a_input) const
	{
		auto& r = *region;
		const auto& object = tables.objects[o];
		if (object.flags & kObjectNoBindings) {
			// A cull-only candidate: bounds for culling, nothing to draw.
			a_input = { 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, o, 0 };
			SetFadeRow(a_input, tables, o);
			return 0;
		}
		if (ObjectDecalGroup(object.flags) && !ObjectFits(tables, o, in.addresses.fit)) {
			a_input = DecalBlank(o);
			return 0;
		}
		const auto& blocks = pipelineBlocks[object.pipelineIndex];
		const auto pair = r.pairs.find(PairKeyOf(object));
		// A decal's slots in its group's range (one per partition: DecalSpanOf); an undrawable decal is still an input (zero-count draws).
		const bool decal = ObjectDecalGroup(object.flags) != 0;
		const std::uint32_t ordinal = decal ? tables.decalOrdinal[o] : 0u;
		const std::uint32_t partitions = PartitionsOf(tables, o);
		const bool drawable = InSet(o) && blocks.setIndex != Lookups::kNone && pair != r.pairs.end() && pair->second.ok;
		a_input = { drawable ? blocks.setIndex : 0u, drawable ? pair->second.slot : 0u, object.geometryIndex, object.flags | (drawable ? kInputDrawable : 0u),
			{ MainMaskOf(tables, o) }, o, ordinal, partitions, FaceStreamGeometry(tables, o, in.addresses.facePositions) };
		SetFadeRow(a_input, tables, o);
		if (!drawable)
			return 0;
		return decal ? kRegionDecal : static_cast<std::uint8_t>(PartitionDraws(partitions));
	}

	void MainBuild::RegionAcquire(std::uint64_t a_key)
	{
		auto& r = *region;
		auto [pair, fresh] = r.pairs.try_emplace(a_key);
		if (fresh) {
			// Its rows, once UpdateRegionPairs finds the pair can draw.
			pair->second.slot = kNoRecord;
			pair->second.ok = false;
			pair->second.skip = kNoSkip;
			r.materialPairs[static_cast<std::uint32_t>(a_key >> 32)].push_back(a_key);
			r.freshPairs.push_back(a_key);
		}
		++pair->second.count;
		const auto pipeline = static_cast<std::uint32_t>(a_key);
		auto [state, freshPipeline] = r.pipelines.try_emplace(pipeline, std::pair{ pipelineBlocks[pipeline].setIndex, 0u });
		++state->second.second;
	}

	void MainBuild::RegionRelease(std::uint64_t a_key)
	{
		auto& r = *region;
		if (a_key == kNoPair)
			return;
		if (const auto pair = r.pairs.find(a_key); pair != r.pairs.end() && --pair->second.count == 0) {
			if (!pair->second.ok && pair->second.skip != kNoSkip)
				--r.skipCounts[pair->second.skip];
			if (const auto list = r.materialPairs.find(static_cast<std::uint32_t>(a_key >> 32)); list != r.materialPairs.end()) {
				auto& keys = list->second;
				if (const auto at = std::find(keys.begin(), keys.end(), a_key); at != keys.end()) {
					*at = keys.back();
					keys.pop_back();
				}
				if (keys.empty())
					r.materialPairs.erase(list);
			}
			r.pairs.erase(pair);
			r.pairsChanged = true;
		}
		if (const auto state = r.pipelines.find(static_cast<std::uint32_t>(a_key)); state != r.pipelines.end() && --state->second.second == 0)
			r.pipelines.erase(state);
	}

	void MainBuild::RegionSetDraws(std::uint32_t i, std::uint8_t a_draws)
	{
		auto& r = *region;
		r.undrawable += (a_draws ? 0 : 1) - (r.drawsOf[i] ? 0 : 1);
		r.draws = r.draws - RegionDraws(r.drawsOf[i]) + RegionDraws(a_draws);
		r.decals = r.decals - (r.drawsOf[i] == kRegionDecal ? 1 : 0) + (a_draws == kRegionDecal ? 1 : 0);
		r.drawsOf[i] = a_draws;
	}

	DrawInput MainBuild::DecalBlank(std::uint32_t o) const
	{
		const auto& object = tables.objects[o];
		// No kInputDrawable; stream and fade row stay none. The partitions word names the decal's slots, each a zero-count draw.
		return { 0, 0, object.geometryIndex < in.addresses.fit.geometryRows ? object.geometryIndex : 0u, object.flags, { MainMaskOf(tables, o) }, o, tables.decalOrdinal[o],
			o < tables.skinPartitions.size() ? tables.skinPartitions[o] : 0u };
	}

	void MainBuild::RegionWrite(std::uint32_t i, const DrawInput& a_entry)
	{
		auto& r = *region;
		if (reuseKeptStorage)
			r.inputs.Set(i, a_entry);
		else {
			r.inputs.Mutable()[i] = a_entry;
			r.inputs.Mark(i);
		}
	}

	void MainBuild::RegionRemove(std::uint32_t o)
	{
		auto& r = *region;
		const std::uint32_t i = r.EntryOf(o);
		if (i == kNoRegion || !(r.sideOf[i] & kSideMain))
			return;
		RegionRelease(r.pairOf[i]);
		r.touched.push_back(o);
		r.undrawable -= r.drawsOf[i] ? 0 : 1;
		r.draws -= RegionDraws(r.drawsOf[i]);
		r.decals -= r.drawsOf[i] == kRegionDecal ? 1 : 0;
		if (r.sideOf[i] & kSideShadow) {
			// The shadow build's words stay: the entry becomes its alone.
			r.pairOf[i] = kNoPair;
			r.drawsOf[i] = 0;
			r.sideOf[i] = kSideShadow;
			DrawInput entry = r.inputs.Get()[i];
			ClearMainPart(entry);
			RegionWrite(i, entry);
			return;
		}
		r.RemoveEntry(o);
	}

	void MainBuild::RegionUpsert(std::uint32_t o)
	{
		auto& r = *region;
		if (!RegionEligible(o)) {
			RegionRemove(o);
			return;
		}
		r.Cover(tables.objects.size());
		const auto& object = tables.objects[o];
		const std::uint64_t key = (object.flags & kObjectNoBindings) ? kNoPair : PairKeyOf(object);
		std::uint32_t i = r.indexOf[o];
		if (i == kNoRegion || !(r.sideOf[i] & kSideMain)) {
			// No share to stay within: region and loop together have at most an input per object, which the buffers hold. A
			// shadow-only entry takes the main part too.
			if (i == kNoRegion)
				i = r.AddEntry(o, DrawInput{}, kSideMain);
			else
				r.sideOf[i] |= kSideMain;
			r.pairOf[i] = key;
			++r.undrawable;
			if (key != kNoPair)
				RegionAcquire(key);
		} else if (r.pairOf[i] != key) {
			RegionRelease(r.pairOf[i]);
			if (key != kNoPair)
				RegionAcquire(key);
			r.pairOf[i] = key;
		}
		DrawInput fresh{};
		RegionSetDraws(i, RegionEntry(o, fresh));
		DrawInput entry = r.inputs.Get()[i];
		SetMainPart(entry, fresh);
		RegionWrite(i, entry);
		r.touched.push_back(o);
	}

	void MainBuild::RegionTake(std::uint32_t o)
	{
		auto& r = *region;
		if (!ResidentAt(o) && !wholeScene)
			RegionRemove(o);
		else
			RegionUpsert(o);
		r.touched.push_back(o);
	}

	void MainBuild::UpdateRegionEntries()
	{
		auto& r = *region;
		r.touched.clear();
		// The buffer holds the version last sent; only later changes are sent (once per publication, by whichever build writes first).
		r.Begin(in.residentUploaded);
		// Resync reasons (for the report): log lost, segments, tables shrunk, scope, buffers' fit.
		const std::array<bool, kResyncReasons> reasons{ !r.cursor.Continues(tables.changeLog, in.tablesGeneration), r.segments != Segments(),
			r.indexOf.size() > tables.objects.size(), r.wholeScene != wholeScene, r.fit != in.addresses.fit };
		const bool resync = std::find(reasons.begin(), reasons.end(), true) != reasons.end();
		if (resync) {
			for (std::size_t k = 0; k < reasons.size(); ++k)
				out.residentResyncReasons |= reasons[k] ? 1u << k : 0u;
			r.Reset();
			r.fit = in.addresses.fit;
			r.decalCount = decalCount;
			r.cursor.Restart(in.tablesGeneration);
			r.segments = Segments();
			r.wholeScene = wholeScene;
			r.indexOf.assign(tables.objects.size(), kNoRegion);
			++out.residentResyncs;
			for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
				RegionTake(o);
		} else {
			// Changes a draw input carries (fade distance, bindings, geometry, partitions, residency); placement is the object record's.
			constexpr std::uint32_t kInputCauses = kChangeBindings | kChangeSkin | kChangeGeometry | kChangeMembership;
			for (const auto& change : r.cursor.Unread(tables.changeLog))
				if (change.causes & kInputCauses)
					RegionTake(change.slot);
			// Face stream indices count from the geometry slots: when that count or the positions buffer changes, retake every face.
			if (const std::size_t base = in.addresses.facePositions ? tables.geometries.size() : 0; base != r.faceBase) {
				for (const auto& stream : tables.faceStreams)
					if (stream.object != SceneTables::kNoFaceObject && stream.object < tables.objects.size())
						RegionTake(stream.object);
			}
			// A decal group's capped count moved: retake the decals between the old and new counts (changed ordinals are in the log).
			if (r.decalCount != decalCount) {
				++out.residentDecalRetakes;
				for (std::uint32_t o = 0; o < tables.decalOrdinal.size() && o < tables.objects.size(); ++o) {
					const std::uint32_t ordinal = tables.decalOrdinal[o];
					const std::uint32_t group = ordinal != ~0u ? ObjectDecalGroup(tables.objects[o].flags) : 0;
					if (group && ordinal >= std::min(r.decalCount[group - 1], decalCount[group - 1]) &&
						ordinal < std::max(r.decalCount[group - 1], decalCount[group - 1]))
						RegionTake(o);
				}
				r.decalCount = decalCount;
			}
		}
		r.faceBase = in.addresses.facePositions ? tables.geometries.size() : 0;
		r.cursor.Advance(tables.changeLog);
	}

	std::uint64_t MainBuild::PipelineWitness(std::uint32_t p) const
	{
		// A hash of what ResolvePair reads of the pipeline (bindings, projection, constant tables, set, row state). The material's
		// half is three versions (UpdateRegionPairs).
		std::uint64_t value = 0xcbf29ce484222325ull;
		auto mix = [&](std::uint64_t a_word) { value = (value ^ a_word) * 0x100000001b3ull; };
		const auto& blocks = pipelineBlocks[p];
		mix(lookups.sharedVersion);
		// A pair past the row tables resolves again once their growth is adopted.
		mix((std::uint64_t(in.addresses.recordCapacity) << 32) | in.addresses.pipelineCapacity);
		mix(tables.TechniqueOf(p).bindingVersion);
		mix((tables.pipelines[p].passDescriptor & 0x8000u) != 0 ? 1u : 0u);
		mix(blocks.tables);
		mix(blocks.setIndex);
		const auto& pipeline = rows.pipelines[p];
		for (const auto word : pipeline.key)
			mix(word);
		mix((pipeline.written ? 1u : 0u) | (pipeline.blocksOk ? 2u : 0u) | (pipeline.shadowMask ? 4u : 0u) | (pipeline.shadowMaskSampler ? 8u : 0u));
		return value;
	}

	void MainBuild::UpdateRegionPairs()
	{
		auto& r = *region;
		// Entries of pipelines whose set index changed and of pairs that became (un)buildable are rewritten (both rare).
		std::vector<std::uint32_t> changedPipelines;
		for (auto& [pipeline, state] : r.pipelines)
			if (pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != state.first) {
				state.first = pipelineBlocks[pipeline].setIndex;
				changedPipelines.push_back(pipeline);
			}
		// A pair resolves again only when its witness changed (material, textures, pipeline rows or set, frame slots). Candidates
		// come from events: the material logs, new pairs, moved pipelines; every pair when the frame's half moved or a log is lost.
		std::uint64_t frameWitness = 0xcbf29ce484222325ull ^ Segments();
		for (const auto* segment : segmentIn)
			if (segment)
				frameWitness = (frameWitness ^ ((std::uint64_t(segment->vsFrameMask) << 32) | segment->psFrameMask)) * 0x100000001b3ull;
		frameWitness |= 1;
		bool everyPair = frameWitness != r.frameWitness;
		r.frameWitness = frameWitness;
		if (!r.materialCursor.Continues(tables.materialLog, in.tablesGeneration) || !r.lookupCursor.Continues(lookups.materialLog, lookups.logGeneration)) {
			everyPair = true;
			r.materialCursor.Restart(in.tablesGeneration);
			r.lookupCursor.Restart(lookups.logGeneration);
		}
		std::vector<std::uint64_t> changedPairs;
		currentObject = ~0u;
		TracyCZoneN(pairsPipelinesZone, "CS.DCLF.BuildMain.Pairs.Pipelines", true);
		std::vector<std::uint64_t> pipelineWitness(pipelineBlocks.size(), 0);
		bool pipelineMoved = false;
		for (const auto& [pipeline, state] : r.pipelines) {
			if (pipeline >= pipelineBlocks.size())
				continue;
			if (pipelineBlocks[pipeline].setIndex != Lookups::kNone)
				pipelineWitness[pipeline] = PipelineWitness(pipeline);
			auto& seen = r.pipelineWitness[pipeline];
			pipelineMoved |= seen != pipelineWitness[pipeline];
			seen = pipelineWitness[pipeline];
		}
		TracyCZoneEnd(pairsPipelinesZone);
		TracyCZoneN(pairsResolveZone, "CS.DCLF.BuildMain.Pairs.Resolve", true);
		std::vector<std::uint64_t> check;
		if (everyPair || pipelineMoved) {
			check.reserve(r.pairs.size());
			for (const auto& [key, pair] : r.pairs)
				check.push_back(key);
		} else {
			auto material = [&](std::uint32_t a_material) {
				if (const auto list = r.materialPairs.find(a_material); list != r.materialPairs.end())
					check.insert(check.end(), list->second.begin(), list->second.end());
			};
			for (const std::uint32_t m : r.materialCursor.Unread(tables.materialLog))
				material(m);
			for (const std::uint32_t m : r.lookupCursor.Unread(lookups.materialLog))
				material(m);
			check.insert(check.end(), r.freshPairs.begin(), r.freshPairs.end());
			std::sort(check.begin(), check.end());
			check.erase(std::unique(check.begin(), check.end()), check.end());
		}
		r.freshPairs.clear();
		r.materialCursor.Advance(tables.materialLog);
		r.lookupCursor.Advance(lookups.materialLog);
		for (const std::uint64_t key : check) {
			const auto found = r.pairs.find(key);
			if (found == r.pairs.end())
				continue;
			auto& pair = found->second;
			const auto pipeline = static_cast<std::uint32_t>(key);
			const auto material = static_cast<std::uint32_t>(key >> 32);
			const bool pipelineOk = pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != Lookups::kNone;
			std::uint64_t witness = 1ull;
			if (pipelineOk) {
				witness = pipelineWitness[pipeline];
				witness = (witness ^ (material < tables.materialVersion.size() ? tables.materialVersion[material] : 0u)) * 0x100000001b3ull;
				witness = (witness ^ (material < tables.materialFrameVersion.size() ? tables.materialFrameVersion[material] : 0u)) * 0x100000001b3ull;
				witness = (witness ^ (material < lookups.materialVersions.size() ? lookups.materialVersions[material] : ~0u)) * 0x100000001b3ull;
				witness |= 1;  // never 0, which is "never resolved"
			}
			if (!everyPair && witness == pair.witness)
				continue;
			bool ok = pipelineOk;
			std::uint32_t slot = pair.slot;
			const auto registersBefore = out.frameRegisters;
			out.frameRegisters = {};
			if (!pair.ok && pair.skip != kNoSkip)
				--r.skipCounts[pair.skip];
			pair.skip = kNoSkip;
			if (ok) {
				ObjectRecord object{};
				object.materialIndex = material;
				object.pipelineIndex = pipeline;
				slot = AssembleRecord(~0u, object, pipelineBlocks[pipeline]);
				ok = slot != kNoRecord;
				if (!ok)
					if (const auto resolved = resolvedBindings.find(PairKeyOf(object)); resolved != resolvedBindings.end())
						pair.skip = resolved->second.skipReason;
			}
			if (!ok && pair.skip != kNoSkip)
				++r.skipCounts[pair.skip];
			// Rebuild the bundle only when what it holds for this pair moved.
			const void* binding = material < lookups.materials.size() ? lookups.materials[material].bindingBlock.get() : nullptr;
			const void* mask = pipeline < lookups.pipelines.size() ? lookups.pipelines[pipeline].shadowMaskOwner.get() : nullptr;
			if (out.frameRegisters != pair.frameRegisters || binding != pair.bindingSeen || mask != pair.maskSeen || !pair.witness)
				r.pairsChanged = true;
			pair.frameRegisters = out.frameRegisters;
			out.frameRegisters[0] = registersBefore[0] | pair.frameRegisters[0];
			out.frameRegisters[1] = registersBefore[1] | pair.frameRegisters[1];
			pair.witness = witness;
			if (ok != pair.ok || (ok && slot != pair.slot)) {
				pair.ok = ok;
				if (ok)
					pair.slot = slot;
				changedPairs.push_back(key);
			}
		}
		TracyCZoneEnd(pairsResolveZone);
		TracyCZoneN(pairsBundleZone, "CS.DCLF.BuildMain.Pairs.Bundle", true);
		for (std::size_t reason = 0; reason < r.skipCounts.size(); ++reason)
			out.skipped[reason] += r.skipCounts[reason];
		// Every pair's frame textures and owners, rebuilt when a pair changed or left.
		if (r.pairsChanged) {
			r.pairsChanged = false;
			r.frameRegisters = {};
			auto owners = std::make_shared<std::vector<std::shared_ptr<const void>>>();
			owners->reserve(r.pairs.size() * 2);
			for (auto& [key, pair] : r.pairs) {
				r.frameRegisters[0] |= pair.frameRegisters[0];
				r.frameRegisters[1] |= pair.frameRegisters[1];
				const auto pipeline = static_cast<std::uint32_t>(key);
				const auto material = static_cast<std::uint32_t>(key >> 32);
				pair.bindingSeen = pair.maskSeen = nullptr;
				if (material < lookups.materials.size() && lookups.materials[material].bindingBlock) {
					owners->push_back(lookups.materials[material].bindingBlock);
					pair.bindingSeen = lookups.materials[material].bindingBlock.get();
				}
				if (pipeline < lookups.pipelines.size() && lookups.pipelines[pipeline].shadowMaskOwner) {
					owners->push_back(lookups.pipelines[pipeline].shadowMaskOwner);
					pair.maskSeen = lookups.pipelines[pipeline].shadowMaskOwner.get();
				}
			}
			r.owners = std::move(owners);
		}
		out.frameRegisters[0] |= r.frameRegisters[0];
		out.frameRegisters[1] |= r.frameRegisters[1];
		if (r.owners)
			out.bindingOwners.push_back(r.owners);
		TracyCZoneEnd(pairsBundleZone);
		ZoneScopedN("CS.DCLF.BuildMain.Pairs.Inputs");
		if (!changedPipelines.empty() || !changedPairs.empty()) {
			auto& inputs = r.inputs.Mutable();
			for (std::uint32_t i = 0; i < inputs.size(); ++i) {
				const auto key = r.pairOf[i];
				if (!(r.sideOf[i] & kSideMain) || (std::find(changedPairs.begin(), changedPairs.end(), key) == changedPairs.end() &&
														std::find(changedPipelines.begin(), changedPipelines.end(), static_cast<std::uint32_t>(key)) == changedPipelines.end()))
					continue;
				DrawInput fresh{};
				RegionSetDraws(i, RegionEntry(inputs[i].objectIndex, fresh));
				SetMainPart(inputs[i], fresh);
				r.inputs.Mark(i);
				r.touched.push_back(inputs[i].objectIndex);
			}
		}
	}

	void MainBuild::RouteTouched()
	{
		auto& r = *region;
		if (r.loopIndex.size() < tables.objects.size()) {
			r.loopIndex.resize(tables.objects.size(), kNoRegion);
			r.candidate.resize(tables.objects.size(), 0);
		}
		for (const std::uint32_t o : r.touched) {
			if (o >= r.loopIndex.size())
				continue;
			const auto flags = o < tables.objects.size() ? tables.objects[o].flags : kObjectFree;
			const bool inRegion = r.MainHolds(o);
			const bool isCandidate = !(flags & kObjectFree) && (flags & (kObjectNoBindings | kObjectShadowOnly));
			// The one list holds every segment's candidates, cull-only ones and decals too.
			const bool loop = !inRegion && !(flags & (kObjectFree | kObjectShadowOnly));
			if (loop && r.loopIndex[o] == kNoRegion) {
				r.loopIndex[o] = static_cast<std::uint32_t>(r.loopList.size());
				r.loopList.push_back(o);
			} else if (!loop && r.loopIndex[o] != kNoRegion) {
				const std::uint32_t at = r.loopIndex[o];
				const std::uint32_t tail = r.loopList.back();
				r.loopList[at] = tail;
				r.loopIndex[tail] = at;
				r.loopList.pop_back();
				r.loopIndex[o] = kNoRegion;
			}
			if (r.candidate[o] != (isCandidate ? 1 : 0)) {
				r.candidates += isCandidate ? 1 : std::size_t(-1);
				r.candidate[o] = isCandidate ? 1 : 0;
			}
		}
	}

	void MainBuild::PublishRegion()
	{
		auto& r = *region;
		// Per-object states, for the set parity alone.
		const auto& inputs = r.inputs.Get();
		if (!out.objectState.empty()) {
			for (std::uint32_t i = 0; i < inputs.size(); ++i)
				if ((r.sideOf[i] & kSideMain) && inputs[i].objectIndex < out.objectState.size())
					out.objectState[inputs[i].objectIndex] = r.drawsOf[i] == kRegionDecal ? kObjectStateDecal :
					                                         r.drawsOf[i]                  ? kObjectStateDrawable :
					                                         r.pairOf[i] == kNoPair        ? static_cast<std::uint8_t>(Skip::CandidateOnly) :
					                                         !InSet(inputs[i].objectIndex) ? static_cast<std::uint8_t>(Skip::NotInSet) :
					                                                                         static_cast<std::uint8_t>(Skip::Pipeline);
			for (std::uint32_t o = 0; o < r.candidate.size() && o < out.objectState.size(); ++o)
				if (r.candidate[o] && out.objectState[o] == kObjectStateAbsent)
					out.objectState[o] = static_cast<std::uint8_t>(Skip::CandidateOnly);
		}
		// Every candidate counts, whether the loop sees it or not.
		out.skipped[static_cast<std::size_t>(Skip::CandidateOnly)] += static_cast<std::uint32_t>(r.candidates);
		out.residentUndrawable = static_cast<std::uint32_t>(r.undrawable);
		// Not the region's view: the shadow build writes the scene list after this; RunAhead hands out the view once both have.
		out.residentDraws = static_cast<std::uint32_t>(r.draws);
		out.decalsDrawn += static_cast<std::uint32_t>(r.decals);
		out.residentPairs = static_cast<std::uint32_t>(r.pairs.size());
		regionInputs = inputs.size();
		regionDraws = r.draws;
	}

	void MainBuild::RunObjectLoop()
	{
		TracyCZoneN(objectLoopZone, "CS.DCLF.BuildMain.ObjectLoop", true);
		// With a region, the loop visits only its loopList; the region counted the candidates.
		const std::vector<std::uint32_t>* loopObjects = region ? &region->loopList : nullptr;
		const std::size_t loopCount = loopObjects ? loopObjects->size() : tables.objects.size();
		for (std::size_t n = 0; n < loopCount; ++n) {
			const std::uint32_t o = loopObjects ? (*loopObjects)[n] : static_cast<std::uint32_t>(n);
			if (o < tables.objects.size())
				LoopObject(o);
		}
		TracyCZoneEnd(objectLoopZone);
	}

	void MainBuild::LoopObject(std::uint32_t o)
	{
		currentObject = o;
		const auto& object = tables.objects[o];
		if (region && region->MainHolds(o))
			return;
		// Free slots and shadow-only records are not even culling candidates.
		if (object.flags & kObjectFree)
			return;
		if (object.flags & kObjectShadowOnly) {
			if (!region)
				Skipped(Skip::CandidateOnly);
			return;
		}
		// Past the scene's buffers (growth outstanding): no input, except a decal's blank, which keeps its slot as a zero-count draw.
		if (!ObjectFits(tables, o, in.addresses.fit)) {
			if (const std::uint32_t group = ObjectDecalGroup(object.flags);
				group && o < tables.decalOrdinal.size() && tables.decalOrdinal[o] < decalCount[group - 1])
				drawInputs.push_back(DecalBlank(o));
			Skipped(Skip::Capacity);
			return;
		}
		if (object.flags & kObjectNoBindings) {
			// No material or pipeline entry: the depth segment submits it cull-only, with its bounds.
			drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, static_cast<std::uint32_t>(o), 0 });
			SetFadeRow(drawInputs.back(), tables, o);
			if (!region)
				Skipped(Skip::CandidateOnly);
			return;
		}
		// Decals are not occluders: only the colour segment's second pass draws them; without it no decal has a slot.
		const std::uint32_t decalGroup = ObjectDecalGroup(object.flags);
		if (decalGroup && (o >= tables.decalOrdinal.size() || tables.decalOrdinal[o] >= decalCount[decalGroup - 1]))
			return;
		// An undrawable decal must still reach BuildDraws so its slot is a zero-count draw, not a stale one: this guard pushes
		// the blank on every early return below.
		struct DecalSlot
		{
			std::vector<DrawInput>* inputs = nullptr;
			DrawInput blank{};
			~DecalSlot()
			{
				if (inputs)
					inputs->push_back(blank);
			}
		} decalSlot;
		if (decalGroup) {
			decalSlot.inputs = &drawInputs;
			decalSlot.blank = { 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, static_cast<std::uint32_t>(o), tables.decalOrdinal[o],
				o < tables.skinPartitions.size() ? tables.skinPartitions[o] : 0u };
		}
		const auto& blocks = pipelineBlocks[object.pipelineIndex];
		if (blocks.setIndex == Lookups::kNone) {
			Skipped(Skip::Pipeline);
			return;
		}
		const auto& geometry = tables.geometries[object.geometryIndex];
		if (!geometry.vertexAddress || !geometry.indexAddress) {
			Skipped(Skip::Geometry);
			return;
		}
		// Outside the set the engine draws depth and colour; DCLF depth would break its EQUAL test against its own prepass.
		if (!InSet(o)) {
			// Still cull-only: the depth segment publishes every candidate's verdict for the colour segment. A decal gets its blank.
			if (!decalGroup) {
				drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags, { MainMaskOf(tables, o) }, static_cast<std::uint32_t>(o), 0 });
				SetFadeRow(drawInputs.back(), tables, o);
			}
			Skipped(Skip::NotInSet);
			return;
		}
		// A multi-partition skin writes a sequence per drawn partition; a skinned decal (blood) one per slot of its span.
		const std::uint32_t partitions = PartitionsOf(tables, o);
		// A face shape or second-stream position needs its positions stream, else the geometry's other attributes draw as positions.
		const std::uint32_t streamIndex = FaceStreamGeometry(tables, o, in.addresses.facePositions);
		if (streamIndex == ~0u && (IsFaceObject(tables, o) || (tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream))) {
			Skipped(Skip::Geometry);
			return;
		}
		// No draw cap: the sequences hold every draw the scene can produce.
		const std::uint32_t recordIndex = AssembleRecord(o, object, blocks);
		if (recordIndex == kNoRecord)
			return;
		// The CPU template of what BuildDraws writes (for the build parity).
		Mark(2);
		auto sequence = tables.draws[o];
		sequence.pipelineIndex = blocks.setIndex;
		sequence.objectIndex = o;
		sequence.pipelineRowAddress = in.addresses.pipelineRows + std::uint64_t(recordIndex >> kRowPipelineShift) * kPipelineRowBytes;
		sequence.materialRowAddress = in.addresses.records + std::uint64_t(recordIndex & kRowMaterialMask) * kMaterialRowBytes;
		if (streamIndex != ~0u)
			SetSequenceStream(sequence, tables, o, in.addresses.facePositions);
		if (decalGroup) {
			const std::uint32_t ordinal = tables.decalOrdinal[o];
			decalSlot.inputs = nullptr;  // drawn: the blank is not needed
			// The partitions word names the slots (BuildDrawsCS draws a partition at each).
			drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex, object.flags | kInputDrawable, { MainMaskOf(tables, o) },
				static_cast<std::uint32_t>(o), ordinal, partitions, streamIndex });
			decalTemplates[decalGroup - 1][ordinal] = sequence;
			++out.decalsDrawn;
			if (o < out.objectState.size())
				out.objectState[o] = kObjectStateDecal;
		} else {
			if (o < out.objectState.size())
				out.objectState[o] = kObjectStateDrawable;
			drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex, object.flags | kInputDrawable, { MainMaskOf(tables, o) },
				static_cast<std::uint32_t>(o), 0, partitions, streamIndex });
			SetFadeRow(drawInputs.back(), tables, o);
			if (!partitions) {
				sequences.push_back(sequence);
			} else {
				ForEachDrawnGeometry(tables, object.geometryIndex, partitions, [&](std::uint32_t a_slot) {
					auto partitionSequence = sequence;
					SetSequenceGeometry(partitionSequence, tables.geometries[a_slot], streamIndex != ~0u);
					sequences.push_back(partitionSequence);
				});
			}
		}
		// The draw fetches through the buffer's address and size: a short slice reads zeros and the object silently collapses.
		// Checked here because nothing on the D3D11 side sees the Vulkan slice.
		const std::uint64_t vertexNeeded = std::uint64_t(geometry.vertexCount) * geometry.vertexStride;
		const std::uint64_t indexNeeded = (std::uint64_t(geometry.firstIndex) + geometry.indexCount) * sizeof(std::uint16_t);
		if (geometry.vertexBytes < vertexNeeded || geometry.indexBytes < indexNeeded) {
			if (out.shortBuffers++ == 0)
				out.shortBuffer = { o, vertexNeeded, indexNeeded };
		}
		Mark(3);
	}

	void MainBuild::Finish()
	{
		TakeGeometryDraws(streams.get(), tables, in.tablesGeneration, frameNumber, out.geometryDraws);
		out.streams = streams;
		AppendFaceStreams(tables, in.addresses.facePositions, out.geometryDraws);
		if (in.addresses.facePositions)
			out.faceStreams = tables.faceStreams;
		out.materialRows = rows.material.View();
		out.pipelineRows = rows.pipeline.View();
		Mark(5);
		// The other segment's payload: the same list, rows and templates with its own inputs. One-off counts stay the primary's;
		// the depth variant resolves no frame textures.
		for (std::size_t j = 0; j < segmentOut.size(); ++j) {
			if (j == primary || !segmentOut[j] || !segmentIn[j])
				continue;
			auto& other = *segmentOut[j];
			other = out;
			other.inputs = *segmentIn[j];
			if (j == kAsyncZPrepass)
				other.frameRegisters = {};
			other.residentResyncs = other.residentResyncReasons = other.residentDecalRetakes = 0;
			other.rowTableConflicts = 0;
			other.materialRowsWritten = other.pipelineRowsWritten = 0;
			other.partMs = {};
		}
	}

	void BuildMainPayloads(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
		const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams)
	{
		if (!a_in[kAsyncColour] && !a_in[kAsyncZPrepass])
			return;
		MainBuild(a_in, a_tables, a_frame, a_lookups, a_out, a_rows, a_cache, std::move(a_streams)).Run();
	}

	void BuildMainPayload(const MainInputs& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups, MainPayload& a_out,
		MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams)
	{
		const std::size_t j = a_in.depthOnly ? kAsyncZPrepass : kAsyncColour;
		std::array<const MainInputs*, 2> in{};
		std::array<MainPayload*, 2> out{};
		in[j] = &a_in;
		out[j] = &a_out;
		BuildMainPayloads(in, a_tables, a_frame, a_lookups, out, a_rows, a_cache, std::move(a_streams));
	}
}

#endif
