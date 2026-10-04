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
		 * Run calls the parts in order: the pipelines' blocks, the per-object states, the object records, the resident region,
		 * the object loop (each draw's record through AssembleRecord), then the kept stores. Both segments draw the frame's DCLF
		 * set (kObjectMember) and nothing else: every other object is the engine's in the main camera's views.
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

			decltype(MainPayload::sequences)& sequences = out.sequences;
			decltype(MainPayload::inputList)& drawInputs = out.inputList;
			decltype(MainPayload::decalCount)& decalCount = out.decalCount;
			decltype(MainPayload::decalTemplates)& decalTemplates = out.decalTemplates;
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
			void BeginStates();
			void PrepareObjects();
			void Skipped(Skip a_reason);
			void Mark(std::size_t a_part);
			// In the frame's set (SceneStore::CommitSet): the only objects either segment draws.
			bool InSet(std::uint32_t o) const { return o < tables.objects.size() && (tables.objects[o].flags & kObjectMember); }

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
			// The input the loop would write for it, drawable while it is in the set and its pair's record is built.
			std::uint8_t RegionEntry(std::uint32_t o, DrawInput& a_input);
			void RegionAcquire(std::uint64_t a_key);
			void RegionRelease(std::uint64_t a_key);
			// An entry's draws, with the region's totals.
			void RegionSetDraws(std::uint32_t i, std::uint8_t a_draws);
			void RegionRemove(std::uint32_t o);
			void RegionUpsert(std::uint32_t o);
			void RegionTake(std::uint32_t o);
			/** @brief The entries the change log names since the region's last build, or every slot on a resync. */
			void UpdateRegionEntries();
			/** @brief The entries of a pipeline whose set index changed and of a pair whose record could or could no longer be built. */
			void UpdateRegionPairs();
			std::uint64_t PipelineWitness(std::uint32_t p) const;
			void CheckRegionParity();
			/** @brief Every slot this build touched: the region's, the loop's (loopList) or nobody's, and whether it is a candidate only. */
			void RouteTouched();
			/** @brief What the region draws, into the payload. */
			void PublishRegion();

			void RunObjectLoop();
			/** @brief One object of the loop: its draw input, and its sequence and record when it is drawn. */
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
			if (!tables.PipelineUsed(p) || p >= lookups.pipelines.size())
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
		const std::uint64_t vertexLayout = tables.pipelines[p].vertexLayout;
		const std::array<std::uint32_t, 8> key{ p < tables.pipelineConstantsVersion.size() ? tables.pipelineConstantsVersion[p] : 0u, techniqueRow.constantsVersion,
			techniqueRow.bindingVersion, lookups.pipelines[p].version, lookups.sharedVersion, 1u, static_cast<std::uint32_t>(vertexLayout),
			static_cast<std::uint32_t>(vertexLayout >> 32) };
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
		// The per-object states are set parity's alone (CS_DCLF_SET_PARITY).
		if (SetParityEnabled())
			out.objectState.assign(tables.objects.size(), kObjectStateAbsent);
	}

	void MainBuild::PrepareObjects()
	{
		// The per-object records and the palettes are the commit's (CommitSceneStreams), not the build's: they change every
		// frame, and the build only with the scene's structure.

		// Everything before the loop: the per-epoch maps and the object records.
		Mark(6);
		if (!depthOnly) {
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				decalCount[group] = tables.decalCount[group];  // the sequence buffer's decal ranges hold them all (ReserveMainSequences)
				decalTemplates[group].resize(decalCount[group]);
			}
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
			else if (t == kObjectBufferRegister || t == kBonesBufferRegister || t == kTreeWindRegister)
				given = true;
			else if (depthOnly)
				// The Z-prepass has no frame textures bound yet (its frame record binds the null texture), but for the character
				// light's noise, which only shades: the depth variant's output never reads it.
				given = t == kCharacterLightRegister;
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
		// A character-light pass's t11 is the frame's (kCharacterLightRegister, which its shaders read instead): the null texture.
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
		if (o >= tables.objects.size())
			return;
		auto [templateIt, newTemplate] = geometryTemplates.try_emplace(object.pipelineIndex);
		auto& geometryTemplate = templateIt->second;
		if (newTemplate)
			PackGeometryTemplate(tables.geometryConstants[object.pipelineIndex], blocks.vsTable, blocks.psTable, true, geometryTemplate);
		parityVS.assign(geometryTemplate.vs.begin(), geometryTemplate.vs.end());
		parityPS.assign(geometryTemplate.ps.begin(), geometryTemplate.ps.end());
		PatchObjectGeometry(tables, o, renderFlags, eye, previousEye, geometryTemplate.offsets, parityVS, parityPS);
		IndirectDraws::Stats parityStats{};
		BindlessObject record;
		BuildObjectRecord(tables, o, SceneStore::kMainPassRenderFlags, record);
		CheckBindlessRecord(tables, o, record, eye, previousEye, geometryTemplate.offsets, parityVS, parityPS, parityStats);
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
		// The whole scene (every pair's rows are the scene's), in both segments: the two draw the same set.
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
			CheckRegionParity();
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
		if (object.flags & kObjectNoBindings)
			return wholeScene && depthOnly && object.geometryIndex < tables.geometries.size();
		if (object.pipelineIndex >= pipelineBlocks.size() || object.pipelineIndex >= tables.pipelines.size() || object.geometryIndex >= tables.geometries.size())
			return false;
		const auto& geometry = tables.geometries[object.geometryIndex];
		if (!geometry.vertexAddress || !geometry.indexAddress)
			return false;
		// A decal: the colour segment's alone, with its ordinal in its group's range (OrderDecals, which logs a change for every
		// decal it moves). One of several partitions is an entry too, never drawable (RegionEntry).
		if (const std::uint32_t group = ObjectDecalGroup(object.flags))
			if (depthOnly || o >= tables.decalOrdinal.size() || tables.decalOrdinal[o] >= tables.decalCount[group - 1])
				return false;
		// A face shape, or a pipeline reading its position from the second stream: only with its positions' stream.
		const bool needsStream = IsFaceObject(tables, o) || (tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream);
		return !needsStream || FaceStreamGeometry(tables, o, in.addresses.facePositions) != ~0u;
	}

	std::uint8_t MainBuild::RegionEntry(std::uint32_t o, DrawInput& a_input)
	{
		auto& r = *region;
		const auto& object = tables.objects[o];
		if (object.flags & kObjectNoBindings) {
			// A cull-only candidate: its bounds for the culling, nothing to draw (the input the loop writes for one).
			a_input = { 0, 0, object.geometryIndex, object.flags, {}, 0.0f,
				o, 0 };
			SetFadeRow(a_input, tables, o);
			return 0;
		}
		const auto& blocks = pipelineBlocks[object.pipelineIndex];
		const auto pair = r.pairs.find(PairKeyOf(object));
		// A decal's slot in its group's range, which holds one sequence: one of several partitions cannot draw there. A decal it
		// cannot draw is still an input, which BuildDraws writes as a zero-count draw (the loop's blank).
		const bool decal = ObjectDecalGroup(object.flags) != 0;
		const std::uint32_t ordinal = decal ? tables.decalOrdinal[o] : 0u;
		const std::uint32_t partitions = decal ? 0u : PartitionsOf(tables, o);
		const bool drawable = InSet(o) && blocks.setIndex != Lookups::kNone && pair != r.pairs.end() && pair->second.ok && !(decal && PartitionsOf(tables, o));
		// The pair's slot is its rows (RowsOf).
		a_input = { drawable ? blocks.setIndex : 0u, drawable ? pair->second.slot : 0u, object.geometryIndex, object.flags | (drawable ? kInputDrawable : 0u),
			{}, 0.0f, o, ordinal, partitions, FaceStreamGeometry(tables, o, in.addresses.facePositions) };
		if (depthOnly)
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
			// Its rows, once the check below (UpdateRegionPairs) finds the pair can draw.
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

	void MainBuild::RegionRemove(std::uint32_t o)
	{
		auto& r = *region;
		const std::uint32_t i = r.EntryOf(o);
		if (i == kNoRegion)
			return;
		RegionRelease(r.pairOf[i]);
		r.touched.push_back(o);
		r.undrawable -= r.drawsOf[i] ? 0 : 1;
		r.draws -= RegionDraws(r.drawsOf[i]);
		r.decals -= r.drawsOf[i] == kRegionDecal ? 1 : 0;
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
			// No share to stay within: the region and the loop together have at most an input per object, which the input buffer
			// holds (ReserveSceneTables), and the sequence buffer holds every draw the scene can produce.
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
			// The log since this region's last build, whenever its changes were made.
			// What a draw input carries: its fade distance and bindings, its geometry and partitions, its residency. Not its
			// placement: the bound, the sun entry and the fade node are its object record's.
			constexpr std::uint32_t kInputCauses = kChangeBindings | kChangeSkin | kChangeGeometry | kChangeMembership;
			for (const auto& change : r.cursor.Unread(tables.changeLog))
				if (change.causes & kInputCauses)
					RegionTake(change.slot);
			// A face shape's stream index counts from the geometry slots (FaceStreamGeometry): when their count moves, or the
			// positions buffer comes or goes, every face entry again.
			if (const std::size_t base = in.addresses.facePositions ? tables.geometries.size() : 0; base != r.faceBase) {
				for (const auto& stream : tables.faceStreams)
					if (stream.object != SceneStore::Tables::kNoFaceObject && stream.object < tables.objects.size())
						RegionTake(stream.object);
			}
		}
		r.faceBase = in.addresses.facePositions ? tables.geometries.size() : 0;
		r.cursor.Advance(tables.changeLog);
	}

	std::uint64_t MainBuild::PipelineWitness(std::uint32_t p) const
	{
		// What ResolvePair reads of the pipeline: the technique's bindings, whether it is projected, the constant tables, the set,
		// and the pipeline row as this build wrote it (PackPipelines). The material's half is three versions (UpdateRegionPairs).
		std::uint64_t value = 0xcbf29ce484222325ull;
		auto mix = [&](std::uint64_t a_word) { value = (value ^ a_word) * 0x100000001b3ull; };
		const auto& blocks = pipelineBlocks[p];
		mix(lookups.sharedVersion);
		mix(tables.TechniqueRowOf(p).bindingVersion);
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
		// A pipeline whose set index changed, and the pairs whose record could or could no longer be built: their
		// entries are written again (both rare).
		std::vector<std::uint32_t> changedPipelines;
		for (auto& [pipeline, state] : r.pipelines)
			if (pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != state.first) {
				state.first = pipelineBlocks[pipeline].setIndex;
				changedPipelines.push_back(pipeline);
			}
		// A pair is resolved again only when what its resolution reads changed (PairWitness): a material written or its
		// textures resolved, a pipeline's rows or set, the frame slots bound. The rest keep their verdict, their frame textures
		// and their owners (in the region's bundle). Which pairs could have changed follows from events: the material logs, the
		// pairs new since the last build, the pipelines whose half moved; everything when the frame's half moved or a log
		// cannot be read on.
		const std::uint64_t frameWitness = (std::uint64_t(in.vsFrameMask) << 32) ^ std::uint64_t(in.psFrameMask) ^ (depthOnly ? (1ull << 63) : 0ull) ^ 1ull;
		bool everyPair = frameWitness != r.frameWitness;
		r.frameWitness = frameWitness;
		if (!r.materialCursor.Continues(tables.materialLog, in.tablesGeneration) || !r.lookupCursor.Continues(lookups.materialLog, lookups.logGeneration)) {
			everyPair = true;
			r.materialCursor.Restart(in.tablesGeneration);
			r.lookupCursor.Restart(lookups.logGeneration);
		}
		std::vector<std::uint64_t> changedPairs;
		currentObject = ~0u;
		// The pipelines' halves once per build; a pair's own half is its material's three versions.
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
			// The bundle is made again only when what it holds for this pair moved.
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
		// The pairs that cannot draw, as the report counts them every build.
		for (std::size_t reason = 0; reason < r.skipCounts.size(); ++reason)
			out.skipped[reason] += r.skipCounts[reason];
		// Every pair's frame textures and owners, made again when one was resolved (or one left: RegionRelease).
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
				if (RegionEligible(o) && (o >= r.indexOf.size() || r.indexOf[o] == kNoRegion))
					++out.residentMissing;
			// Every pair's witness as UpdateRegionPairs makes it, against the one it holds: a difference is a change no event named.
			for (const auto& [key, pair] : r.pairs) {
				const auto pipeline = static_cast<std::uint32_t>(key);
				const auto material = static_cast<std::uint32_t>(key >> 32);
				if (pipeline >= pipelineBlocks.size() || pipelineBlocks[pipeline].setIndex == Lookups::kNone)
					continue;
				std::uint64_t witness = PipelineWitness(pipeline);
				witness = (witness ^ (material < tables.materialVersion.size() ? tables.materialVersion[material] : 0u)) * 0x100000001b3ull;
				witness = (witness ^ (material < tables.materialFrameVersion.size() ? tables.materialFrameVersion[material] : 0u)) * 0x100000001b3ull;
				witness = (witness ^ (material < lookups.materialVersions.size() ? lookups.materialVersions[material] : ~0u)) * 0x100000001b3ull;
				witness |= 1;
				++out.residentPairsChecked;
				out.residentPairsStale += witness != pair.witness ? 1 : 0;
			}
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
		// What the region draws: the build's per-object states, for set parity alone.
		const auto& inputs = r.inputs.Get();
		if (!out.objectState.empty()) {
			for (std::uint32_t i = 0; i < inputs.size(); ++i)
				if (inputs[i].objectIndex < out.objectState.size())
					out.objectState[inputs[i].objectIndex] = r.drawsOf[i] == kRegionDecal ? kObjectStateDecal :
					                                         r.drawsOf[i]                  ? kObjectStateDrawable :
					                                         r.pairOf[i] == kNoPair        ? static_cast<std::uint8_t>(Skip::CandidateOnly) :
					                                         !InSet(inputs[i].objectIndex) ? static_cast<std::uint8_t>(Skip::NotInSet) :
					                                                                         static_cast<std::uint8_t>(Skip::Pipeline);
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
		out.decalsDrawn += static_cast<std::uint32_t>(r.decals);
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
			if (depthOnly) {
				drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
					{}, 0.0f,
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
		if (decalGroup && (depthOnly || o >= tables.decalOrdinal.size() || tables.decalOrdinal[o] >= decalCount[decalGroup - 1]))
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
				{}, 0.0f,
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
		// Both segments draw the frame's set and nothing else: an object outside it is the engine's, which draws its depth and
		// its colour. Writing depth for one would strand it - the engine's draw tests EQUAL against the depth its own
		// prepass wrote - and a member is drawn by both segments or neither.
		if (!InSet(o)) {
			// Cull-only: the object still goes to the culling, because the depth segment is where the verdict for every
			// candidate is decided and published, and a candidate left out here would reach the colour segment with no
			// verdict at all.
			if (depthOnly) {
				drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
					{}, 0.0f,
					static_cast<std::uint32_t>(o), 0 });
				SetFadeRow(drawInputs.back(), tables, o);
			}
			Skipped(Skip::NotInSet);
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
				{}, 0.0f,
				static_cast<std::uint32_t>(o), ordinal, 0, streamIndex });
			decalTemplates[decalGroup - 1][ordinal] = sequence;
			++out.decalsDrawn;
			if (o < out.objectState.size())
				out.objectState[o] = kObjectStateDecal;
		} else {
			if (o < out.objectState.size())
				out.objectState[o] = kObjectStateDrawable;
			drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex,
				object.flags | kInputDrawable,
				{}, 0.0f,
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
		// The indirect draw fetches vertices and indices through the buffer's device address and size:
		// a slice that does not cover the draw reads zeros, and the object collapses without any
		// other sign. Checked here because nothing on the D3D11 side sees the Vulkan slice.
		const std::uint64_t vertexNeeded = std::uint64_t(geometry.vertexCount) * geometry.vertexStride;
		const std::uint64_t indexNeeded = (std::uint64_t(geometry.firstIndex) + geometry.indexCount) * sizeof(std::uint16_t);
		if (geometry.vertexBytes < vertexNeeded || geometry.indexBytes < indexNeeded) {
			if (out.shortBuffers++ == 0)
				out.shortBuffer = { o, vertexNeeded, indexNeeded };
		}
		// Per DRAW: the sequence, the draw input and the slice check.
		Mark(3);
	}

	void MainBuild::Finish()
	{
		UpdateGeometryDraws(geometryStore, in.tablesHeld.geometries, tables, in.tablesGeneration, frameNumber, out.geometryDraws);
		AppendFaceStreams(tables, in.addresses.facePositions, out.geometryDraws);
		if (in.addresses.facePositions)
			out.faceStreams = tables.faceStreams;
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
