// The revision producer, on the snapshot builder (DCLF's preparation pool, one pass at a time): MakeRevision makes a scene
// revision's shapes and latches from a publication and the frame's posted RevisionInputs, then AssembleRevision seals them. Also the
// main shape's plans (shared with the commits).

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	namespace Draws
	{
		void PlanZBuckets(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out)
		{
			const auto slots = static_cast<std::uint32_t>(a_layout.zBucketCapacity.size());
			const auto& groups = a_indirect.zGroups;
			const auto& keys = a_tables.pipelines;
			a_out.map.assign(slots, kNoBucket);
			a_out.calls.clear();
			// Per group, its bucket.
			std::vector<std::uint32_t> groupBucket(a_indirect.zPipelines.size(), kNoBucket);
			for (std::uint32_t p = 0; p < slots; ++p) {
				if (!a_layout.zBucketCapacity[p] || p >= a_lookups.pipelines.size() || p >= keys.size())
					continue;
				const auto& entry = a_lookups.pipelines[p];
				if (entry.setIndex == Lookups::kNone || !(entry.key == keys[p]) || entry.setIndex >= groups.size())
					continue;
				// No bucket for a family key not a depth segment input (ProgramBuild::ForwardOrDecal, the Effect port's: no forward object is
				// a depth input, and its pipeline has no Z-prepass pipeline).
				if (!Extensions::FamilyRules(keys[p].rasterFlags).depthSegment)
					continue;
				const std::uint32_t group = groups[entry.setIndex];
				auto& bucket = groupBucket[group];
				// A terrain group 1 slot's group (its depth pipeline's key holds no other: DrawPipelinesImpl.h, ZPipelineKey) has a twin
				// bucket right after its own, as large, for its near blocks (BuildDrawsCS, kBucketTerrainPair; MainOpaquePass).
				const bool terrain = RasterTerrainGroup(keys[p].rasterFlags) == 1;
				if (bucket == kNoBucket) {
					bucket = a_out.Buckets();
					a_out.calls.push_back({ bucket, 0u, 0u, a_indirect.zPipelines[group] });
					if (terrain)
						a_out.calls.push_back({ bucket + 1, 0u, 0u, a_indirect.zPipelines[group], true });
				}
				a_out.calls[bucket].capacity += a_layout.zBucketCapacity[p];
				if (terrain)
					a_out.calls[bucket + 1].capacity += a_layout.zBucketCapacity[p];
				a_out.map[p] = terrain ? (bucket | kBucketTerrainPair) : bucket;
			}
			const std::uint32_t buckets = a_out.Buckets();
			a_out.table.assign(std::size_t(buckets) * 4, 0u);
			std::uint32_t first = 0;
			for (std::uint32_t b = 0; b < buckets; ++b) {
				auto& call = a_out.calls[b];
				call.first = first;
				a_out.table[2 * b] = first;
				a_out.table[2 * b + 1] = call.capacity;
				// Phase 2's ranges are the same past the sequences' first draw range.
				a_out.table[2 * (buckets + b)] = first + a_layout.sequenceDraws;
				a_out.table[2 * (buckets + b) + 1] = call.capacity;
				first += call.capacity;
			}
		}

		namespace
		{
			/**
			 * @brief A pipeline slot's state class and the range of the colour segment its draws go to (BuildDrawsCS routes them by the same
			 * key bits, as object flags: decal group, forward, terrain group); false for a slot without a pipeline of the set version, or
			 * one whose draws no call of a class draws (refraction's plain draw).
			 */
			bool SlotClassRange(const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, std::uint32_t a_slot, std::uint32_t& a_class,
				std::uint32_t& a_range)
			{
				if (a_slot >= a_lookups.pipelines.size() || a_slot >= a_tables.pipelines.size())
					return false;
				const auto& entry = a_lookups.pipelines[a_slot];
				const auto& key = a_tables.pipelines[a_slot];
				if (entry.setIndex == Lookups::kNone || !(entry.key == key) || entry.setIndex >= a_indirect.classOf.size() || a_indirect.classOf[entry.setIndex] == kNoSetClass)
					return false;
				a_class = a_indirect.classOf[entry.setIndex] >> kSetClassShift;
				const std::uint32_t flags = key.rasterFlags;
				if (const std::uint32_t group = RasterDecalGroup(flags))
					a_range = ClassRangeOfSlot(group - 1);
				else if (RasterForward(flags))
					a_range = kClassRangeForward;
				else if (const std::uint32_t terrain = RasterTerrainGroup(flags))
					a_range = ClassRangeOfSlot(kDecalGroups + terrain - 1);
				else
					a_range = kClassRangeMain;
				return a_class < a_indirect.classes.size() && !(a_range == kClassRangeForward && RasterRefraction(flags));
			}

			/** @brief a_plan's sub-ranges laid out from their capacities (PlanSetClasses), and its latch words. */
			void LayOutSetClasses(const MainLayout& a_layout, const IndirectState& a_indirect, std::span<const std::array<bool, kDecalGroups>> a_decals,
				std::span<const std::uint32_t> a_forwardSets, std::span<const std::uint32_t> a_mainSets, SetClassPlan& a_plan)
			{
				auto& classes = a_plan.classes;
				std::uint32_t keys = 0, main = 0, decals = 0;
				std::array<std::uint32_t, kTerrainGroups> terrain{ a_layout.sequenceDraws, a_layout.sequenceDraws + a_layout.terrainCapacity[0] };
				std::uint32_t forward = a_layout.sequenceDraws + a_layout.terrainCapacity[0] + a_layout.terrainCapacity[1];
				for (std::size_t c = 0; c < classes.size(); ++c) {
					auto& ranges = classes[c].ranges;
					classes[c].keyBase = keys;
					classes[c].size = a_indirect.classes[c].pipelines;
					keys += classes[c].size;
					ranges[kClassRangeMain].first = main;
					main += ranges[kClassRangeMain].capacity;
					for (std::uint32_t g = 0; g < kTerrainGroups; ++g) {
						auto& range = ranges[ClassRangeOfSlot(kDecalGroups + g)];
						range.first = terrain[g];
						terrain[g] += range.capacity;
					}
					// The forward pairs: the class's staging half, then its sorted half.
					auto& pair = ranges[kClassRangeForward];
					pair.first = forward;
					forward += 2 * pair.capacity;
				}
				// The decal sub-ranges, group by group, each group's classes in order.
				for (std::uint32_t g = 0; g < kDecalGroups; ++g)
					for (std::size_t c = 0; c < classes.size(); ++c)
						if (a_decals[c][g])
							classes[c].ranges[ClassRangeOfSlot(g)] = { static_cast<std::uint32_t>(2ull * a_layout.sequenceDraws + std::uint64_t(decals++) * a_layout.sequenceDecals),
								a_layout.sequenceDecals };
				a_plan.decalRanges = decals;
				a_plan.table.assign(kClassHeaderWords, 0u);
				a_plan.table[0] = static_cast<std::uint32_t>(classes.size());
				a_plan.table[1] = kClassRanges;
				for (const auto& entry : classes) {
					const std::size_t at = a_plan.table.size();
					a_plan.table.resize(at + kClassWords, 0u);
					a_plan.table[at] = entry.keyBase;
					a_plan.table[at + 1] = entry.size;
					for (std::uint32_t r = 0; r < kClassRanges; ++r) {
						a_plan.table[at + 2 + 2 * r] = entry.ranges[r].first;
						a_plan.table[at + 3 + 2 * r] = entry.ranges[r].capacity;
					}
				}
				a_plan.map.assign(kClassHeaderWords, 0u);
				a_plan.map[0] = static_cast<std::uint32_t>(a_indirect.classOf.size());
				a_plan.map.insert(a_plan.map.end(), a_indirect.classOf.begin(), a_indirect.classOf.end());
				// The plain draws' buckets: a set index's slots' capacities, from a_first (a_halves each: the forward buckets' staging and sorted
				// halves); none unless every set index with any has a plain colour pipeline (else one frame would draw some draws twice or not
				// at all), then the section's words after the map's (the header's a_count word its buckets).
				const auto plan = [&](bool a_on, std::span<const std::uint32_t> a_sets, std::uint32_t a_first, std::uint32_t a_halves,
									  std::vector<SetClassPlan::ForwardBucket>& a_buckets, std::uint32_t a_count) {
					a_buckets.clear();
					bool plain = a_on && std::ranges::any_of(a_sets, [](std::uint32_t a_capacity) { return a_capacity != 0; });
					for (std::uint32_t set = 0; plain && set < a_sets.size(); ++set)
						plain = !a_sets[set] || (set < a_indirect.plainColour.size() && a_indirect.plainColour[set].valid());
					if (!plain)
						return;
					std::vector<std::uint32_t> bucketOf(a_indirect.classOf.size(), kNoBucket);
					for (std::uint32_t set = 0; set < a_sets.size(); ++set) {
						if (!a_sets[set])
							continue;
						bucketOf[set] = static_cast<std::uint32_t>(a_buckets.size());
						a_buckets.push_back({ set, a_first, a_sets[set] });
						a_first += a_halves * a_sets[set];
					}
					a_plan.map[a_count] = static_cast<std::uint32_t>(a_buckets.size());
					a_plan.map.insert(a_plan.map.end(), bucketOf.begin(), bucketOf.end());
					for (const auto& bucket : a_buckets) {
						a_plan.map.push_back(bucket.first);
						a_plan.map.push_back(bucket.capacity);
					}
				};
				// The forward buckets (CS_DCLF_FORWARD_PLAIN) over the forward range, then the main ones (CS_DCLF_MAIN_PLAIN) over the main range.
				plan(SwitchOnUnlessZero(Switch::ForwardPlain), a_forwardSets, a_layout.sequenceDraws + a_layout.terrainCapacity[0] + a_layout.terrainCapacity[1], 2,
					a_plan.forward, 1);
				plan(SwitchOnUnlessZero(Switch::MainPlain), a_mainSets, 0, 1, a_plan.main, 2);
			}
		}

		void PlanSetClasses(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, SetClassPlan& a_out)
		{
			a_out = {};
			a_out.classes.resize(a_indirect.classes.size());
			// Each slot's capacity into its class's sub-range of its range (a decal group's: whether the class has one).
			std::vector<std::array<bool, kDecalGroups>> decals(a_indirect.classes.size());
			// And each set index's forward and main slots' capacities (its buckets', CS_DCLF_FORWARD_PLAIN, CS_DCLF_MAIN_PLAIN).
			std::vector<std::uint32_t> forwardSets(a_indirect.classOf.size(), 0u), mainSets(a_indirect.classOf.size(), 0u);
			const auto slots = static_cast<std::uint32_t>(std::min(a_layout.slotCapacity.size(), a_tables.pipelines.size()));
			for (std::uint32_t p = 0; p < slots; ++p) {
				std::uint32_t c = 0, r = 0;
				if (!SlotClassRange(a_lookups, a_tables, a_indirect, p, c, r))
					continue;
				if (r >= ClassRangeOfSlot(0) && r < ClassRangeOfSlot(kDecalGroups))
					decals[c][r - ClassRangeOfSlot(0)] = true;
				else
					a_out.classes[c].ranges[r].capacity += a_layout.slotCapacity[p];
				if (r == kClassRangeForward)
					forwardSets[a_lookups.pipelines[p].setIndex] += a_layout.slotCapacity[p];
				else if (r == kClassRangeMain)
					mainSets[a_lookups.pipelines[p].setIndex] += a_layout.slotCapacity[p];
			}
			LayOutSetClasses(a_layout, a_indirect, decals, forwardSets, mainSets, a_out);
		}

		std::vector<LatchedCopy> MainLatchedLayout(const Resources& a_resources, bool a_depthOnly, const FrameBlockSizes& a_blocks, std::uint32_t a_buckets,
			std::uint32_t a_classWords)
		{
			std::vector<LatchedCopy> out;
			const auto targets = a_resources.latchedTargets.load(std::memory_order_acquire);
			std::size_t used = 0;
			// As LatchedUploads: a target the passes did not declare is staged, not latched.
			auto add = [&](const void* a_key, std::size_t a_bytes, std::uint64_t a_offset) {
				if (!a_key || !a_bytes || !targets || std::find(targets->begin(), targets->end(), a_key) == targets->end())
					return;
				const std::size_t at = (used + 15) & ~std::size_t(15);
				out.push_back({ a_key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(a_bytes), a_offset });
				used = at + a_bytes;
			};
			// In CommitMainPayload's order.
			const void* constants = a_resources.frameConstants.get();
			if (!a_depthOnly)
				for (const auto& frameBuffer : a_resources.frameBuffers)
					if (frameBuffer.copy)
						add(frameBuffer.copy.get(), std::size_t(frameBuffer.elements) * frameBuffer.stride, 0);
			add(constants, sizeof(DrawBindings), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				add(constants, a_blocks.vs[slot], FrameSlotOffset(false, slot));
				add(constants, a_blocks.ps[slot], FrameSlotOffset(true, slot));
			}
			// The frame-wide rows (LatchFrameRows: the lighting, LOD fades, fog, extras and the frame constant rows in ascending row order).
			auto addConstants = [&](std::size_t a_bytes, std::uint64_t a_offset) { add(constants, a_bytes, a_offset); };
			FrameRowWrites([](void* a_add, std::size_t a_bytes, std::uint64_t a_offset) { (*static_cast<decltype(addConstants)*>(a_add))(a_bytes, a_offset); }, &addConstants);
			if (!a_depthOnly && a_resources.foliage)
				add(constants, 8 * sizeof(std::uint32_t), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame));
			// The features' scene resources' latched buffers (Api::SceneResources::latched: Features/TreeLod's draw row and list header), the
			// depth commit's.
			if (a_depthOnly)
				Extensions::LatchSceneResources(a_resources.scene->sceneResources,
					[](void* a_add, const std::shared_ptr<org::Buffer>& a_buffer, const Versioned& a_versioned, std::size_t a_bytes) {
						(*static_cast<decltype(add)*>(a_add))(a_versioned ? a_versioned->Key() : a_buffer.get(), a_bytes, 0);
					},
					&add);
			add(a_resources.count.get(), a_depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t), 0);
			if (!a_depthOnly) {
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), sizeof(std::uint32_t), kCountDecalLayerWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), kTerrainGroups * sizeof(std::uint32_t), kCountTerrainGroupWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountForwardWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 4 * sizeof(std::uint32_t), kCountRefractionWord * sizeof(std::uint32_t));
				add(a_resources.count.get(), 2 * sizeof(std::uint32_t), kCountClassUnplannedWord * sizeof(std::uint32_t));
				// The state class sub-ranges' counts (the colour commit's class plan's).
				if (a_resources.classCounts)
					add(a_resources.classCounts->Key(), std::size_t(a_classWords) * sizeof(std::uint32_t), 0);
			}
			if (a_depthOnly && a_resources.pool && a_buckets)
				for (const auto& counts : a_resources.zBucketCounts)
					if (counts)
						add(counts->Key(), std::size_t(a_buckets) * sizeof(std::uint32_t), 0);
			return out;
		}

		std::size_t LatchedBytes(const std::vector<LatchedCopy>& a_copies)
		{
			return a_copies.empty() ? 0 : std::size_t(a_copies.back().latchOffset) + a_copies.back().bytes;
		}

		/**
		 * @brief A view's own frame values' latched copies, in CommitView's order, packed from a_at (its latch layout's FrameOffset): its
		 * copies of the main frame buffers (a_frameBuffers, whole; null: none), then into a_key, its frame constants, the record, CS's
		 * SharedData and FeatureData at the depth commit's sizes (a_depth: the same blocks, this frame's), the frame-wide rows. The faces'
		 * programs read no other frame slot.
		 */
		std::vector<LatchedCopy> ViewFrameLatched(const void* a_key, const std::vector<FrameBuffer>* a_frameBuffers, const FrameBlockSizes& a_depth, std::uint32_t a_at)
		{
			std::vector<LatchedCopy> out;
			std::size_t used = 0;
			auto addTo = [&](const void* a_target, std::size_t a_bytes, std::uint64_t a_offset) {
				if (!a_bytes)
					return;
				const std::size_t at = (used + 15) & ~std::size_t(15);
				out.push_back({ a_target, static_cast<std::uint32_t>(a_at + at), static_cast<std::uint32_t>(a_bytes), a_offset });
				used = at + a_bytes;
			};
			auto add = [&](std::size_t a_bytes, std::uint64_t a_offset) { addTo(a_key, a_bytes, a_offset); };
			for (const auto& frameBuffer : a_frameBuffers ? std::span<const FrameBuffer>(*a_frameBuffers) : std::span<const FrameBuffer>())
				if (frameBuffer.copy)
					addTo(frameBuffer.copy.get(), std::size_t(frameBuffer.elements) * frameBuffer.stride, 0);
			add(sizeof(DrawBindings), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			add(a_depth.ps[kSharedDataRegister], FrameSlotOffset(true, kSharedDataRegister));
			add(a_depth.ps[kFeatureDataRegister], FrameSlotOffset(true, kFeatureDataRegister));
			FrameRowWrites([](void* a_add, std::size_t a_bytes, std::uint64_t a_offset) { (*static_cast<decltype(add)*>(a_add))(a_bytes, a_offset); }, &add);
			return out;
		}

		/** @brief Producer: a latched-copies block holding a_copies (none: nothing asked). */
		void RequireLatchedBlock(GrowthBuild& a_build, LatchHandle a_latch, const std::vector<LatchedCopy>& a_copies)
		{
			if (const std::size_t bytes = LatchedBytes(a_copies))
				SceneBufferSet::Require(a_build, a_latch, static_cast<std::uint32_t>(bytes), 0);
		}

		MainShapeInputs MainShapeInputsOf(const Resources&, const MainLayout& a_layout, const NamedVersions&, bool a_depthOnly, std::uint32_t a_cullMode)
		{
			MainShapeInputs in;
			in.depthOnly = a_depthOnly;
			in.sequenceDraws = a_layout.sequenceDraws;
			in.sequenceDecals = a_layout.sequenceDecals;
			in.terrainCapacity = a_layout.terrainCapacity;
			in.forwardCapacity = a_layout.forwardCapacity;
			in.refractionCapacity = a_layout.refractionCapacity;
			in.cullMode = a_cullMode;
			return in;
		}

		std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in)
		{
			auto frame = std::make_shared<PassFrame>();
			frame->sequenceDraws = a_in.sequenceDraws;
			frame->sequenceDecals = a_in.sequenceDecals;
			// The colour calls' sub-ranges and max counts: the state classes' (PlanSetClasses), from the slots' capacities, which hold
			// every draw the scene can produce (RequireMain). Not the counts of the tables the shape was made from: a publication a
			// revision covers may hold newer tables (a decal group's first decal, more draws), and its commit would find the shape short.
			// They change with the layout and the slots' classes alone, whose buffers the revision's set holds. The depth segment has
			// none: its draws are bucketed plain draws (zCalls), and it is given no decals (BuildDrawsCS).
			// The terrain groups' and forward ranges, the colour segment's alone (BuildDrawsCS appends them there).
			if (!a_in.depthOnly) {
				frame->classPlan = a_in.classPlan;
				if (a_in.classPlan)
					frame->classes = a_in.classPlan->classes;
				frame->terrainCapacity = a_in.terrainCapacity;
				frame->forwardCapacity = a_in.forwardCapacity;  // the colour segment's and the forward epoch's
				frame->refractionCapacity = a_in.refractionCapacity;  // likewise
			}
			frame->width = a_in.viewport.width;
			frame->height = a_in.viewport.height;
			frame->minDepth = a_in.viewport.minDepth;
			frame->maxDepth = a_in.viewport.maxDepth;
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->cullMode = a_in.cullMode;
			frame->latch = a_in.latch;
			frame->latchLayout = a_in.latchLayout;
			frame->zCalls = a_in.zCalls;
			frame->zPlan = a_in.zPlan;
			frame->latched = a_in.latched;
			return frame;
		}

	}

	namespace
	{
		void MixWitness(std::uint64_t& a_hash, std::uint64_t a_value)
		{
			a_hash = (a_hash ^ a_value) * 0x100000001b3ull;
			a_hash ^= a_hash >> 29;
		}

		/**
		 * @brief What the requirements and the shapes read of a_lookups' pipeline slots (RevisionMake::lookupsWitness): each entry's set
		 * index (PlanZBuckets, the views' Z groups), whether it holds the tables' key, the tables' raster flags there (the buckets' family
		 * and terrain rules), and the shadow key slots' count (append-only per instance: the shadow latch's keys, ShadowBoundsOf's slots).
		 * The lookups' versions and generations are none of it: a material lookup's write moves those alone.
		 */
		std::uint64_t LookupsWitness(const Lookups& a_lookups, const SceneTables& a_tables)
		{
			std::uint64_t hash = 0xcbf29ce484222325ull;
			const std::size_t slots = a_lookups.pipelines.size(), tabled = a_tables.pipelines.size();
			MixWitness(hash, a_lookups.instance);
			MixWitness(hash, slots);
			MixWitness(hash, tabled);
			for (std::size_t p = 0; p < slots; ++p) {
				const auto& entry = a_lookups.pipelines[p];
				const bool keyed = p < tabled && entry.key == a_tables.pipelines[p];
				MixWitness(hash, (std::uint64_t(entry.setIndex) << 1) | (keyed ? 1u : 0u));
				if (p < tabled)
					MixWitness(hash, a_tables.pipelines[p].rasterFlags);
			}
			MixWitness(hash, a_lookups.shadowSlotKeys.size());
			return hash;
		}

		/**
		 * @brief The map rows of every (mode, rasterizer state) the shadow layouts draw (a_placements, the occlusion maps' a_recent):
		 * what the shadow shapes' row buckets are made of (ShadowRowBuckets), by content. Each pair once while few are seen.
		 */
		template <class Recent>
		std::uint64_t ShadowRowsWitness(const Lookups& a_lookups, const std::vector<ShadowViewLayout>& a_placements, const Recent& a_recent)
		{
			std::uint64_t hash = 0xcbf29ce484222325ull;
			std::array<std::uint64_t, 16> seen{};
			std::size_t pairs = 0;
			const auto rows = [&](const std::vector<ShadowViewLayout>& a_layouts) {
				for (const auto& view : a_layouts) {
					const std::uint64_t pair = (std::uint64_t(view.modeIndex) << 32) | view.rasterState;
					if (std::find(seen.begin(), seen.begin() + pairs, pair) != seen.begin() + pairs)
						continue;
					if (pairs < seen.size())
						seen[pairs++] = pair;
					const auto row = a_lookups.ShadowMapRow(view.modeIndex, view.rasterState);
					MixWitness(hash, pair);
					MixWitness(hash, row.size());
					for (const std::uint32_t pipeline : row)
						MixWitness(hash, pipeline);
				}
			};
			rows(a_placements);
			for (const auto& layouts : a_recent)
				rows(layouts);
			return hash;
		}

		/**
		 * @brief What the requirement pass reads besides the revision inputs and the publication: the visibility events the frames
		 * asked for, the index pool's fragmentation, the latched targets the passes declared (main, shadow) and the views' frame buffers'
		 * copies, by content.
		 */
		std::uint64_t VolatileWitness(const RevisionInputs& a_inputs)
		{
			std::uint64_t hash = 0xcbf29ce484222325ull;
			const auto targets = [&hash](const auto& a_latched) {
				const auto keys = a_latched.load(std::memory_order_acquire);
				MixWitness(hash, keys ? keys->size() + 1 : 0);
				if (keys)
					for (const void* key : *keys)
						MixWitness(hash, reinterpret_cast<std::uintptr_t>(key));
			};
			if (const auto& s = a_inputs.scene) {
				MixWitness(hash, s->visibilityEvents && s->visibilityReadback ? s->visibilityReadback->wanted.load(std::memory_order_acquire) : 0u);
				MixWitness(hash, s->pool ? s->pool->fragmentedAt.load(std::memory_order_relaxed) : 0u);
			}
			if (a_inputs.main)
				targets(a_inputs.main->latchedTargets);
			if (a_inputs.shadow)
				targets(a_inputs.shadow->latchedTargets);
			for (const auto& view : a_inputs.views) {
				const auto frameBuffers = view.resources ? view.resources->frameBuffers.load(std::memory_order_acquire) : nullptr;
				MixWitness(hash, frameBuffers ? frameBuffers->size() + 1 : 0);
				if (!frameBuffers)
					continue;
				for (const auto& frameBuffer : *frameBuffers) {
					MixWitness(hash, reinterpret_cast<std::uintptr_t>(frameBuffer.copy.get()));
					MixWitness(hash, (std::uint64_t(frameBuffer.elements) << 32) | frameBuffer.stride);
				}
			}
			return hash;
		}

		/**
		 * @brief What the requirement pass reads of a_tables (RevisionReuse::Key::tables), by value: the object slots' count; the draw bound
		 * a_bound brought up to date from them (UpdateDrawBound: its draws, by pipeline slot, and its casting shares' version - RequireMain's
		 * sequences, buckets and ranges, the shadow slots' sequences, the casting bound's shapes); the pipeline slots' keys and used set (the
		 * views' forward pipelines and slots, the Z buckets' and the main layout's raster rules; the lookups' entries are LookupsWitness'); the
		 * main ranges' counts (the decal sequences); the geometry slots and their log's end (the index pool's bound, RequireIndexPool, follows
		 * that log); and what the features' scene resources require from (Api::SceneResources::require: the table families' rows or stamps,
		 * TableFamilies::RequirementsWitness - Core's visibility rows by the root states' among them - and the vertex stream's rows' ranges).
		 */
		std::uint64_t TablesWitness(const SceneTables& a_tables, const Draws::DrawBoundStore& a_bound)
		{
			std::uint64_t hash = 0xcbf29ce484222325ull;
			MixWitness(hash, a_tables.objects.size());
			MixWitness(hash, a_bound.draws);
			MixWitness(hash, a_bound.shadowVersion);
			MixWitness(hash, a_bound.perPipeline.size());
			for (const std::uint64_t draws : a_bound.perPipeline)
				MixWitness(hash, draws);
			MixWitness(hash, a_tables.pipelines.size());
			for (const auto& key : a_tables.pipelines) {
				MixWitness(hash, (std::uint64_t(key.vertexDescriptor) << 32) | key.pixelDescriptor);
				MixWitness(hash, (std::uint64_t(key.rasterFlags) << 32) | key.passDescriptor);
				MixWitness(hash, key.vertexLayout);
			}
			MixWitness(hash, a_tables.usedPipelineBits.size());
			for (const std::uint64_t word : a_tables.usedPipelineBits)
				MixWitness(hash, word);
			for (const std::uint32_t count : a_tables.rangeCount)
				MixWitness(hash, count);
			MixWitness(hash, a_tables.geometries.size());
			MixWitness(hash, a_tables.geometryLog.End());
			MixWitness(hash, a_tables.families.RequirementsWitness());
			MixWitness(hash, a_tables.streamRows.size());
			for (const auto& row : a_tables.streamRows)
				MixWitness(hash, row.object != Api::VertexStreamRow::kNoObject ? (std::uint64_t(row.first) << 32) | row.count : ~std::uint64_t{ 0 });
			return hash;
		}

		/** @brief The parts of a_next that moved from a_last (a bit by RevisionReuse::Moved); none: the same requirements. */
		std::uint32_t MovedRequirements(const ImplSnapshotBuilder::RevisionReuse::Key& a_last, const ImplSnapshotBuilder::RevisionReuse::Key& a_next)
		{
			using Reuse = ImplSnapshotBuilder::RevisionReuse;
			const auto same = [](const auto& a_left, const auto& a_right) { return !a_left.owner_before(a_right) && !a_right.owner_before(a_left); };
			std::uint32_t moved = 0;
			const auto note = [&moved](bool a_moved, Reuse::Moved a_part) { moved |= a_moved ? 1u << a_part : 0u; };
			note(a_last.tables != a_next.tables, Reuse::kMovedTables);
			note(!same(a_last.catalog, a_next.catalog), Reuse::kMovedCatalog);
			note(a_last.tablesGeneration != a_next.tablesGeneration, Reuse::kMovedGeneration);
			note(a_last.request.pipelines != a_next.request.pipelines || a_last.request.shadowSlotKeys != a_next.request.shadowSlotKeys ||
					 a_last.request.shadowKeys != a_next.request.shadowKeys,
				Reuse::kMovedRequest);
			note(a_last.inputs != a_next.inputs || a_last.toggles != a_next.toggles, Reuse::kMovedInputs);
			note(a_last.lookups != a_next.lookups, Reuse::kMovedLookups);
			note(a_last.shadowRows != a_next.shadowRows, Reuse::kMovedShadowRows);
			note(a_last.volatiles != a_next.volatiles, Reuse::kMovedVolatiles);
			return moved;
		}

		/**
		 * @brief MakeRevision's reuse key for a_publication into a_key (a_producer's draw bound brought up to date from its tables first:
		 * the requirement pass reads it so), and the parts of it that moved from the last sealed revision's (~0u: none stands to keep).
		 */
		std::uint32_t RequirementsMoved(ImplProducer& a_producer, const ImplSnapshotBuilder::RevisionReuse& a_reuse, const ScenePublication& a_publication,
			const Lookups& a_lookups, ImplSnapshotBuilder::RevisionReuse::Key& a_key)
		{
			ZoneScopedN("CS.DCLF.MakeRevisionShapes.Key");
			const auto& tables = *a_publication.tables;
			const auto& producer = a_producer.producer;
			const auto& inputs = producer.inputs;
			a_producer.UpdateDrawBound(tables, a_publication.tablesGeneration);
			a_key = { TablesWitness(tables, producer.drawBound), a_publication.catalog, a_publication.tablesGeneration, a_publication.revision, inputs.generation,
				a_publication.toggles, LookupsWitness(a_lookups, tables), ShadowRowsWitness(a_lookups, inputs.shadowPlacements, producer.recentOcclusionLayouts),
				VolatileWitness(inputs) };
			return a_reuse.valid ? MovedRequirements(a_reuse.key, a_key) : ~0u;
		}

		/** @brief CS_DCLF_PERSISTENT_PARITY: whether every shape of a_made is a_kept's (the same object or the same shape). */
		bool SameMadeShapes(const MadeShapes& a_kept, const MadeShapes& a_made)
		{
			const auto same = [](const auto& a_old, const auto& a_new) { return a_old == a_new || (a_old && a_new && a_old->SameShape(*a_new)); };
			for (std::size_t s = 0; s < a_kept.main.size(); ++s)
				if (!same(a_kept.main[s], a_made.main[s]))
					return false;
			for (std::size_t v = 0; v < a_kept.views.size(); ++v)
				if (!same(a_kept.views[v], a_made.views[v]))
					return false;
			for (std::size_t kind = 0; kind < a_kept.shadow.size(); ++kind) {
				if (a_kept.shadow[kind].size() != a_made.shadow[kind].size())
					return false;
				for (std::size_t i = 0; i < a_kept.shadow[kind].size(); ++i)
					if (!same(a_kept.shadow[kind][i], a_made.shadow[kind][i]))
						return false;
			}
			return true;
		}

		// CS_DCLF_PERSISTENT_PARITY's report: the first field a kept shape and the one made again differ in, field by field as their
		// SameShape compares (empty: none). Only on a difference.
		template <class H>
		std::string HandleText(const H& a_handle)
		{
			return fmt::format("{}:{}", a_handle.index, a_handle.generation);
		}

		template <class H>
		std::string HandleDifference(const char* a_what, const H& a_old, const H& a_new)
		{
			return SameHandle(a_old, a_new) ? std::string() : fmt::format("{} {} -> {}", a_what, HandleText(a_old), HandleText(a_new));
		}

		template <class T>
		std::string ValueDifference(const char* a_what, const T& a_old, const T& a_new)
		{
			return a_old == a_new ? std::string() : fmt::format("{} {} -> {}", a_what, a_old, a_new);
		}

		std::string LatchedDifference(const char* a_what, const LatchedList& a_old, const LatchedList& a_new)
		{
			if (a_old.latch != a_new.latch)
				return fmt::format("{}: latch block {} -> {}", a_what, fmt::ptr(a_old.latch.get()), fmt::ptr(a_new.latch.get()));
			if (a_old.fixed != a_new.fixed)
				return fmt::format("{}: fixed {} -> {}", a_what, a_old.fixed, a_new.fixed);
			if (a_old.copies.size() != a_new.copies.size())
				return fmt::format("{}: copies {} -> {}", a_what, a_old.copies.size(), a_new.copies.size());
			for (std::size_t c = 0; c < a_old.copies.size(); ++c)
				if (const auto &o = a_old.copies[c], &n = a_new.copies[c]; !(o == n))
					return fmt::format("{}: copy {} (target {} at {} bytes {} to {}) -> (target {} at {} bytes {} to {})", a_what, c, fmt::ptr(o.target), o.latchOffset,
						o.bytes, o.dstOffset, fmt::ptr(n.target), n.latchOffset, n.bytes, n.dstOffset);
			return {};
		}

		std::string IndirectDifference(const IndirectState& a_old, const IndirectState& a_new)
		{
			if (a_old.valid != a_new.valid)
				return fmt::format("indirect valid {} -> {}", a_old.valid, a_new.valid);
			if (a_old.classes.size() != a_new.classes.size())
				return fmt::format("indirect state classes {} -> {}", a_old.classes.size(), a_new.classes.size());
			for (std::size_t c = 0; c < a_old.classes.size(); ++c) {
				const auto &o = a_old.classes[c], &n = a_new.classes[c];
				if (auto d = ValueDifference("indirect class pipelines", o.pipelines, n.pipelines); !d.empty())
					return fmt::format("{} (class {})", d, c);
				for (std::size_t i = 0; i < o.sets.size(); ++i) {
					if (auto d = HandleDifference("indirect set", o.sets[i], n.sets[i]); !d.empty())
						return fmt::format("{} (class {} variant {})", d, c, i);
					if (auto d = HandleDifference("indirect signature", o.signatures[i], n.signatures[i]); !d.empty())
						return fmt::format("{} (class {} variant {})", d, c, i);
				}
			}
			for (auto d : { HandleDifference("indirect layout", a_old.layout, a_new.layout), HandleDifference("indirect zLayout", a_old.zLayout, a_new.zLayout),
					 HandleDifference("indirect zDrawSignature", a_old.zDrawSignature, a_new.zDrawSignature),
					 HandleDifference("indirect refraction pipeline", a_old.refraction, a_new.refraction),
					 ValueDifference("indirect refraction format", static_cast<int>(a_old.refractionFormat), static_cast<int>(a_new.refractionFormat)) })
				if (!d.empty())
					return d;
			return {};
		}

		std::string ShapeDifference(const PassFrame& a_old, const PassFrame& a_new)
		{
			if (!(a_old.zCalls == a_new.zCalls)) {
				if (a_old.zCalls.size() != a_new.zCalls.size())
					return fmt::format("zCalls {} -> {}", a_old.zCalls.size(), a_new.zCalls.size());
				for (std::size_t c = 0; c < a_old.zCalls.size(); ++c)
					if (const auto &o = a_old.zCalls[c], &n = a_new.zCalls[c]; !(o == n))
						return fmt::format("zCall {} (bucket {} first {} capacity {} pipeline {} near {}) -> (bucket {} first {} capacity {} pipeline {} near {})", c, o.bucket,
							o.first, o.capacity, HandleText(o.pipeline), o.terrainNear, n.bucket, n.first, n.capacity, HandleText(n.pipeline), n.terrainNear);
			}
			if (auto d = LatchedDifference("latched", a_old.latched, a_new.latched); !d.empty())
				return d;
			if (a_old.classes.size() != a_new.classes.size())
				return fmt::format("state classes {} -> {}", a_old.classes.size(), a_new.classes.size());
			for (std::size_t c = 0; c < a_old.classes.size(); ++c) {
				const auto &o = a_old.classes[c], &n = a_new.classes[c];
				if (o.keyBase != n.keyBase || o.size != n.size)
					return fmt::format("class {} keys (first {} size {}) -> (first {} size {})", c, o.keyBase, o.size, n.keyBase, n.size);
				for (std::size_t r = 0; r < o.ranges.size(); ++r)
					if (!(o.ranges[r] == n.ranges[r]))
						return fmt::format("class {} range {} (first {} capacity {}) -> (first {} capacity {})", c, r, o.ranges[r].first, o.ranges[r].capacity, n.ranges[r].first,
							n.ranges[r].capacity);
			}
			for (std::size_t g = 0; g < a_old.terrainCapacity.size(); ++g)
				if (auto d = ValueDifference("terrainCapacity", a_old.terrainCapacity[g], a_new.terrainCapacity[g]); !d.empty())
					return fmt::format("{} (group {})", d, g);
			for (auto d : { ValueDifference("forwardCapacity", a_old.forwardCapacity, a_new.forwardCapacity),
					 ValueDifference("refractionCapacity", a_old.refractionCapacity, a_new.refractionCapacity),
					 ValueDifference("sequenceDraws", a_old.sequenceDraws, a_new.sequenceDraws), ValueDifference("sequenceDecals", a_old.sequenceDecals, a_new.sequenceDecals),
					 ValueDifference("cullMode", a_old.cullMode, a_new.cullMode), ValueDifference("width", a_old.width, a_new.width),
					 ValueDifference("height", a_old.height, a_new.height), ValueDifference("minDepth", a_old.minDepth, a_new.minDepth),
					 ValueDifference("maxDepth", a_old.maxDepth, a_new.maxDepth), HandleDifference("resourceHeap", a_old.resourceHeap, a_new.resourceHeap),
					 HandleDifference("samplerHeap", a_old.samplerHeap, a_new.samplerHeap), IndirectDifference(a_old.indirect, a_new.indirect) })
				if (!d.empty())
					return d;
			if (a_old.latch != a_new.latch)
				return fmt::format("latch block {} -> {}", fmt::ptr(a_old.latch.get()), fmt::ptr(a_new.latch.get()));
			return {};
		}

		std::string ShapeDifference(const ViewFrame& a_old, const ViewFrame& a_new)
		{
			if (a_old.latch != a_new.latch)
				return fmt::format("latch block {} -> {}", fmt::ptr(a_old.latch.get()), fmt::ptr(a_new.latch.get()));
			for (std::size_t l = 0; l < a_old.latched.size(); ++l)
				if (auto d = LatchedDifference(l == 0 ? "latched (from latch)" : l == 1 ? "latched (zeros)" : "latched (frame)", a_old.latched[l], a_new.latched[l]); !d.empty())
					return d;
			for (auto d : { HandleDifference("resourceHeap", a_old.resourceHeap, a_new.resourceHeap), HandleDifference("samplerHeap", a_old.samplerHeap, a_new.samplerHeap),
					 IndirectDifference(a_old.indirect, a_new.indirect), ValueDifference("width", a_old.width, a_new.width), ValueDifference("height", a_old.height, a_new.height),
					 ValueDifference("depthRange", a_old.depthRange, a_new.depthRange), ValueDifference("sequenceDraws", a_old.sequenceDraws, a_new.sequenceDraws) })
				if (!d.empty())
					return d;
			if (a_old.buckets.size() != a_new.buckets.size())
				return fmt::format("buckets {} -> {}", a_old.buckets.size(), a_new.buckets.size());
			for (std::size_t b = 0; b < a_old.buckets.size(); ++b)
				if (const auto &o = a_old.buckets[b], &n = a_new.buckets[b]; !(o == n))
					return fmt::format("bucket {} (depth {} colour {} first {} capacity {}) -> (depth {} colour {} first {} capacity {})", b, HandleText(o.depth), HandleText(o.colour),
						o.first, o.capacity, HandleText(n.depth), HandleText(n.colour), n.first, n.capacity);
			if (a_old.push.size() != a_new.push.size())
				return fmt::format("push faces {} -> {}", a_old.push.size(), a_new.push.size());
			for (std::size_t f = 0; f < a_old.push.size(); ++f)
				for (std::size_t w = 0; w < a_old.push[f].size(); ++w)
					if (a_old.push[f][w] != a_new.push[f][w])
						return fmt::format("push face {} word {} {:#x} -> {:#x}", f, w, a_old.push[f][w], a_new.push[f][w]);
			if (auto d = ValueDifference("forwardEntry", a_old.forwardEntry, a_new.forwardEntry); !d.empty())
				return d;
			if (a_old.forwardRanges.size() != a_new.forwardRanges.size())
				return fmt::format("forward classes {} -> {}", a_old.forwardRanges.size(), a_new.forwardRanges.size());
			for (std::size_t c = 0; c < a_old.forwardRanges.size(); ++c)
				if (const auto &o = a_old.forwardRanges[c], &n = a_new.forwardRanges[c]; !(o == n))
					return fmt::format("forward class {} (first {} capacity {}) -> (first {} capacity {})", c, o.first, o.capacity, n.first, n.capacity);
			for (std::size_t d = 0; d < a_old.faces.size(); ++d)
				if (const auto &o = a_old.faces[d], &n = a_new.faces[d]; !(o == n))
					return fmt::format("faces of family {} (pipeline {} signature {} groups {}) -> (pipeline {} signature {} groups {})", d, HandleText(o.pipeline),
						HandleText(o.signature), o.groups, HandleText(n.pipeline), HandleText(n.signature), n.groups);
			return {};
		}

		std::string ShapeDifference(const ShadowFrame& a_old, const ShadowFrame& a_new)
		{
			if (a_old.latch != a_new.latch)
				return fmt::format("latch block {} -> {}", fmt::ptr(a_old.latch.get()), fmt::ptr(a_new.latch.get()));
			for (std::size_t l = 0; l < a_old.latched.size(); ++l)
				if (auto d = LatchedDifference(l == 0 ? "latched (from latch)" : l == 1 ? "latched (zeros)" : "latched (commit)", a_old.latched[l], a_new.latched[l]); !d.empty())
					return d;
			for (auto d : { HandleDifference("resourceHeap", a_old.resourceHeap, a_new.resourceHeap), HandleDifference("samplerHeap", a_old.samplerHeap, a_new.samplerHeap),
					 ValueDifference("indirect valid", a_old.indirect.valid, a_new.indirect.valid), HandleDifference("indirect layout", a_old.indirect.layout, a_new.indirect.layout) })
				if (!d.empty())
					return d;
			if (a_old.indirect.version != a_new.indirect.version)
				return fmt::format("indirect version {} -> {}", fmt::ptr(a_old.indirect.version.get()), fmt::ptr(a_new.indirect.version.get()));
			if (a_old.views.size() != a_new.views.size())
				return fmt::format("views {} -> {}", a_old.views.size(), a_new.views.size());
			for (std::size_t v = 0; v < a_old.views.size(); ++v) {
				const auto &o = a_old.views[v], &n = a_new.views[v];
				if (o == n)
					continue;
				const auto inView = [&](const std::string& a_field) { return fmt::format("view {} (slot {} mode {}): {}", v, o.slot, o.modeIndex, a_field); };
				for (auto d : { ValueDifference("slot", o.slot, n.slot), ValueDifference("modeIndex", o.modeIndex, n.modeIndex),
						 ValueDifference("capacity", o.capacity, n.capacity), ValueDifference("x", o.x, n.x), ValueDifference("y", o.y, n.y),
						 ValueDifference("width", o.width, n.width), ValueDifference("height", o.height, n.height), ValueDifference("minDepth", o.minDepth, n.minDepth),
						 ValueDifference("maxDepth", o.maxDepth, n.maxDepth), ValueDifference("target", o.target, n.target), ValueDifference("slice", o.slice, n.slice),
						 ValueDifference("sequenceDraws", o.sequenceDraws, n.sequenceDraws), ValueDifference("rasterState", o.rasterState, n.rasterState) })
					if (!d.empty())
						return inView(d);
				for (std::size_t w = 0; w < o.push.size(); ++w)
					if (o.push[w] != n.push[w])
						return inView(fmt::format("push word {} {:#x} -> {:#x}", w, o.push[w], n.push[w]));
				if (o.buckets.size() != n.buckets.size())
					return inView(fmt::format("buckets {} -> {}", o.buckets.size(), n.buckets.size()));
				for (std::size_t b = 0; b < o.buckets.size(); ++b)
					if (const auto &ob = o.buckets[b], &nb = n.buckets[b]; !(ob == nb))
						return inView(fmt::format("bucket {} (pipeline {} first {} capacity {}) -> (pipeline {} first {} capacity {})", b, ob.pipeline, ob.first, ob.capacity,
							nb.pipeline, nb.first, nb.capacity));
			}
			return {};
		}

		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY's report: which participant's shape made again differs from a_kept's (the main segment, the
		 * shadow or occlusion epoch's layout, the view) and the first field it differs in. Called only when SameMadeShapes found one.
		 */
		std::string MadeShapesDifference(const MadeShapes& a_kept, const MadeShapes& a_made)
		{
			const auto shape = [](const std::string& a_what, const auto& a_old, const auto& a_new) -> std::string {
				if (a_old == a_new)
					return {};
				if (!a_old || !a_new)
					return fmt::format("{}: {} -> {}", a_what, a_old ? "a shape" : "none", a_new ? "a shape" : "none");
				auto field = ShapeDifference(*a_old, *a_new);
				return field.empty() ? std::string() : fmt::format("{}: {}", a_what, field);
			};
			std::string out;
			for (std::size_t s = 0; s < a_kept.main.size() && out.empty(); ++s)
				out = shape(s == kDepthShape ? "main depth" : "main colour", a_kept.main[s], a_made.main[s]);
			for (std::size_t kind = 0; kind < a_kept.shadow.size() && out.empty(); ++kind) {
				const char* epoch = kind == 0 ? "shadow" : "occlusion";
				if (a_kept.shadow[kind].size() != a_made.shadow[kind].size()) {
					out = fmt::format("{}: layouts {} -> {}", epoch, a_kept.shadow[kind].size(), a_made.shadow[kind].size());
					break;
				}
				for (std::size_t i = 0; i < a_kept.shadow[kind].size() && out.empty(); ++i)
					out = shape(fmt::format("{} layout {}", epoch, i), a_kept.shadow[kind][i], a_made.shadow[kind][i]);
			}
			for (std::size_t v = 0; v < a_kept.views.size() && out.empty(); ++v)
				out = shape(ImplProducer::kKeyGroupNames[kViewSlots[v].keyGroup], a_kept.views[v], a_made.views[v]);
			return "a shape differs from the kept one: " + (out.empty() ? std::string("(no field found)") : out);
		}
	}

	void IndirectDraws::Impl::EnsureRevisionLatches()
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto& latches = producer.latches;
		auto& handles = producer.latchHandles;
		const auto& inputs = producer.inputs;
		// The blocks' handles in the scene's set, once: the seals make the blocks (off the host's world, on the producer's thread).
		if (!handles) {
			auto& buffers = SceneBufferSet::Get();
			const std::uint32_t slots = host->FrameSlots();
			// The latched copies' blocks grow with headroom past what a layout lists (a commit's frame blocks change size).
			const auto latched = org::services::GrowthPolicy::Headroom(100, 4096);
			handles.main = buffers.RegisterLatch("cs.dclf.latch", slots);
			for (auto& block : handles.mainLatched)
				block = buffers.RegisterLatch("cs.dclf.latched-copies", slots, latched);
			handles.shadow = buffers.RegisterLatch("cs.dclf.shadow.latch", slots);
			handles.shadowZeros = buffers.RegisterLatch("cs.dclf.shadow.zeros", 1);
			handles.shadowLatched = buffers.RegisterLatch("cs.dclf.latched-copies", slots, latched);
			// A latch for each view a feature took (one none took draws nothing).
			for (const auto& slot : kViewSlots) {
				if (!Extensions::ViewRegistered(slot.view))
					continue;
				const std::size_t v = ViewIndex(slot.view);
				handles.views[v] = buffers.RegisterLatch(fmt::format("{}.latch", TraitsOf(slot.view).prefix), slots);
				handles.viewZeros[v] = buffers.RegisterLatch(fmt::format("{}.zeros", TraitsOf(slot.view).prefix), 1);
			}
		}
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		// The main latch at the first sizes Setup's resources had (it is no resource of theirs: one block for every main resources made).
		if (!latches.mainLaidOut && inputs.main) {
			latches.main = {};
			latches.main.cascades = smallTables ? 1u : kInitialSunCascades;
			latches.main.shadowVolumes = smallTables ? 1u : kInitialShadowVolumes;
			latches.mainLaidOut = true;
		}
		// The shadow latch at the layout the shadow resources' first slots were made for (SetupShadow).
		if (!latches.shadowLaidOut && inputs.shadow) {
			latches.shadow = InitialShadowLatchLayout();
			latches.shadowLaidOut = true;
		}
		for (const auto& slot : kViewSlots)
			if (const std::size_t v = ViewIndex(slot.view); !latches.viewsLaidOut[v] && inputs.views[v].resources) {
				latches.views[v] = ViewLatchLayout::Initial(TraitsOf(slot.view));
				latches.viewsLaidOut[v] = true;
			}
	}

	void IndirectDraws::Impl::MakeRevision(const ScenePublication& a_publication, std::uint32_t a_frame)
	{
		const auto& request = a_publication.revision;
		ZoneScopedN("CS.DCLF.MakeRevisionShapes");
		// The frame's inputs as last posted (RevisionInputs): with the request, all the revision reads.
		auto& made = producer.made;
		auto& reuse = revisionReuse;
		reuse.reusedNow = false;
		revisionInputsSlot.TakeInto(producer.inputs);
		// The toggles are the publication's, never the inputs': its revision is drawn by the frames that install it, under them.
		producer.toggles = a_publication.toggles;
		producer.frame = a_frame;
		auto& buffers = SceneBufferSet::Get();
		// The uploader a growth's fills go through, as posted.
		buffers.SetUploads(producer.inputs.uploads);
		const auto& inputs = producer.inputs;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!inputs.claims || !inputs.main || !inputs.main->scene || !host || !a_publication.tables) {
			reuse.valid = false;
			return;
		}
		const auto& tables = *a_publication.tables;
		// The revision is its publication's alone - its tables, its lookups and the catalog they were resolved from (named explicitly:
		// GetIndirectState, GetShadowIndirectState of it, for the generations its inputs hold) - never the frame's installed lookups and
		// catalog: shapes made from those would follow whichever publication the frame has installed, pairing a late revision's tables
		// with older lookups and changing its bucket plans and rows again.
		static const Lookups kNoLookups;
		const Lookups& lookups = a_publication.lookups ? *a_publication.lookups : kNoLookups;
		const PipelineCatalog* catalog = a_publication.catalog.get();
		EnsureRevisionLatches();
		// The requirement pass reads the publication's tables, catalog and request, the lookups' pipeline slots, the revision inputs and
		// a few values the frames move (RevisionReuse::Key). All of them as the last make that sealed a revision read them: this make
		// would seal the same set and assemble the same revision, so that one is kept (its recordings stand; SnapshotPass takes it as
		// sealed). CS_DCLF_PERSISTENT_PARITY makes it anyway and compares. The tables by what the pass reads of them (TablesWitness), the
		// draw bound among it brought up to date from them first (the pass reads it as updated; a kept make's update is the next one's).
		ImplSnapshotBuilder::RevisionReuse::Key key;
		const std::uint32_t moved = RequirementsMoved(*this, reuse, a_publication, lookups, key);
		const bool unchanged = moved == 0;
		const bool parity = Diagnostics::Observe().scene;
		if (unchanged && !parity) {
			++reuse.kept;
			reuse.reusedNow = true;
			return;
		}
		reuse.NoteRan(moved);
		reuse.valid = false;
		const std::uint64_t sequenceBefore = revisions.assembler.LatestSequence();
		// The revision assembled from this make's set and shapes; once sealed, what it was made from is the key a later make compares.
		const auto assemble = [&](bool a_shapesKept) {
			ZoneScopedN("CS.DCLF.MakeRevisionShapes.Assemble");
			AssembleRevision(a_frame);
			if (revisions.assembler.LatestSequence() == sequenceBefore) {
				if (parity && unchanged)
					reuse.parity.Check(false, [] { return std::string("nothing sealed for an unchanged key"); });
				return;
			}
			const std::uint64_t setSerial = revisions.sealed.named->Serial();
			if (parity && unchanged)
				reuse.parity.Check(a_shapesKept && setSerial == reuse.setSerial,
					[&] { return fmt::format("set serial {} -> {}, shapes {}", reuse.setSerial, setSerial, a_shapesKept ? "kept" : "made again"); });
			reuse.key = std::move(key);
			reuse.setSerial = setSerial;
			reuse.valid = true;
		};
		// What the set must hold for the tables as the publication holds them (Begin, the requirements, Seal): the shapes are made from
		// a set that holds every one. Each participant's (Revision/Participant.h: the main segments', the shadow engine's, the views'),
		// then their latched blocks, then every latch at its layout as the reserves left it.
		RevisionMake make{ *this, a_publication, tables, lookups, catalog };
		make.lookupsWitness = key.lookups;
		auto build = buffers.Begin();
		{
			ZoneScopedN("CS.DCLF.MakeRevisionShapes.Require");
			for (auto* participant : participants)
				participant->RequireSequences(make, build);
			for (auto* participant : participants)
				participant->RequireLatched(make, build);
			RequireLatches(build);
		}
		// The set: sealed once it holds every requirement; else a growth batch is in flight and the builder makes the revision again when
		// it settles (it wakes the builder). The same set (its serial, its fragment) while nothing changed.
		auto seal = [&] {
			ZoneScopedN("CS.DCLF.MakeRevisionShapes.Seal");
			return buffers.Seal(std::move(build));
		}();
		if (!seal) {
			++revisions.growthWaits;
			if (parity && unchanged)
				reuse.parity.Check(false, [] { return std::string("a growth for an unchanged key"); });
			return;
		}
		const auto& sealed = *seal.named;
		// The event lists' staging sets for the capacities the set names, before any shape of it.
		if (const auto& s = inputs.scene) {
			// The features' scene resources' (Api::SceneResources::sealed: Features/Fades' write-back).
			Extensions::SealSceneResources(sealed, s->sceneResources);
			if (s->visibilityEvents && s->visibilityReadback)
				s->visibilityReadback->AddSet(static_cast<std::uint32_t>(sealed.Of(s->visibilityEvents).units));
		}
		if (!revisions.layout || !(*revisions.layout == producer.mainLayout))
			revisions.layout = std::make_shared<const MainLayout>(producer.mainLayout);
		revisions.sealed = std::move(seal);
		const MainLayout& mainLayout = *revisions.layout;
		// The shapes are made again only when what they are made from moved (RevisionShapesKey): otherwise the revision sealed at this
		// make keeps the last ones. What the participants note of the make first (the shadow engine's casting bound outgrown).
		for (auto* participant : participants)
			participant->NoteSealed(make);
		const auto shapesKey = [&] {
			ZoneScopedN("CS.DCLF.MakeRevisionShapes.ShapesKey");
			return RevisionShapesKey(sealed, make, request.pipelines);
		}();
		if (shapesKey == revisionShapesKey) {
			++shapesKept;
			// CS_DCLF_PERSISTENT_PARITY: the shapes made anyway, against the kept ones (which stay the revision's).
			if (parity) {
				const MadeShapes kept = made;
				for (auto* participant : participants)
					participant->MakeShapes(make, sealed, mainLayout);
				reuse.shapesParity.Check(SameMadeShapes(kept, made), [&] { return MadeShapesDifference(kept, made); });
				made = kept;
			}
			assemble(true);
			return;
		}
		for (std::size_t g = 0; g < shapesKey.size(); ++g)
			shapesMadeBy[g] += shapesKey[g] != revisionShapesKey[g] ? 1 : 0;
		revisionShapesKey = shapesKey;
		++shapesMade;
		{
			ZoneScopedN("CS.DCLF.MakeRevisionShapes.Shapes");
			for (auto* participant : participants)
				participant->MakeShapes(make, sealed, mainLayout);
		}
		Diagnostics::Notify(Diagnostics::RevisionShapesMade{ sealed, mainLayout, made, inputs, tables.geometries.size() });
		// The revision of these shapes: assembled, its changed epochs recorded for it.
		assemble(false);
	}

	void IndirectDraws::Impl::RequireRevisionSet(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& request = a_make.publication.revision;
		const auto& tables = a_make.tables;
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& latches = producer.latches;
		if (inputs.scene)
			RequireSceneTables(a_build, *inputs.scene, tables);
		// The request's catalog's set version first: the main layout's colour sub-ranges are its state classes' (RequireMain).
		a_make.indirect = a_make.catalog ? GetIndirectState(*a_make.catalog, inputs.targetsGeneration) : IndirectState{};
		RequireMain(a_build, r, a_make);
		if (inputs.scene)
			RequireIndexPool(a_build, *inputs.scene, tables, a_make.publication.tablesGeneration);
		// The colour commit's sun cascades and local shadow light volumes, a shadow view each (the last Rebuild's candidates): the latch
		// a revision names holds them from its make on, not from a colour commit's growth (which a frame without claims never runs).
		{
			const std::uint32_t views = inputs.shadowCandidates;
			// And the Z-prepass's bucket map for every pipeline slot the tables have: a publication a revision made now covers holds no more
			// (its commit is this make's or older), and a commit past the map draws nothing for the slots it lacks.
			const auto& layout = latches.main;
			ReserveMainLatch(std::max(views, layout.cascades), std::max(views, layout.shadowVolumes), std::max(layout.buckets, request.pipelines));
		}
	}

	void IndirectDraws::Impl::RequireRevisionViews(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& request = a_make.publication.revision;
		const auto& tables = a_make.tables;
		const auto& inputs = producer.inputs;
		// The views' forward pipelines (and the faces' draw families'), as the request's catalog has them for its tables, and each view's plan
		// from the one main layout, its buffers required to hold that plan: the shape made below from the same layout and the set sealed
		// with those requirements can never index past its buffers. Their maps for every pipeline slot the tables have, as the
		// Z-prepass's (above).
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			a_make.familyOn[d] = CatalogFamilyPipelines(a_make.catalog, static_cast<Api::DrawFamily>(d), inputs.targetsGeneration, a_make.familyPipelines[d]);
		auto& viewOn = a_make.viewOn;
		for (const auto& slot : kViewSlots) {
			const auto kind = slot.view;
			if (!Extensions::ViewRegistered(kind))
				continue;
			const std::size_t v = ViewIndex(kind);
			const auto& traits = TraitsOf(kind);
			const auto& view = inputs.views[v];
			viewOn[v] = view.targets.colour != DXGI_FORMAT_UNKNOWN && Extensions::ViewOn(producer.toggles, kind) && inputs.claims;
			ViewPipelinesOf(kind, a_make.catalog, tables, view.targets, viewOn[v], viewRevisionPipelines[v]);
			if (traits.forwardPart)
				ViewForwardSlotsOf(tables, viewOn[v], viewForwardSlots);
			for (std::size_t d = 0; traits.trees && d < Extensions::kDrawFamilies; ++d) {
				const auto family = static_cast<Api::DrawFamily>(d);
				viewRevisionFaces[d] = viewOn[v] && Extensions::DrawFamilyOn(producer.toggles, family) ?
				                           ForwardPipelineOf(a_make.catalog, ForwardPipelineKey{ 0, 0, FamilySlotOf(family).forwardFlags }, view.targets) :
				                           rhi::PipelineHandle{};
			}
			if (!view.resources || (traits.depthPass && !viewOn[v]))
				continue;
			// A view's own frame values (the faces': its frame buffers' copies, then its frame constants'): its latch's frame region holds them
			// at the depth commit's block sizes, grown by a page (another layout: a new block at the seal).
			if (OwnsFrameConstants(traits) && view.resources->frameConstants && producer.latches.viewsLaidOut[v]) {
				const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire);
				const auto bytes = static_cast<std::uint32_t>(LatchedBytes(ViewFrameLatched(view.resources->frameConstants.get(), frameBuffers.get(), inputs.blockSizes[kDepthShape], 0)));
				if (auto& layout = producer.latches.views[v]; bytes > layout.frameBytes)
					layout.frameBytes = (bytes + 4095u) & ~4095u;
			}
			ViewPlan plan;
			PlanViewBuckets(traits, producer.mainLayout, &a_make.lookups, a_make.indirect, viewRevisionPipelines[v], traits.forwardPart ? std::span<const std::uint8_t>(viewForwardSlots) : std::span<const std::uint8_t>{}, plan);
			// The forward range's table entries and count words after the buckets' (one per state class), and its class map's words.
			RequireView(a_build, *view.resources, static_cast<std::uint32_t>(std::max<std::size_t>(plan.map.size(), request.pipelines)),
				static_cast<std::uint32_t>(plan.buckets.size() + plan.forwardRanges.size()), plan.draws,
				plan.forwardRanges.empty() ? 0u : static_cast<std::uint32_t>(a_make.indirect.classOf.size()));
		}
	}

	void IndirectDraws::Impl::RequireRevisionLatched(RevisionMake& a_make, GrowthBuild& a_build)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		// The main segments' Z plans and latched copies, from what the shapes are made of: the set holds their latched blocks.
		auto& zPlans = a_make.zPlans;
		auto& mainLatched = a_make.mainLatched;
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			if (!inputs.mainKnown[shape] || !a_make.indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			if (depthOnly && r.pool)
				PlanZBuckets(producer.mainLayout, a_make.lookups, a_make.tables, a_make.indirect, zPlans[shape]);
			const std::uint32_t classWords = !depthOnly && a_make.classPlan ? a_make.classPlan->Words() : 0u;
			mainLatched[shape] = MainLatchedLayout(r, depthOnly, inputs.blockSizes[shape], static_cast<std::uint32_t>(zPlans[shape].calls.size()), classWords);
			RequireLatchedBlock(a_build, producer.latchHandles.mainLatched[shape], mainLatched[shape]);
		}
	}

	void IndirectDraws::Impl::MakeMainShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& handles = producer.latchHandles;
		// The heaps every recording binds: the graph's own, as built (posted at the build point).
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			auto& mainShape = producer.made.main[shape];
			mainShape = nullptr;
			// A segment no commit has captured for yet: no viewport, no frame blocks.
			if (!inputs.mainKnown[shape] || !a_make.indirect.valid)
				continue;
			const bool depthOnly = shape == kDepthShape;
			auto in = MainShapeInputsOf(r, a_layout, a_sealed, depthOnly, producer.toggles.cullMode);
			in.latch = a_sealed.Latch(handles.main);
			in.latchLayout = producer.latches.main;
			in.viewport = inputs.viewport[shape];
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = a_make.indirect;
			in.zCalls = a_make.zPlans[shape].calls;
			in.zPlan = std::make_shared<const ZBucketPlan>(std::move(a_make.zPlans[shape]));
			in.classPlan = a_make.classPlan;
			in.latched.copies = std::move(a_make.mainLatched[shape]);
			if (!in.latched.copies.empty())
				in.latched.latch = a_sealed.Latch(handles.mainLatched[shape]);
			mainShape = MakeMainShape(in);
		}
	}

	void IndirectDraws::Impl::MakeViewShapes(const RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout)
	{
		const auto& inputs = producer.inputs;
		auto& r = *inputs.main;
		const auto& handles = producer.latchHandles;
		const auto& latches = producer.latches;
		const auto& indirect = a_make.indirect;
		const auto& viewOn = a_make.viewOn;
		const rhi::DescriptorHeapHandle resourceHeap = inputs.resourceHeap, samplerHeap = inputs.samplerHeap;
		// The views', from their resources and the main layout (the heaps the graph's); first person's render area is the colour commit's
		// viewport. The faces' lists hold each draw family's units (its faces.lists: tree LOD's shape slots; one unit group with its
		// tables: the least any of them holds).
		std::array<std::uint32_t, Extensions::kDrawFamilies> familyUnits{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			const auto& faces = Extensions::DrawFamilyOf(family).contribution.faces;
			if (const auto& s = inputs.scene; s && faces.lists && FamilySetUp(*s, family)) {
				UnitGroup units;
				Api::DrawFamilyLists lists{ *s, {}, &units };
				faces.lists(lists);
				for (const auto& view : inputs.views)
					if (view.resources)
						for (const auto& list : view.resources->faceLists[d])
							units.Add(list);
				familyUnits[d] = static_cast<std::uint32_t>(std::min<std::uint64_t>(units.Units(a_sealed), UINT32_MAX));
			}
		}
		for (const auto& slot : kViewSlots) {
			const std::size_t v = ViewIndex(slot.view);
			const auto& traits = TraitsOf(slot.view);
			const auto& view = inputs.views[v];
			auto& shape = producer.made.views[v];
			shape = nullptr;
			// The view's latch and zeros as the set names them, at the layout they were required at.
			ViewLatches viewLatches;
			if (latches.viewsLaidOut[v])
				viewLatches = { a_sealed.Latch(handles.views[v]), a_sealed.Latch(handles.viewZeros[v]), latches.views[v] };
			const bool ready = traits.depthPass ? viewOn[v] && inputs.mainKnown[kColourShape] && viewLatches.latch : inputs.claims && inputs.scene;
			if (!view.resources || !indirect.valid || !ready)
				continue;
			ViewShapeInputs in;
			PlanViewBuckets(traits, a_layout, &a_make.lookups, indirect, viewRevisionPipelines[v], traits.forwardPart ? std::span<const std::uint8_t>(viewForwardSlots) : std::span<const std::uint8_t>{}, in.plan);
			in.resourceHeap = resourceHeap;
			in.samplerHeap = samplerHeap;
			in.indirect = indirect;
			in.latches = &viewLatches;
			// The view's own frame constants (the faces': its commit's, from this frame's sources) or the colour segment's.
			const auto& own = view.resources->frameConstants;
			in.frameConstantsAddress = own ? view.resources->frameConstantsAddress : r.frameConstantsAddress;
			if (own && viewLatches.latch && viewLatches.layout.frameBytes) {
				const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire);
				in.frameCopies = ViewFrameLatched(own.get(), frameBuffers.get(), inputs.blockSizes[kDepthShape], viewLatches.layout.FrameOffset());
				if (LatchedBytes(in.frameCopies) > viewLatches.layout.Bytes())
					in.frameCopies.clear();  // laid out for smaller blocks: the commit stages them
			}
			in.width = view.width;
			in.height = view.height;
			in.depthRange = view.depthRange;
			// The range the named version holds: the set holds this plan's draws (RequireView), so the plan never reaches past it.
			in.sequenceDraws = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_sealed.Of(view.resources->sequences).units, UINT32_MAX));
			for (std::size_t d = 0; traits.trees && d < Extensions::kDrawFamilies; ++d)
				if (viewRevisionFaces[d].valid() && a_make.familyOn[d] && familyUnits[d])
					in.faces[d] = { viewRevisionFaces[d], a_make.familyPipelines[d].drawSignature, familyUnits[d] };
			if (!traits.depthPass || ((!in.plan.buckets.empty() || !in.plan.forwardRanges.empty()) && viewLatches.zeros))
				shape = MakeViewShape(*view.resources, in);
		}
	}

	IndirectDraws::Impl::ShapesKey IndirectDraws::Impl::RevisionShapesKey(const NamedVersions& a_sealed, const RevisionMake& a_make, std::size_t a_pipelines) const
	{
		ShapesKey key;
		key.fill(0xcbf29ce484222325ull);
		ShapesKeyMixer mixer{ key };
		auto& group = mixer.group;
		auto mix = [&mixer](std::uint64_t a_value) { mixer.Mix(a_value); };
		auto ptr = [&mixer](const void* a_pointer) { mixer.Ptr(a_pointer); };
		auto raw = [&mixer]<class T>(const T& a_value) { mixer.Raw(a_value); };
		// The revision's inputs (RevisionInputs): what its shapes are made from besides the request.
		const auto& inputs = producer.inputs;
		// The request's lookups (the publication's, which the shapes are made from).
		const auto& lookups = a_make.lookups;
		// What every shape names: the sealed set (its versions' addresses and capacities, and its latch blocks: a replaced block moves
		// its serial; the same serial while nothing changed) and the main layout, the heaps, the toggles (the pipeline sets: each
		// participant's, below).
		group = kKeyVersions;
		mix(a_sealed.Serial());
		if (const auto& layout = revisions.layout) {
			mix(layout->zBucketsLayout);
			// The slots' capacities the state classes' sub-ranges are summed from (their classes are the pipeline set's and the lookups'),
			// and the decal sub-ranges the sequences hold.
			mix(layout->slotsLayout);
			raw(std::array<std::uint32_t, 7>{ layout->sequenceDraws, layout->sequenceDecals, layout->decalRanges, layout->terrainCapacity[0], layout->terrainCapacity[1],
				layout->forwardCapacity, layout->refractionCapacity });
		}
		group = kKeyPipelines;
		mix(inputs.resourceHeap.index);
		mix(inputs.resourceHeap.generation);
		mix(inputs.samplerHeap.index);
		mix(inputs.samplerHeap.generation);
		mix(producer.toggles.cullMode);
		// What the shapes read of them: the pipeline slots' entries (the bucket plans' set indices, the shadow key slots' count:
		// RevisionMake::lookupsWitness) and the map rows the shadow layouts draw, not the lookups' versions and generations (a material
		// lookup's write moves those alone).
		group = kKeyLookups;
		mix(a_make.lookupsWitness);
		mix(ShadowRowsWitness(lookups, inputs.shadowPlacements, producer.recentOcclusionLayouts));
		mix(a_pipelines);  // the request's pipeline slots (the publication's tables, not the coordinator's)
		// CS's SharedData and FeatureData sizes the shadow shapes' arena blocks take (ShadowArenaBlocksOf: the views' push data, the
		// commit's latched copies), which no shadow input carries.
		if (inputs.shadow) {
			group = kKeyShadow;
			const auto blocks = ShadowArenaBlocksOf();
			mix((std::uint64_t(blocks.sharedBytes) << 32) | blocks.featureBytes);
		}
		// Each participant's parts, in order: the main segments', the shadow engine's, the views'.
		for (const auto* participant : participants)
			participant->MixShapesKey(a_make, mixer);
		return key;
	}

	void IndirectDraws::Impl::MixMainShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const
	{
		auto& group = a_mixer.group;
		auto mix = [&a_mixer](std::uint64_t a_value) { a_mixer.Mix(a_value); };
		auto ptr = [&a_mixer](const void* a_pointer) { a_mixer.Ptr(a_pointer); };
		auto raw = [&a_mixer]<class T>(const T& a_value) { a_mixer.Raw(a_value); };
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The main pipeline set, and the refraction draw the main and view shapes' indirect state names (SameIndirect): the catalog's own,
		// not its set version's, so a catalog publishing it under the same version moves only this.
		group = kKeyPipelines;
		ptr(a_make.indirect.version.get());
		mix(a_make.indirect.valid);
		mix(a_make.indirect.refraction.index);
		mix(a_make.indirect.refraction.generation);
		mix(static_cast<std::uint64_t>(a_make.indirect.refractionFormat));
		// The main segments': their latch's layout, latched targets, and what their commits captured (viewport, frame blocks). The
		// blocks are the set's (its serial).
		group = kKeyMain;
		const auto& r = *inputs.main;
		ptr(&r);
		raw(latches.main);
		ptr(r.latchedTargets.load(std::memory_order_acquire).get());
		ptr(r.pool.get());
		for (const std::size_t shape : { kDepthShape, kColourShape }) {
			mix(inputs.mainKnown[shape]);
			raw(inputs.viewport[shape]);
			raw(inputs.blockSizes[shape]);
		}
	}

	void IndirectDraws::Impl::ViewParticipant::MixShapesKey(const RevisionMake& a_make, ShapesKeyMixer& a_mixer) const
	{
		// By draw family, its pipelines as the request's catalog has them (null: none).
		FamilyPipelinesSet families{};
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
			families[d] = a_make.familyOn[d] ? &a_make.familyPipelines[d] : nullptr;
		owner.MixViewShapesKey(a_mixer, families);
	}

	void IndirectDraws::Impl::MixViewShapesKey(ShapesKeyMixer& a_mixer, const FamilyPipelinesSet& a_families) const
	{
		const auto& inputs = producer.inputs;
		const auto& latches = producer.latches;
		// The views': their resources, latch layouts, targets, render area and depth range, the slots' pipelines, first person's forward slots,
		// the faces' draw families' (the request's catalog's).
		for (const auto& slot : kViewSlots) {
			const auto kind = slot.view;
			const std::size_t v = ViewIndex(kind);
			a_mixer.group = slot.keyGroup;
			const auto& view = inputs.views[v];
			if (!view.resources)
				continue;
			a_mixer.Ptr(view.resources.get());
			// A view's own frame buffers' copies (its frame values' latched copies name them), made again with the main frame buffers' shape.
			if (const auto frameBuffers = view.resources->frameBuffers.load(std::memory_order_acquire))
				a_mixer.Ptr(frameBuffers.get());
			a_mixer.Mix(latches.viewsLaidOut[v]);
			a_mixer.Raw(latches.views[v]);
			a_mixer.Raw(view.targets);
			a_mixer.Mix(view.width);
			a_mixer.Mix(view.height);
			a_mixer.Raw(view.depthRange);
			for (const auto& pipeline : viewRevisionPipelines[v]) {
				a_mixer.Mix(pipeline.index);
				a_mixer.Mix(pipeline.generation);
			}
			if (TraitsOf(kind).forwardPart)
				for (const std::uint8_t forward : viewForwardSlots)
					a_mixer.Mix(forward);
			if (TraitsOf(kind).trees)
				for (const auto& pipeline : viewRevisionFaces) {
					a_mixer.Mix(pipeline.index);
					a_mixer.Mix(pipeline.generation);
				}
		}
		// Every draw family slot's (tree LOD's), in the group of the view that draws them per face; each view whose shape is dropped while
		// it is off, its toggle.
		for (const auto& slot : kViewSlots) {
			if (!TraitsOf(slot.view).trees)
				continue;
			a_mixer.group = slot.keyGroup;
			if (const auto& sceneState = inputs.scene)
				for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
					if (const auto* family = a_families[d]) {
						a_mixer.Mix(family->drawSignature.index);
						a_mixer.Mix(family->drawSignature.generation);
					}
					a_mixer.Mix(a_families[d] ? 1u : 0u);
					a_mixer.Mix(FamilySetUp(*sceneState, static_cast<Api::DrawFamily>(d)) ? 1u : 0u);
				}
		}
		for (const auto& slot : kViewSlots)
			if (Extensions::Views().views[ViewIndex(slot.view)].life == Api::ViewShapeLife::DropWhenOff) {
				a_mixer.group = slot.keyGroup;
				a_mixer.Mix(Extensions::ViewOn(producer.toggles, slot.view));
			}
	}
}
