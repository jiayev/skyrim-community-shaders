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

		// Pairs.Resolve goes to the pool for a check list worth the chunks; a chunk ends on a material boundary past this many pairs.
		constexpr std::size_t kPairFanoutMin = 512;
		constexpr std::size_t kPairGrain = 128;
		// A region resync goes to the pool for this many object slots, in chunks of this many.
		constexpr std::size_t kResyncFanoutMin = 4096;
		constexpr std::size_t kResyncGrain = 512;

		std::uint64_t Mix(std::uint64_t a_value, std::uint64_t a_word) { return (a_value ^ a_word) * 0x100000001b3ull; }

		// A kept array's writes from pool chunks (KeptArray: the chunked copy-on-write's PrepareWrites/WriteParts/MarkParts when it has
		// them; else the whole array made writable once before the loop, the parts compared and written by hand). The marks are applied
		// on the owner thread afterwards, in chunk order.
		template <class Rows>
		void PrepareKeptWrites(Rows& a_rows, std::size_t a_first, std::size_t a_count)
		{
			if constexpr (requires { a_rows.PrepareWrites(a_first, a_count); })
				a_rows.PrepareWrites(a_first, a_count);
			else
				(void)a_rows.Mutable();
		}
		template <class Rows, class Row>
		std::uint64_t WriteKeptParts(Rows& a_rows, std::size_t a_index, const Row& a_row, std::size_t a_parts)
		{
			if constexpr (requires { a_rows.WriteParts(a_index, a_row, a_parts); }) {
				return a_rows.WriteParts(a_index, a_row, a_parts);
			} else {
				const std::size_t partBytes = sizeof(Row) / a_parts;
				const auto* held = reinterpret_cast<const std::byte*>(&a_rows.Get()[a_index]);
				const auto* value = reinterpret_cast<const std::byte*>(&a_row);
				std::uint64_t mask = 0;
				for (std::size_t p = 0; p < a_parts; ++p)
					if (std::memcmp(held + p * partBytes, value + p * partBytes, partBytes) != 0)
						mask |= 1ull << p;
				if (mask)
					a_rows.Mutable()[a_index] = a_row;
				return mask;
			}
		}
		template <class Rows>
		void MarkKeptParts(Rows& a_rows, std::size_t a_index, std::uint64_t a_mask, std::size_t a_parts)
		{
			if constexpr (requires { a_rows.MarkParts(a_index, a_mask, a_parts); }) {
				a_rows.MarkParts(a_index, a_mask, a_parts);
			} else {
				for (std::size_t p = 0; p < a_parts; ++p)
					if ((a_mask >> p) & 1)
						a_rows.Mark(a_index * a_parts + p);
			}
		}

		/**
		 * @brief The texture registers by who gives them (ResolvePair): the pipeline row (t14), the material row (t0-t15 but t14, and the
		 * features' registers), the payload's buffers, and the frame record (the rest; the Z-prepass's binds only the character light's).
		 */
		struct TextureClasses
		{
			std::array<std::uint64_t, 2> shadowMask{}, material{}, buffers{}, frame{}, characterLight{};
			std::array<std::uint32_t, kFeatureMaterialTextures> featureRegister{};
		};
		static_assert(kTextureRegisters == 128 && kPixelTextureSlots <= 32 && kSamplerRegisters <= 32 && kConstantBufferRegisters <= 32);
		const TextureClasses& TextureClassesOf()
		{
			static const TextureClasses classes = [] {
				TextureClasses c;
				for (std::uint32_t t = 0; t < kTextureRegisters; ++t) {
					const std::uint64_t bit = 1ull << (t % 64);
					const std::size_t w = t / 64;
					if (t == kShadowMaskSlot)
						c.shadowMask[w] |= bit;
					else if (t < kPixelTextureSlots)
						c.material[w] |= bit;
					else if (const int f = FeatureMaterialSlot(t); f >= 0)
						c.featureRegister[f] = t;
					else if (t == kObjectBufferRegister || t == kExtrasBufferRegister || t == kTreeWindRegister || t == kPlacementBufferRegister ||
							 t == kPaletteBufferRegister || t == kShadingBufferRegister || t == kFadeStatesRegister || t == kFadeRootsRegister)
						c.buffers[w] |= bit;
					else {
						c.frame[w] |= bit;
						if (t == kCharacterLightRegister)
							c.characterLight[w] |= bit;
					}
				}
				return c;
			}();
			return classes;
		}

		/** @brief A material row's state as a pair's verdict reads it (MainRows::MaterialState but its key), hashed. */
		std::uint64_t MaterialStateWitness(const MainRows::MaterialState& a_state)
		{
			std::uint64_t value = 0xcbf29ce484222325ull;
			value = Mix(value, (a_state.written ? 1u : 0u) | (a_state.texturesOk ? 2u : 0u) | (a_state.deferred ? 4u : 0u) | (a_state.blocksOk ? 8u : 0u) |
								   (std::uint64_t(a_state.missingTexture) << 32));
			value = Mix(value, (std::uint64_t(a_state.textures) << 32) | a_state.samplers);
			value = Mix(value, a_state.features);
			return Mix(value, a_state.tables);
		}
		bool SameMaterialState(const MainRows::MaterialState& a_left, const MainRows::MaterialState& a_right)
		{
			return a_left.written == a_right.written && a_left.texturesOk == a_right.texturesOk && a_left.deferred == a_right.deferred &&
			       a_left.missingTexture == a_right.missingTexture && a_left.textures == a_right.textures && a_left.samplers == a_right.samplers &&
			       a_left.features == a_right.features && a_left.tables == a_right.tables && a_left.blocksOk == a_right.blocksOk &&
			       a_left.texcoordDword == a_right.texcoordDword && a_left.texcoordMask == a_right.texcoordMask;
		}

		/**
		 * @brief What resolving pairs writes besides the pairs (MainBuild::WriteMaterialRow, ResolvePair): the loop's, merged after each
		 * resolution, or one pool chunk's, merged in chunk order (MainBuild::MergeWork) with its row marks applied there (the rows'
		 * journal is single-threaded). A chunk writes only its own materials' rows (chunks split on material boundaries).
		 */
		struct PairWork
		{
			bool deferred = false;  // a pool chunk's: rows written unmarked into prepared storage, marked by MergeWork
			std::vector<std::pair<std::uint32_t, std::uint64_t>> rowMarks;  // (material, its parts written)
			std::array<std::uint64_t, 8> keyMoved{};
			std::uint64_t materialsWritten = 0, materialRowsWritten = 0;
			std::uint32_t missingVertexConstants = 0, missingPixelConstants = 0;
			std::uint32_t rowTableConflicts = 0, conflictMaterial = 0, conflictPipeline = 0;
			// Region pairs that cannot draw, counted per resolution as AssembleRecord counts a draw: skips, the texture sample, deferrals.
			std::array<std::uint32_t, static_cast<std::size_t>(Skip::Count)> skipped{};
			std::vector<std::uint32_t> missingTextures;
			std::uint32_t deferredTextures = 0;
			// Region: each pair resolved again, its verdict, applied to the pair in check order (UpdateRegionPairs).
			struct Verdict
			{
				ResidentRegion::Pair* pair = nullptr;
				std::uint64_t key = 0, witness = 0;
				std::uint32_t slot = 0, skip = kNoSkip;
				bool ok = false;
				std::array<std::uint64_t, 2> registers{};
				const void* binding = nullptr;
				const void* mask = nullptr;
			};
			std::vector<Verdict> verdicts;

			void Clear()
			{
				const bool keep = deferred;
				*this = PairWork{};
				deferred = keep;
			}
		};

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
				const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache) :
				segmentIn(a_in), segmentOut(a_out), primary(PrimarySegment(a_in)), in(*a_in[primary]), tables(a_tables), frame(a_frame), lookups(a_lookups),
				out(*a_out[primary]), rows(a_rows), cache(a_cache)
			{}

			/** @brief The build but what reads the stream views (Finish). */
			void Run();
			/** @brief The stream views' part (the geometry draws, the rows' views) and the other segment's payload. */
			void Finish(std::shared_ptr<const StreamViews> a_streams);

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
			std::shared_ptr<const StreamViews> streams;  // the frame's (Finish), or none: the build packs its own geometry slots
			// The parts' timings (partMs) are the report's alone: the clock is read only when it is wanted.
			const bool timeParts = Diagnostics::Observe().stats || Diagnostics::Observe().profile;
			// Pool fan-out (Policy::SerialFanout keeps every loop on the builder's thread).
			const bool serialFanout = Policy::SerialFanout();
			// Policy::CheckRowPacking: per material, the first pipeline packing its row this build (whole builds; the region checks its pairs).
			const bool checkPacking = Policy::CheckRowPacking(rows.builds);
			ankerl::unordered_dense::map<std::uint32_t, std::pair<std::uint32_t, std::array<std::uint64_t, 3>>> packingSeen;
			PairWork loopWork;  // the object loop's resolutions (merged after each)

			decltype(MainPayload::sequences)& sequences = out.sequences;
			decltype(MainPayload::inputList)& drawInputs = out.inputList;
			decltype(MainPayload::decalCount)& decalCount = out.decalCount;
			decltype(MainPayload::decalTemplates)& decalTemplates = out.decalTemplates;
			const bool colourBuilt = segmentIn[kAsyncColour] != nullptr;
			const std::uint32_t frameNumber = in.frameNumber;
			static constexpr auto renderFlags = GeometryPort::kMainPassRenderFlags;

			// The frame registers' blocks (FrameRegisters), per segment.
			std::array<std::array<std::uint64_t, kConstantBufferRegisters>, 2> frameVS{}, framePS{};
			std::array<std::uint32_t, 2> frameVSBits{}, framePSBits{};  // the registers above that are bound (bit per register)
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
			/**
			 * @brief The pair's verdict: what its rows give (a_material: its material row's state, written for this pipeline) against what
			 * its pipeline reads; the frame textures it asks of the commit into a_registers. Pure but for a_work: any thread.
			 */
			void ResolvePair(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const MainRows::MaterialState& a_material, ResolvedBindings& resolved,
				std::array<std::uint64_t, 2>& a_registers, PairWork& a_work) const;
			/** @brief Rewrites the pipeline's row when its sources changed. */
			void WritePipelineRow(std::uint32_t p, const PipelineBlocks& blocks);
			/** @brief The material row's sources for pipeline p (MainRows::MaterialState::key). */
			std::array<std::uint32_t, 8> MaterialRowKey(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks) const;
			/** @brief The material's row and its state packed for pipeline p, from scratch (pure). */
			void PackMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const std::array<std::uint32_t, 8>& a_key, MainRows::MaterialState& a_state,
				MaterialRow& a_row) const;
			/**
			 * @brief Rewrites the material's row when its sources changed: marked now, or for a pool chunk (a_work.deferred) written into the
			 * prepared rows and marked by MergeWork. Only the chunk holding m writes m's row.
			 */
			void WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, PairWork& a_work);
			/** @brief A_work's rows marks, counters, skips and samples into the build and the rows (owner thread, in order); a_work cleared. */
			void MergeWork(PairWork& a_work);
			/** @brief What packing a material row for pipeline p depends on of p (technique bindings, projected, constant tables). */
			std::array<std::uint64_t, 3> PackingOf(std::uint32_t p) const;
			/** @brief Policy::CheckRowPacking: logs a material whose pipelines pack its one row differently (a_first, a_second). */
			void NotePackingConflict(std::uint32_t m, std::uint32_t a_first, std::uint32_t a_second);

			void BuildResidentRegion();
			static std::uint64_t PairKeyOf(const ObjectRecord& a_object) { return (std::uint64_t(a_object.materialIndex) << 32) | a_object.pipelineIndex; }
			bool ResidentAt(std::uint32_t o) const { return o < tables.residentSlot.size() && tables.residentSlot[o] != 0; }
			// What the region can hold: a resident's (with the whole scene, anyone's) single-input record, plus with the whole
			// scene the depth segment's cull-only candidates.
			bool RegionEligible(std::uint32_t o) const;
			// The input the loop would write, drawable while in the set with its pair's record built.
			std::uint8_t RegionEntry(std::uint32_t o, DrawInput& a_input) const;
			// An object's main entry taking or leaving pair a_key (its lists, the pair's objects, its bundle share).
			void RegionAcquire(std::uint64_t a_key, std::uint32_t o);
			void RegionRelease(std::uint64_t a_key, std::uint32_t o);
			// A pair's share of the bundle: its frame textures (by count per register) and its owners (by count per owner).
			void SetPairRegisters(ResidentRegion::Pair& a_pair, const std::array<std::uint64_t, 2>& a_registers);
			void SetPairOwners(ResidentRegion::Pair& a_pair, std::uint32_t m, std::uint32_t p, const void* a_binding, const void* a_mask);
			void AcquireOwner(const void* a_owner, const std::shared_ptr<const void>& a_held);
			void ReleaseOwner(const void* a_owner);
			/** @brief Every slot's entry again after a Reset: evaluated on the pool, indexed by a prefix sum, scattered (one journal resync). */
			void ResyncRegionParallel();
			/** @brief Policy::CheckBuilderPairs: the parallel resync's entries against the serial path's verdicts, slot by slot. */
			void CheckRegionResync();
			/** @brief Resolves check[a_begin, a_end) (whole materials) into a_work: a pool chunk, or the whole list serially. */
			void ResolvePairsChunk(const std::vector<std::uint64_t>& a_check, std::size_t a_begin, std::size_t a_end, bool a_everyPair, PairWork& a_work);
			/** @brief Policy::CheckBuilderPairs: every pair resolved again serially from scratch, against what the narrowed checks left. */
			void CheckNarrowedPairs();
			/** @brief Policy::CheckRowPacking: every material's live pairs pack its row alike. */
			void CheckRegionPacking();
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
		};
	}

	void MainBuild::Run()
	{
		ZoneScopedN("CS.DCLF.BuildMainPayload");
		Reset();
		FrameRegisters();
		PackPipelines();
		BeginStates();
		if (timeParts)
			partStart = std::chrono::steady_clock::now();
		PrepareObjects();
		BuildResidentRegion();
		RunObjectLoop();
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
		// The journals keep what the oldest payload backing lacks (its content epoch); only what is written from here on is new.
		rows.material.BeginBuild(in.materialRowsOldestContentEpoch);
		rows.pipeline.BeginBuild(in.pipelineRowsOldestContentEpoch);
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
		if (rows.tablesHashes.size() < tables.pipelines.size())
			rows.tablesHashes.resize(tables.pipelines.size());
	}

	void MainBuild::FrameRegisters()
	{
		// A frame slot the commit does not supply stays zero, which the constants check rejects for a pipeline reading it.
		frameVS = {};
		framePS = {};
		frameVSBits = framePSBits = {};
		for (std::size_t j = 0; j < segmentIn.size(); ++j) {
			if (!segmentIn[j])
				continue;
			const auto& segment = *segmentIn[j];
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((segment.vsFrameMask >> slot) & 1)
					frameVS[j][slot] = segment.addresses.frameConstants + FrameSlotOffset(false, slot);
				if ((segment.psFrameMask >> slot) & 1)
					framePS[j][slot] = segment.addresses.frameConstants + FrameSlotOffset(true, slot);
				frameVSBits[j] |= frameVS[j][slot] ? 1u << slot : 0u;
				framePSBits[j] |= framePS[j][slot] ? 1u << slot : 0u;
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
			// Hashed again only when the entry moved (its version is new with any change of its tables).
			auto& hashed = rows.tablesHashes[p];
			if (hashed.instance != lookups.instance || hashed.version != entry.version)
				hashed = { lookups.instance, entry.version, TablesHash(blocks.vsTable, blocks.psTable) };
			blocks.tables = hashed.hash;
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
		// The per-object records are the stream views' (MakeStreamViews, published in the payload by PayloadBuilder) and the palettes the
		// frame values' (FrameValues), not the build's.
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
		if (!timeParts)
			return;
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
			const std::uint32_t m = object.materialIndex, p = object.pipelineIndex;
			if (m < lookups.materials.size() && lookups.materials[m].bindingBlock)
				out.bindingOwners.push_back(lookups.materials[m].bindingBlock);
			if (p < lookups.pipelines.size() && lookups.pipelines[p].shadowMaskOwner)
				out.bindingOwners.push_back(lookups.pipelines[p].shadowMaskOwner);
			if (checkPacking) {
				const auto [seen, first] = packingSeen.try_emplace(m, std::pair{ p, PackingOf(p) });
				if (!first && seen->second.second != PackingOf(p))
					NotePackingConflict(m, seen->second.first, p);
			}
			// No row waits for a table: the payload's row tables hold every row (PayloadBuilder).
			Mark(0);
			WriteMaterialRow(m, p, blocks, loopWork);
			Mark(1);
			std::array<std::uint64_t, 2> registers{};
			ResolvePair(m, p, blocks, rows.materials[m], resolved, registers, loopWork);
			out.frameRegisters[0] |= registers[0];
			out.frameRegisters[1] |= registers[1];
			MergeWork(loopWork);
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

	void MainBuild::MergeWork(PairWork& a_work)
	{
		for (const auto& [m, parts] : a_work.rowMarks)
			MarkKeptParts(rows.material, m, parts, kMaterialRowParts);
		for (std::size_t k = 0; k < rows.keyMoved.size(); ++k)
			rows.keyMoved[k] += a_work.keyMoved[k];
		rows.materialsWritten += a_work.materialsWritten;
		out.materialRowsWritten += a_work.materialRowsWritten;
		out.missingVertexConstants |= a_work.missingVertexConstants;
		out.missingPixelConstants |= a_work.missingPixelConstants;
		if (a_work.rowTableConflicts) {
			if (out.rowTableConflicts == 0)
				logger::warn("[DCLF] material slot {} was packed with other constant tables than pipeline slot {} has; its draws stay native", a_work.conflictMaterial,
					a_work.conflictPipeline);
			out.rowTableConflicts += a_work.rowTableConflicts;
		}
		for (std::size_t reason = 0; reason < out.skipped.size(); ++reason)
			out.skipped[reason] += a_work.skipped[reason];
		for (const std::uint32_t texture : a_work.missingTextures)
			if (out.missingNext < out.missingTextures.size())
				out.missingTextures[out.missingNext++] = texture;
		out.deferredTextures += a_work.deferredTextures;
		a_work.Clear();
	}

	void MainBuild::ResolvePair(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const MainRows::MaterialState& material, ResolvedBindings& resolved,
		std::array<std::uint64_t, 2>& a_registers, PairWork& a_work) const
	{
		// An input names its rows in one word (RowsOf): past its fields is a hard failure, like the sequence limits.
		if (m > kRowMaterialMask || p >= (1u << (32 - kRowPipelineShift)))
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: material slot {} or pipeline slot {} past an input's row fields", m, p));
		auto fail = [&](Skip a_reason) { resolved.skipReason = static_cast<std::uint32_t>(a_reason); };
		const auto& pipeline = rows.pipelines[p];
		// The row was packed with its technique's constant tables; a pipeline of the same slot with others would read it wrong.
		if (material.tables != blocks.tables) {
			if (a_work.rowTableConflicts++ == 0) {
				a_work.conflictMaterial = m;
				a_work.conflictPipeline = p;
			}
			return fail(Skip::Constants);
		}
		if (!material.texturesOk) {
			resolved.deferred = material.deferred;
			resolved.missingTexture = material.missingTexture;
			return fail(Skip::Texture);
		}
		// Whether segment j's variant gets what it reads from the rows and its frame; false after fail() recorded why. By register
		// class (TextureClassesOf), as masks: the first register not given is the skip's sample, and the frame textures asked of the
		// commit are those below it.
		const auto& classes = TextureClassesOf();
		auto reads = [&](std::size_t j) {
			const auto& usage = *blocks.usage[j];
			const bool depthOnly = j == kAsyncZPrepass;
			// Textures: the material row's (t0-t15 but t14, and the features'), the pipeline row's (t14), the frame record's (rest).
			std::array<std::uint64_t, 2> given{};
			for (std::size_t w = 0; w < 2; ++w)
				given[w] = classes.buffers[w] | (pipeline.shadowMask ? classes.shadowMask[w] : 0) | (depthOnly ? classes.characterLight[w] : classes.frame[w]);
			given[0] |= std::uint64_t(material.textures) & classes.material[0];
			for (std::uint32_t features = material.features; features;) {
				const auto f = static_cast<std::uint32_t>(std::countr_zero(features));
				features &= features - 1;
				if (f < kFeatureMaterialTextures) {
					const std::uint32_t t = classes.featureRegister[f];
					given[t / 64] |= 1ull << (t % 64);
				}
			}
			const std::array<std::uint64_t, 2> missing{ usage.textures[0] & ~given[0], usage.textures[1] & ~given[1] };
			std::array<std::uint64_t, 2> below{ ~0ull, ~0ull };
			if (missing[0] || missing[1]) {
				const std::uint32_t t = missing[0] ? static_cast<std::uint32_t>(std::countr_zero(missing[0])) : 64 + static_cast<std::uint32_t>(std::countr_zero(missing[1]));
				below = t < 64 ? std::array<std::uint64_t, 2>{ (1ull << t) - 1, 0 } : std::array<std::uint64_t, 2>{ ~0ull, t == 64 ? 0 : (1ull << (t - 64)) - 1 };
				resolved.missingTexture = t;
			}
			if (!depthOnly)
				for (std::size_t w = 0; w < 2; ++w)
					a_registers[w] |= usage.textures[w] & classes.frame[w] & below[w];  // the commit resolves them into the frame record
			if (missing[0] || missing[1]) {
				fail(Skip::Texture);
				return false;
			}
			constexpr std::uint32_t samplerMask = kSamplerRegisters >= 32 ? ~0u : (1u << kSamplerRegisters) - 1;
			const std::uint32_t samplersGiven = (material.samplers & ~(1u << kShadowMaskSlot)) | (pipeline.shadowMaskSampler ? 1u << kShadowMaskSlot : 0u);
			if (usage.samplers & samplerMask & ~samplersGiven) {
				fail(Skip::Sampler);
				return false;
			}
			// Constant buffers: the rows' (b0, b1, b2, b4), the frame slots', and the pass's (PS b3, b13).
			constexpr std::uint32_t registerMask = kConstantBufferRegisters >= 32 ? ~0u : (1u << kConstantBufferRegisters) - 1;
			constexpr std::uint32_t rowRegisters = (1u << kPerTechnique) | (1u << kPerMaterial) | (1u << kPerGeometry) | (1u << 4);
			const std::uint32_t rowsMissing = (material.blocksOk ? 0u : 1u << kPerMaterial) | (pipeline.blocksOk ? 0u : rowRegisters & ~(1u << kPerMaterial));
			const std::uint32_t vsUsed = usage.vertexConstants & registerMask, psUsed = usage.pixelConstants & registerMask;
			const std::uint32_t pass = (1u << 3) | (1u << kFrameLightingRegister);
			const std::uint32_t vsMissing = (vsUsed & rowRegisters & rowsMissing) | (vsUsed & ~rowRegisters & ~frameVSBits[j]);
			const std::uint32_t psMissing = (psUsed & rowRegisters & rowsMissing) | (psUsed & ~rowRegisters & ~(pass | framePSBits[j]));
			a_work.missingVertexConstants |= vsMissing;
			a_work.missingPixelConstants |= psMissing;
			if (vsMissing || psMissing) {
				fail(Skip::Constants);
				return false;
			}
			return true;
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
		resolved.recordIndex = RowsOf(p, m);
	}

	std::array<std::uint32_t, 8> MainBuild::MaterialRowKey(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks) const
	{
		// The row's sources: the material record and frame values, its lookup entry, the shared lookups, its technique's bindings
		// (filter modes a material leaves unwritten) and the constant tables it is packed with.
		const Lookups::Material* lookup = m < lookups.materials.size() ? &lookups.materials[m] : nullptr;
		// The Lighting pass descriptor's ProjectedUV (an effect key's bit 15 is Membrane's, not ported).
		const bool projected = (tables.pipelines[p].passDescriptor & 0x8000u) != 0 && !RasterEffect(tables.pipelines[p].rasterFlags);
		return { m < tables.materialVersion.size() ? tables.materialVersion[m] : 0u, m < tables.materialFrameVersion.size() ? tables.materialFrameVersion[m] : 0u,
			lookup ? lookup->version : ~0u, lookups.sharedVersion, tables.TechniqueOf(p).bindingVersion, projected ? 1u : 0u, static_cast<std::uint32_t>(blocks.tables),
			static_cast<std::uint32_t>(blocks.tables >> 32) };
	}

	std::array<std::uint64_t, 3> MainBuild::PackingOf(std::uint32_t p) const
	{
		const bool projected = (tables.pipelines[p].passDescriptor & 0x8000u) != 0 && !RasterEffect(tables.pipelines[p].rasterFlags);
		return { tables.TechniqueOf(p).bindingVersion, projected ? 1u : 0u, pipelineBlocks[p].tables };
	}

	void MainBuild::NotePackingConflict(std::uint32_t m, std::uint32_t a_first, std::uint32_t a_second)
	{
		static std::atomic<std::uint32_t> logged{ 0 };
		if (rows.pairStats.packingConflicts++ < 16 && logged.fetch_add(1, std::memory_order_relaxed) < 16) {
			const auto first = PackingOf(a_first), second = PackingOf(a_second);
			logger::warn("[DCLF] material slot {}: pipeline slots {} and {} pack its one row differently (technique bindings {}/{}, projected {}/{}, constant tables "
						 "{:016x}/{:016x}): the row holds the last one's <- ROW PACKING CONFLICT",
				m, a_first, a_second, first[0], second[0], first[1], second[1], first[2], second[2]);
		}
	}

	void MainBuild::WriteMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, PairWork& a_work)
	{
		auto& state = rows.materials[m];
		const auto key = MaterialRowKey(m, p, blocks);
		if (state.written && state.key == key)
			return;
		// Which of the key's parts moved (the report's).
		if (state.written)
			for (std::size_t k = 0; k < key.size(); ++k)
				a_work.keyMoved[k] += state.key[k] != key[k] ? 1u : 0u;
		MaterialRow row;
		PackMaterialRow(m, p, blocks, key, state, row);
		// Only the parts that moved are marked (an animated effect's TexcoordOffset or falloff moves one, not the row): a backing catches up
		// those parts alone (EmitMainRowParts).
		if (!a_work.deferred) {
			if (rows.material.SetParts(m, row, kMaterialRowParts))
				++a_work.materialRowsWritten;
		} else if (const std::uint64_t parts = WriteKeptParts(rows.material, m, row, kMaterialRowParts)) {
			a_work.rowMarks.emplace_back(m, parts);
			++a_work.materialRowsWritten;
		}
		++a_work.materialsWritten;
	}

	void MainBuild::PackMaterialRow(std::uint32_t m, std::uint32_t p, const PipelineBlocks& blocks, const std::array<std::uint32_t, 8>& a_key, MainRows::MaterialState& state,
		MaterialRow& row) const
	{
		const Lookups::Material* lookup = m < lookups.materials.size() ? &lookups.materials[m] : nullptr;
		const bool projected = a_key[5] != 0;
		const auto& material = tables.materials[m];
		const auto& technique = tables.TechniqueOf(p).value;
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
			// TexcoordOffset's place: the row holds the record's (capture-time) value, which the frame's patch overwrites with the buffer
			// the frame's shaders read (TextureTransformList, TextureTransformsCS).
			constexpr std::uint32_t kLightingVSTexcoordOffset = 11;
			const std::uint32_t variable = effect ? kEffectVSTexcoordOffset : kLightingVSTexcoordOffset;
			if (variable < vsLayout.count && ((vsGroup >> variable) & 1)) {
				const std::uint32_t first = vsLayout.offset[variable];
				const std::uint32_t dword = PackedPositionOf(vsLayout, blocks.vsTable, vsGroup, vsFirst, first);
				std::uint8_t mask = 0;
				for (std::uint32_t c = 0; dword != ~0u && c < vsLayout.size[variable] && c < 4; ++c)
					if (material.vs.Written(first + c) && PackedPositionOf(vsLayout, blocks.vsTable, vsGroup, vsFirst, first + c) == dword + c &&
						(dword + c) * 4 + 4 <= kMaterialRowVSBytes)
						mask |= static_cast<std::uint8_t>(1u << c);
				if (mask) {
					state.texcoordDword = dword;
					state.texcoordMask = mask;
				}
			}
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
		state.key = a_key;
		state.written = true;
	}

	// The resident region (drawcall-limit-fix.md, "Persistent resident draws"): draw inputs kept across frames, changed only by
	// the tables' change log, a pipeline's set index or a pair's record failing. The loop skips its objects; PayloadBuilder sends
	// a backing only what changed since its content epoch.
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

	void MainBuild::RegionAcquire(std::uint64_t a_key, std::uint32_t o)
	{
		auto& r = *region;
		auto [found, fresh] = r.pairs.try_emplace(a_key);
		auto& pair = found->second;
		if (fresh) {
			// Its rows, once UpdateRegionPairs finds the pair can draw.
			pair.slot = kNoRecord;
			pair.ok = false;
			pair.skip = kNoSkip;
			auto& materialList = r.materialPairs[static_cast<std::uint32_t>(a_key >> 32)];
			pair.materialAt = static_cast<std::uint32_t>(materialList.size());
			materialList.push_back(a_key);
			auto& pipelineList = r.pipelinePairs[static_cast<std::uint32_t>(a_key)];
			pair.pipelineAt = static_cast<std::uint32_t>(pipelineList.size());
			pipelineList.push_back(a_key);
			r.freshPairs.push_back(a_key);
		}
		if (r.pairObjectAt.size() <= o)
			r.pairObjectAt.resize(std::max<std::size_t>(std::size_t(o) + 1, tables.objects.size()), 0);
		r.pairObjectAt[o] = static_cast<std::uint32_t>(pair.objects.size());
		pair.objects.push_back(o);
		const auto pipeline = static_cast<std::uint32_t>(a_key);
		auto [state, freshPipeline] = r.pipelines.try_emplace(pipeline, std::pair{ pipelineBlocks[pipeline].setIndex, 0u });
		++state->second.second;
	}

	void MainBuild::RegionRelease(std::uint64_t a_key, std::uint32_t o)
	{
		auto& r = *region;
		if (a_key == kNoPair)
			return;
		if (const auto found = r.pairs.find(a_key); found != r.pairs.end()) {
			auto& pair = found->second;
			if (const std::uint32_t at = o < r.pairObjectAt.size() ? r.pairObjectAt[o] : ~0u; at < pair.objects.size() && pair.objects[at] == o) {
				const std::uint32_t moved = pair.objects.back();
				pair.objects[at] = moved;
				r.pairObjectAt[moved] = at;
				pair.objects.pop_back();
			}
			if (pair.objects.empty()) {
				if (!pair.ok && pair.skip != kNoSkip)
					--r.skipCounts[pair.skip];
				SetPairRegisters(pair, {});
				ReleaseOwner(pair.bindingSeen);
				ReleaseOwner(pair.maskSeen);
				// Out of its material's and its pipeline's lists by place: the last one takes it.
				auto unlist = [&](auto& a_lists, std::uint32_t a_list, std::uint32_t a_at, std::uint32_t ResidentRegion::Pair::*a_place) {
					const auto list = a_lists.find(a_list);
					if (list == a_lists.end())
						return;
					auto& keys = list->second;
					if (a_at < keys.size() && keys[a_at] == a_key) {
						const std::uint64_t moved = keys.back();
						keys[a_at] = moved;
						keys.pop_back();
						if (moved != a_key)
							if (const auto other = r.pairs.find(moved); other != r.pairs.end())
								other->second.*a_place = a_at;
					}
					if (keys.empty())
						a_lists.erase(list);
				};
				unlist(r.materialPairs, static_cast<std::uint32_t>(a_key >> 32), pair.materialAt, &ResidentRegion::Pair::materialAt);
				unlist(r.pipelinePairs, static_cast<std::uint32_t>(a_key), pair.pipelineAt, &ResidentRegion::Pair::pipelineAt);
				r.pairs.erase(found);
			}
		}
		if (const auto state = r.pipelines.find(static_cast<std::uint32_t>(a_key)); state != r.pipelines.end() && --state->second.second == 0)
			r.pipelines.erase(state);
	}

	void MainBuild::SetPairRegisters(ResidentRegion::Pair& a_pair, const std::array<std::uint64_t, 2>& a_registers)
	{
		auto& r = *region;
		for (std::size_t w = 0; w < 2; ++w) {
			const std::uint64_t held = a_pair.frameRegisters[w], now = a_registers[w];
			for (std::uint64_t gone = held & ~now; gone; gone &= gone - 1) {
				const auto bit = static_cast<std::uint32_t>(std::countr_zero(gone));
				if (--r.frameRegisterCounts[w * 64 + bit] == 0)
					r.frameRegisters[w] &= ~(1ull << bit);
			}
			for (std::uint64_t added = now & ~held; added; added &= added - 1) {
				const auto bit = static_cast<std::uint32_t>(std::countr_zero(added));
				if (r.frameRegisterCounts[w * 64 + bit]++ == 0)
					r.frameRegisters[w] |= 1ull << bit;
			}
		}
		a_pair.frameRegisters = a_registers;
	}

	void MainBuild::AcquireOwner(const void* a_owner, const std::shared_ptr<const void>& a_held)
	{
		if (!a_owner)
			return;
		auto& r = *region;
		auto [owner, fresh] = r.ownerCounts.try_emplace(a_owner, std::pair{ a_held, 0u });
		r.ownersChanged |= fresh;
		++owner->second.second;
	}

	void MainBuild::ReleaseOwner(const void* a_owner)
	{
		if (!a_owner)
			return;
		auto& r = *region;
		if (const auto owner = r.ownerCounts.find(a_owner); owner != r.ownerCounts.end() && --owner->second.second == 0) {
			r.ownerCounts.erase(owner);
			r.ownersChanged = true;
		}
	}

	void MainBuild::SetPairOwners(ResidentRegion::Pair& a_pair, std::uint32_t m, std::uint32_t p, const void* a_binding, const void* a_mask)
	{
		// The owner counted holds the pointer's object, so a new owner never reuses a counted one's address.
		if (a_binding != a_pair.bindingSeen) {
			AcquireOwner(a_binding, a_binding ? lookups.materials[m].bindingBlock : nullptr);
			ReleaseOwner(a_pair.bindingSeen);
			a_pair.bindingSeen = a_binding;
		}
		if (a_mask != a_pair.maskSeen) {
			AcquireOwner(a_mask, a_mask ? lookups.pipelines[p].shadowMaskOwner : nullptr);
			ReleaseOwner(a_pair.maskSeen);
			a_pair.maskSeen = a_mask;
		}
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
		return { 0, 0, object.geometryIndex < tables.geometries.size() ? object.geometryIndex : 0u, object.flags, { MainMaskOf(tables, o) }, o, tables.decalOrdinal[o],
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
		RegionRelease(r.pairOf[i], o);
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
				RegionAcquire(key, o);
		} else if (r.pairOf[i] != key) {
			RegionRelease(r.pairOf[i], o);
			if (key != kNoPair)
				RegionAcquire(key, o);
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
		// The journal keeps what the oldest payload backing lacks; only later changes are new (once per publication, by whichever
		// build writes first).
		r.Begin(in.inputsOldestContentEpoch);
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
			++rows.pairStats.resyncs;
			if (!serialFanout && tables.objects.size() >= kResyncFanoutMin) {
				ResyncRegionParallel();
				if (Policy::CheckRegionResync())
					CheckRegionResync();
			} else {
				for (std::uint32_t o = 0; o < tables.objects.size(); ++o)
					RegionTake(o);
			}
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

	void MainBuild::ResyncRegionParallel()
	{
		auto& r = *region;
		const std::size_t count = tables.objects.size();
		// 1. Per slot on the pool: whether the region holds it, its pair and its entry. The pairs are all new (the Reset emptied them), so
		// RegionEntry finds none and every entry waits for UpdateRegionPairs, as the serial path's fresh pairs do.
		std::vector<std::uint8_t> eligible(count, 0), draws(count, 0);
		std::vector<std::uint64_t> keys(count, kNoPair);
		std::vector<DrawInput> entries(count);
		auto& executor = SceneScheduler::Executor();
		executor.ParallelFor("CS.DCLF.BuildMain.Region.Resync", count, kResyncGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.BuildMain.Region.ResyncShards");
			for (std::size_t n = a_begin; n < a_end; ++n) {
				const auto o = static_cast<std::uint32_t>(n);
				if (!RegionEligible(o))
					continue;
				eligible[n] = 1;
				const auto& object = tables.objects[o];
				keys[n] = (object.flags & kObjectNoBindings) ? kNoPair : PairKeyOf(object);
				DrawInput fresh{};
				draws[n] = RegionEntry(o, fresh);
				SetMainPart(entries[n], fresh);
			}
		});
		// 2. In slot order (the serial path's): dense entry indices (a prefix sum), the pairs acquired, the region's totals.
		std::size_t held = 0;
		for (std::size_t n = 0; n < count; ++n)
			held += eligible[n];
		r.pairOf.assign(held, kNoPair);
		r.drawsOf.assign(held, 0);
		r.sideOf.assign(held, kSideMain);
		r.Cover(count);
		r.inputs.Mutable().resize(held);
		std::uint32_t i = 0;
		for (std::size_t n = 0; n < count; ++n) {
			const auto o = static_cast<std::uint32_t>(n);
			r.touched.push_back(o);
			if (!eligible[n])
				continue;
			r.indexOf[o] = i;
			r.pairOf[i] = keys[n];
			++r.undrawable;
			if (keys[n] != kNoPair)
				RegionAcquire(keys[n], o);
			RegionSetDraws(i, draws[n]);
			++i;
		}
		// 3. The entries scattered into their places on the pool, and one journal resync for them all (the Reset began one).
		PrepareKeptWrites(r.inputs, 0, held);
		auto& list = r.inputs.Mutable();
		executor.ParallelFor("CS.DCLF.BuildMain.Region.Scatter", count, kResyncGrain, [&](std::size_t a_begin, std::size_t a_end) {
			ZoneScopedN("CS.DCLF.BuildMain.Region.ScatterShards");
			for (std::size_t n = a_begin; n < a_end; ++n)
				if (eligible[n])
					list[r.indexOf[n]] = entries[n];
		});
		r.inputs.Resync();
		++rows.pairStats.resyncsFannedOut;
	}

	void MainBuild::CheckRegionResync()
	{
		auto& r = *region;
		std::uint64_t mismatches = 0;
		std::string first;
		const auto& inputs = r.inputs.Get();
		for (std::uint32_t o = 0; o < tables.objects.size(); ++o) {
			const bool eligible = RegionEligible(o);
			const std::uint32_t i = r.EntryOf(o);
			std::string what;
			if (eligible != (i != kNoRegion && (r.sideOf[i] & kSideMain)))
				what = fmt::format("slot {}: eligible {}, held {}", o, eligible, i != kNoRegion);
			else if (eligible) {
				const auto& object = tables.objects[o];
				const std::uint64_t key = (object.flags & kObjectNoBindings) ? kNoPair : PairKeyOf(object);
				DrawInput fresh{}, expected{};
				const std::uint8_t draws = RegionEntry(o, fresh);
				SetMainPart(expected, fresh);
				if (key != r.pairOf[i] || draws != r.drawsOf[i] || std::memcmp(&expected, &inputs[i], sizeof(DrawInput)) != 0)
					what = fmt::format("slot {} (entry {}): pair {:x}/{:x}, draws {}/{}", o, i, key, r.pairOf[i], draws, r.drawsOf[i]);
			}
			if (!what.empty() && !mismatches++)
				first = std::move(what);
		}
		++rows.pairStats.parityChecks;
		rows.pairStats.parityMismatches += mismatches;
		if (mismatches)
			logger::warn("[DCLF] main region resync on the pool: {} slots differ from the serial path (first: {}) <- REGION RESYNC", mismatches, first);
	}

	std::uint64_t MainBuild::PipelineWitness(std::uint32_t p) const
	{
		// A hash of what a pair's verdict and its material row's packing read of the pipeline: the shared lookups, the technique's
		// bindings, projection, the constant tables, the set, the raster flags (forward, effect), both variants' register usage, its row's
		// verdict parts and its shadow mask's owner. Not its constants: they move its row's bytes (WritePipelineRow), never a verdict.
		std::uint64_t value = 0xcbf29ce484222325ull;
		const auto& blocks = pipelineBlocks[p];
		value = Mix(value, lookups.sharedVersion);
		value = Mix(value, tables.TechniqueOf(p).bindingVersion);
		value = Mix(value, (tables.pipelines[p].passDescriptor & 0x8000u) != 0 ? 1u : 0u);
		value = Mix(value, blocks.tables);
		value = Mix(value, blocks.setIndex);
		value = Mix(value, tables.pipelines[p].rasterFlags);
		for (const auto* usage : blocks.usage) {
			if (!usage)
				continue;
			value = Mix(value, (std::uint64_t(usage->vertexConstants) << 32) | usage->pixelConstants);
			value = Mix(value, usage->textures[0]);
			value = Mix(value, usage->textures[1]);
			value = Mix(value, usage->samplers);
		}
		const auto& pipeline = rows.pipelines[p];
		value = Mix(value, (pipeline.written ? 1u : 0u) | (pipeline.blocksOk ? 2u : 0u) | (pipeline.shadowMask ? 4u : 0u) | (pipeline.shadowMaskSampler ? 8u : 0u));
		return Mix(value, reinterpret_cast<std::uintptr_t>(p < lookups.pipelines.size() ? lookups.pipelines[p].shadowMaskOwner.get() : nullptr));
	}

	void MainBuild::ResolvePairsChunk(const std::vector<std::uint64_t>& a_check, std::size_t a_begin, std::size_t a_end, bool a_everyPair, PairWork& a_work)
	{
		// Reads the pairs (no insert or erase meanwhile: each a distinct slot of the map) and writes only its materials' rows and states.
		auto& r = *region;
		for (std::size_t k = a_begin; k < a_end; ++k) {
			const std::uint64_t key = a_check[k];
			const auto found = r.pairs.find(key);
			if (found == r.pairs.end())
				continue;
			auto& pair = found->second;
			const auto p = static_cast<std::uint32_t>(key);
			const auto m = static_cast<std::uint32_t>(key >> 32);
			const bool pipelineOk = p < pipelineBlocks.size() && pipelineBlocks[p].setIndex != Lookups::kNone;
			const void* binding = m < lookups.materials.size() ? lookups.materials[m].bindingBlock.get() : nullptr;
			const void* mask = p < lookups.pipelines.size() ? lookups.pipelines[p].shadowMaskOwner.get() : nullptr;
			// The row first (its bytes follow the material's frame values whatever the verdict); the verdict again only when what it reads
			// moved: the pipeline's witness or the row's state.
			std::uint64_t witness = 1ull;
			if (pipelineOk) {
				WriteMaterialRow(m, p, pipelineBlocks[p], a_work);
				witness = Mix(r.pipelineWitnessNow[p], MaterialStateWitness(rows.materials[m])) | 1;  // never 0, which is "never resolved"
			}
			if (!a_everyPair && witness == pair.witness && binding == pair.bindingSeen && mask == pair.maskSeen)
				continue;
			PairWork::Verdict verdict{ &pair, key, witness, pair.slot, kNoSkip, pipelineOk, {}, binding, mask };
			if (pipelineOk) {
				ResolvedBindings resolved;
				ResolvePair(m, p, pipelineBlocks[p], rows.materials[m], resolved, verdict.registers, a_work);
				if (resolved.skipReason != kNoSkip) {
					// Counted as the draw AssembleRecord would refuse: once per resolution.
					verdict.ok = false;
					verdict.skip = resolved.skipReason;
					if (resolved.skipReason == static_cast<std::uint32_t>(Skip::Texture))
						a_work.missingTextures.push_back(resolved.missingTexture);
					if (resolved.deferred)
						++a_work.deferredTextures;
					++a_work.skipped[resolved.skipReason];
				} else {
					verdict.slot = resolved.recordIndex;
				}
			}
			a_work.verdicts.push_back(verdict);
		}
	}

	void MainBuild::UpdateRegionPairs()
	{
		auto& r = *region;
		auto& stats = rows.pairStats;
		++stats.builds;
		// Entries of pipelines whose set index changed and of pairs that became (un)buildable are rewritten (both rare).
		std::vector<std::uint32_t> changedPipelines;
		for (auto& [pipeline, state] : r.pipelines)
			if (pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != state.first) {
				state.first = pipelineBlocks[pipeline].setIndex;
				changedPipelines.push_back(pipeline);
			}
		// A pair resolves again only when what it reads changed (its pipeline's witness, its material row's state, frame slots, owners).
		// Candidates come from events: the material logs, new pairs, the pairs of moved pipelines; every pair when the frame's half moved
		// or a log is lost.
		std::uint64_t frameWitness = 0xcbf29ce484222325ull ^ Segments();
		for (const auto* segment : segmentIn)
			if (segment)
				frameWitness = Mix(frameWitness, (std::uint64_t(segment->vsFrameMask) << 32) | segment->psFrameMask);
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
		r.pipelineWitnessNow.assign(pipelineBlocks.size(), 0);
		std::vector<std::uint32_t> movedPipelines;
		for (const auto& [pipeline, state] : r.pipelines) {
			if (pipeline >= pipelineBlocks.size())
				continue;
			if (pipelineBlocks[pipeline].setIndex != Lookups::kNone)
				r.pipelineWitnessNow[pipeline] = PipelineWitness(pipeline);
			auto& seen = r.pipelineWitness[pipeline];
			if (seen != r.pipelineWitnessNow[pipeline])
				movedPipelines.push_back(pipeline);
			seen = r.pipelineWitnessNow[pipeline];
		}
		TracyCZoneEnd(pairsPipelinesZone);
		const auto resolveStart = std::chrono::steady_clock::now();
		TracyCZoneN(pairsResolveZone, "CS.DCLF.BuildMain.Pairs.Resolve", true);
		std::vector<std::uint64_t> check;
		if (everyPair) {
			++stats.everyPair;
			check.reserve(r.pairs.size());
			for (const auto& [key, pair] : r.pairs)
				check.push_back(key);
		} else {
			auto listed = [&](const auto& a_lists, std::uint32_t a_list) {
				if (const auto list = a_lists.find(a_list); list != a_lists.end())
					check.insert(check.end(), list->second.begin(), list->second.end());
			};
			for (const std::uint32_t m : r.materialCursor.Unread(tables.materialLog))
				listed(r.materialPairs, m);
			for (const std::uint32_t m : r.lookupCursor.Unread(lookups.materialLog))
				listed(r.materialPairs, m);
			for (const std::uint32_t p : movedPipelines)
				listed(r.pipelinePairs, p);
			check.insert(check.end(), r.freshPairs.begin(), r.freshPairs.end());
		}
		// Sorted: a material's pairs are adjacent (its slot is the key's high half), so chunks split on material boundaries.
		std::sort(check.begin(), check.end());
		check.erase(std::unique(check.begin(), check.end()), check.end());
		stats.pipelinesMoved += movedPipelines.size();
		stats.checked += check.size();
		r.freshPairs.clear();
		r.materialCursor.Advance(tables.materialLog);
		r.lookupCursor.Advance(lookups.materialLog);
		// The chunks: whole materials, at least kPairGrain pairs each; one for the serial path.
		const bool fanout = !serialFanout && check.size() >= kPairFanoutMin;
		std::vector<std::size_t> bounds{ 0 };
		if (fanout)
			for (std::size_t next = kPairGrain; next < check.size(); next += kPairGrain) {
				while (next < check.size() && (check[next] >> 32) == (check[next - 1] >> 32))
					++next;
				if (next >= check.size())
					break;
				bounds.push_back(next);
			}
		bounds.push_back(check.size());
		const std::size_t chunks = bounds.size() - 1;
		std::vector<PairWork> works(chunks);
		if (fanout) {
			// The rows the chunks write are made writable here, on the builder's thread; the chunks mark nothing (MergeWork does).
			for (std::size_t k = 0; k < check.size(); ++k)
				if (k == 0 || (check[k] >> 32) != (check[k - 1] >> 32))
					PrepareKeptWrites(rows.material, static_cast<std::size_t>(check[k] >> 32), 1);
			for (auto& work : works)
				work.deferred = true;
			SceneScheduler::Executor().ParallelFor("CS.DCLF.BuildMain.Pairs.Resolve", chunks, 1, [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.BuildMain.Pairs.ResolveShards");
				for (std::size_t c = a_begin; c < a_end; ++c)
					ResolvePairsChunk(check, bounds[c], bounds[c + 1], everyPair, works[c]);
			});
			++stats.fannedOut;
			stats.chunks += chunks;
		} else {
			ResolvePairsChunk(check, 0, check.size(), everyPair, works[0]);
		}
		// Applied in chunk order (the check list's): the rows' marks and counters, then each verdict onto its pair and the bundle's counts.
		for (auto& work : works) {
			for (const auto& verdict : work.verdicts) {
				auto& pair = *verdict.pair;
				const auto p = static_cast<std::uint32_t>(verdict.key);
				const auto m = static_cast<std::uint32_t>(verdict.key >> 32);
				if (!pair.ok && pair.skip != kNoSkip)
					--r.skipCounts[pair.skip];
				pair.skip = verdict.skip;
				if (!verdict.ok && verdict.skip != kNoSkip)
					++r.skipCounts[verdict.skip];
				SetPairRegisters(pair, verdict.registers);
				SetPairOwners(pair, m, p, verdict.binding, verdict.mask);
				pair.witness = verdict.witness;
				if (verdict.ok != pair.ok || (verdict.ok && verdict.slot != pair.slot)) {
					pair.ok = verdict.ok;
					if (verdict.ok)
						pair.slot = verdict.slot;
					changedPairs.push_back(verdict.key);
				}
			}
			stats.resolved += work.verdicts.size();
			MergeWork(work);
		}
		TracyCZoneEnd(pairsResolveZone);
		const auto bundleStart = std::chrono::steady_clock::now();
		const double resolveMs = std::chrono::duration<double, std::milli>(bundleStart - resolveStart).count();
		stats.resolveMs += resolveMs;
		stats.resolveMaxMs = std::max(stats.resolveMaxMs, resolveMs);
		TracyCZoneN(pairsBundleZone, "CS.DCLF.BuildMain.Pairs.Bundle", true);
		for (std::size_t reason = 0; reason < r.skipCounts.size(); ++reason)
			out.skipped[reason] += r.skipCounts[reason];
		// Every pair's owners in one list, made again only when an owner joined or left (the counts follow each resolution).
		if (r.ownersChanged) {
			r.ownersChanged = false;
			auto owners = std::make_shared<std::vector<std::shared_ptr<const void>>>();
			owners->reserve(r.ownerCounts.size());
			for (const auto& [pointer, held] : r.ownerCounts)
				owners->push_back(held.first);
			r.owners = std::move(owners);
			++stats.bundles;
		}
		out.frameRegisters[0] |= r.frameRegisters[0];
		out.frameRegisters[1] |= r.frameRegisters[1];
		if (r.owners)
			out.bindingOwners.push_back(r.owners);
		TracyCZoneEnd(pairsBundleZone);
		const auto inputsStart = std::chrono::steady_clock::now();
		stats.bundleMs += std::chrono::duration<double, std::milli>(inputsStart - bundleStart).count();
		{
			ZoneScopedN("CS.DCLF.BuildMain.Pairs.Inputs");
			// The entries of the changed pairs and of the pipelines whose set moved, through their objects (Pair::objects).
			if (!changedPipelines.empty() || !changedPairs.empty()) {
				auto& inputs = r.inputs.Mutable();
				auto rewrite = [&](const ResidentRegion::Pair& a_pair) {
					for (const std::uint32_t o : a_pair.objects) {
						const std::uint32_t i = r.EntryOf(o);
						if (i == kNoRegion || !(r.sideOf[i] & kSideMain))
							continue;
						DrawInput fresh{};
						RegionSetDraws(i, RegionEntry(o, fresh));
						SetMainPart(inputs[i], fresh);
						r.inputs.Mark(i);
						r.touched.push_back(o);
						++stats.entriesRewritten;
					}
				};
				for (const std::uint64_t key : changedPairs)
					if (const auto pair = r.pairs.find(key); pair != r.pairs.end())
						rewrite(pair->second);
				for (const std::uint32_t pipeline : changedPipelines)
					if (const auto list = r.pipelinePairs.find(pipeline); list != r.pipelinePairs.end())
						for (const std::uint64_t key : list->second)
							if (const auto pair = r.pairs.find(key); pair != r.pairs.end())
								rewrite(pair->second);
			}
		}
		stats.inputsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
		if (Policy::CheckBuilderPairs(rows.builds))
			CheckNarrowedPairs();
		if (checkPacking)
			CheckRegionPacking();
	}

	void MainBuild::CheckNarrowedPairs()
	{
		// Every pair resolved again serially from scratch (its row packed anew, nothing stored), against what the narrowed checks and the
		// pool's chunks left: the pair's verdict, frame textures and owners, its row's bytes and state, the bundle's counts, the pair index.
		auto& r = *region;
		auto& stats = rows.pairStats;
		++stats.parityChecks;
		std::uint64_t mismatches = 0;
		std::string first;
		auto note = [&](auto&& a_what) {
			if (!mismatches++)
				first = a_what();
		};
		PairWork scratch;
		std::array<std::uint64_t, 2> registersAll{};
		ankerl::unordered_dense::map<const void*, std::uint32_t> ownersAll;
		std::size_t objectsAll = 0;
		const auto& materialRows = rows.material.Get();
		for (const auto& [key, pair] : r.pairs) {
			const auto p = static_cast<std::uint32_t>(key);
			const auto m = static_cast<std::uint32_t>(key >> 32);
			registersAll[0] |= pair.frameRegisters[0];
			registersAll[1] |= pair.frameRegisters[1];
			for (const void* owner : { pair.bindingSeen, pair.maskSeen })
				if (owner)
					++ownersAll[owner];
			objectsAll += pair.objects.size();
			const void* binding = m < lookups.materials.size() ? lookups.materials[m].bindingBlock.get() : nullptr;
			const void* mask = p < lookups.pipelines.size() ? lookups.pipelines[p].shadowMaskOwner.get() : nullptr;
			if (binding != pair.bindingSeen || mask != pair.maskSeen)
				note([&] { return fmt::format("pair ({}, {}): owners not the lookups'", m, p); });
			const bool pipelineOk = p < pipelineBlocks.size() && pipelineBlocks[p].setIndex != Lookups::kNone;
			if (!pipelineOk) {
				if (pair.ok)
					note([&] { return fmt::format("pair ({}, {}): drawable without its pipeline's set", m, p); });
				continue;
			}
			const auto& blocks = pipelineBlocks[p];
			const auto rowKey = MaterialRowKey(m, p, blocks);
			const auto& held = rows.materials[m];
			if (held.key != rowKey) {
				// Another pipeline's packing (the row's material half the same): CheckRegionPacking's finding, not the narrowing's.
				if (std::equal(rowKey.begin(), rowKey.begin() + 4, held.key.begin()) && held.written)
					continue;
				note([&] { return fmt::format("pair ({}, {}): material row's sources moved unseen", m, p); });
				continue;
			}
			MainRows::MaterialState state;
			MaterialRow row;
			PackMaterialRow(m, p, blocks, rowKey, state, row);
			if (!SameMaterialState(state, held) || m >= materialRows.size() || std::memcmp(&row, &materialRows[m], sizeof(MaterialRow)) != 0)
				note([&] { return fmt::format("material row {} (pipeline {}): bytes or state differ from a fresh packing", m, p); });
			ResolvedBindings resolved;
			std::array<std::uint64_t, 2> registers{};
			ResolvePair(m, p, blocks, state, resolved, registers, scratch);
			const bool ok = resolved.skipReason == kNoSkip;
			if (ok != pair.ok || (ok && resolved.recordIndex != pair.slot) || (!ok && resolved.skipReason != pair.skip) || registers != pair.frameRegisters)
				note([&] {
					return fmt::format("pair ({}, {}): verdict {} slot {:x} skip {} registers {:x}/{:x}, kept {} {:x} {} {:x}/{:x}", m, p, ok, resolved.recordIndex,
						resolved.skipReason, registers[0], registers[1], pair.ok, pair.slot, pair.skip, pair.frameRegisters[0], pair.frameRegisters[1]);
				});
		}
		if (registersAll != r.frameRegisters)
			note([&] { return std::string("the bundle's frame textures"); });
		bool ownersSame = ownersAll.size() == r.ownerCounts.size();
		for (const auto& [owner, count] : ownersAll) {
			const auto held = r.ownerCounts.find(owner);
			ownersSame = ownersSame && held != r.ownerCounts.end() && held->second.second == count;
		}
		if (!ownersSame)
			note([&] { return fmt::format("the bundle's owners ({} counted, {} held)", ownersAll.size(), r.ownerCounts.size()); });
		// The pair index: every main entry with a pair is listed once, at its place, in its pair's objects.
		std::size_t entriesAll = 0;
		const auto& inputs = r.inputs.Get();
		for (std::uint32_t i = 0; i < r.pairOf.size(); ++i) {
			if (!(r.sideOf[i] & kSideMain) || r.pairOf[i] == kNoPair)
				continue;
			++entriesAll;
			const std::uint32_t o = inputs[i].objectIndex;
			const auto pair = r.pairs.find(r.pairOf[i]);
			if (pair == r.pairs.end() || o >= r.pairObjectAt.size() || r.pairObjectAt[o] >= pair->second.objects.size() || pair->second.objects[r.pairObjectAt[o]] != o)
				note([&] { return fmt::format("entry {} (slot {}): not in its pair's objects", i, o); });
		}
		if (entriesAll != objectsAll)
			note([&] { return fmt::format("{} entries with pairs, {} pair objects", entriesAll, objectsAll); });
		stats.parityMismatches += mismatches;
		if (mismatches)
			logger::warn("[DCLF] main pairs: {} differ from every pair resolved again (first: {}) <- PAIRS NARROWED", mismatches, first);
	}

	void MainBuild::CheckRegionPacking()
	{
		auto& r = *region;
		++rows.pairStats.packingChecks;
		for (const auto& [m, keys] : r.materialPairs) {
			std::uint32_t firstPipeline = ~0u;
			std::array<std::uint64_t, 3> packing{};
			for (const std::uint64_t key : keys) {
				const auto p = static_cast<std::uint32_t>(key);
				if (p >= pipelineBlocks.size() || pipelineBlocks[p].setIndex == Lookups::kNone)
					continue;
				if (firstPipeline == ~0u) {
					firstPipeline = p;
					packing = PackingOf(p);
				} else if (PackingOf(p) != packing) {
					NotePackingConflict(m, firstPipeline, p);
					break;
				}
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
		// Past the scene's buffers' named versions: no input, except a decal's blank, which keeps its slot as a zero-count draw.
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
		// The CPU template of what BuildDraws writes (for the build parity). Its row addresses are byte offsets into the tables: the
		// tables are the payload's backings (PayloadVersions), which the build parity adds; the draws embed none of them.
		Mark(2);
		auto sequence = tables.draws[o];
		sequence.pipelineIndex = blocks.setIndex;
		sequence.objectIndex = o;
		sequence.pipelineRowAddress = std::uint64_t(recordIndex >> kRowPipelineShift) * kPipelineRowBytes;
		sequence.materialRowAddress = std::uint64_t(recordIndex & kRowMaterialMask) * kMaterialRowBytes;
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

	void MainBuild::Finish(std::shared_ptr<const StreamViews> a_streams)
	{
		ZoneScopedN("CS.DCLF.BuildMain.Finish");
		streams = std::move(a_streams);
		TakeGeometryDraws(streams.get(), tables, in.tablesGeneration, frameNumber, out.geometryDraws);
		out.streams = streams;
		AppendFaceStreams(tables, in.addresses.facePositions, out.geometryDraws);
		if (in.addresses.facePositions)
			out.faceStreams = tables.faceStreams;
		out.materialRows = rows.material.View();
		out.pipelineRows = rows.pipeline.View();
		// The rows the frame patches TexcoordOffset into (TextureTransformList): every kept row (MainRows::materials, whatever build packed
		// it) packed from its slot's current record that places it, its slot keyed (its material held while the publication lives). All of
		// them, scrolling or not: a backing's bytes are then the frame's value whatever patches its history holds. A slot's liveness is its
		// key (cleared when freed: Slots.cpp): the slot tables' allocation state is the coordinator's alone, never published.
		{
			ZoneScopedN("CS.DCLF.BuildMain.TextureTransforms");
			auto list = std::make_shared<TextureTransformList>();
			const std::size_t slots = std::min({ rows.materials.size(), tables.materials.size(), tables.materialSlotKey.size(), tables.materialVersion.size() });
			std::uint64_t unpacked = 0, noTexcoord = 0, freeSlot = 0, stale = 0;
			for (std::uint32_t m = 0; m < slots; ++m) {
				const auto& state = rows.materials[m];
				if (!state.written || !state.blocksOk) {
					++unpacked;
					continue;
				}
				if (!state.texcoordMask) {
					++noTexcoord;
					continue;
				}
				if (!tables.materialSlotKey[m].first) {
					++freeSlot;
					continue;
				}
				if (state.key[0] != tables.materialVersion[m]) {
					++stale;
					continue;
				}
				list->entries.push_back({ tables.materialSlotKey[m].first,
					static_cast<std::uint32_t>(std::uint64_t(m) * kMaterialRowBytes + kMaterialRowVS + std::uint64_t(state.texcoordDword) * 4), state.texcoordMask });
			}
			namespace stats = TextureTransformStats;
			stats::built.fetch_add(1, std::memory_order_relaxed);
			stats::builtEntries.fetch_add(list->entries.size(), std::memory_order_relaxed);
			stats::rowsUnpacked.fetch_add(unpacked, std::memory_order_relaxed);
			stats::rowsNoTexcoord.fetch_add(noTexcoord, std::memory_order_relaxed);
			stats::rowsFreeSlot.fetch_add(freeSlot, std::memory_order_relaxed);
			stats::rowsStale.fetch_add(stale, std::memory_order_relaxed);
			out.textureTransforms = std::move(list);
		}
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
		MainBuild build(a_in, a_tables, a_frame, a_lookups, a_out, a_rows, a_cache);
		build.Run();
		build.Finish(std::move(a_streams));
	}

	struct MainBuildStages::State
	{
		MainBuild build;
	};

	MainBuildStages::MainBuildStages(const std::array<const MainInputs*, 2>& a_in, const SceneTables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
		const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache)
	{
		if (a_in[kAsyncColour] || a_in[kAsyncZPrepass])
			state.reset(new State{ MainBuild(a_in, a_tables, a_frame, a_lookups, a_out, a_rows, a_cache) });
	}

	MainBuildStages::~MainBuildStages() = default;

	void MainBuildStages::Run()
	{
		if (state)
			state->build.Run();
	}

	void MainBuildStages::Finish(std::shared_ptr<const StreamViews> a_streams)
	{
		if (state)
			state->build.Finish(std::move(a_streams));
	}

	std::string MainPairsReport(MainRows& a_rows)
	{
		auto& s = a_rows.pairStats;
		std::string text;
		if (s.builds || s.aheads) {
			const double builds = static_cast<double>(std::max<std::uint64_t>(s.builds, 1)), aheads = static_cast<double>(std::max<std::uint64_t>(s.aheads, 1));
			text = fmt::format("[DCLF] main pairs: {} builds; a build: {:.1f} pairs checked, {:.1f} resolved, {:.1f} entries rewritten; {} checked every pair, {} pipelines "
							   "moved; Pairs.Resolve {:.3f} ms a build (max {:.3f}), {} on the pool in {} chunks; bundle {:.3f} ms, {} made; inputs {:.3f} ms; resyncs {} ({} on "
							   "the pool); stages: {} aheads, {} overlapped, stream views {:.3f} ms, main {:.3f} ms, both {:.3f} ms, shadow {:.3f} ms, exclusions {:.3f} ms; "
							   "parity {} checks, {} differ, packing {} checks, {} conflicts{}\n",
				s.builds, s.checked / builds, s.resolved / builds, s.entriesRewritten / builds, s.everyPair, s.pipelinesMoved, s.resolveMs / builds, s.resolveMaxMs,
				s.fannedOut, s.chunks, s.bundleMs / builds, s.bundles, s.inputsMs / builds, s.resyncs, s.resyncsFannedOut, s.aheads, s.overlapped, s.streamsMs / aheads,
				s.mainMs / aheads, s.stagesMs / aheads, s.shadowMs / aheads, s.exclusionsMs / aheads, s.parityChecks, s.parityMismatches, s.packingChecks,
				s.packingConflicts, s.parityMismatches ? " <- PAIRS NARROWED" : s.packingConflicts ? " <- ROW PACKING CONFLICT" : "");
		}
		s = {};
		return text;
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
