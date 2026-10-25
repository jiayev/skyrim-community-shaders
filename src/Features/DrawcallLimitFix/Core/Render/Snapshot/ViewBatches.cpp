// The CPU pipeline's view batches (ViewBatches.h): the snapshot builder's per-view lists of its draws' scene list, as fixed-size SoA
// batches, and their resident sequences (what BuildDrawsCS would store for each drawn partition, resolved against the snapshot's payload)
// in a pooled version of their own.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <ORGModuleServices/VersionedBufferBuild.h>
#include <Render/Runtime/IUploadService.h>

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <bit>
#include <cstdlib>
#include <cstring>

namespace DCLF::Draws
{
	namespace
	{
		using Direct::ViewType;
		constexpr std::size_t View(ViewType a_view) { return static_cast<std::size_t>(a_view); }
		constexpr std::uint32_t kNoKey = ~0u;
		constexpr std::uint32_t kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT

		/** @brief The batches' lanes: CS_DCLF_CPU_BATCH, else the default, rounded up to a multiple of 8 (a lane index is 16 bits). */
		std::uint32_t BatchLanes()
		{
			static const std::uint32_t lanes = [] {
				std::uint32_t value = Direct::kDefaultBatchLanes;
				if (const auto& text = SwitchValue(Switch::CpuBatch); !text.empty())
					if (const unsigned long parsed = std::strtoul(text.c_str(), nullptr, 10); parsed != 0)
						value = static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 65528));
				return std::max<std::uint32_t>(8, (value + 7) & ~7u);
			}();
			return lanes;
		}

		/** @brief One lane before its batch is laid out: its sort key (k0, k1, object), its bucket's key and its columns. */
		struct Lane
		{
			std::uint32_t k0 = 0, k1 = 0;
			std::uint32_t object = 0, sequence = 0;
			std::uint16_t sequenceCount = 0;
			std::uint32_t bucketKey = 0, pipelineSlot = ~0u;
			std::uint32_t flags = 0, fadeRoot = 0;
			float fadeDistance = 0.0f;
			std::uint32_t partitions = 0, lodPartitions = 0;
			std::uint32_t decalGroup = 0;  // the decal list's: its group (1-3)
		};
		/** @brief An entry's run of resident sequences, per view class (made at its first view of the class). */
		struct Run
		{
			std::uint32_t first = kNoKey;
			std::uint16_t count = 0;
		};

		/** @brief MakeViewBatches' scratch, the builder's alone (kept warm across sets). */
		struct Scratch
		{
			std::array<std::vector<Lane>, Direct::kViewTypes> lanes;
			std::vector<std::uint32_t> moverCount;
		};
		Scratch& BuilderScratch()
		{
			static Scratch scratch;
			return scratch;
		}

		/** @brief The resident sequences of one set, as they are made (the GPU copy and the recorder's arguments). */
		struct SequenceSink
		{
			std::vector<DrawSequence>& sequences;
			std::vector<Direct::DrawArgs>& args;
			const GeometryDrawsOut& geometries;

			const GeometryDraw* At(std::uint32_t a_index) const
			{
				const std::size_t slots = geometries.SlotCount();
				if (a_index < slots)
					return geometries.slots.At(a_index);
				return a_index - slots < geometries.streamRows.size() ? &geometries.streamRows[a_index - slots] : nullptr;
			}
			/** @brief A plain pulled draw's sequence (StoreShadowSequence's, its pool first left out: the recorder adds it). */
			void Emit(const DrawSequence& a_rows, std::uint32_t a_slot, const GeometryDraw& a_geometry, const GeometryDraw* a_stream)
			{
				DrawSequence s = a_rows;
				const GeometryDraw& second = a_stream ? *a_stream : a_geometry;
				s.vertexBufferAddress = a_geometry.vertexBufferAddress;
				s.vertexBufferSize = a_geometry.vertexBufferSize;
				s.vertexStride = a_geometry.vertexStride;
				s.streamBufferAddress = second.vertexBufferAddress;
				s.streamBufferSize = second.vertexBufferSize;
				s.streamStride = second.vertexStride;
				s.blockPad = 0;
				SetSequenceBlocks(s);
				s.indexBufferAddress = a_geometry.indexBufferAddress;
				s.indexBufferSize = a_geometry.indexBufferSize;
				s.indexFormat = kIndexFormatR16;
				s.indexCount = a_geometry.indexCount;
				s.instanceCount = 1;
				s.firstIndex = a_geometry.firstIndex;
				s.vertexOffset = 0;
				s.firstInstance = 0;
				sequences.push_back(s);
				args.push_back({ a_geometry.indexCount, a_geometry.firstIndex, a_slot });
			}
			/**
			 * @brief An input's partitions' sequences, BuildDrawsCS's walk (the GeometryDraw links from its geometry) over every partition any
			 * LOD level may draw: a LOD skin's levels' masks ORed (Cull.cpp draws the k-th of them as the k-th set bit), else its partitions.
			 */
			Run EmitPartitions(const DrawSequence& a_rows, const DrawInput& a_input)
			{
				const std::uint32_t lod = a_input.lodPartitions;
				const std::uint32_t mask = lod ? ((lod | (lod >> 8) | (lod >> 16) | (lod >> 24)) & 0xFFu) : a_input.partitions;
				Run run{ static_cast<std::uint32_t>(sequences.size()), 0 };
				if (mask == kNoPartitions)
					return run;
				const GeometryDraw* stream = a_input.streamIndex != ~0u ? At(a_input.streamIndex) : nullptr;
				const bool chain = (mask & kPartitionChain) != 0;
				const std::uint32_t links = chain ? (mask & kPartitionChainCount) : kMaxSkinPartitions;
				std::uint32_t slot = a_input.geometryIndex;
				for (std::uint32_t partition = 0; partition < links && slot != kNoPartition; ++partition) {
					const GeometryDraw* geometry = At(slot);
					if (!geometry)
						break;
					if (chain || mask == 0 || ((mask >> partition) & 1u)) {
						Emit(a_rows, slot, *geometry, stream);
						++run.count;
					}
					slot = geometry->nextPartition;
				}
				return run;
			}
			/** @brief A decal's slots of its span (BuildDrawsCS's decal walk), the drawn partitions' sequences in slot order. */
			Run EmitDecal(const DrawSequence& a_rows, const DrawInput& a_input)
			{
				const std::uint32_t p = a_input.partitions;
				const bool chain = (p & kPartitionChain) != 0;
				const std::uint32_t span = chain ? (p & kPartitionChainCount) :
				                           (p == 0 || p == kNoPartitions) ? 1u :
				                                                            static_cast<std::uint32_t>(std::bit_width(p & (kNoPartitions - 1)));
				const GeometryDraw* stream = a_input.streamIndex != ~0u ? At(a_input.streamIndex) : nullptr;
				Run run{ static_cast<std::uint32_t>(sequences.size()), 0 };
				std::uint32_t slot = a_input.geometryIndex;
				for (std::uint32_t k = 0; k < span && slot != kNoPartition; ++k) {
					const GeometryDraw* geometry = At(slot);
					if (!geometry)
						break;
					if (chain || p == 0 || ((p >> k) & 1u)) {
						Emit(a_rows, slot, *geometry, stream);
						++run.count;
					}
					slot = geometry->nextPartition;
				}
				return run;
			}
		};

		/** @brief Where a set's sequences' rows are: the payload's row parts' versions (their addresses), and the shadow key slots' entries. */
		struct RowBases
		{
			std::uint64_t pipelineRows = 0, materialRows = 0, shadowRows = 0;
			const std::vector<ShadowPipelineKey>* slotKeys = nullptr;

			static std::uint64_t AddressOf(const ScenePayload& a_payload, PayloadPart a_part)
			{
				const auto& version = a_payload.versions[static_cast<std::size_t>(a_part)];
				return version ? version->lease->address : 0;
			}
			/** @brief A main input's rows (BuildDrawsCS RowsOf: the installed rows; an animated row's frame copy is not resolved here). */
			DrawSequence Main(const DrawInput& a_input) const
			{
				DrawSequence s{};
				s.pipelineRowAddress = pipelineRows + std::uint64_t(a_input.recordIndex >> kRowPipelineShift) * kPipelineRowBytes;
				s.materialRowAddress = materialRows + std::uint64_t(a_input.recordIndex & kRowMaterialMask) * kMaterialRowBytes;
				s.objectIndex = a_input.objectIndex & kObjectIndexMask;
				return s;
			}
			/** @brief A shadow input's rows for key slot a_key: its slot's vertex layout in the pipeline-row words, its technique in the pipeline word. */
			DrawSequence Shadow(const DrawInput& a_input, std::uint32_t a_key) const
			{
				DrawSequence s{};
				if (slotKeys && a_key < slotKeys->size()) {
					const auto& key = (*slotKeys)[a_key];
					s.pipelineIndex = key.technique | kShadowSlotTechniqueKnown;
					s.pipelineRowAddress = key.vertexLayout;
				}
				s.materialRowAddress = shadowRows + std::uint64_t(a_input.shadowRow & kRowMaterialMask) * kConstantAlignment;
				s.objectIndex = a_input.objectIndex & kObjectIndexMask;
				return s;
			}
		};

		/** @brief The lane flags an input's object flags carry. */
		std::uint32_t LaneFlagsOf(const DrawInput& a_input)
		{
			const std::uint32_t f = a_input.flags;
			std::uint32_t out = 0;
			out |= (f & kObjectFadeTest) ? Direct::kLaneFadeTest : 0u;
			out |= (f & kObjectHeightTest) ? Direct::kLaneHeightTest : 0u;
			out |= (f & kObjectSunTest) ? Direct::kLaneSunTest : 0u;
			out |= (f & kObjectForward) ? Direct::kLaneForward : 0u;
			out |= ObjectDecalGroup(f) ? Direct::kLaneDecal : 0u;
			out |= (f & kObjectVolumetricOnly) ? Direct::kLaneVolumetricOnly : 0u;
			out |= (f & kObjectLandscapeLights) ? Direct::kLaneLandscapeLights : 0u;
			const std::uint32_t terrain = ObjectTerrainGroup(f);
			out |= terrain == 1 ? Direct::kLaneTerrainGroup1 : terrain == 2 ? Direct::kLaneTerrainGroup2 : 0u;
			return out;
		}

		/** @brief Every input of the snapshot's scene list in its order: the region (the main payload's, else the shadow payload's), then the frame inputs. */
		template <class F>
		void ForEachInput(const ViewBatchInputs& a_in, F&& a_visit)
		{
			const auto visit = [&](const KeptView<DrawInput>& a_view) {
				for (std::size_t i = 0, n = a_view.Count(); i < n; ++i)
					a_visit(*a_view.At(i));
			};
			if (a_in.main) {
				visit(a_in.main->resident);
				for (const auto& input : a_in.main->body->inputList)
					a_visit(input);
			} else if (a_in.shadow) {
				visit(a_in.shadow->regionInputs);
			}
			if (a_in.shadow)
				for (const auto& input : a_in.shadow->inputList)
					a_visit(input);
		}

		/** @brief The lists' lanes from the scene list: each input in the views its mask names, its sequences per view class made once. */
		class ListBuilder
		{
		public:
			ListBuilder(const ViewBatchInputs& a_in, const RowBases& a_rows, SequenceSink& a_sink, Scratch& a_scratch) :
				rows(a_rows), sink(a_sink), lanes(a_scratch.lanes)
			{
				if (a_in.catalog)
					zGroups = GetIndirectState(*a_in.catalog, a_in.targetsGeneration).zGroups;
				reflectionBit = Extensions::ViewEntryOf(Api::EngineView::Reflection).viewBit;
				firstPersonBit = Extensions::ViewEntryOf(Api::EngineView::FirstPerson).viewBit;
				for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
					const std::uint32_t bit = Extensions::OcclusionMapOf(v).viewBit;
					// Features/OcclusionMaps' bits (GpuLayouts.h: 1u << 5 the sky occluder's, 1u << 6 the precipitation mask's).
					occlusionBits[v] = bit;
					occlusionView[v] = bit == (1u << 5) ? ViewType::SkyOccluder : bit == (1u << 6) ? ViewType::PrecipOccluder : ViewType::kCount;
				}
			}

			void Add(const DrawInput& a_input)
			{
				Run mainRun, casterRun;
				std::array<Run, kOcclusionViews> occlusionRuns{};
				const std::uint32_t mask = a_input.view.mask;
				const bool drawable = (a_input.flags & kInputDrawable) != 0;
				const std::uint32_t decalGroup = ObjectDecalGroup(a_input.flags);
				const bool forward = (a_input.flags & kObjectForward) != 0;
				const auto mainRuns = [&]() -> const Run& {
					if (mainRun.first == kNoKey)
						mainRun = decalGroup ? sink.EmitDecal(rows.Main(a_input), a_input) : sink.EmitPartitions(rows.Main(a_input), a_input);
					return mainRun;
				};
				// The main family's views (keyWord 0: the drawable bit is theirs).
				if (!decalGroup && !forward && (mask & (kViewMain | kViewMainCull))) {
					if (drawable && (mask & kViewMain))
						AddMain(ViewType::Main, a_input, mainRuns(), ZGroupOf(a_input.pipelineIndex), a_input.pipelineIndex);
					else if (mask & kViewMainCull)
						AddCullOnly(a_input);
				}
				if (drawable && decalGroup && (mask & kViewDecal))
					AddMain(ViewType::Decal, a_input, mainRuns(), decalGroup, a_input.decalOrdinal).decalGroup = decalGroup;
				if (drawable && forward && (mask & kViewForward) && !(mask & kViewRefraction))
					AddMain(ViewType::Forward, a_input, mainRuns(), 0, a_input.pipelineIndex);
				if (drawable && (mask & kViewRefraction))
					AddMain(ViewType::Refraction, a_input, mainRuns(), 0, 0).bucketKey = 0;
				if (drawable && reflectionBit && (mask & reflectionBit))
					AddMain(ViewType::Reflection, a_input, mainRuns(), 0, a_input.pipelineIndex);
				if (drawable && firstPersonBit && (mask & firstPersonBit))
					AddMain(ViewType::FirstPerson, a_input, mainRuns(), 0, a_input.pipelineIndex);
				// The shadow family's (their keys' slots; every input their bits select draws).
				const std::uint32_t caster = a_input.view.shadowKey;
				const auto casters = [&]() -> const Run& {
					if (casterRun.first == kNoKey)
						casterRun = sink.EmitPartitions(rows.Shadow(a_input, caster), a_input);
					return casterRun;
				};
				if (caster != kNoKey) {
					if (mask & kViewSunCaster)
						AddShadow(ViewType::Sun, a_input, casters(), caster);
					if (mask & kViewSpotCaster)
						AddShadow(ViewType::Spot, a_input, casters(), caster);
					if (mask & kViewPointCaster)
						AddShadow(ViewType::Point, a_input, casters(), caster);
				}
				for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
					const std::uint32_t key = a_input.view.occlusionKey[v];
					if (key == kNoKey || !occlusionBits[v] || !(mask & occlusionBits[v]) || occlusionView[v] == ViewType::kCount)
						continue;
					if (occlusionRuns[v].first == kNoKey)
						occlusionRuns[v] = sink.EmitPartitions(rows.Shadow(a_input, key), a_input);
					AddShadow(occlusionView[v], a_input, occlusionRuns[v], key);
				}
			}

		private:
			std::uint32_t ZGroupOf(std::uint32_t a_set) const { return a_set < zGroups.size() ? zGroups[a_set] : 0xFFFFFu; }

			Lane& Push(ViewType a_view, const DrawInput& a_input, const Run& a_run)
			{
				auto& lane = lanes[View(a_view)].emplace_back();
				lane.object = a_input.objectIndex & kObjectIndexMask;
				lane.sequence = a_run.first == kNoKey ? 0u : a_run.first;
				lane.sequenceCount = a_run.count;
				lane.flags = LaneFlagsOf(a_input);
				lane.fadeRoot = a_input.fadeRoot == ~0u ? 0u : a_input.fadeRoot + 1;
				lane.fadeDistance = a_input.fadeDistance;
				lane.partitions = a_input.partitions;
				lane.lodPartitions = a_input.lodPartitions;
				return lane;
			}
			// A main family view's lane: its bucket the set index, ordered by (a_k0, a_k1, object).
			Lane& AddMain(ViewType a_view, const DrawInput& a_input, const Run& a_run, std::uint32_t a_k0, std::uint32_t a_k1)
			{
				auto& lane = Push(a_view, a_input, a_run);
				lane.k0 = a_k0;
				lane.k1 = a_k1;
				lane.bucketKey = a_input.pipelineIndex;
				lane.pipelineSlot = a_input.recordIndex >> kRowPipelineShift;
				return lane;
			}
			// Main's cull-only candidate: no sequences, after every drawn lane.
			void AddCullOnly(const DrawInput& a_input)
			{
				auto& lane = Push(ViewType::Main, a_input, Run{});
				lane.k0 = 1u << 31;
				lane.flags |= Direct::kLaneCullOnly;
				lane.bucketKey = kNoKey;
			}
			// A shadow family view's lane: its bucket the key slot (the view's map row names its pipeline under its mode and state).
			void AddShadow(ViewType a_view, const DrawInput& a_input, const Run& a_run, std::uint32_t a_key)
			{
				auto& lane = Push(a_view, a_input, a_run);
				lane.k1 = a_key;
				lane.bucketKey = a_key;
			}

			const RowBases& rows;
			SequenceSink& sink;
			std::array<std::vector<Lane>, Direct::kViewTypes>& lanes;
			std::vector<std::uint32_t> zGroups;
			std::uint32_t reflectionBit = 0, firstPersonBit = 0;
			std::array<std::uint32_t, kOcclusionViews> occlusionBits{};
			std::array<ViewType, kOcclusionViews> occlusionView{};
		};

		/** @brief The bytes a lane takes in its batch: its columns (Direct::ViewBatch), the 4-byte ones then the 2-byte ones. */
		constexpr std::size_t kLaneWords = 20;  // object, sequence, pipelineBucket, flags, fadeRoot, fadeDistance, partitions, lodPartitions, 4 spheres, 8 live
		constexpr std::size_t kLaneHalves = 1;  // sequenceCount
		constexpr std::size_t kLaneBytes = kLaneWords * 4 + kLaneHalves * 2;

		/** @brief a_batch's columns laid out at a_base (a_lanes each). */
		void LayOut(Direct::ViewBatch& a_batch, std::byte* a_base, std::uint32_t a_lanes)
		{
			auto words = reinterpret_cast<std::uint32_t*>(a_base);
			const auto column = [&]() {
				auto* at = words;
				words += a_lanes;
				return at;
			};
			const auto floats = [&]() { return reinterpret_cast<float*>(column()); };
			a_batch.object = column();
			a_batch.sequence = column();
			a_batch.pipelineBucket = column();
			a_batch.flags = column();
			a_batch.fadeRoot = column();
			a_batch.fadeDistance = floats();
			a_batch.partitions = column();
			a_batch.lodPartitions = column();
			a_batch.cx = floats();
			a_batch.cy = floats();
			a_batch.cz = floats();
			a_batch.r = floats();
			for (auto& live : a_batch.liveSpheres) {
				live.cx = floats();
				live.cy = floats();
				live.cz = floats();
				live.r = floats();
			}
			auto halves = reinterpret_cast<std::uint16_t*>(words);
			a_batch.sequenceCount = halves;
		}

		/** @brief Whether a_object's snapshot sphere is known (a_spheres' row is its geometry's and does not move), and the sphere. */
		bool SnapshotSphere(const SceneTables& a_tables, const Direct::SnapshotSpheres* a_spheres, std::uint32_t a_object, std::array<float, 4>& a_out)
		{
			if (!a_spheres || a_object >= a_spheres->sphere.size() || a_object >= a_tables.objectGeometry.size())
				return false;
			const void* owner = a_spheres->owner[a_object];
			if (!owner || owner != a_tables.objectGeometry[a_object] || a_spheres->moving[a_object])
				return false;
			a_out = a_spheres->sphere[a_object];
			return true;
		}

		/** @brief A list's lanes sorted (by key, then object) and given dense buckets; the view's buckets filled. */
		void SortList(std::vector<Lane>& a_lanes, Direct::ViewBuckets& a_buckets, ankerl::unordered_dense::map<std::uint32_t, std::uint32_t>& a_bucketOf)
		{
			std::sort(a_lanes.begin(), a_lanes.end(),
				[](const Lane& a, const Lane& b) { return a.k0 != b.k0 ? a.k0 < b.k0 : a.k1 != b.k1 ? a.k1 < b.k1 : a.object < b.object; });
			a_bucketOf.clear();
			for (auto& lane : a_lanes) {
				if (lane.bucketKey == kNoKey) {
					lane.bucketKey = 0;  // a cull-only lane draws nothing: its bucket is never read
					continue;
				}
				const auto [it, fresh] = a_bucketOf.try_emplace(lane.bucketKey, static_cast<std::uint32_t>(a_buckets.pipeline.size()));
				if (fresh) {
					a_buckets.pipeline.push_back(lane.bucketKey);
					a_buckets.pipelineSlot.push_back(lane.pipelineSlot);
				}
				lane.bucketKey = it->second;
			}
		}
	}

	std::shared_ptr<Direct::ViewBatchSet> MakeViewBatches(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, std::vector<DrawSequence>& a_sequences)
	{
		ZoneScopedN("CS.DCLF.ViewBatches");
		if (!a_in.tables || !a_in.payload || (!a_in.main && !a_in.shadow))
			return nullptr;
		auto set = std::make_shared<Direct::ViewBatchSet>();
		set->lanes = BatchLanes();
		set->keep = a_in.keep;
		const auto& geometries = a_in.main ? a_in.main->body->geometryDraws : a_in.shadow->geometries;
		set->geometryIndexAddress.assign(geometries.SlotCount(), 0);
		for (std::size_t g = 0; g < set->geometryIndexAddress.size(); ++g)
			if (const GeometryDraw* geometry = geometries.slots.At(static_cast<std::uint32_t>(g)))
				set->geometryIndexAddress[g] = geometry->indexBufferAddress;
		RowBases rows;
		rows.pipelineRows = RowBases::AddressOf(*a_in.payload, PayloadPart::PipelineRows);
		rows.materialRows = RowBases::AddressOf(*a_in.payload, PayloadPart::MaterialRows);
		rows.shadowRows = RowBases::AddressOf(*a_in.payload, PayloadPart::ShadowRows);
		rows.slotKeys = a_in.lookups ? &a_in.lookups->shadowSlotKeys : nullptr;
		auto& scratch = BuilderScratch();
		for (auto& lanes : scratch.lanes)
			lanes.clear();
		a_sequences.clear();
		SequenceSink sink{ a_sequences, set->sequenceArgs, geometries };
		ListBuilder lists(a_in, rows, sink, scratch);
		ForEachInput(a_in, [&](const DrawInput& a_input) { lists.Add(a_input); });
		set->sequenceCount = static_cast<std::uint32_t>(a_sequences.size());
		// Sorted, bucketed and sized: one allocation for every batch's columns.
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> bucketOf;
		const std::uint32_t laneCount = set->lanes;
		std::size_t batches = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			SortList(scratch.lanes[v], set->buckets[v], bucketOf);
			batches += (scratch.lanes[v].size() + laneCount - 1) / laneCount;
		}
		const std::size_t batchBytes = std::size_t(laneCount) * kLaneBytes;
		set->storage = std::make_unique<std::byte[]>(batches * batchBytes);  // zeroed: the live columns until the producer fills them
		const std::size_t objects = a_in.tables->objects.size();
		auto& moverCount = scratch.moverCount;
		moverCount.assign(objects + 1, 0u);
		std::size_t next = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			const auto& lanes = scratch.lanes[v];
			auto& list = set->lists[v];
			list.resize((lanes.size() + laneCount - 1) / laneCount);
			for (std::size_t b = 0; b < list.size(); ++b) {
				auto& batch = list[b];
				batch.index = static_cast<std::uint32_t>(b);
				batch.count = static_cast<std::uint32_t>(std::min<std::size_t>(laneCount, lanes.size() - b * laneCount));
				LayOut(batch, set->storage.get() + (next++) * batchBytes, laneCount);
				for (std::uint32_t l = 0; l < batch.count; ++l) {
					const auto& lane = lanes[b * laneCount + l];
					batch.object[l] = lane.object;
					batch.sequence[l] = lane.sequence;
					batch.sequenceCount[l] = lane.sequenceCount;
					batch.pipelineBucket[l] = lane.bucketKey;
					batch.fadeRoot[l] = lane.fadeRoot;
					batch.fadeDistance[l] = lane.fadeDistance;
					batch.partitions[l] = lane.partitions;
					batch.lodPartitions[l] = lane.lodPartitions;
					std::array<float, 4> sphere{ 0.0f, 0.0f, 0.0f, -1.0f };
					std::uint32_t flags = lane.flags;
					if (!SnapshotSphere(*a_in.tables, a_spheres, lane.object, sphere)) {
						flags |= Direct::kLaneMover;
						if (lane.object < objects)
							++moverCount[lane.object];
					}
					batch.flags[l] = flags;
					batch.cx[l] = sphere[0];
					batch.cy[l] = sphere[1];
					batch.cz[l] = sphere[2];
					batch.r[l] = sphere[3];
				}
			}
		}
		// The movers' lanes, by object (FrameValues' producer writes their live spheres every frame).
		auto& first = set->moverLaneFirst;
		first.assign(objects + 1, 0u);
		for (std::size_t o = 0; o < objects; ++o)
			first[o + 1] = first[o] + moverCount[o];
		set->moverLanes.resize(first[objects]);
		std::fill(moverCount.begin(), moverCount.end(), 0u);
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v)
			for (const auto& batch : set->lists[v])
				for (std::uint32_t l = 0; l < batch.count; ++l)
					if (const std::uint32_t o = batch.object[l]; (batch.flags[l] & Direct::kLaneMover) && o < objects)
						set->moverLanes[first[o] + moverCount[o]++] = { static_cast<std::uint16_t>(v), batch.index, static_cast<std::uint16_t>(l) };
		// The decal list's groups by main range slot (group g's slot is g - 1): its lanes are sorted by group.
		const auto& decals = scratch.lanes[View(ViewType::Decal)];
		if (!decals.empty()) {
			auto& groupFirst = set->decalGroupFirst;
			groupFirst.assign(Extensions::kMainRangeSlots + 1, 0u);
			for (const auto& lane : decals)
				if (lane.decalGroup >= 1 && lane.decalGroup <= kDecalGroups)
					++groupFirst[lane.decalGroup];
			for (std::size_t g = 1; g < groupFirst.size(); ++g)
				groupFirst[g] += groupFirst[g - 1];
		}
		return set;
	}

	// ---- The sequences' version (the payload's publication rules: a pooled version, written whole into a backing no frame reads, GPU
	// ready before the snapshot is posted).

	struct ViewBatchBuilder::Pending
	{
		std::atomic<bool> done{ false };
		std::shared_ptr<const org::services::PublishedBufferVersion> version;  // null: failed (error)
		std::string error;
	};

	namespace
	{
		/** @brief The sequences' backing pool (DCLF's retirement clock), made at the first post; never destroyed (a lease may return late). */
		const std::shared_ptr<org::services::BackingPool>& SequencesPool()
		{
			static const auto* pool = new std::shared_ptr<org::services::BackingPool>(org::services::BackingPool::Create(PooledBackings::Hooks(), FrameRetirement(),
				{ .name = "cs.dclf.cpu.sequences",
					// A frame in flight's set, the installed one's, the one being built, and a set a site still holds per frame slot.
					.maxPerClass = RenderGraphRuntime::kGameFramesInFlight + 3,
					.framesInFlight = RenderGraphRuntime::kGameFramesInFlight,
					.spareFree = 1 }));
			return *pool;
		}

		/** @brief Posts a_sequences as a version (whole: no predecessor); false when refused (a_pending's done never runs). */
		bool PostSequences(std::shared_ptr<const std::vector<DrawSequence>> a_sequences, const std::shared_ptr<org::runtime::IUploadService>& a_uploads,
			std::uint64_t a_version, const std::shared_ptr<ViewBatchBuilder::Pending>& a_pending);
	}

	ViewBatchBuilder::ViewBatchBuilder() = default;
	ViewBatchBuilder::~ViewBatchBuilder() = default;

	void ViewBatchBuilder::Reset()
	{
		payload = nullptr;
		set.reset();
		building.reset();
		pending.reset();  // a version in flight finishes into its own Pending, then drops
		failed = false;
	}

	bool ViewBatchBuilder::Ready(const ViewBatchInputs& a_in)
	{
		if (payload != a_in.payload) {
			Reset();
			payload = a_in.payload;
		}
		if (!a_in.payload || set || failed)
			return true;
		if (!pending) {
			// The newest spheres a producer posted, and the next producer's asked for (the next set's).
			auto& values = FrameValues::Get();
			if (auto taken = values.TakeSnapshotSpheres())
				spheres = std::move(taken);
			values.RequestSnapshotSpheres();
			auto sequences = std::make_shared<std::vector<DrawSequence>>();
			building = MakeViewBatches(a_in, spheres.get(), *sequences);
			if (!building) {
				failed = true;
				return true;
			}
			if (sequences->empty()) {
				set = std::move(building);
				return true;
			}
			if (!a_in.uploads || !a_in.uploads->HasDedicatedStreamingQueue()) {
				static std::atomic<bool> logged{ false };
				if (!logged.exchange(true))
					logger::info("[DCLF] CPU pipeline: the view batches' sequences wait for the dedicated upload queue; snapshots have no batches until it is made");
				building.reset();
				failed = true;
				return true;
			}
			auto posted = std::make_shared<Pending>();
			if (!PostSequences(std::move(sequences), a_in.uploads, ++versions, posted)) {
				building.reset();
				failed = true;
				return true;
			}
			pending = std::move(posted);
			return false;
		}
		if (!pending->done.load(std::memory_order_acquire))
			return false;
		const auto done = std::move(pending);
		if (!done->version) {
			static std::atomic<std::uint32_t> logged{ 0 };
			if (logged++ < 4)
				logger::error("[DCLF] CPU pipeline: the view batches' sequences version failed: {}; the snapshot has no batches", done->error);
			building.reset();
			failed = true;
			return true;
		}
		building->sequencesVersion = done->version;
		building->sequencesAddress = done->version->lease->address;
		set = std::move(building);
		return true;
	}

	namespace
	{
		bool PostSequences(std::shared_ptr<const std::vector<DrawSequence>> a_sequences, const std::shared_ptr<org::runtime::IUploadService>& a_uploads,
			std::uint64_t a_version, const std::shared_ptr<ViewBatchBuilder::Pending>& a_pending)
		{
			const auto* view = a_sequences.get();
			org::services::VersionedBufferBuildInput input;
			input.uploadOwner = a_uploads;
			input.uploadService = a_uploads.get();
			input.debugName = "cs.dclf.cpu.sequences";
			input.writeSequence = a_version;
			input.elementStride = sizeof(DrawSequence);
			input.elementCount = view->size();
			input.capacity = std::max<std::uint64_t>(input.elementCount, 1);
			input.growth = org::services::GrowthPolicy::Headroom(25, 64);
			input.flags.deviceAddress = true;
			input.backingPool = SequencesPool();
			// No predecessor: every version is written whole into a backing no frame reads (the pool's retirement), never patched in place.
			input.predecessorCopy = true;
			input.keep = a_sequences;
			input.changedSince = [](std::uint64_t) -> std::optional<std::vector<org::services::BufferByteRange>> { return std::nullopt; };
			input.emitDelta = [view](const std::function<void(const void*, std::size_t, std::uint64_t)>& a_write) {
				a_write(view->data(), view->size() * sizeof(DrawSequence), 0);
			};
			auto tag = std::make_shared<Runtime::PayloadPartTag>();
			tag->built = [](const org::services::PublishedBufferVersion&) {};
			input.tag = std::move(tag);
			return SceneScheduler::Graph().PostPayloadPart(std::move(input), [a_pending](std::shared_ptr<const org::services::PublishedBufferVersion> a_built, std::string a_error) {
				a_pending->version = std::move(a_built);
				a_pending->error = std::move(a_error);
				a_pending->done.store(true, std::memory_order_release);
				WakeSnapshotBuilder();
			});
		}
	}

	void BindViewBatches(const Direct::ViewBatchSet& a_set, std::uint64_t a_point)
	{
		if (a_set.sequencesVersion && a_point)
			a_set.sequencesVersion->lease.Bind(a_point);
	}
}
