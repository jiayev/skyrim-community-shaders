#pragma once

// The CPU pipeline's view batches' internals (ViewBatches.cpp: the full build; SceneLists.cpp: the incremental lists): a lane before its
// batch is laid out, the resident sequences' sink and rows, the list builder, the batches' layout and sort. The builder thread's alone.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"

#include <ORGModuleServices/VersionedBufferBuild.h>
#include <tbb/concurrent_queue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <ankerl/unordered_dense.h>
#include <bit>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <memory>
#include <vector>

namespace DCLF::Draws::detail
{
	using Direct::ViewType;
	constexpr std::size_t View(ViewType a_view) { return static_cast<std::size_t>(a_view); }
	constexpr std::uint32_t kNoKey = ~0u;
	constexpr std::uint32_t kIndexFormatR16 = 57;  // DXGI_FORMAT_R16_UINT

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
	/** @brief An input's runs, by view class: the main family's, the shadow casters', each occlusion map's (ListBuilder::Add). */
	struct InputRuns
	{
		Run main, caster;
		std::array<Run, kOcclusionViews> occlusion{};
	};

	/** @brief MakeViewBatches' scratch, the builder's alone (kept warm across sets). */
	struct Scratch
	{
		std::array<std::vector<Lane>, Direct::kViewTypes> lanes;
		std::vector<std::uint32_t> moverCount;
		std::size_t lastSequences = 0;  // the last set's resident sequences (the next's reserve)
	};
	inline Scratch& BuilderScratch()
	{
		static Scratch scratch;
		return scratch;
	}

	/**
	 * @brief The sets' batch storage, recycled: a set's block comes back when the set goes (any thread), and the builder takes one at
	 * least as large, its pages already touched (a fresh block faults every page at its first fill). A few blocks kept; never destroyed.
	 */
	class StoragePool
	{
	public:
		static StoragePool& Get()
		{
			static auto* pool = new StoragePool;
			return *pool;
		}
		/** @brief Builder thread: a block of at least a_bytes (not zeroed). */
		std::shared_ptr<std::byte[]> Take(std::size_t a_bytes)
		{
			Block block;
			while (free.try_pop(block)) {
				kept.fetch_sub(1, std::memory_order_relaxed);
				if (block.size >= a_bytes)
					return Wrap(block);
				delete[] block.bytes;
			}
			const std::size_t size = a_bytes + a_bytes / 4;  // room for the next sets' growth
			return Wrap({ new std::byte[size], size });
		}

	private:
		struct Block
		{
			std::byte* bytes = nullptr;
			std::size_t size = 0;
		};
		// The sets alive at once: the frames in flight's, the installed one's, the one being made.
		static constexpr int kKept = 6;
		std::shared_ptr<std::byte[]> Wrap(Block a_block)
		{
			return std::shared_ptr<std::byte[]>(a_block.bytes, [this, size = a_block.size](std::byte* a_bytes) { Give({ a_bytes, size }); });
		}
		void Give(Block a_block)
		{
			if (kept.fetch_add(1, std::memory_order_relaxed) < kKept) {
				free.push(a_block);
				return;
			}
			kept.fetch_sub(1, std::memory_order_relaxed);
			delete[] a_block.bytes;
		}
		tbb::concurrent_queue<Block> free;
		std::atomic<int> kept{ 0 };
	};

	/** @brief The resident sequences of one set, as they are made (the GPU copy and the recorder's arguments). */
	struct SequenceSink
	{
		std::vector<DrawSequence>& sequences;
		std::vector<Direct::DrawArgs>& args;
		const GeometryDrawsOut& geometries;
		// The first sequence's index: the sequences already emitted before these (SceneLists' persistent store).
		std::uint32_t base = 0;
		// Non-null: every geometry slot a walk read is appended (SceneLists' reverse map; a stream row's are not slots).
		std::vector<std::uint32_t>* walked = nullptr;

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
			Run run{ base + static_cast<std::uint32_t>(sequences.size()), 0 };
			if (mask == kNoPartitions)
				return run;
			const GeometryDraw* stream = a_input.streamIndex != ~0u ? At(a_input.streamIndex) : nullptr;
			const bool chain = (mask & kPartitionChain) != 0;
			const std::uint32_t links = chain ? (mask & kPartitionChainCount) : kMaxSkinPartitions;
			std::uint32_t slot = a_input.geometryIndex;
			for (std::uint32_t partition = 0; partition < links && slot != kNoPartition; ++partition) {
				const GeometryDraw* geometry = At(slot);
				if (walked)
					walked->push_back(slot);
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
			Run run{ base + static_cast<std::uint32_t>(sequences.size()), 0 };
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

	/**
	 * @brief A set's sequences' rows: offsets into the payload's row parts (their versions' addresses are the set's bases, pushed per
	 * list: kDirectFlagRelativeRows), and the shadow key slots' entries. The blocks SetSequenceBlocks derives are then offsets too:
	 * only the GPU path's indirect arguments read them, never a direct draw.
	 */
	struct RowBases
	{
		std::uint64_t pipelineRows = 0, materialRows = 0, shadowRows = 0;
		const std::vector<ShadowPipelineKey>* slotKeys = nullptr;

		static std::uint64_t AddressOf(const ScenePayload& a_payload, PayloadPart a_part)
		{
			const auto& version = a_payload.versions[static_cast<std::size_t>(a_part)];
			return version ? version->lease->address : 0;
		}
		/** @brief A main input's rows (BuildDrawsCS RowsOf: the installed rows; the draw resolves an animated row to the frame's copy, DCLFSequenceRows). */
		DrawSequence Main(const DrawInput& a_input) const
		{
			DrawSequence s{};
			s.pipelineRowAddress = std::uint64_t(a_input.recordIndex >> kRowPipelineShift) * kPipelineRowBytes;
			s.materialRowAddress = std::uint64_t(a_input.recordIndex & kRowMaterialMask) * kMaterialRowBytes;
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
			s.materialRowAddress = std::uint64_t(a_input.shadowRow & kRowMaterialMask) * kConstantAlignment;
			s.objectIndex = a_input.objectIndex & kObjectIndexMask;
			return s;
		}
	};

	/** @brief The lane flags an input's object flags carry. */
	inline std::uint32_t LaneFlagsOf(const DrawInput& a_input)
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
			rows(a_rows), sinkTarget(&a_sink), lanes(&a_scratch.lanes)
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

		/** @brief Where Add pushes its lanes and emits its sequences from here on. */
		void Target(std::array<std::vector<Lane>, Direct::kViewTypes>& a_lanes, SequenceSink& a_sink)
		{
			lanes = &a_lanes;
			sinkTarget = &a_sink;
		}
		/** @brief The Z groups the main list is ordered by (the catalog's targets'). */
		const std::vector<std::uint32_t>& ZGroups() const { return zGroups; }

		/**
		 * @brief a_input's lanes, and its runs: its sequences by view class. With a_reuse, those runs are its (its sequences already
		 * emitted: SceneLists re-adding an object whose sphere changed) and none are emitted.
		 */
		InputRuns Add(const DrawInput& a_input, const InputRuns* a_reuse = nullptr)
		{
			InputRuns out = a_reuse ? *a_reuse : InputRuns{};
			Run& mainRun = out.main;
			Run& casterRun = out.caster;
			auto& occlusionRuns = out.occlusion;
			auto& sink = *sinkTarget;
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
			return out;
		}

	private:
		std::uint32_t ZGroupOf(std::uint32_t a_set) const { return a_set < zGroups.size() ? zGroups[a_set] : 0xFFFFFu; }

		Lane& Push(ViewType a_view, const DrawInput& a_input, const Run& a_run)
		{
			auto& lane = (*lanes)[View(a_view)].emplace_back();
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
		SequenceSink* sinkTarget;
		std::array<std::vector<Lane>, Direct::kViewTypes>* lanes;
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
	inline void LayOut(Direct::ViewBatch& a_batch, std::byte* a_base, std::uint32_t a_lanes)
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
	inline bool SnapshotSphere(const SceneTables& a_tables, const Direct::SnapshotSpheres* a_spheres, std::uint32_t a_object, std::array<float, 4>& a_out)
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
	/** With a_extend, a_buckets and a_bucketOf already hold buckets (a base's) the lanes' keys take, new keys appended after them. */
	inline void SortList(std::vector<Lane>& a_lanes, Direct::ViewBuckets& a_buckets, ankerl::unordered_dense::map<std::uint32_t, std::uint32_t>& a_bucketOf,
		bool a_extend = false)
	{
		std::sort(a_lanes.begin(), a_lanes.end(),
			[](const Lane& a, const Lane& b) { return a.k0 != b.k0 ? a.k0 < b.k0 : a.k1 != b.k1 ? a.k1 < b.k1 : a.object < b.object; });
		if (!a_extend)
			a_bucketOf.clear();
		// Sorted lanes of one bucket key are mostly adjacent (it is, or follows, their k1): the last key's bucket is kept.
		std::uint32_t lastKey = kNoKey, lastBucket = 0;
		for (auto& lane : a_lanes) {
			if (lane.bucketKey == kNoKey) {
				lane.bucketKey = 0;  // a cull-only lane draws nothing: its bucket is never read
				continue;
			}
			if (lane.bucketKey != lastKey) {
				const auto [it, fresh] = a_bucketOf.try_emplace(lane.bucketKey, static_cast<std::uint32_t>(a_buckets.pipeline.size()));
				if (fresh) {
					a_buckets.pipeline.push_back(lane.bucketKey);
					a_buckets.pipelineSlot.push_back(lane.pipelineSlot);
				}
				lastKey = lane.bucketKey;
				lastBucket = it->second;
			}
			lane.bucketKey = lastBucket;
		}
	}

	/**
	 * @brief A set's sequences' version as the builder posts it (PostSequences): its count, its own writes [first, total) (emit: each
	 * piece's bytes at its sequence offset), built from previous (null: whole, first 0), and what previous's backing journal says
	 * changed since a backing's content epoch (changedSince; null: nothing beyond the writes).
	 */
	struct SequencesUpload
	{
		std::shared_ptr<const org::services::PublishedBufferVersion> previous;
		std::uint64_t first = 0, total = 0;
		std::function<void(const std::function<void(const void*, std::size_t, std::uint64_t)>&)> emit;
		std::function<std::optional<std::vector<org::services::BufferByteRange>>(std::uint64_t)> changedSince;
		std::shared_ptr<const void> keep;
	};

	/**
	 * @brief The incremental scene lists (SceneLists.cpp; dclf-architecture.md, "The CPU pipeline's scene lists"): the view batch sets of
	 * successive snapshots made from one base (a full build's sorted batches, shared) less its lanes gone since (each set's tombstones),
	 * plus the lanes added since (the delta) and those rebuilt every set (the volatile: the tails, stream rows, decals), with an
	 * append-only store of persistent sequences. The resident region's journal names the inputs that changed; geometry slots and snapshot
	 * spheres re-emit their objects. A compaction (the base made again) when the delta or the dead grow, or what every lane depends on
	 * changes. The builder thread's alone.
	 */
	class SceneLists
	{
	public:
		SceneLists();
		~SceneLists();
		/**
		 * @brief a_in's set (sequences in its version a_write: a_upload), from the last set's lists and the snapshot's deltas, or a
		 * compaction. Null: a_in cannot be taken (the caller makes a full set).
		 */
		std::shared_ptr<Direct::ViewBatchSet> Make(const ViewBatchInputs& a_in, const std::shared_ptr<const Direct::SnapshotSpheres>& a_spheres, std::uint64_t a_write,
			SequencesUpload& a_upload);
		/** @brief Set a_write's version is GPU ready (null: failed: the next set compacts). */
		void Posted(std::uint64_t a_write, std::shared_ptr<const org::services::PublishedBufferVersion> a_version);
		/** @brief Forgets everything: the next set compacts. */
		void Reset();
		/** @brief A set's resident sequence's bytes (the equivalence check), or null. */
		const DrawSequence* Sequence(const Direct::ViewBatchSet& a_set, std::uint32_t a_sequence) const;
		/** @brief The equivalence check's: what the lists hold of a_object (its lanes' segment, its input, its slots) against a_in. */
		std::string Explain(std::uint32_t a_object, const ViewBatchInputs& a_in) const;
		/** @brief The report window's line (sets made, compactions, ms, delta and dead), or empty; the counters start again. */
		std::string TakeReport();

		struct State;
		struct Compaction;  // a compaction on the pool (its snapshot's inputs, its State once done)

	private:
		std::unique_ptr<State> state;
		std::shared_ptr<Compaction> compaction;
	};

	/**
	 * @brief The equivalence check (Direct::kExpIncrementalCheck): a_lists' set a_incremental against a full build a_full of the same
	 * snapshot (its sequences a_fullSequences), every view's lanes as multisets (object, flags, fade, partitions, sphere, bucket's pipeline,
	 * each sequence's bytes). Returns the lanes that differ; logs the first few.
	 */
	std::size_t CompareSets(const SceneLists& a_lists, const Direct::ViewBatchSet& a_incremental, const Direct::ViewBatchSet& a_full,
		const std::vector<DrawSequence>& a_fullSequences, const ViewBatchInputs& a_in);
}
