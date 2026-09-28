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
			std::uint64_t techniqueVS = 0, techniquePS = 0;
		};

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
			MainBuild(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out, BuildCache* a_cache,
				ObjectRecordStore* a_objects, BonesStore* a_bones, GeometryStore* a_geometries) :
				in(a_in), tables(a_tables), lookups(a_lookups), out(a_out), cache(a_cache), objectStore(a_objects), boneStore(a_bones), geometryStore(a_geometries)
			{}

			void Run();

		private:
			const MainInputs& in;
			const SceneStore::Tables& tables;
			const Lookups& lookups;
			MainPayload& out;
			BuildCache* cache;
			ObjectRecordStore* objectStore;
			BonesStore* boneStore;
			GeometryStore* geometryStore;

			decltype(MainPayload::arena)& arena = out.arena;
			decltype(MainPayload::records)& records = out.records;
			decltype(MainPayload::sequences)& sequences = out.sequences;
			decltype(MainPayload::inputList)& drawInputs = out.inputList;
			decltype(MainPayload::decalCount)& decalCount = out.decalCount;
			decltype(MainPayload::decalTemplates)& decalTemplates = out.decalTemplates;
			const decltype(MainPayload::objectRecords)& objectRecords = out.objectRecords;
			const bool depthOnly = in.depthOnly;
			const std::uint64_t base = in.addresses.constants;
			const std::uint32_t frameNumber = in.frameNumber;
			// The segment's constant blocks and binding records kept across frames (PersistentBindings), except under the build
			// parity, which reads the per-build layout.
			const bool persistent = cache && !BuildParityEnabled();
			PersistentBindings* kept = persistent ? &cache->persistent : nullptr;
			// The record is identical for every draw of a (material, pipeline) pair: everything per object is in the object
			// record the shaders read by the draw's index (DCLF_BINDLESS, DCLF_BINDLESS_DRAW).
			const bool dedupParity = in.dedupParity;
			const bool bindlessParity = in.bindlessParity;
			const decltype(MainInputs::eye)& eye = in.eye;
			const decltype(MainInputs::previousEye)& previousEye = in.previousEye;
			static constexpr auto renderFlags = SceneStore::kMainPassRenderFlags;

			// The frame registers' blocks (FrameRegisters).
			std::array<std::uint64_t, kConstantBufferRegisters> frameVS{}, framePS{};
			std::uint64_t sharedLightBlock = 0, frameLightingBlock = 0;
			// Blocks shared by many objects: per pipeline, and per (material, pipeline) pair.
			std::vector<PipelineBlocks> pipelineBlocks;
			ankerl::unordered_dense::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> materialBlocks;  // (material, pipeline) -> VS, PS
			// Resolved descriptor heap indices per (material, pipeline) pair (ResolvedBindings).
			ankerl::unordered_dense::map<std::uint64_t, ResolvedBindings> resolvedBindings;  // (material, pipeline)
			ankerl::unordered_dense::map<std::uint32_t, GeometryTemplate> geometryTemplates;  // pipeline
			ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> permutationBlocks;  // (pipeline, extra bits)
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
			std::uint64_t Block(const void* a_data, std::size_t a_size);
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

			// The (material, pipeline) pair's bindings record, assembled once per build:
			// its index, or kNoRecord after skip() recorded why. At a_slot when the pair has a stable slot in the resident
			// region (below), appended otherwise.
			std::uint32_t AssembleRecord(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks, std::uint32_t a_slot);
			/** @brief A pair whose record cannot be built: why, per draw, and its kept entry no longer clean. */
			void FailPair(ResolvedBindings& resolved, const ObjectRecord& object, Skip a_reason);
			/** @brief The pair's resolution from its cache entry, when everything it is derived from is unchanged. */
			bool PairFromCache(std::uint64_t pairKey, const ObjectRecord& object, const PipelineBlocks& blocks, bool projectedPipeline, ResolvedBindings& resolved);
			/** @brief The pair's texture and sampler heap indices, into its cache entry too. */
			void ResolvePair(std::uint64_t pairKey, const ObjectRecord& object, const PipelineBlocks& blocks, bool projectedPipeline, ResolvedBindings& resolved);
			/** @brief The pair's PerMaterial blocks (VS, PS). */
			const std::pair<std::uint64_t, std::uint64_t>& MaterialBlocksOf(std::uint64_t pairKey, const MaterialRecord& material, const PipelineBlocks& blocks);
			/** @brief The pipeline's PerGeometry template blocks (VS, PS). */
			std::pair<std::uint64_t, std::uint64_t> GeometryBlocksOf(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks);
			/** @brief The pipeline's permutation block. */
			std::uint64_t PermutationBlockOf(const ObjectRecord& object);

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
		if (cache && (in.frameNumber % 64) == 0)
			cache->Sweep(in.frameNumber);
		out.Reset();
		out.inputs = in;
		if (lookups.sharedBindingBlock)
			out.bindingOwners.push_back(lookups.sharedBindingBlock);
		arena.Reset(depthOnly ? kDepthConstantBytes : kConstantBytes);
		if (kept)
			cache->BeginPersistent(in.addresses, depthOnly ? kDepthConstantBytes : kConstantBytes, in.constantsUploaded, in.recordsUploaded);
		TracyCZoneEnd(resetMainZone);
	}

	std::uint64_t MainBuild::Block(const void* a_data, std::size_t a_size)
	{
		const auto offset = arena.Allocate(a_size);
		if (offset == ~0ull)
			return 0;
		if (a_data)
			std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
		return base + offset;
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
			// get blocks (a swept or idle slot has no object pointing at it).
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
			const auto& technique = tables.TechniqueOf(p);
			const std::uint32_t techniqueVersion = tables.TechniqueRowOf(p).constantsVersion;
			BuildCache::Pipeline* cached = nullptr;
			// Kept and clean: the constants and the lookup entry are the versions its blocks were written from.
			if (kept) {
				auto& entryKept = cache->pipelines[static_cast<std::uint32_t>(p)];
				entryKept.lastUsed = frameNumber;
				const std::uint32_t constantsVersion = p < tables.pipelineConstantsVersion.size() ? tables.pipelineConstantsVersion[p] : 0u;
				if (entryKept.clean && entryKept.constantsVersion == constantsVersion && entryKept.techniqueVersion == techniqueVersion &&
					entryKept.lookupVersion == entry.version &&
					entryKept.techniqueVSBlock.offset != ~0ull && entryKept.techniquePSBlock.offset != ~0ull) {
					blocks.techniqueVS = kept->constantsBase + entryKept.techniqueVSBlock.offset;
					blocks.techniquePS = kept->constantsBase + entryKept.techniquePSBlock.offset;
					++cache->persistentCleanPipelines;
					continue;
				}
				++cache->persistentDirtyPipelines;
			}
			if (cache) {
				// Everything the pipeline's technique groups and PerGeometry template are packed from.
				auto& sources = cache->scratch;
				sources.clear();
				AppendSource(sources, technique.vs.floats);
				AppendSource(sources, technique.ps.floats);
				AppendSource(sources, tables.geometryConstants[p].vs.floats);
				AppendSource(sources, tables.geometryConstants[p].ps.floats);
				AppendSource(sources, blocks.vsTable);
				AppendSource(sources, blocks.psTable);
				cached = &cache->pipelines[static_cast<std::uint32_t>(p)];
				cached->lastUsed = frameNumber;
				if (SameSources(cached->sources, sources)) {
					++cache->pipelineHits;
				} else {
					++cache->pipelineMisses;
					cached->techniqueVS.valid = cached->techniquePS.valid = cached->hasGeometry = false;
				}
			}
			auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
							BuildCache::PackedGroup* a_packed, PersistentBlock* a_kept) {
				if (kept && a_packed && a_kept) {
					// Kept: packed only when the sources changed, written only when the bytes did.
					if (!a_packed->valid)
						PackGroupInto(a_block, a_layout, a_table, a_variables, a_first, *a_packed);
					return kept->Place(*a_kept, a_packed->bytes.data(), a_packed->bytes.size(), a_packed->size);
				}
				if (a_packed && a_packed->valid) {
					const auto address = Block(nullptr, a_packed->size);
					if (address)
						std::memcpy(arena.At(address - base, a_packed->bytes.size()).data(), a_packed->bytes.data(), a_packed->bytes.size());
					return address;
				}
				const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
				const auto address = Block(nullptr, size);
				if (address) {
					const auto slice = arena.At(address - base, std::max<std::size_t>(size, 16));
					PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, slice);
					if (a_packed) {
						a_packed->size = size;
						a_packed->bytes.assign(slice.begin(), slice.end());
						a_packed->valid = true;
					}
				}
				return address;
			};
			const std::array<std::uint64_t, 5> offsetsBefore = cached ? std::array<std::uint64_t, 5>{ cached->techniqueVSBlock.offset, cached->techniquePSBlock.offset,
				cached->geometryVSBlock.offset, cached->geometryPSBlock.offset, cached->permutationBlock.offset } : std::array<std::uint64_t, 5>{};
			blocks.techniqueVS = pack(technique.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique],
				cached ? &cached->techniqueVS : nullptr, cached ? &cached->techniqueVSBlock : nullptr);
			blocks.techniquePS = pack(technique.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique],
				cached ? &cached->techniquePS : nullptr, cached ? &cached->techniquePSBlock : nullptr);
			if (kept && cached) {
				// The PerGeometry template and the permutation too: its pairs' records name these blocks, and a clean pair
				// is not looked at again, so they are brought up to date here, with the pipeline's constants.
				if (!cached->hasGeometry) {
					PackGeometryTemplate(tables.geometryConstants[p], blocks.vsTable, blocks.psTable, true, cached->geometry);
					cached->hasGeometry = true;
				}
				if (!cached->geometry.vs.empty())
					kept->Place(cached->geometryVSBlock, cached->geometry.vs.data(), cached->geometry.vs.size(), cached->geometry.vs.size());
				if (!cached->geometry.ps.empty())
					kept->Place(cached->geometryPSBlock, cached->geometry.ps.data(), cached->geometry.ps.size(), cached->geometry.ps.size());
				const auto& permutation = tables.permutations[p];
				const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, permutation.extraShaderDescriptor,
					permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
				kept->Place(cached->permutationBlock, reinterpret_cast<const std::byte*>(data), sizeof(data), sizeof(data));
				const std::array<std::uint64_t, 5> offsetsAfter{ cached->techniqueVSBlock.offset, cached->techniquePSBlock.offset, cached->geometryVSBlock.offset,
					cached->geometryPSBlock.offset, cached->permutationBlock.offset };
				if (offsetsAfter != offsetsBefore)
					++cached->addressVersion;
				cached->constantsVersion = p < tables.pipelineConstantsVersion.size() ? tables.pipelineConstantsVersion[p] : 0u;
				cached->techniqueVersion = techniqueVersion;
				cached->lookupVersion = entry.version;
				cached->clean = blocks.techniqueVS && blocks.techniquePS;
			}
		}
		TracyCZoneEnd(pipelinesZone);
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
				decalCount[group] = std::min(tables.decalCount[group], kMaxDecalDraws);
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

	std::uint32_t MainBuild::AssembleRecord(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks, std::uint32_t a_slot)
	{
		const auto& usage = *blocks.usage;
		const auto& material = tables.materials[object.materialIndex];
		// A ProjectedUV pipeline binds the engine's four projected textures (SceneStore captured them
		// from a native draw) at the slots SetupGeometry fills, with its wrap/anisotropic modes.
		const bool projectedPipeline = (tables.pipelines[object.pipelineIndex].passDescriptor & 0x8000u) != 0;
		DrawBindings bindings{};

		auto [resolvedIt, newResolved] = resolvedBindings.try_emplace((std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex);
		auto& resolved = resolvedIt->second;
		if (newResolved) {
			if (object.materialIndex < lookups.materials.size() && lookups.materials[object.materialIndex].bindingBlock)
				out.bindingOwners.push_back(lookups.materials[object.materialIndex].bindingBlock);
			if (object.pipelineIndex < lookups.pipelines.size() && lookups.pipelines[object.pipelineIndex].shadowMaskOwner)
				out.bindingOwners.push_back(lookups.pipelines[object.pipelineIndex].shadowMaskOwner);
		}
		// Kept: the versions of everything the pair's record and blocks are written from. The same as when they were
		// written, and the pair is its slot: nothing else about it is looked at.
		std::array<std::uint32_t, 12> pairVersions{};
		if (kept) {
			const std::uint32_t m = object.materialIndex, p = object.pipelineIndex;
			const auto& pipelineEntry = cache->pipelines[p];
			pairVersions = { m < tables.materialVersion.size() ? tables.materialVersion[m] : 0u,
				m < tables.materialFrameVersion.size() ? tables.materialFrameVersion[m] : 0u, m < lookups.materials.size() ? lookups.materials[m].version : 0u,
				p < lookups.pipelines.size() ? lookups.pipelines[p].version : 0u, p < tables.pipelineBindingVersion.size() ? tables.pipelineBindingVersion[p] : 0u,
				lookups.sharedVersion, pipelineEntry.addressVersion, in.vsFrameMask, in.psFrameMask, 1u,
				pipelineEntry.clean ? 1u : 0u, tables.TechniqueRowOf(p).bindingVersion };
			if (newResolved && !dedupParity) {
				if (const auto found = cache->pairs.find((std::uint64_t(m) << 32) | p);
					found != cache->pairs.end() && found->second.clean && found->second.cleanKey == pairVersions && found->second.recordSlot != kNoRecord) {
					found->second.lastUsed = frameNumber;
					resolved.recordIndex = found->second.recordSlot;
					++cache->persistentCleanPairs;
					return found->second.recordSlot;
				}
				++cache->persistentDirtyPairs;
			}
		}
		// Assembled once per (material, pipeline) pair when the record no longer varies per object, and per
		// draw otherwise. Everything in here - the texture and sampler heap indices, the material,
		// technique, geometry, permutation, light, alpha and emissive blocks, and the per-frame defaults
		// - is then a property of the pair or of the epoch, so a second draw of the same pair needs
		// nothing but its index.
		if (resolved.skipReason != kNoSkip) {
			Skipped(static_cast<Skip>(resolved.skipReason));  // per draw, not once per pair
			if (resolved.deferred)
				++out.deferredTextures;
			return kNoRecord;
		}
		std::uint32_t recordIndex = resolved.recordIndex;
		// Per DRAW: the loop entry and the resolvedBindings probe above, which every candidate pays
		// whether or not it assembles a record.
		Mark(4);
		if (recordIndex == kNoRecord || dedupParity) {
			const std::uint64_t pairKey = (std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex;
			const bool pairFromCache = newResolved && cache && PairFromCache(pairKey, object, blocks, projectedPipeline, resolved);
			if (newResolved && !pairFromCache)
				ResolvePair(pairKey, object, blocks, projectedPipeline, resolved);
			if (!resolved.texturesOk) {
				// The sample is recorded per skipped draw, not per distinct pair.
				if (out.missingNext < out.missingTextures.size())
					out.missingTextures[out.missingNext++] = resolved.missingTexture;
				if (resolved.deferred)
					++out.deferredTextures;
				FailPair(resolved, object, Skip::Texture);
				return kNoRecord;
			}
			if (!resolved.samplersOk) {
				FailPair(resolved, object, Skip::Sampler);
				return kNoRecord;
			}
			std::copy(resolved.textures.begin(), resolved.textures.end(), bindings.textures);
			std::copy(resolved.samplers.begin(), resolved.samplers.end(), bindings.samplers);
			Mark(0);

			const auto& materialBlock = MaterialBlocksOf(pairKey, material, blocks);
			const auto [geometryVS, geometryPS] = GeometryBlocksOf(o, object, blocks);
			// NumStrictLights 0, and nothing else is read: the zeroed frame slot, for every draw.
			const std::uint64_t lightBlock = sharedLightBlock;
			const std::uint64_t permutationBlock = PermutationBlockOf(object);

			Mark(1);
			std::copy(frameVS.begin(), frameVS.end(), bindings.vertexConstants);
			std::copy(framePS.begin(), framePS.end(), bindings.pixelConstants);
			bindings.vertexConstants[kPerTechnique] = blocks.techniqueVS;
			bindings.vertexConstants[kPerMaterial] = materialBlock.first;
			bindings.vertexConstants[kPerGeometry] = geometryVS;
			bindings.vertexConstants[4] = permutationBlock;
			bindings.pixelConstants[kPerTechnique] = blocks.techniquePS;
			bindings.pixelConstants[kPerMaterial] = materialBlock.second;
			bindings.pixelConstants[kPerGeometry] = geometryPS;
			bindings.pixelConstants[3] = lightBlock;
			bindings.pixelConstants[4] = permutationBlock;
			bindings.pixelConstants[kFrameLightingRegister] = frameLightingBlock;
			// The alpha test reference (PS b11), Linear Lighting's emissive multiplier and Advanced Skin's wetness
			// come from the object record (DCLF_BINDLESS_DRAW), not from blocks of their own.
			bool constantsOk = true;
			for (std::uint32_t b = 0; b < kConstantBufferRegisters; ++b) {
				if (((usage.vertexConstants >> b) & 1) && !bindings.vertexConstants[b]) {
					constantsOk = false;
					out.missingVertexConstants |= 1u << b;
				}
				if (((usage.pixelConstants >> b) & 1) && !bindings.pixelConstants[b]) {
					constantsOk = false;
					out.missingPixelConstants |= 1u << b;
				}
			}
			if (!constantsOk) {
				FailPair(resolved, object, Skip::Constants);
				return kNoRecord;
			}

			// Kept records are immutable templates. Current-frame descriptor indices live only in
			// commit-owned upload copies, so a frame texture change cannot dirty the scene cache.
			std::array<std::uint64_t, 2> patchMask{};
			if (kept) {
				for (const auto t : resolved.patchRegisters) {
					bindings.textures[t] = 0;
					patchMask[t / 64] |= 1ull << (t % 64);
				}
			}
			if (recordIndex != kNoRecord) {
				// CS_DCLF_DEDUP_PARITY=1: the pair already has a record and this draw just rebuilt
				// one from scratch, so they must be byte-identical. This is the direct answer to
				// "is the record really the same for every draw of a pair", and the only check that
				// would catch a per-object dependency nobody has noticed.
				++out.recordParityChecks;
				const DrawBindings& held = kept ? kept->records.Get()[recordIndex] : records[recordIndex];
				if (std::memcmp(&held, &bindings, sizeof(DrawBindings)) != 0 && out.recordParityMismatches++ == 0)
					logger::warn("[DCLF] record dedup parity: object {} rebuilds a different record than its (material {}, pipeline {}) pair holds",
						o, object.materialIndex, object.pipelineIndex);
			} else if (kept) {
				// The pair's slot, its own for as long as the pair is cached; written only when the record changed.
				auto& cachedPair = cache->pairs[pairKey];
				if (cachedPair.recordSlot == kNoRecord)
					cachedPair.recordSlot = kept->AcquireRecord();
				if (cachedPair.recordSlot == kNoRecord) {
					FailPair(resolved, object, Skip::RecordCapacity);
					return kNoRecord;
				}
				kept->WriteRecord(cachedPair.recordSlot, bindings, patchMask);
				cachedPair.cleanKey = pairVersions;
				cachedPair.clean = true;
				recordIndex = cachedPair.recordSlot;
				resolved.recordIndex = recordIndex;
			} else {
				if (a_slot != kNoRecord) {
					// A resident region's pair: its stable slot (the records up to the region's slot count are its).
					recordIndex = a_slot;
					records[a_slot] = bindings;
				} else {
					if (records.size() >= in.addresses.recordCapacity) {
						FailPair(resolved, object, Skip::RecordCapacity);
						return kNoRecord;
					}
					recordIndex = static_cast<std::uint32_t>(records.size());
					records.push_back(bindings);
				}
				for (const auto t : resolved.patchRegisters)
					out.framePatches.emplace_back(recordIndex, t);
				resolved.recordIndex = recordIndex;
			}
		}

		return recordIndex;
	}

	void MainBuild::FailPair(ResolvedBindings& resolved, const ObjectRecord& object, Skip a_reason)
	{
		resolved.skipReason = static_cast<std::uint32_t>(a_reason);
		if (kept)
			if (const auto found = cache->pairs.find(PairKeyOf(object)); found != cache->pairs.end())
				found->second.clean = false;
		Skipped(a_reason);
	}

	bool MainBuild::PairFromCache(std::uint64_t pairKey, const ObjectRecord& object, const PipelineBlocks& blocks, bool projectedPipeline, ResolvedBindings& resolved)
	{
		const auto& usage = *blocks.usage;
		const auto& technique = tables.TechniqueOf(object.pipelineIndex);
		// Everything the pair's resolution and PerMaterial groups are derived from.
		auto& sources = cache->scratch;
		sources.clear();
		// The material record by its version (Tables::materialVersion), which is new whenever the record is
		// rewritten; the frame's floats MaterialSources writes into it are not part of it, and a reused
		// group gets them written over below.
		AppendSource(sources, object.materialIndex < tables.materialVersion.size() ? tables.materialVersion[object.materialIndex] : 0u);
		AppendSource(sources, tables.materialSlotKey[object.materialIndex].first);
		AppendSource(sources, tables.materialSlotKey[object.materialIndex].second);
		if (object.materialIndex < lookups.materials.size()) {
			const auto& entry = lookups.materials[object.materialIndex];
			AppendSource(sources, entry.key.first);
			AppendSource(sources, entry.key.second);
			AppendSource(sources, entry.textureIndex);
			AppendSource(sources, entry.featureIndex);
			AppendSource(sources, entry.resolved);
		} else {
			AppendSource(sources, ~0u);
		}
		AppendSource(sources, technique.filterModes);
		AppendSource(sources, technique.shadowMask);
		AppendSource(sources, blocks.shadowMaskIndex);
		AppendSource(sources, usage.vertexConstants);
		AppendSource(sources, usage.pixelConstants);
		AppendSource(sources, usage.textures);
		AppendSource(sources, usage.samplers);
		AppendSource(sources, blocks.vsTable);
		AppendSource(sources, blocks.psTable);
		AppendSource(sources, projectedPipeline);
		AppendSource(sources, lookups.nullTexture);
		AppendSource(sources, lookups.samplers);
		AppendSource(sources, lookups.projectedTextures);
		AppendSource(sources, in.addresses.objectsIndex);
		AppendSource(sources, in.addresses.bonesIndex);
		AppendSource(sources, depthOnly);
		auto& entry = cache->pairs[pairKey];
		entry.lastUsed = frameNumber;
		if (SameSources(entry.sources, sources) && entry.hasResolved) {
			++cache->pairHits;
			resolved.textures = entry.resolved.textures;
			resolved.samplers = entry.resolved.samplers;
			resolved.patchRegisters = entry.resolved.patchRegisters;
			resolved.texturesOk = entry.resolved.texturesOk;
			resolved.samplersOk = entry.resolved.samplersOk;
			resolved.deferred = entry.resolved.deferred;
			resolved.missingTexture = entry.resolved.missingTexture;
			return true;
		} else {
			++cache->pairMisses;
			entry.hasResolved = false;
			entry.vs.valid = entry.ps.valid = false;
		}
		return false;
	}

	void MainBuild::ResolvePair(std::uint64_t pairKey, const ObjectRecord& object, const PipelineBlocks& blocks, bool projectedPipeline, ResolvedBindings& resolved)
	{
		const auto& usage = *blocks.usage;
		const auto& material = tables.materials[object.materialIndex];
		const auto& technique = tables.TechniqueOf(object.pipelineIndex);
		// Textures: the material's, the technique's shadow mask, then the frame's.
		resolved.texturesOk = true;
		const Lookups::Material* materialLookup = object.materialIndex < lookups.materials.size() ? &lookups.materials[object.materialIndex] : nullptr;
		const bool materialResolved = materialLookup && materialLookup->resolved &&
		                              materialLookup->key.first == tables.materialSlotKey[object.materialIndex].first &&
		                              materialLookup->key.second == tables.materialSlotKey[object.materialIndex].second;
		for (std::uint32_t t = 0; t < kTextureRegisters; ++t) {
			// Slots below 16 the material and technique leave alone read a null view (natively: whatever
			// an earlier draw left bound). A frame register the pipeline reads
			// is the epoch's own descriptor, patched in by the commit; on the Z-prepass the pixel stage's
			// per-frame textures are not bound yet, so a depth pipeline reading one is skipped.
			std::uint32_t index = t < kPixelTextureSlots ? lookups.nullTexture : kInvalidIndex;
			bool patch = false;
			if (t == kObjectBufferRegister)
				index = in.addresses.objectsIndex;
			else if (t == kBonesBufferRegister)
				index = in.addresses.bonesIndex;
			else if (t < kPixelTextureSlots && ((material.textureWritten >> t) & 1)) {
				if (!materialResolved) {
					resolved.deferred = true;
					resolved.texturesOk = false;
					resolved.missingTexture = t;
					break;
				}
				index = materialLookup->textureIndex[t];
			} else if (t == kShadowMaskSlot && technique.shadowMask)
				index = blocks.shadowMaskIndex;
			else if (const auto p = ProjectedSlot(projectedPipeline, t); p >= 0)
				index = lookups.projectedTextures[p];
			else if (const int f = FeatureMaterialSlot(t); f >= 0 && material.featureTextures[f]) {
				// A feature's per-material texture (Advanced Skin's t71, t74).
				if (!materialResolved) {
					resolved.deferred = true;
					resolved.texturesOk = false;
					resolved.missingTexture = t;
					break;
				}
				index = materialLookup->featureIndex[f];
			}
			else if (t >= kPixelTextureSlots && !depthOnly) {
				index = 0;
				patch = usage.UsesTexture(t);
			}
			if (index == kInvalidIndex && usage.UsesTexture(t)) {
				resolved.texturesOk = false;
				resolved.missingTexture = t;
				break;
			}
			resolved.textures[t] = index == kInvalidIndex ? 0 : index;
			if (patch)
				resolved.patchRegisters.push_back(t);
		}

		// Samplers: the modes the material (or, for the shadow mask, the technique) sets.
		resolved.samplersOk = true;
		for (std::uint32_t s = 0; s < kSamplerRegisters; ++s) {
			std::uint32_t address = 0, filter = 0;
			if (s < kPixelTextureSlots && ((material.textureWritten >> s) & 1)) {
				address = material.addressModes[s];
				filter = material.filterModes[s] != kUnwrittenFilterMode ? material.filterModes[s] : technique.filterModes[s];
			} else if (s == kShadowMaskSlot && technique.shadowMask) {
				filter = technique.filterModes[s];
			} else if (ProjectedSlot(projectedPipeline, s) >= 0) {
				address = 3;  // SetupGeometry: wrap, anisotropic (engine notes: samplers)
				filter = 1;
			}
			if (filter == kUnwrittenFilterMode)
				filter = 0;
			const auto index = lookups.Sampler(address, filter);
			if (index == kInvalidIndex && ((usage.samplers >> s) & 1)) {
				resolved.samplersOk = false;
				break;
			}
			resolved.samplers[s] = index == kInvalidIndex ? 0 : index;
		}
		if (cache) {
			auto& entry = cache->pairs[pairKey];
			entry.resolved = resolved;
			entry.resolved.recordIndex = kNoRecord;
			entry.resolved.skipReason = kNoSkip;
			entry.hasResolved = true;
		}
	}

	const std::pair<std::uint64_t, std::uint64_t>& MainBuild::MaterialBlocksOf(std::uint64_t pairKey, const MaterialRecord& material, const PipelineBlocks& blocks)
	{
		// Constant buffers.
		auto& materialBlock = materialBlocks[pairKey];
		if (kept && !materialBlock.first && !materialBlock.second) {
			// Kept: the pair's two blocks, packed when its sources changed, with this frame's floats written over
			// them as below, and written to the buffer only where the bytes differ.
			auto& cachedPair = cache->pairs[pairKey];
			auto keep = [&](BuildCache::PackedGroup& a_packed, PersistentBlock& a_block, std::vector<std::uint32_t>& a_positions, const std::vector<std::uint32_t>& a_floats,
							const ConstantBlock& a_group, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables,
							std::uint32_t a_first) {
				if (!a_packed.valid || a_positions.size() != a_floats.size()) {
					PackGroupInto(a_group, a_layout, a_table, a_variables, a_first, a_packed);
					a_positions.clear();
					for (const auto index : a_floats)
						a_positions.push_back(PackedPositionOf(a_layout, a_table, a_variables, a_first, index));
				}
				auto& bytes = cache->scratch;
				bytes.assign(a_packed.bytes.begin(), a_packed.bytes.end());
				for (std::size_t i = 0; i < a_positions.size(); ++i) {
					const auto position = a_positions[i];
					const auto index = a_floats[i];
					if (position == ~0u || std::size_t(position) * 4 + 4 > bytes.size() || index >= a_group.floats.size())
						continue;
					const float value = a_group.Written(index) ? a_group.floats[index] : 0.0f;
					std::memcpy(bytes.data() + std::size_t(position) * 4, &value, 4);
				}
				return kept->Place(a_block, bytes.data(), bytes.size(), a_packed.size);
			};
			materialBlock.first = keep(cachedPair.vs, cachedPair.materialVS, cachedPair.vsPatchPositions, in.materialPatchedVSFloats, material.vs, LightingVSLayout(),
				blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial]);
			materialBlock.second = keep(cachedPair.ps, cachedPair.materialPS, cachedPair.psPatchPositions, in.materialPatchedFloats, material.ps, LightingPSLayout(),
				blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial]);
		}
		if (!materialBlock.first && !materialBlock.second) {
			// The pair's entry was checked (or rebuilt) when the pair was first met this build.
			BuildCache::Pair* cachedPair = nullptr;
			if (cache) {
				const auto found = cache->pairs.find(pairKey);
				cachedPair = found != cache->pairs.end() ? &found->second : nullptr;
			}
			auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
							BuildCache::PackedGroup* a_packed) {
				if (a_packed && a_packed->valid) {
					const auto address = Block(nullptr, a_packed->size);
					if (address)
						std::memcpy(arena.At(address - base, a_packed->bytes.size()).data(), a_packed->bytes.data(), a_packed->bytes.size());
					return address;
				}
				const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
				const auto address = Block(nullptr, size);
				if (address) {
					const auto slice = arena.At(address - base, std::max<std::size_t>(size, 16));
					PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, slice);
					if (a_packed) {
						a_packed->size = size;
						a_packed->bytes.assign(slice.begin(), slice.end());
						a_packed->valid = true;
					}
				}
				return address;
			};
			const bool vsReused = cachedPair && cachedPair->vs.valid && cachedPair->vsPatchPositions.size() == in.materialPatchedVSFloats.size();
			materialBlock.first = pack(material.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial],
				cachedPair ? &cachedPair->vs : nullptr);
			if (cachedPair && !vsReused) {
				cachedPair->vsPatchPositions.clear();
				for (const auto index : in.materialPatchedVSFloats)
					cachedPair->vsPatchPositions.push_back(PackedPositionOf(LightingVSLayout(), blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial], index));
			}
			if (vsReused && materialBlock.first) {
				auto slice = arena.At(materialBlock.first - base, cachedPair->vs.bytes.size());
				for (std::size_t i = 0; i < cachedPair->vsPatchPositions.size(); ++i) {
					const auto position = cachedPair->vsPatchPositions[i];
					const auto index = in.materialPatchedVSFloats[i];
					if (position == ~0u || std::size_t(position) * 4 + 4 > slice.size() || index >= material.vs.floats.size())
						continue;
					const float value = material.vs.Written(index) ? material.vs.floats[index] : 0.0f;
					std::memcpy(slice.data() + std::size_t(position) * 4, &value, 4);
				}
			}
			// The PS group carries the frame's floats (IBLParams): a reused group has this frame's values
			// written over them, as PackConstantGroup would write them (an unwritten float packs as zero).
			const bool psReused = cachedPair && cachedPair->ps.valid && cachedPair->psPatchPositions.size() == in.materialPatchedFloats.size();
			materialBlock.second = pack(material.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial],
				cachedPair ? &cachedPair->ps : nullptr);
			if (cachedPair && !psReused) {
				cachedPair->psPatchPositions.clear();
				for (const auto index : in.materialPatchedFloats)
					cachedPair->psPatchPositions.push_back(PackedPositionOf(LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial], index));
			}
			if (psReused && materialBlock.second) {
				auto slice = arena.At(materialBlock.second - base, cachedPair->ps.bytes.size());
				for (std::size_t i = 0; i < cachedPair->psPatchPositions.size(); ++i) {
					const auto position = cachedPair->psPatchPositions[i];
					const auto index = in.materialPatchedFloats[i];
					if (position == ~0u || std::size_t(position) * 4 + 4 > slice.size() || index >= material.ps.floats.size())
						continue;
					const float value = material.ps.Written(index) ? material.ps.floats[index] : 0.0f;
					std::memcpy(slice.data() + std::size_t(position) * 4, &value, 4);
				}
			}
		}
		return materialBlock;
	}

	std::pair<std::uint64_t, std::uint64_t> MainBuild::GeometryBlocksOf(std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks)
	{
		std::uint64_t geometryVS = 0, geometryPS = 0;
		// A pipeline's PerGeometry template: one block per stage (kept, or the build's).
		auto uploadTemplate = [&](const std::vector<std::byte>& a_group, BuildCache::Pipeline* a_pipeline, bool a_pixel) -> std::uint64_t {
			if (a_group.empty())
				return 0;  // the stage does not declare the buffer
			if (kept && a_pipeline)
				return kept->Place(a_pixel ? a_pipeline->geometryPSBlock : a_pipeline->geometryVSBlock, a_group.data(), a_group.size(), a_group.size());
			const auto address = Block(nullptr, a_group.size());
			if (address)
				std::memcpy(arena.At(address - base, std::max<std::size_t>(a_group.size(), 16)).data(), a_group.data(), a_group.size());
			return address;
		};
		{
			auto [templateIt, newTemplate] = geometryTemplates.try_emplace(object.pipelineIndex);
			auto& geometryTemplate = templateIt->second;
			BuildCache::Pipeline* cachedPipeline = nullptr;
			if (newTemplate && cache) {
				const auto found = cache->pipelines.find(object.pipelineIndex);
				cachedPipeline = found != cache->pipelines.end() ? &found->second : nullptr;
			}
			if (newTemplate && cachedPipeline && cachedPipeline->hasGeometry) {
				geometryTemplate.vs = cachedPipeline->geometry.vs;
				geometryTemplate.ps = cachedPipeline->geometry.ps;
				geometryTemplate.offsets = cachedPipeline->geometry.offsets;
				geometryTemplate.vsAddress = uploadTemplate(geometryTemplate.vs, cachedPipeline, false);
				geometryTemplate.psAddress = uploadTemplate(geometryTemplate.ps, cachedPipeline, true);
			} else if (newTemplate) {
				// The pipeline's own values, which is everything the objects do not override.
				PackGeometryTemplate(tables.geometryConstants[object.pipelineIndex], blocks.vsTable, blocks.psTable, true, geometryTemplate);
				if (cachedPipeline) {
					cachedPipeline->geometry.vs = geometryTemplate.vs;
					cachedPipeline->geometry.ps = geometryTemplate.ps;
					cachedPipeline->geometry.offsets = geometryTemplate.offsets;
					cachedPipeline->hasGeometry = true;
				}
				geometryTemplate.vsAddress = uploadTemplate(geometryTemplate.vs, cachedPipeline, false);
				geometryTemplate.psAddress = uploadTemplate(geometryTemplate.ps, cachedPipeline, true);
			}
			// One pair of blocks for the whole pipeline, written when the template was built.
			geometryVS = geometryTemplate.vsAddress;
			geometryPS = geometryTemplate.psAddress;
			// CS_DCLF_BINDLESS_PARITY=1: the record the shaders read against the PerGeometry group the engine's
			// constant buffer holds for the same object (PatchObjectGeometry). The two derive from the same inputs
			// through the same unwritten-component rule, so anything but bit equality is a defect in the
			// record's layout or in the way it is filled, caught on the CPU with no readback.
			if (bindlessParity && o < objectRecords.Count()) {
				parityVS.assign(geometryTemplate.vs.begin(), geometryTemplate.vs.end());
				parityPS.assign(geometryTemplate.ps.begin(), geometryTemplate.ps.end());
				PatchObjectGeometry(tables, o, renderFlags, eye, previousEye, geometryTemplate.offsets, parityVS, parityPS);
				IndirectDraws::Stats parityStats{};
				CheckBindlessRecord(tables, o, *objectRecords.At(o), eye, previousEye, geometryTemplate.offsets, parityVS, parityPS, parityStats);
				out.bindlessParityChecks += parityStats.bindlessParityChecks;
				out.bindlessParityMismatches += parityStats.bindlessParityMismatches;
			}
		}
		return { geometryVS, geometryPS };
	}

	std::uint64_t MainBuild::PermutationBlockOf(const ObjectRecord& object)
	{
		const auto& permutation = tables.permutations[object.pipelineIndex];
		// SuppressExternalEmittance is the only per-object bit DCLF puts in this block, and it is
		// read at exactly one place in the whole shader tree - Effect.hlsl's GetLightingColor -
		// never by Lighting.hlsl or anything it includes. So for these pipelines it is dead, and the
		// block keys on the pipeline alone, which is what makes the binding record identical for a
		// (material, pipeline) pair.
		const std::uint32_t extra = permutation.extraShaderDescriptor;
		auto& permutationBlock = permutationBlocks[(std::uint64_t(object.pipelineIndex) << 32) | extra];
		if (!permutationBlock) {
			const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, extra, permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
			// Kept per pipeline: nothing in it is per object.
			if (kept)
				permutationBlock = kept->Place(cache->pipelines[object.pipelineIndex].permutationBlock, reinterpret_cast<const std::byte*>(data), sizeof(data), sizeof(data));
			else
				permutationBlock = Block(data, sizeof(data));
		}
		return permutationBlock;
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
		// The whole scene with kept bindings (its pairs' records are theirs); the Z-prepass's only where its gate is not the
		// colour epoch's last frame (withholding): with that gate, what is not resident stays the loop's.
		wholeScene = kept && (!depthOnly || in.withholding);
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
		if (fresh && kept) {
			// Its record is the pair's kept one, whose slot the assembly below gives it.
			pair->second.slot = kNoRecord;
			pair->second.ok = false;
		} else if (fresh) {
			if (!r.freeSlots.empty()) {
				pair->second.slot = r.freeSlots.back();
				r.freeSlots.pop_back();
			} else {
				pair->second.slot = r.slotCount++;
			}
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
			if (!kept)
				r.freeSlots.push_back(pair->second.slot);
			r.pairs.erase(pair);
		}
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
			// Past the region's share of the buffers, or of the record slots, it stays with the loop.
			const std::uint32_t partitions = PartitionsOf(tables, o);
			const std::size_t objectDraws = partitions ? static_cast<std::size_t>(std::popcount(partitions)) : 1;
			const bool slot = kept || r.pairs.contains(key) || !r.freeSlots.empty() || r.slotCount < in.addresses.recordCapacity / 2;
			const std::size_t inputLimit = wholeScene ? kMaxInputs - kLoopReserve : kMaxInputs / 2;
			const std::size_t drawLimit = wholeScene ? kMaxDraws - kLoopReserve : kMaxDraws / 2;
			if (r.inputs.Size() >= inputLimit || r.draws + objectDraws > drawLimit || !slot)
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
		if (!kept)
			records.resize(r.slotCount);
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
				if (kept) {
					slot = AssembleRecord(~0u, object, pipelineBlocks[pipeline], kNoRecord);
					ok = slot != kNoRecord;
				} else {
					ok = AssembleRecord(~0u, object, pipelineBlocks[pipeline], pair.slot) == pair.slot;
				}
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
		// The draw cap: BuildDraws writes into the first half of the sequence buffer, and the count
		// is ExecuteIndirect's maxCount. Decals have their own ranges.
		const std::size_t objectDraws = partitions ? static_cast<std::size_t>(std::popcount(partitions)) : 1;
		if (!decalGroup && sequences.size() + regionDraws + objectDraws > kMaxDraws) {
			Skipped(Skip::Capacity);
			return;
		}
		const std::uint32_t recordIndex = AssembleRecord(o, object, blocks, kNoRecord);
		if (recordIndex == kNoRecord)
			return;
		// The CPU template of what BuildDraws writes (checked with CS_DCLF_BUILD_PARITY).
		Mark(2);
		auto sequence = tables.draws[o];
		sequence.pipelineIndex = blocks.setIndex;
		sequence.objectIndex = o;
		sequence.bindingsAddress = in.addresses.records + std::uint64_t(recordIndex) * sizeof(DrawBindings);
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
		if (kept) {
			out.persistent = true;
			out.keptConstants = kept->constants.View();
			out.keptRecords = kept->records.View();
			out.patchMasks = kept->patchMasks;
			out.recordsHeld = static_cast<std::uint32_t>(kept->records.Size() - kept->recordFree.size());
			out.blocksWritten = kept->blocksWritten;
			out.recordsWritten = kept->recordsRewritten;
			++cache->persistentBuilds;
			cache->persistentBlocks += kept->blocksWritten;
			cache->persistentRecords += kept->recordsRewritten;
			if (PersistentParityEnabled() && ParityDue(frameNumber)) {
				MainPayload reference;
				BuildMainPayload(in, tables, lookups, reference);
				CheckPersistentBindings(out, reference, *cache, base, in.frameTextures, cache->persistentParityChecks, cache->persistentParityMismatches,
					cache->persistentParityFirst);
			}
		}
		Mark(5);
	}

	void BuildMainPayload(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out,
		BuildCache* a_cache, ObjectRecordStore* a_objects, BonesStore* a_bones, GeometryStore* a_geometries)
	{
		MainBuild(a_in, a_tables, a_lookups, a_out, a_cache, a_objects, a_bones, a_geometries).Run();
	}
}

#endif
