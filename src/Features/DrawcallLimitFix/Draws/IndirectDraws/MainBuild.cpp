#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

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
			const Lookups::RegisterUsageBits* usage = nullptr;
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

		/** @brief The slot of the projected textures (SceneStore::ProjectedTextures) a ProjectedUV pipeline binds at a_slot, or -1. */
		std::int32_t ProjectedSlot(bool a_projectedPipeline, std::uint32_t a_slot)
		{
			if (!a_projectedPipeline)
				return -1;
			for (std::size_t i = 0; i < SceneStore::ProjectedTextures::kSlots.size(); ++i) {
				if (SceneStore::ProjectedTextures::kSlots[i] == a_slot)
					return static_cast<std::int32_t>(i);
			}
			return -1;
		}

		/**
		 * @brief One build of a main segment's payload (BuildMainPayload). Its members are what the build's parts share;
		 * Run calls the parts in order: the pipelines' blocks, the drawn marks, the object records, the resident region,
		 * the object loop (each draw's record through AssembleRecord), then the marks' changes and the kept stores.
		 */
		class MainBuild
		{
		public:
			MainBuild(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out, MainRows& a_rows, BuildCache* a_cache,
				ObjectRecordStore* a_objects, BonesStore* a_bones, GeometryStore* a_geometries) :
				in(a_in), tables(a_tables), lookups(a_lookups), out(a_out), rows(a_rows), cache(a_cache), objectStore(a_objects), boneStore(a_bones),
				geometryStore(a_geometries)
			{}

			void Run();

		private:
			const MainInputs& in;
			const SceneStore::Tables& tables;
			const Lookups& lookups;
			MainPayload& out;
			MainRows& rows;
			BuildCache* cache;
			ObjectRecordStore* objectStore;
			BonesStore* boneStore;
			GeometryStore* geometryStore;

			decltype(MainPayload::arena)& arena = out.arena;
			decltype(MainPayload::sequences)& sequences = out.sequences;
			decltype(MainPayload::inputList)& drawInputs = out.inputList;
			decltype(MainPayload::decalCount)& decalCount = out.decalCount;
			decltype(MainPayload::decalTemplates)& decalTemplates = out.decalTemplates;
			const decltype(MainPayload::objectRecords)& objectRecords = out.objectRecords;
			const bool depthOnly = in.depthOnly;
			const std::uint32_t frameNumber = in.frameNumber;
			const bool bindlessParity = in.bindlessParity;
			const decltype(MainInputs::eye)& eye = in.eye;
			const decltype(MainInputs::previousEye)& previousEye = in.previousEye;
			static constexpr auto renderFlags = SceneStore::kMainPassRenderFlags;

			// The frame registers' blocks (FrameRegisters).
			std::array<std::uint64_t, kConstantBufferRegisters> frameVS{}, framePS{};
			std::uint64_t sharedLightBlock = 0, frameLightingBlock = 0;
			// Per pipeline: its set index, constant tables and register usage, and whether its row is whole.
			std::vector<PipelineBlocks> pipelineBlocks;
			// Per (material, pipeline) pair: its verdict this build (ResolvedBindings).
			ankerl::unordered_dense::map<std::uint64_t, ResolvedBindings> resolvedBindings;
			ankerl::unordered_dense::map<std::uint32_t, GeometryTemplate> geometryTemplates;  // pipeline: the bindless parity's
			// The colour segment's drawn marks (DrawnMarks): kept across builds, sent as changes.
			DrawnMarks* marks = nullptr;
			std::vector<std::uint32_t> loopDrawn;  // what the loop draws this build (DrawnMarks)
			std::uint32_t currentObject = ~0u;
			std::chrono::steady_clock::time_point partStart;
			// Scratch for the bindless check only, reused across objects so it costs no allocation per draw.
			std::vector<std::byte> parityVS, parityPS;
			// The resident region (BuildResidentRegion) and what it holds of the buffers' capacity.
			ResidentRegion* region = nullptr;
			bool wholeScene = false;
			std::size_t regionInputs = 0, regionDraws = 0;

			void Reset();
			void BeginRows();
			void FrameRegisters();
			void PackPipelines();
			void BeginMarks();
			void PrepareObjects();
			void Skipped(Skip a_reason);
			void Mark(std::size_t a_part);
			// Drawn, for the marks: what BuildDraws writes a sequence for, and the engine kept (only what the engine's culling
			// kept may be drawn; the culling counters still measure the whole tracked set).
			bool NativeDrawn(std::uint32_t o) const { return o < tables.objects.size() && (tables.objects[o].flags & kObjectNativeVisible); }
			// The Z-prepass's gate without withholding: what the colour epoch drew last frame.
			bool DrewLastFrame(std::uint32_t o) const;

			// The (material, pipeline) pair's rows, checked once per build: RowsOf(pipeline, material), or kNoRecord after
			// Skipped() recorded why (per draw).
			std::uint32_t AssembleRecord(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks);
			/** @brief What the pair's rows give against what its pipeline reads: its verdict. */
			void ResolvePair(const ObjectRecord& object, const PipelineBlocks& blocks, ResolvedBindings& resolved);
			/** @brief The pipeline's row (MainRows), written again when what it is written from changed. */
			void WritePipelineRow(std::uint32_t p, const PipelineBlocks& blocks);
			/** @brief The material's row, written again when what it is written from changed. */
			void WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks);
			/** @brief CS_DCLF_BINDLESS_PARITY: the object record against the PerGeometry group it replaces. */
			void CheckBindless(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks);

			void BuildResidentRegion();
			static std::uint64_t PairKeyOf(const ObjectRecord& a_object) { return (std::uint64_t(a_object.materialIndex) << 32) | a_object.pipelineIndex; }
			bool ResidentAt(std::uint32_t o) const { return o < tables.residentSlot.size() && tables.residentSlot[o] != 0; }
			// What the region can hold: a record the loop would draw as one input with its pair's record (no decal slot, no
			// face stream, no position in the second stream) - a resident's, or with the whole scene anyone's - and with the
			// whole scene, the depth segment's cull-only candidates.
			bool RegionEligible(std::uint32_t o) const;
			// The input the loop would write for it, drawable while its pipeline is in the set and its pair's record built.
			std::uint8_t RegionEntry(std::uint32_t o, DrawInput& a_input);
			void RegionAcquire(std::uint64_t a_key);
			void RegionRelease(std::uint64_t a_key);
			// An entry's draws, with the region's totals.
			void RegionSetDraws(std::uint32_t i, std::uint8_t a_draws);
			void RegionRemove(std::uint32_t o);
			void RegionUpsert(std::uint32_t o);
			// A resident the depth segment has not seen the colour epoch draw waits (the loop's rule); anything else is written.
			void RegionTake(std::uint32_t o);
			/** @brief The entries the change log names since the region's last build, or every slot on a resync. */
			void UpdateRegionEntries();
			/** @brief The entries of a pipeline whose set index changed and of a pair whose record could or could no longer be built. */
			void UpdateRegionPairs();
			void CheckRegionParity();
			/** @brief Every slot this build touched: the region's, the loop's (loopList) or nobody's, and whether it is a candidate only. */
			void RouteTouched();
			/** @brief What the region draws, into the payload. */
			void PublishRegion();

			void RunObjectLoop();
			/** @brief One object of the loop: its draw input, and its sequence and record when it is drawn. */
			void LoopObject(std::uint32_t o);
			void FinishMarks();
			void Finish();
		};
	}

	void MainBuild::Run()
	{
		ZoneScopedN("CS.DCLF.BuildMainPayload");
		Reset();
		FrameRegisters();
		PackPipelines();
		BeginMarks();
		partStart = std::chrono::steady_clock::now();
		PrepareObjects();
		BuildResidentRegion();
		RunObjectLoop();
		FinishMarks();
		Finish();
	}

	void MainBuild::Reset()
	{
		TracyCZoneN(resetMainZone, "CS.DCLF.BuildMain.Reset", true);
		out.Reset();
		out.inputs = in;
		if (lookups.sharedBindingBlock)
			out.bindingOwners.push_back(lookups.sharedBindingBlock);
		arena.Reset(kConstantBytes);
		BeginRows();
		TracyCZoneEnd(resetMainZone);
	}

	void MainBuild::BeginRows()
	{
		// What the tables hold is the version they were sent: what is written from here on is sent alone.
		rows.material.BeginBuild(in.materialRowsHeld);
		rows.pipeline.BeginBuild(in.pipelineRowsHeld);
		if (!rows.active || rows.identity != in.addresses.identity || rows.generation != in.tablesGeneration) {
			// New resources or new tables (the slots mean other things): every row again; the journals count on.
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
		// The frame registers name their slots; a slot the commit does not supply stays at zero, which is
		// what the constants check below rejects for a pipeline that reads it.
		frameVS = {};
		framePS = {};
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if ((in.vsFrameMask >> slot) & 1)
				frameVS[slot] = in.addresses.frameConstants + FrameSlotOffset(false, slot);
			if ((in.psFrameMask >> slot) & 1)
				framePS[slot] = in.addresses.frameConstants + FrameSlotOffset(true, slot);
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
			// The pipeline table keeps its slots across frames; only the ones this frame's objects use
			// get rows (a swept or idle slot has no object pointing at it).
			if (!tables.PipelineUsed(p, frameNumber) || p >= lookups.pipelines.size())
				continue;
			const auto& entry = lookups.pipelines[p];
			if (entry.setIndex == Lookups::kNone || !(entry.key == tables.pipelines[p]))
				continue;
			blocks.setIndex = entry.setIndex;
			blocks.vsTable = entry.vsTable;
			blocks.psTable = entry.psTable;
			blocks.usage = &entry.usage[depthOnly ? kDepthVariant : kColorVariant];
			blocks.shadowMaskIndex = entry.shadowMaskIndex;
			blocks.tables = TablesHash(blocks.vsTable, blocks.psTable);
			WritePipelineRow(static_cast<std::uint32_t>(p), blocks);
			blocks.rowOk = rows.pipelines[p].blocksOk;
		}
		TracyCZoneEnd(pipelinesZone);
	}

	void MainBuild::WritePipelineRow(std::uint32_t p, const PipelineBlocks& blocks)
	{
		// What the row is written from: the pipeline's constants and permutation, its technique's constants and bindings (the
		// shadow mask's filter), its lookup entry (the tables, the shadow mask), and the shared lookups (the samplers).
		auto& state = rows.pipelines[p];
		const auto& techniqueRow = tables.TechniqueRowOf(p);
		const std::array<std::uint32_t, 6> key{ p < tables.pipelineConstantsVersion.size() ? tables.pipelineConstantsVersion[p] : 0u, techniqueRow.constantsVersion,
			techniqueRow.bindingVersion, lookups.pipelines[p].version, lookups.sharedVersion, 1u };
		if (state.written && state.key == key)
			return;
		const auto& technique = tables.TechniqueOf(p);
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
		pack(technique.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique], kPipelineRowTechniqueVS, kPipelineRowTechniqueBytes);
		pack(technique.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique], kPipelineRowTechniquePS, kPipelineRowTechniqueBytes);
		// The PerGeometry template: everything the objects do not override (they read theirs from the object record).
		GeometryTemplate geometry;
		PackGeometryTemplate(tables.geometryConstants[p], blocks.vsTable, blocks.psTable, true, geometry);
		if (geometry.vs.size() > kPipelineRowGeometryVSBytes || geometry.ps.size() > kPipelineRowGeometryPSBytes)
			ok = false;
		else {
			std::memcpy(row.bytes.data() + kPipelineRowGeometryVS, geometry.vs.data(), geometry.vs.size());
			std::memcpy(row.bytes.data() + kPipelineRowGeometryPS, geometry.ps.data(), geometry.ps.size());
		}
		// The permutation: SuppressExternalEmittance is the only per-object bit DCLF puts in this block, and it is read at
		// exactly one place in the whole shader tree - Effect.hlsl's GetLightingColor - never by Lighting.hlsl or anything it
		// includes: for these pipelines the block is the pipeline's alone.
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
		state.shadowMask = shadowMask != Lookups::kNone;
		state.shadowMaskSampler = sampler != Lookups::kNone;
		state.blocksOk = ok;
		state.key = key;
		state.written = true;
		if (rows.pipeline.Set(p, row))
			++out.pipelineRowsWritten;
		++rows.pipelinesWritten;
	}

	void MainBuild::BeginMarks()
	{
		// The per-object states are set parity's alone (CS_DCLF_SET_PARITY).
		if (SetParityEnabled())
			out.objectState.assign(std::min<std::size_t>(tables.objects.size(), kMaxObjects), kObjectStateAbsent);
		// The colour segment's drawn marks (DrawnMarks): kept across builds, sent as changes.
		marks = cache && !depthOnly ? &cache->drawnMarks : nullptr;
		if (marks) {
			auto& m = *marks;
			// What the render thread applied is the version it holds: what changes from here on is sent alone.
			m.changes.BeginBuild(in.drawnCommitted);
			if (!m.active || m.generation != in.tablesGeneration || in.drawnResync) {
				// Every slot again: the first build, new tables, or the render thread asked for it. Whatever the old marks
				// held is withdrawn by the full send, which covers every slot either knows. The journal counts on.
				const auto known = std::max(m.drawn.size(), tables.objects.size());
				auto changes = std::move(m.changes);
				m = {};
				m.changes = std::move(changes);
				m.changes.Resync();
				m.active = true;
				m.generation = in.tablesGeneration;
				m.drawn.assign(known, 0);
				m.geometry.assign(known, nullptr);
				if (cache->region.cursor.active)
					cache->region.Reset();  // its entries are marked as it reads them again
			}
			++m.serial;
		}
	}

	void MainBuild::PrepareObjects()
	{
		// The per-object record table the DCLF_BINDLESS builds read. It is indexed by the object's table
		// index, which the draw carries as its third root constant word, so it is filled for every
		// candidate rather than only the drawn ones - a candidate skipped here still reaches the
		// culling, and an index that addressed nothing would be worse than one that addresses a record
		// no draw reads. Kept across frames in the store (the records the change log names are written again).
		UpdateObjectRecords(objectStore, in.tablesHeld.objects, tables, in.tablesGeneration, frameNumber, out.objectRecords);

		// Everything before the loop: the per-epoch maps and the object records.
		Mark(6);
		if (!depthOnly) {
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				decalCount[group] = tables.decalCount[group];  // the sequence buffer's decal ranges hold them all (ReserveMainSequences)
				decalTemplates[group].resize(decalCount[group]);
			}
		}
	}

	bool MainBuild::DrewLastFrame(std::uint32_t o) const
	{
		return in.withholding ||
		       (in.drawnSlots && o < in.drawnSlots->size() && o < tables.objectGeometry.size() &&
				   (*in.drawnSlots)[o].DrewLast(tables.objectGeometry[o], in.frameNumber));
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
		// Per DRAW: the pair probe above, which every candidate pays whether or not it draws.
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
		// An input names its rows in one word (RowsOf): past its fields is a hard failure, like the sequence limits, until the
		// input gets a word of its own.
		if (m > kRowMaterialMask || p >= (1u << (32 - kRowPipelineShift)))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: material slot {} or pipeline slot {} past an input's row fields", m, p));
		auto fail = [&](Skip a_reason) { resolved.skipReason = static_cast<std::uint32_t>(a_reason); };
		Mark(0);
		WriteMaterialRow(m, p, blocks);
		const auto& material = rows.materials[m];
		const auto& pipeline = rows.pipelines[p];
		const auto& usage = *blocks.usage;
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
		// Textures: the material row's (t0-t15 but t14, the features'), the pipeline row's (t14), the frame record's (the rest).
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
			else if (t == kObjectBufferRegister || t == kBonesBufferRegister)
				given = true;
			else if (depthOnly)
				given = false;  // the Z-prepass has no frame textures bound yet
			else
				out.frameRegisters[t / 64] |= 1ull << (t % 64);  // the commit resolves it into the frame record
			if (!given) {
				resolved.missingTexture = t;
				return fail(Skip::Texture);
			}
		}
		for (std::uint32_t sampler = 0; sampler < kSamplerRegisters; ++sampler) {
			if (!((usage.samplers >> sampler) & 1))
				continue;
			const bool given = sampler == kShadowMaskSlot ? pipeline.shadowMaskSampler : ((material.samplers >> sampler) & 1) != 0;
			if (!given)
				return fail(Skip::Sampler);
		}
		// Constant buffers: the rows' (b0, b1, b2, b4), the pass's (the frame slots, PS b3 and b13).
		bool constantsOk = true;
		for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
			const bool rowRegister = r == kPerTechnique || r == kPerMaterial || r == kPerGeometry || r == 4;
			const bool rowsOk = r == kPerMaterial ? material.blocksOk : pipeline.blocksOk;
			if ((usage.vertexConstants >> r) & 1) {
				if (rowRegister ? !rowsOk : !frameVS[r]) {
					constantsOk = false;
					out.missingVertexConstants |= 1u << r;
				}
			}
			if ((usage.pixelConstants >> r) & 1) {
				const bool pass = r == 3 || r == kFrameLightingRegister;
				if (rowRegister ? !rowsOk : !(pass || framePS[r])) {
					constantsOk = false;
					out.missingPixelConstants |= 1u << r;
				}
			}
		}
		if (!constantsOk)
			return fail(Skip::Constants);
		resolved.recordIndex = RowsOf(p, m);
	}

	void MainBuild::WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks)
	{
		// What the row is written from: the material record and its frame values, its lookup entry (the descriptors), the
		// shared lookups (the null texture, the samplers, the projected textures), its technique's bindings (the filter
		// modes a material leaves unwritten) and the constant tables it is packed with.
		auto& state = rows.materials[m];
		const Lookups::Material* lookup = m < lookups.materials.size() ? &lookups.materials[m] : nullptr;
		const bool projected = (tables.pipelines[p].passDescriptor & 0x8000u) != 0;
		const std::array<std::uint32_t, 8> key{ m < tables.materialVersion.size() ? tables.materialVersion[m] : 0u,
			m < tables.materialFrameVersion.size() ? tables.materialFrameVersion[m] : 0u, lookup ? lookup->version : ~0u, lookups.sharedVersion,
			tables.TechniqueRowOf(p).bindingVersion, projected ? 1u : 0u, static_cast<std::uint32_t>(blocks.tables), static_cast<std::uint32_t>(blocks.tables >> 32) };
		if (state.written && state.key == key)
			return;
		const auto& material = tables.materials[m];
		const auto& technique = tables.TechniqueOf(p);
		MaterialRow row;
		state = {};
		// The PerMaterial blocks, packed through the technique's tables.
		const auto vsSize = ConstantGroupSize(LightingVSLayout(), blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial]);
		const auto psSize = ConstantGroupSize(LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial]);
		state.blocksOk = vsSize <= kMaterialRowVSBytes && psSize <= kMaterialRowPSBytes;
		if (state.blocksOk) {
			PackConstantGroup(material.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial],
				std::span(row.bytes.data() + kMaterialRowVS, kMaterialRowVSBytes));
			PackConstantGroup(material.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial],
				std::span(row.bytes.data() + kMaterialRowPS, kMaterialRowPSBytes));
		}
		auto& header = HeaderOf(row);
		header.vsMaterial = kMaterialRowVS;
		header.psMaterial = kMaterialRowPS;
		// The textures: the material's, a projected technique's projected ones, the null texture where neither binds one. The
		// shadow mask (t14) is the pipeline row's.
		const bool resolved = lookup && lookup->resolved && lookup->key.first == tables.materialSlotKey[m].first && lookup->key.second == tables.materialSlotKey[m].second;
		state.texturesOk = true;
		for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
			std::uint32_t index = lookups.nullTexture;
			if ((material.textureWritten >> t) & 1) {
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
		// The samplers: the modes the material sets (its technique's filter where it leaves it), a projected technique's.
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

	void MainBuild::CheckBindless(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks)
	{
		// CS_DCLF_BINDLESS_PARITY=1: the record the shaders read against the PerGeometry group the engine's constant buffer holds
		// for the same object (PatchObjectGeometry). The two derive from the same inputs through the same unwritten-component
		// rule, so anything but bit equality is a defect in the record's layout or in the way it is filled, caught on the CPU
		// with no readback.
		if (o >= objectRecords.Count())
			return;
		auto [templateIt, newTemplate] = geometryTemplates.try_emplace(object.pipelineIndex);
		auto& geometryTemplate = templateIt->second;
		if (newTemplate)
			PackGeometryTemplate(tables.geometryConstants[object.pipelineIndex], blocks.vsTable, blocks.psTable, true, geometryTemplate);
		parityVS.assign(geometryTemplate.vs.begin(), geometryTemplate.vs.end());
		parityPS.assign(geometryTemplate.ps.begin(), geometryTemplate.ps.end());
		PatchObjectGeometry(tables, o, renderFlags, eye, previousEye, geometryTemplate.offsets, parityVS, parityPS);
		IndirectDraws::Stats parityStats{};
		CheckBindlessRecord(tables, o, *objectRecords.At(o), eye, previousEye, geometryTemplate.offsets, parityVS, parityPS, parityStats);
		out.bindlessParityChecks += parityStats.bindlessParityChecks;
		out.bindlessParityMismatches += parityStats.bindlessParityMismatches;
	}

	// The resident region (BuildCache::ResidentRegion; drawcall-limit-fix.md, "Persistent resident draws"): the resident
	// objects' draw inputs, kept across frames and changed only by the tables' change log (Tables::changeLog), a
	// pipeline's set index or a pair's record failing. Its pairs' records sit at stable slots, assembled here once per
	// pair; the loop below skips its objects, and the commit uploads the inputs only when their version is new.
	void MainBuild::BuildResidentRegion()
	{
		region = cache && !BuildParityEnabled() ? &cache->region : nullptr;
		if (!region && cache && cache->region.cursor.active)
			cache->region.Reset();
		// The whole scene (every pair's rows are the scene's); the Z-prepass's only where its gate is not the colour epoch's last
		// frame (withholding): with that gate, what is not resident stays the loop's.
		wholeScene = region && (!depthOnly || in.withholding);
		TracyCZoneN(residentRegionZone, "CS.DCLF.BuildMain.ResidentRegion", true);
		if (region) {
			UpdateRegionEntries();
			UpdateRegionPairs();
			CheckRegionParity();
			RouteTouched();
			PublishRegion();
		}
		TracyCZoneEnd(residentRegionZone);
	}

	bool MainBuild::RegionEligible(std::uint32_t o) const
	{
		if (o >= tables.objects.size() || o >= kMaxObjects)
			return false;
		const bool resident = ResidentAt(o);
		if (!resident && !wholeScene)
			return false;
		const auto& object = tables.objects[o];
		if (object.flags & (kObjectFree | kObjectShadowOnly))
			return false;
		if (object.flags & kObjectNoBindings)
			return wholeScene && depthOnly && object.geometryIndex < tables.geometries.size();
		if ((resident && !(object.flags & kObjectNativeVisible)) || ObjectDecalGroup(object.flags))
			return false;
		if (object.pipelineIndex >= pipelineBlocks.size() || object.pipelineIndex >= tables.pipelines.size() || object.geometryIndex >= tables.geometries.size())
			return false;
		const auto& geometry = tables.geometries[object.geometryIndex];
		if (!geometry.vertexAddress || !geometry.indexAddress)
			return false;
		return !IsFaceObject(tables, o) && FaceStreamGeometry(tables, o, in.addresses.facePositions) == ~0u &&
		       !(tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream);
	}

	std::uint8_t MainBuild::RegionEntry(std::uint32_t o, DrawInput& a_input)
	{
		auto& r = *region;
		const auto& object = tables.objects[o];
		if (object.flags & kObjectNoBindings) {
			// A cull-only candidate: its bounds for the culling, nothing to draw (the input the loop writes for one).
			a_input = { 0, 0, object.geometryIndex, object.flags, { object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
				o, 0 };
			SetFadeRow(a_input, tables, o);
			return 0;
		}
		const auto& blocks = pipelineBlocks[object.pipelineIndex];
		const auto pair = r.pairs.find(PairKeyOf(object));
		const bool drawable = blocks.setIndex != Lookups::kNone && pair != r.pairs.end() && pair->second.ok;
		// The pair's slot is its rows (RowsOf).
		const std::uint32_t partitions = PartitionsOf(tables, o);
		a_input = { drawable ? blocks.setIndex : 0u, drawable ? pair->second.slot : 0u, object.geometryIndex, object.flags | (drawable ? kInputDrawable : 0u),
			{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, o, 0, partitions };
		if (depthOnly)
			SetFadeRow(a_input, tables, o);
		return drawable ? static_cast<std::uint8_t>(partitions ? std::popcount(partitions) : 1) : std::uint8_t{ 0 };
	}

	void MainBuild::RegionAcquire(std::uint64_t a_key)
	{
		auto& r = *region;
		auto [pair, fresh] = r.pairs.try_emplace(a_key);
		if (fresh) {
			// Its rows, once the check below (UpdateRegionPairs) finds the pair can draw.
			pair->second.slot = kNoRecord;
			pair->second.ok = false;
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
		if (const auto pair = r.pairs.find(a_key); pair != r.pairs.end() && --pair->second.count == 0)
			r.pairs.erase(pair);
		if (const auto state = r.pipelines.find(static_cast<std::uint32_t>(a_key)); state != r.pipelines.end() && --state->second.second == 0)
			r.pipelines.erase(state);
	}

	void MainBuild::RegionSetDraws(std::uint32_t i, std::uint8_t a_draws)
	{
		auto& r = *region;
		r.undrawable += (a_draws ? 0 : 1) - (r.drawsOf[i] ? 0 : 1);
		r.draws = r.draws - r.drawsOf[i] + a_draws;
		r.drawsOf[i] = a_draws;
	}

	void MainBuild::RegionRemove(std::uint32_t o)
	{
		auto& r = *region;
		const std::uint32_t i = r.EntryOf(o);
		if (i == kNoRegion)
			return;
		RegionRelease(r.pairOf[i]);
		r.touched.push_back(o);
		r.undrawable -= r.drawsOf[i] ? 0 : 1;
		r.draws -= r.drawsOf[i];
		// The entry's columns follow the entry the region moves into its place.
		const auto removal = r.Remove(o);
		r.pairOf[removal.at] = r.pairOf[removal.from];
		r.drawsOf[removal.at] = r.drawsOf[removal.from];
		r.pairOf.pop_back();
		r.drawsOf.pop_back();
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
		if (i == kNoRegion) {
			// Past the region's share of the inputs it stays with the loop. Its draws need no share (the sequence buffer holds every
			// draw the scene can produce), nor its pair a slot (the pair's rows are the scene's).
			const std::size_t inputLimit = wholeScene ? kMaxInputs - kLoopReserve : kMaxInputs / 2;
			if (r.inputs.Size() >= inputLimit)
				return;
			i = r.Add(o);
			r.pairOf.push_back(key);
			r.drawsOf.push_back(0);
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
		if (reuseKeptStorage)
			r.inputs.Set(i, fresh);
		else {
			r.inputs.Mutable()[i] = fresh;
			r.inputs.Mark(i);
		}
		r.touched.push_back(o);
	}

	void MainBuild::RegionTake(std::uint32_t o)
	{
		auto& r = *region;
		if (!ResidentAt(o) && !wholeScene)
			RegionRemove(o);
		else if (depthOnly && ResidentAt(o) && (o >= r.indexOf.size() || r.indexOf[o] == kNoRegion) && !DrewLastFrame(o))
			r.pending.Add(o);
		else
			RegionUpsert(o);
		r.touched.push_back(o);
	}

	void MainBuild::UpdateRegionEntries()
	{
		auto& r = *region;
		r.touched.clear();
		// What the buffer holds is the version it was sent: what changes from here on is sent alone.
		r.inputs.BeginBuild(in.residentUploaded);
		const bool resync = !r.cursor.Continues(tables.changeLog, in.tablesGeneration) || r.depth != depthOnly || r.indexOf.size() > tables.objects.size() ||
		                    r.wholeScene != wholeScene;
		if (resync) {
			// Every slot read again: the first build, new tables, or a log this segment fell behind.
			r.Reset();
			r.cursor.Restart(in.tablesGeneration);
			r.depth = depthOnly;
			r.wholeScene = wholeScene;
			r.indexOf.assign(tables.objects.size(), kNoRegion);
			++out.residentResyncs;
			for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
				RegionTake(o);
		} else {
			if (depthOnly) {
				// Joins the colour epoch has drawn since: their depth may be drawn now.
				for (std::size_t k = 0; k < r.pending.Size();) {
					const std::uint32_t slot = r.pending.list[k];
					if (slot >= tables.residentSlot.size() || !tables.residentSlot[slot] || (slot < r.indexOf.size() && r.indexOf[slot] != kNoRegion)) {
						r.pending.RemoveAt(k);
					} else if (DrewLastFrame(slot)) {
						r.pending.RemoveAt(k);
						RegionUpsert(slot);
						r.touched.push_back(slot);
					} else {
						++k;
					}
				}
			}
			// The log since this region's last build, whenever its changes were made.
			// What a draw input carries: its placement and fade row, its bindings, its geometry and partitions, its residency.
			constexpr std::uint32_t kInputCauses = kChangePlacement | kChangeBindings | kChangeSkin | kChangeGeometry | kChangeMembership;
			for (const auto& change : r.cursor.Unread(tables.changeLog))
				if (change.causes & kInputCauses)
					RegionTake(change.slot);
		}
		r.cursor.Advance(tables.changeLog);
	}

	void MainBuild::UpdateRegionPairs()
	{
		auto& r = *region;
		// A pipeline whose set index changed, and the pairs whose record could or could no longer be built: their
		// entries are written again (both rare).
		std::vector<std::uint32_t> changedPipelines;
		for (auto& [pipeline, state] : r.pipelines)
			if (pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != state.first) {
				state.first = pipelineBlocks[pipeline].setIndex;
				changedPipelines.push_back(pipeline);
			}
		std::vector<std::uint64_t> changedPairs;
		currentObject = ~0u;
		for (auto& [key, pair] : r.pairs) {
			const auto pipeline = static_cast<std::uint32_t>(key);
			bool ok = pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != Lookups::kNone;
			std::uint32_t slot = pair.slot;
			if (ok) {
				ObjectRecord object{};
				object.materialIndex = static_cast<std::uint32_t>(key >> 32);
				object.pipelineIndex = pipeline;
				slot = AssembleRecord(~0u, object, pipelineBlocks[pipeline]);
				ok = slot != kNoRecord;
			}
			if (ok != pair.ok || (ok && slot != pair.slot)) {
				pair.ok = ok;
				if (ok)
					pair.slot = slot;
				changedPairs.push_back(key);
			}
		}
		if (!changedPipelines.empty() || !changedPairs.empty()) {
			auto& inputs = r.inputs.Mutable();
			for (std::uint32_t i = 0; i < inputs.size(); ++i) {
				const auto key = r.pairOf[i];
				if (std::find(changedPairs.begin(), changedPairs.end(), key) == changedPairs.end() &&
					std::find(changedPipelines.begin(), changedPipelines.end(), static_cast<std::uint32_t>(key)) == changedPipelines.end())
					continue;
				RegionSetDraws(i, RegionEntry(inputs[i].objectIndex, inputs[i]));
				r.inputs.Mark(i);
				r.touched.push_back(inputs[i].objectIndex);
			}
		}
	}

	void MainBuild::CheckRegionParity()
	{
		auto& r = *region;
		// CS_DCLF_RESIDENT_DRAW_PARITY: every entry written again from the tables and compared, and every resident the
		// region should hold looked for.
		if (ResidentDrawParityEnabled() && ParityDue(frameNumber)) {
			const auto& inputs = r.inputs.Get();
			for (std::uint32_t i = 0; i < inputs.size(); ++i) {
				DrawInput expected;
				const std::uint32_t o = inputs[i].objectIndex;
				const bool known = o < tables.objects.size() && RegionEligible(o);
				const auto draws = known ? RegionEntry(o, expected) : std::uint8_t{ 0 };
				++out.residentParityChecks;
				if (!known || draws != r.drawsOf[i] || std::memcmp(&expected, &inputs[i], sizeof(DrawInput)) != 0 || r.indexOf[o] != i) {
					// The first few, with why.
					if (out.residentParityMismatches++ < 6)
						logger::info("[DCLF] {} region parity: entry {} object {}: {} (resident {}, flags {:#x} vs {:#x}, pipeline {} vs {}, record {} vs {}, draws {} vs {}, indexOf {})",
							depthOnly ? "depth" : "colour", i, o, !known ? "not eligible" : "differs", o < tables.residentSlot.size() ? tables.residentSlot[o] : 9,
							inputs[i].flags, expected.flags, inputs[i].pipelineIndex, expected.pipelineIndex, inputs[i].recordIndex, expected.recordIndex, r.drawsOf[i], draws,
							o < r.indexOf.size() ? r.indexOf[o] : ~0u);
				}
			}
			for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
				if (RegionEligible(o) && (o >= r.indexOf.size() || r.indexOf[o] == kNoRegion) && !r.pending.Contains(o))
					++out.residentMissing;
		}
	}

	void MainBuild::RouteTouched()
	{
		auto& r = *region;
		// Every slot this build touched: the region's, the loop's (loopList) or nobody's, and whether it is a candidate only.
		if (r.loopIndex.size() < tables.objects.size()) {
			r.loopIndex.resize(tables.objects.size(), kNoRegion);
			r.candidate.resize(tables.objects.size(), 0);
		}
		for (const std::uint32_t o : r.touched) {
			if (o >= r.loopIndex.size())
				continue;
			const auto flags = o < tables.objects.size() ? tables.objects[o].flags : kObjectFree;
			const bool inRegion = o < r.indexOf.size() && r.indexOf[o] != kNoRegion;
			const bool isCandidate = !(flags & kObjectFree) && (flags & (kObjectNoBindings | kObjectShadowOnly));
			bool loop = false;
			if (!inRegion && !(flags & (kObjectFree | kObjectShadowOnly))) {
				if (flags & kObjectNoBindings)
					loop = depthOnly;  // the depth segment's cull-only input, where the region has no room
				else
					loop = !(depthOnly && ObjectDecalGroup(flags));  // decals are the colour segment's alone
			}
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
		// What the region draws: the colour segment's marks for the slots this build touched (the rest are as they were),
		// and the build's per-object states for set parity alone.
		const auto& inputs = r.inputs.Get();
		if (marks)
			for (const std::uint32_t o : r.touched) {
				const bool on = o < r.indexOf.size() && r.indexOf[o] != kNoRegion && r.drawsOf[r.indexOf[o]] && NativeDrawn(o);
				marks->Set(o, on && o < tables.objectGeometry.size() ? tables.objectGeometry[o] : nullptr, on);
			}
		if (!out.objectState.empty()) {
			for (std::uint32_t i = 0; i < inputs.size(); ++i)
				if (inputs[i].objectIndex < out.objectState.size())
					out.objectState[inputs[i].objectIndex] =
						r.drawsOf[i] ? kObjectStateDrawable : static_cast<std::uint8_t>(r.pairOf[i] == kNoPair ? Skip::CandidateOnly : Skip::Pipeline);
			// The candidates nobody submits.
			for (std::uint32_t o = 0; o < r.candidate.size() && o < out.objectState.size(); ++o)
				if (r.candidate[o] && out.objectState[o] == kObjectStateAbsent)
					out.objectState[o] = static_cast<std::uint8_t>(Skip::CandidateOnly);
		}
		// Every candidate is one whether the loop sees it or not (the report's "candidate-only").
		out.skipped[static_cast<std::size_t>(Skip::CandidateOnly)] += static_cast<std::uint32_t>(r.candidates);
		out.residentUndrawable = static_cast<std::uint32_t>(r.undrawable);
		out.resident = r.inputs.View();
		out.residentDraws = static_cast<std::uint32_t>(r.draws);
		out.residentPairs = static_cast<std::uint32_t>(r.pairs.size());
		regionInputs = inputs.size();
		regionDraws = r.draws;
	}

	void MainBuild::RunObjectLoop()
	{
		TracyCZoneN(objectLoopZone, "CS.DCLF.BuildMain.ObjectLoop", true);
		// With a region, the loop visits only what the region leaves it (ResidentRegion::loopList); the candidates were
		// counted by the region.
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
		// A resident region's object: its draw input is the region's (above).
		if (region && o < region->indexOf.size() && region->indexOf[o] != kNoRegion)
			return;
		// A free slot is no object: not even a culling candidate.
		if (object.flags & kObjectFree)
			return;
		// A shadow-only record is no main-pass object at all, not even a culling candidate.
		if (object.flags & kObjectShadowOnly) {
			if (!region)
				Skipped(Skip::CandidateOnly);
			return;
		}
		if (object.flags & kObjectNoBindings) {
			// A culling candidate with no material or pipeline entry; its indices are meaningless.
			// The depth segment still submits it cull-only, with its bounds: that is what the tables
			// carry the whole tracked set for, and what the culling is measured against the engine
			// with.
			if (depthOnly && o < kMaxObjects && drawInputs.size() + regionInputs < kMaxInputs) {
				drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
					{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
					static_cast<std::uint32_t>(o), 0 });
				SetFadeRow(drawInputs.back(), tables, o);
			}
			if (!region)
				Skipped(Skip::CandidateOnly);
			return;
		}
		// Decals never reach the depth segment: they are not occluders, and they are drawn by the
		// colour segment's second pass (BuildDrawsCS.hlsl, MainOpaquePass::Record).
		const std::uint32_t decalGroup = ObjectDecalGroup(object.flags);
		if (decalGroup && (depthOnly || o >= tables.decalOrdinal.size() || tables.decalOrdinal[o] >= decalCount[(decalGroup - 1) & 1]))
			return;
		// A decal that cannot be drawn this epoch must still reach BuildDraws, so that its slot is
		// written as a zero-count draw rather than left holding whatever a previous frame put there.
		// This guard does that on every `continue` between here and the drawable push below.
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
			decalSlot.blank = { 0, 0, object.geometryIndex, object.flags,
				{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
				static_cast<std::uint32_t>(o), tables.decalOrdinal[o] };
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
		// The object's table index addresses its record and its visibility word, and it travels to
		// the shaders as the draw's third root constant. Past the table's capacity it addresses
		// neither, and robust buffer access turns that into zeros - a world matrix of zeros collapses
		// the object to a point at the eye with no other sign. Both caps come before the cull-only
		// push below, so no input reaches the buffer unchecked.
		if (o >= kMaxObjects || drawInputs.size() + regionInputs >= kMaxInputs) {
			Skipped(Skip::Capacity);
			return;
		}
		// The Z-prepass must write depth for exactly the objects the native loop is leaving to DCLF,
		// which is the set the colour epoch drew in the frame before (SkipNativePass uses the same
		// rule). Writing depth for anything else strands it: the colour epoch may not draw it, and
		// the native draw that would have cannot either, because its own EQUAL test now compares
		// against a depth DCLF computed rather than the one the native prepass wrote. Such an object
		// keeps its depth but is never shaded, which is what left the architecture flat and grey.
		if (depthOnly && !DrewLastFrame(o)) {
			// Cull-only: the object still goes to the culling, because the depth segment is where the
			// verdict for every candidate is decided and published, and a candidate left out here
			// would reach the colour segment with no verdict at all. What it does not get is a
			// bindings record, which is the expensive part and the only part a draw needs.
			drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
				{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
				static_cast<std::uint32_t>(o), 0 });
			SetFadeRow(drawInputs.back(), tables, o);
			Skipped(Skip::NotSkippedNatively);
			return;
		}
		// A skin of several partitions writes one sequence per partition drawn. A decal has one slot,
		// so a decal that is such a skin cannot be drawn by the decal pass.
		const std::uint32_t partitions = PartitionsOf(tables, o);
		if (decalGroup && partitions) {
			Skipped(Skip::Geometry);
			return;
		}
		// A face shape draws only with its positions as the second stream, and a pipeline that reads its
		// position from the second stream only with them: the slot would fall back to the geometry's own
		// buffer and draw its other attributes as positions.
		const std::uint32_t streamIndex = FaceStreamGeometry(tables, o, in.addresses.facePositions);
		if (streamIndex == ~0u && (IsFaceObject(tables, o) || (tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream))) {
			Skipped(Skip::Geometry);
			return;
		}
		// No draw cap: the sequence buffer's draws range holds every draw the scene can produce (ReserveMainSequences).
		const std::uint32_t recordIndex = AssembleRecord(o, object, blocks);
		if (recordIndex == kNoRecord)
			return;
		if (bindlessParity)
			CheckBindless(o, object, blocks);
		// The CPU template of what BuildDraws writes (checked with CS_DCLF_BUILD_PARITY).
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
			drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex,
				object.flags | kInputDrawable,
				{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
				static_cast<std::uint32_t>(o), ordinal, 0, streamIndex });
			decalTemplates[(decalGroup - 1) & 1][ordinal] = sequence;
			++out.decalsDrawn;
			if (o < out.objectState.size())
				out.objectState[o] = kObjectStateDecal;
		} else {
			if (o < out.objectState.size())
				out.objectState[o] = kObjectStateDrawable;
			drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex,
				object.flags | kInputDrawable,
				{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
				static_cast<std::uint32_t>(o), 0, partitions, streamIndex });
			if (depthOnly)
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
		// Only what BuildDraws will actually write a sequence for counts as drawn. The tables now hold
		// the whole tracked set, so a candidate the gate drops must not be recorded here: the native
		// loop would skip its pass (it has none while the engine culls it, but it regains one the
		// moment the engine sees it again) and, worse, the Z-prepass draws exactly what the colour
		// epoch drew last frame, so a stale mark would write depth for an object nothing then shades.
		// The gate is a per-object flag test and so is predictable here; frustum rejection is not,
		// which is what the false-negative counter exists to catch.
		if (marks && o < tables.objectGeometry.size() && NativeDrawn(o)) {
			marks->Set(o, tables.objectGeometry[o], true);
			if (marks->loopStamp.size() <= o)
				marks->loopStamp.resize(std::size_t(o) + 1, 0);
			marks->loopStamp[o] = marks->serial;
			loopDrawn.push_back(o);
		}
		// The indirect draw fetches vertices and indices through the buffer's device address and size:
		// a slice that does not cover the draw reads zeros, and the object collapses without any
		// other sign. Checked here because nothing on the D3D11 side sees the Vulkan slice.
		const std::uint64_t vertexNeeded = std::uint64_t(geometry.vertexCount) * geometry.vertexStride;
		const std::uint64_t indexNeeded = (std::uint64_t(geometry.firstIndex) + geometry.indexCount) * sizeof(std::uint16_t);
		if (geometry.vertexBytes < vertexNeeded || geometry.indexBytes < indexNeeded) {
			if (out.shortBuffers++ == 0)
				out.shortBuffer = { o, vertexNeeded, indexNeeded };
		}
		// Per DRAW: the sequence, the draw input, the drawn mark and the slice check.
		Mark(3);
	}

	void MainBuild::FinishMarks()
	{
		// The loop's marks: what it drew last build and not now is not drawn, unless the region draws it.
		if (marks) {
			auto& m = *marks;
			for (const std::uint32_t o : m.loopDrawn) {
				if (o < m.loopStamp.size() && m.loopStamp[o] == m.serial)
					continue;
				const auto* regionOf = region && o < region->indexOf.size() && region->indexOf[o] != kNoRegion ? region : nullptr;
				if (regionOf && regionOf->drawsOf[regionOf->indexOf[o]] && NativeDrawn(o))
					continue;
				m.Set(o, nullptr, false);
			}
			m.loopDrawn = std::move(loopDrawn);
			// The changes since the version the render thread applied, or every slot when it holds nothing the journal can
			// build on.
			const auto snapshot = m.changes.Take();
			const std::uint64_t held = in.drawnCommitted;
			out.drawnValid = true;
			out.drawnFull = held < snapshot.floor || held > snapshot.version;
			out.drawnVersion = snapshot.version;
			out.drawnBase = held;
			snapshot.ForEachRun(held, m.drawn.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				for (auto slot = static_cast<std::uint32_t>(a_first); slot < a_first + a_count; ++slot)
					out.drawnChanges.push_back({ slot, m.geometry[slot], m.drawn[slot] != 0 });
			});
		}
	}

	void MainBuild::Finish()
	{
		UpdateGeometryDraws(geometryStore, in.tablesHeld.geometries, tables, in.tablesGeneration, frameNumber, out.geometryDraws);
		AppendFaceStreams(tables, in.addresses.facePositions, out.geometryDraws);
		if (in.addresses.facePositions)
			out.faceStreams = tables.faceStreams;
		UpdateBones(boneStore, in.tablesHeld.bones, tables, in.tablesGeneration, out.bones);
		out.materialRows = rows.material.View();
		out.pipelineRows = rows.pipeline.View();
		Mark(5);
	}

	void BuildMainPayload(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out, MainRows& a_rows,
		BuildCache* a_cache, ObjectRecordStore* a_objects, BonesStore* a_bones, GeometryStore* a_geometries)
	{
		MainBuild(a_in, a_tables, a_lookups, a_out, a_rows, a_cache, a_objects, a_bones, a_geometries).Run();
	}
}

#endif
