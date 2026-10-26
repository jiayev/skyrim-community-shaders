// The CPU pipeline's incremental scene lists (ViewBatchesInternal.h, SceneLists): successive snapshots' view batch sets from one base
// (a compaction's sorted batches, shared) less its lanes gone since (each set's tombstones), plus the delta (the inputs added or changed
// since) and the volatile lanes (rebuilt every set), their persistent sequences in an append-only store.

#include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatchesInternal.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"

#include <Tracy/Tracy.hpp>

#include <chrono>
#include <compare>
#include <tuple>
#include <cstring>
#include <iterator>

namespace DCLF::Draws::detail
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		/** @brief The persistent sequences' bytes (the uploads' source and the equivalence check), in fixed chunks as ArgsChunks. */
		struct SequenceChunks
		{
			static constexpr std::uint32_t kShift = Direct::ArgsChunks::kShift, kChunk = Direct::ArgsChunks::kChunk, kChunks = Direct::ArgsChunks::kChunks;
			std::unique_ptr<std::unique_ptr<DrawSequence[]>[]> chunks = std::make_unique<std::unique_ptr<DrawSequence[]>[]>(kChunks);
			const DrawSequence& At(std::uint32_t a_sequence) const { return chunks[a_sequence >> kShift][a_sequence & (kChunk - 1)]; }
		};

		/** @brief A compaction's batches, shared by every set made on it. Immutable but for the live columns (FrameValues' movers). */
		struct BaseLists
		{
			std::shared_ptr<std::byte[]> storage;
			std::uint32_t lanes = 0;  // a batch's
			std::array<std::vector<Direct::ViewBatch>, Direct::kViewTypes> lists;
			std::array<Direct::ViewBuckets, Direct::kViewTypes> buckets;
			std::array<ankerl::unordered_dense::map<std::uint32_t, std::uint32_t>, Direct::kViewTypes> bucketOf;
			std::array<std::size_t, Direct::kViewTypes> deadFirst{};  // a view's first batch's word in a set's dead bits
			std::size_t deadWords = 0, laneCount = 0;
			std::vector<Direct::LaneRef> movers;
			// By view, by lane (batch * lanes + lane): its pipeline slot (ViewBuckets::pipelineSlot is a live lane's: SlotsOf).
			std::array<std::vector<std::uint32_t>, Direct::kViewTypes> laneSlot;
			std::array<bool, Direct::kViewTypes> slotted{};  // the view's buckets have slots (the main family's)
			std::uint32_t WordsPerBatch() const { return (lanes + 63) / 64; }
		};

		/** @brief What a set holds of the lists (ViewBatchSet::lists): its base, the persistent arguments and sequences. */
		struct SharedParts
		{
			std::shared_ptr<const BaseLists> base;
			std::shared_ptr<const Direct::ArgsChunks> args;
			std::shared_ptr<const SequenceChunks> sequences;
		};

		/** @brief Where one of an object's lanes is: a base batch's lane, or a delta lane (its index in the view's delta). */
		struct LaneAt
		{
			std::uint8_t view = 0;
			bool base = false;
			std::uint16_t lane = 0;
			std::uint32_t at = 0;  // base: the batch; delta: the index
		};

		/** @brief A persistent object's (a region entry not volatile) lanes and what they were made from. */
		struct Owned
		{
			bool present = false, inBase = false;
			bool mover = false;  // inBase: as its base lanes were laid out
			std::array<float, 4> sphere{};
			DrawInput input{};
			InputRuns runs;
			std::vector<LaneAt> lanes;
			std::vector<std::uint32_t> slots;  // the geometry slots its sequences read
		};

		/** @brief An input whose lanes are made again every set: a stream row's (its index moves with the slots), a decal's (the decal list's order). */
		bool Volatile(const DrawInput& a_input)
		{
			return a_input.streamIndex != ~0u || ObjectDecalGroup(a_input.flags) != 0;
		}

		std::uint32_t RunCount(const InputRuns& a_runs)
		{
			std::uint32_t count = a_runs.main.count + a_runs.caster.count;
			for (const auto& run : a_runs.occlusion)
				count += run.count;
			return count;
		}

		void Append(SequenceChunks& a_sequences, Direct::ArgsChunks& a_args, std::uint32_t a_at, const std::vector<DrawSequence>& a_new,
			const std::vector<Direct::DrawArgs>& a_newArgs)
		{
			for (std::size_t i = 0; i < a_new.size(); ++i) {
				const std::uint32_t at = a_at + static_cast<std::uint32_t>(i);
				auto& chunk = a_sequences.chunks[at >> SequenceChunks::kShift];
				auto& argChunk = a_args.chunks[at >> SequenceChunks::kShift];
				if (!chunk) {
					chunk = std::make_unique_for_overwrite<DrawSequence[]>(SequenceChunks::kChunk);
					argChunk = std::make_unique_for_overwrite<Direct::DrawArgs[]>(SequenceChunks::kChunk);
				}
				chunk[at & (SequenceChunks::kChunk - 1)] = a_new[i];
				argChunk[at & (SequenceChunks::kChunk - 1)] = a_newArgs[i];
			}
		}

		/** @brief a_lanes laid out into a_list's batches from a_storage (a_first: the first batch's index), the movers' refs into a_movers. */
		void LayOutLanes(const std::vector<Lane>& a_lanes, std::uint32_t a_view, std::uint32_t a_laneCount, std::byte* a_storage, std::uint32_t a_first,
			std::vector<Direct::ViewBatch>& a_list, const SceneTables& a_tables, const Direct::SnapshotSpheres* a_spheres, std::vector<Direct::LaneRef>& a_movers)
		{
			const std::size_t batchBytes = std::size_t(a_laneCount) * kLaneBytes;
			const std::size_t batches = (a_lanes.size() + a_laneCount - 1) / a_laneCount;
			for (std::size_t b = 0; b < batches; ++b) {
				auto& batch = a_list.emplace_back();
				batch.index = a_first + static_cast<std::uint32_t>(b);
				batch.count = static_cast<std::uint32_t>(std::min<std::size_t>(a_laneCount, a_lanes.size() - b * a_laneCount));
				LayOut(batch, a_storage + b * batchBytes, a_laneCount);
				for (std::uint32_t l = 0; l < batch.count; ++l) {
					const auto& lane = a_lanes[b * a_laneCount + l];
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
					if (!SnapshotSphere(a_tables, a_spheres, lane.object, sphere)) {
						flags |= Direct::kLaneMover;
						a_movers.push_back({ static_cast<std::uint16_t>(a_view), batch.index, static_cast<std::uint16_t>(l) });
					}
					batch.flags[l] = flags;
					batch.cx[l] = sphere[0];
					batch.cy[l] = sphere[1];
					batch.cz[l] = sphere[2];
					batch.r[l] = sphere[3];
					for (const auto& live : batch.liveSpheres) {
						live.cx[l] = sphere[0];
						live.cy[l] = sphere[1];
						live.cz[l] = sphere[2];
						live.r[l] = sphere[3];
					}
				}
			}
		}

		const KeptView<DrawInput>& RegionOf(const ViewBatchInputs& a_in) { return a_in.main ? a_in.main->resident : a_in.shadow->regionInputs; }
		const GeometryDrawsOut& GeometriesOf(const ViewBatchInputs& a_in) { return a_in.main ? a_in.main->body->geometryDraws : a_in.shadow->geometries; }

		/** @brief Every volatile input of the scene list in ForEachInput's order: the region's, then the tails. */
		template <class F>
		void ForEachVolatile(const ViewBatchInputs& a_in, F&& a_visit)
		{
			const auto& region = RegionOf(a_in);
			for (std::size_t i = 0, n = region.Count(); i < n; ++i)
				if (const DrawInput* input = region.At(i); Volatile(*input))
					a_visit(*input);
			if (a_in.main)
				for (const auto& input : a_in.main->body->inputList)
					a_visit(input);
			if (a_in.shadow)
				for (const auto& input : a_in.shadow->inputList)
					a_visit(input);
		}

		RowBases RowsOf(const ViewBatchInputs& a_in)
		{
			RowBases rows;
			rows.pipelineRows = RowBases::AddressOf(*a_in.payload, PayloadPart::PipelineRows);
			rows.materialRows = RowBases::AddressOf(*a_in.payload, PayloadPart::MaterialRows);
			rows.shadowRows = RowBases::AddressOf(*a_in.payload, PayloadPart::ShadowRows);
			rows.slotKeys = a_in.lookups ? &a_in.lookups->shadowSlotKeys : nullptr;
			return rows;
		}
	}

	struct SceneLists::State
	{
		// The base and the store.
		std::shared_ptr<BaseLists> base;
		std::shared_ptr<Direct::ArgsChunks> args;
		std::shared_ptr<SequenceChunks> sequences;
		std::uint32_t persistent = 0;      // sequences in the store
		std::uint32_t deadSequences = 0;   // ... no live lane draws
		std::vector<std::uint64_t> deadBits;
		std::size_t deadLanes = 0;
		// The delta: by view, the lanes added since the base (live or gone).
		std::array<std::vector<Lane>, Direct::kViewTypes> delta;
		std::array<std::vector<std::uint8_t>, Direct::kViewTypes> deltaLive;
		std::size_t deltaLanes = 0;  // live
		// By object slot.
		std::vector<Owned> owned;
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> slotObjects;
		// What the last set was made from.
		KeptView<DrawInput> region;
		bool regionMain = false;
		std::uint64_t geometryEpoch = 0;
		std::vector<std::uint32_t> zGroups;
		std::vector<ShadowPipelineKey> slotKeys;
		std::uint32_t lanes = 0;
		// The sequences' versions: the last GPU ready (its persistent count), and what each version since wrote.
		std::shared_ptr<const org::services::PublishedBufferVersion> posted;
		std::uint32_t postedPersistent = 0;
		struct Written
		{
			std::uint64_t write = 0;
			std::uint32_t persistent = 0, total = 0;
		};
		std::vector<Written> written;
		// Scratch, kept warm.
		std::array<std::vector<Lane>, Direct::kViewTypes> lanesScratch, volatileScratch;
		std::vector<DrawSequence> newSequences, volatileSequences;
		std::vector<Direct::DrawArgs> newArgs;
		std::vector<std::uint32_t> walked, touched, dirty;
		std::vector<std::uint8_t> touchedMark, dirtyMark;
		ankerl::unordered_dense::map<std::uint32_t, const DrawInput*> entryOf;
		// The report window's.
		struct Window
		{
			std::uint32_t sets = 0, compactions = 0, asyncCompactions = 0, reemitted = 0, resphered = 0;
			double setMs = 0.0, compactMs = 0.0, asyncMs = 0.0;
			std::uint64_t uploaded = 0;
		} window;

		bool Stale(const ViewBatchInputs& a_in, const ListBuilder& a_builder) const;
		void Compact(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, ListBuilder& a_builder, SequenceSink& a_sink);
		bool Update(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, ListBuilder& a_builder, SequenceSink& a_sink);
		void Kill(std::uint32_t a_object, bool a_keepSequences);
		void Emit(std::uint32_t a_object, const DrawInput& a_input, const InputRuns* a_reuse, ListBuilder& a_builder);
		std::shared_ptr<Direct::ViewBatchSet> Assemble(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, ListBuilder& a_builder,
			SequenceSink& a_volatileSink);
		Owned& OwnedOf(std::uint32_t a_object)
		{
			if (owned.size() <= a_object)
				owned.resize(std::size_t(a_object) + 1);
			return owned[a_object];
		}
		bool Touch(std::uint32_t a_object)
		{
			if (touchedMark.size() <= a_object)
				touchedMark.resize(std::size_t(a_object) + 1, 0);
			if (touchedMark[a_object])
				return false;
			touchedMark[a_object] = 1;
			touched.push_back(a_object);
			return true;
		}
	};

	struct SceneLists::Compaction
	{
		ViewBatchInputs in;  // its snapshot's (held by in.keep)
		std::shared_ptr<const Direct::SnapshotSpheres> spheres;
		std::unique_ptr<State> state;  // set before done
		std::atomic<bool> done{ false };
	};

	SceneLists::SceneLists() : state(std::make_unique<State>()) {}
	SceneLists::~SceneLists() = default;

	void SceneLists::Reset()
	{
		auto window = state->window;
		state = std::make_unique<State>();
		state->window = window;
		compaction.reset();  // one in flight finishes into its own
	}

	bool SceneLists::State::Stale(const ViewBatchInputs& a_in, const ListBuilder& a_builder) const
	{
		if (!base || lanes != BatchLanes() || regionMain != (a_in.main != nullptr) || zGroups != a_builder.ZGroups())
			return true;
		// A key slot's entry the base's shadow sequences embed changed (appended ones are new keys: no lane has them yet).
		const auto* keys = a_in.lookups ? &a_in.lookups->shadowSlotKeys : nullptr;
		const std::size_t now = keys ? keys->size() : 0;
		if (now < slotKeys.size())
			return true;
		for (std::size_t k = 0; k < slotKeys.size(); ++k)
			if (!((*keys)[k] == slotKeys[k]))
				return true;
		// The store's room (a compaction starts it again).
		return std::size_t(persistent) + RegionOf(a_in).Count() * 8 > std::size_t(Direct::ArgsChunks::kChunk) * Direct::ArgsChunks::kChunks;
	}

	void SceneLists::State::Compact(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, ListBuilder& a_builder, SequenceSink& a_sink)
	{
		ZoneScopedN("CS.DCLF.SceneLists.Compact");
		const auto start = Clock::now();
		base = std::make_shared<BaseLists>();
		args = std::make_shared<Direct::ArgsChunks>();
		sequences = std::make_shared<SequenceChunks>();
		persistent = 0;
		deadSequences = 0;
		deadLanes = 0;
		deltaLanes = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			delta[v].clear();
			deltaLive[v].clear();
		}
		owned.clear();
		slotObjects.clear();
		posted.reset();
		postedPersistent = 0;
		written.clear();
		// Every persistent input's lanes and sequences.
		for (auto& list : lanesScratch)
			list.clear();
		newSequences.clear();
		newArgs.clear();
		a_sink.base = 0;
		a_sink.walked = &walked;
		a_builder.Target(lanesScratch, a_sink);
		const auto& view = RegionOf(a_in);
		for (std::size_t i = 0, n = view.Count(); i < n; ++i) {
			const DrawInput& input = *view.At(i);
			if (Volatile(input))
				continue;
			const std::uint32_t object = input.objectIndex & kObjectIndexMask;
			auto& own = OwnedOf(object);
			if (own.present)
				continue;  // a second entry of one object (the region holds one): not this one
			walked.clear();
			own.runs = a_builder.Add(input);
			own.present = own.inBase = true;
			own.input = input;
			own.slots = walked;
			for (const std::uint32_t slot : own.slots)
				slotObjects[slot].push_back(object);
		}
		Append(*sequences, *args, 0, newSequences, newArgs);
		persistent = static_cast<std::uint32_t>(newSequences.size());
		// Sorted, bucketed, laid out: each object's lanes noted where they landed.
		auto& b = *base;
		b.lanes = BatchLanes();
		std::size_t batches = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			SortList(lanesScratch[v], b.buckets[v], b.bucketOf[v]);
			batches += (lanesScratch[v].size() + b.lanes - 1) / b.lanes;
			b.laneCount += lanesScratch[v].size();
		}
		const std::size_t batchBytes = std::size_t(b.lanes) * kLaneBytes;
		b.storage = StoragePool::Get().Take(std::max<std::size_t>(batches, 1) * batchBytes);
		std::size_t next = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			b.slotted[v] = std::any_of(b.buckets[v].pipelineSlot.begin(), b.buckets[v].pipelineSlot.end(), [](std::uint32_t a_slot) { return a_slot != ~0u; });
			if (b.slotted[v]) {
				b.laneSlot[v].resize(lanesScratch[v].size());
				for (std::size_t k = 0; k < lanesScratch[v].size(); ++k)
					b.laneSlot[v][k] = lanesScratch[v][k].pipelineSlot;
			}
			b.deadFirst[v] = next * b.WordsPerBatch();
			LayOutLanes(lanesScratch[v], static_cast<std::uint32_t>(v), b.lanes, b.storage.get() + next * batchBytes, 0, b.lists[v], *a_in.tables, a_spheres, b.movers);
			next += b.lists[v].size();
			for (const auto& batch : b.lists[v])
				for (std::uint32_t l = 0; l < batch.count; ++l)
					owned[batch.object[l]].lanes.push_back({ static_cast<std::uint8_t>(v), true, static_cast<std::uint16_t>(l), batch.index });
		}
		b.deadWords = next * b.WordsPerBatch();
		deadBits.assign(b.deadWords, 0);
		// Each object's sphere as its base lanes have it.
		for (std::uint32_t o = 0; o < owned.size(); ++o)
			if (auto& own = owned[o]; own.present) {
				std::array<float, 4> sphere{};
				own.mover = !SnapshotSphere(*a_in.tables, a_spheres, o, sphere);
				own.sphere = own.mover ? std::array<float, 4>{} : sphere;
			}
		region = view;
		regionMain = a_in.main != nullptr;
		geometryEpoch = GeometriesOf(a_in).ContentEpoch();
		zGroups = a_builder.ZGroups();
		slotKeys = a_in.lookups ? a_in.lookups->shadowSlotKeys : std::vector<ShadowPipelineKey>{};
		lanes = b.lanes;
		++window.compactions;
		window.compactMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	void SceneLists::State::Kill(std::uint32_t a_object, bool a_keepSequences)
	{
		auto& own = owned[a_object];
		for (const auto& at : own.lanes) {
			if (at.base) {
				const std::size_t bit = (base->deadFirst[at.view] + std::size_t(at.at) * base->WordsPerBatch()) * 64 + at.lane;
				deadBits[bit >> 6] |= std::uint64_t{ 1 } << (bit & 63);
				++deadLanes;
			} else if (deltaLive[at.view][at.at]) {
				deltaLive[at.view][at.at] = 0;
				--deltaLanes;
			}
		}
		own.lanes.clear();
		own.inBase = false;
		if (a_keepSequences)
			return;
		deadSequences += RunCount(own.runs);
		for (const std::uint32_t slot : own.slots)
			if (const auto it = slotObjects.find(slot); it != slotObjects.end())
				std::erase(it->second, a_object);
		own.slots.clear();
		own.present = false;
	}

	void SceneLists::State::Emit(std::uint32_t a_object, const DrawInput& a_input, const InputRuns* a_reuse, ListBuilder& a_builder)
	{
		auto& own = OwnedOf(a_object);
		std::array<std::size_t, Direct::kViewTypes> before{};
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v)
			before[v] = delta[v].size();
		walked.clear();
		const InputRuns runs = a_builder.Add(a_input, a_reuse);
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v)
			for (std::size_t k = before[v]; k < delta[v].size(); ++k) {
				own.lanes.push_back({ static_cast<std::uint8_t>(v), false, 0, static_cast<std::uint32_t>(k) });
				deltaLive[v].push_back(1);
				++deltaLanes;
			}
		own.present = true;
		own.inBase = false;
		own.input = a_input;
		if (!a_reuse) {
			own.runs = runs;
			own.slots = walked;
			for (const std::uint32_t slot : own.slots)
				slotObjects[slot].push_back(a_object);
		}
	}

	bool SceneLists::State::Update(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, ListBuilder& a_builder, SequenceSink& a_sink)
	{
		ZoneScopedN("CS.DCLF.SceneLists.Update");
		const auto& view = RegionOf(a_in);
		const std::size_t oldCount = region.Count(), newCount = view.Count();
		// The region's changed entries: an index whose bytes changed (an entry written, or one moved into a removal's hole) had the old
		// entry and has the new; past the new count, the old entries are gone.
		entryOf.clear();
		touched.clear();
		std::vector<std::uint32_t> candidates;
		const std::uint64_t changed = view.changes.ForEachRun(region.ContentEpoch(), newCount, [&](std::uint64_t a_first, std::uint64_t a_count) {
			for (std::uint64_t i = a_first; i < a_first + a_count; ++i) {
				if (i < oldCount)
					candidates.push_back(region.At(i)->objectIndex & kObjectIndexMask);
				const DrawInput* input = view.At(i);
				const std::uint32_t object = input->objectIndex & kObjectIndexMask;
				entryOf[object] = input;
				candidates.push_back(object);
			}
		});
		if (newCount && changed * 2 > newCount)
			return false;  // most of it (or the journal's floor passed): a compaction
		for (std::size_t i = newCount; i < oldCount; ++i)
			candidates.push_back(region.At(i)->objectIndex & kObjectIndexMask);
		// Geometry slots rewritten: their objects' sequences again.
		const auto& geometries = GeometriesOf(a_in);
		std::vector<std::uint32_t> slots;
		const std::uint64_t slotsChanged = geometries.slots.changes.ForEachRun(geometryEpoch, geometries.SlotCount(), [&](std::uint64_t a_first, std::uint64_t a_count) {
			for (std::uint64_t s = a_first; s < a_first + a_count; ++s)
				slots.push_back(static_cast<std::uint32_t>(s));
		});
		if (geometries.SlotCount() && slotsChanged * 2 > geometries.SlotCount())
			return false;
		// Persistent sequences emitted from here are appended to the store at its count.
		newSequences.clear();
		newArgs.clear();
		a_sink.base = persistent;
		a_sink.walked = &walked;
		a_builder.Target(delta, a_sink);
		// The objects whose sequences read a rewritten slot: never left as they are, whatever the region did with their entry.
		dirty.clear();
		for (const std::uint32_t slot : slots)
			if (const auto it = slotObjects.find(slot); it != slotObjects.end())
				for (const std::uint32_t object : it->second) {
					if (dirtyMark.size() <= object)
						dirtyMark.resize(std::size_t(object) + 1, 0);
					if (!std::exchange(dirtyMark[object], std::uint8_t{ 1 }))
						dirty.push_back(object);
				}
		for (const std::uint32_t object : candidates) {
			if (!Touch(object))
				continue;
			const auto found = entryOf.find(object);
			const DrawInput* input = found != entryOf.end() ? found->second : nullptr;
			const bool owns = object < owned.size() && owned[object].present;
			const bool rewritten = object < dirtyMark.size() && dirtyMark[object];
			if (input && owns && !rewritten && !Volatile(*input) && std::memcmp(&owned[object].input, input, sizeof(DrawInput)) == 0)
				continue;  // moved within the region, unchanged
			if (owns)
				Kill(object, false);
			if (input && !Volatile(*input)) {
				Emit(object, *input, nullptr, a_builder);
				++window.reemitted;
			}
		}
		// The rewritten slots' objects the region left alone: again from their own input.
		for (const std::uint32_t object : dirty) {
			dirtyMark[object] = 0;
			if (!Touch(object) || !owned[object].present)
				continue;
			const DrawInput input = owned[object].input;
			Kill(object, false);
			Emit(object, input, nullptr, a_builder);
			++window.reemitted;
		}
		// A base lane's sphere is the base's: an object whose snapshot sphere or mover state changed has its lanes laid out again (its
		// sequences kept). The delta's are laid out every set.
		for (std::uint32_t o = 0; o < owned.size(); ++o) {
			auto& own = owned[o];
			if (!own.present || !own.inBase)
				continue;
			std::array<float, 4> sphere{};
			const bool mover = !SnapshotSphere(*a_in.tables, a_spheres, o, sphere);
			if (mover == own.mover && (mover || std::memcmp(sphere.data(), own.sphere.data(), sizeof(sphere)) == 0))
				continue;
			const InputRuns runs = own.runs;
			const DrawInput input = own.input;
			Kill(o, true);
			Emit(o, input, &runs, a_builder);
			++window.resphered;
		}
		for (const std::uint32_t object : touched)
			touchedMark[object] = 0;
		Append(*sequences, *args, persistent, newSequences, newArgs);
		persistent += static_cast<std::uint32_t>(newSequences.size());
		region = view;
		geometryEpoch = geometries.ContentEpoch();
		if (a_in.lookups)
			slotKeys = a_in.lookups->shadowSlotKeys;
		return true;
	}

	std::shared_ptr<Direct::ViewBatchSet> SceneLists::State::Assemble(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres, ListBuilder& a_builder,
		SequenceSink& a_volatileSink)
	{
		ZoneScopedN("CS.DCLF.SceneLists.Assemble");
		const auto& b = *base;
		auto set = std::make_shared<Direct::ViewBatchSet>();
		set->lanes = b.lanes;
		set->keep = a_in.keep;
		const auto& geometries = GeometriesOf(a_in);
		set->geometryIndexAddress.assign(geometries.SlotCount(), 0);
		for (std::size_t g = 0; g < set->geometryIndexAddress.size(); ++g)
			if (const GeometryDraw* geometry = geometries.slots.At(static_cast<std::uint32_t>(g)))
				set->geometryIndexAddress[g] = geometry->indexBufferAddress;
		const RowBases rows = RowsOf(a_in);
		set->pipelineRowsBase = rows.pipelineRows;
		set->materialRowsBase = rows.materialRows;
		set->shadowRowsBase = rows.shadowRows;
		// The volatile lanes (their sequences after the store's), then each view's fresh lanes: the delta's live and the volatile.
		for (auto& list : volatileScratch)
			list.clear();
		volatileSequences.clear();
		set->sequenceArgs.clear();
		a_volatileSink.base = persistent;
		a_volatileSink.walked = nullptr;
		a_builder.Target(volatileScratch, a_volatileSink);
		ForEachVolatile(a_in, [&](const DrawInput& a_input) { a_builder.Add(a_input); });
		set->persistentArgs = args;
		set->persistentSequences = persistent;
		set->sequenceCount = persistent + static_cast<std::uint32_t>(volatileSequences.size());
		std::size_t freshBatches = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			auto& fresh = lanesScratch[v];
			fresh.clear();
			for (std::size_t k = 0; k < delta[v].size(); ++k)
				if (deltaLive[v][k])
					fresh.push_back(delta[v][k]);
			fresh.insert(fresh.end(), volatileScratch[v].begin(), volatileScratch[v].end());
			freshBatches += (fresh.size() + b.lanes - 1) / b.lanes;
		}
		const std::size_t batchBytes = std::size_t(b.lanes) * kLaneBytes;
		set->storage = StoragePool::Get().Take(std::max<std::size_t>(freshBatches, 1) * batchBytes);
		set->deadBits = deadBits;
		std::size_t next = 0;
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> bucketOf;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			auto& list = set->lists[v];
			list = b.lists[v];
			for (std::size_t k = 0; k < list.size(); ++k)
				list[k].dead = set->deadBits.data() + b.deadFirst[v] + k * b.WordsPerBatch();
			set->buckets[v] = b.buckets[v];
			bucketOf = b.bucketOf[v];
			SortList(lanesScratch[v], set->buckets[v], bucketOf, true);
			const std::size_t before = list.size();
			LayOutLanes(lanesScratch[v], static_cast<std::uint32_t>(v), b.lanes, set->storage.get() + next * batchBytes, static_cast<std::uint32_t>(before), list,
				*a_in.tables, a_spheres, set->moverLanes);
			next += list.size() - before;
			// A bucket's pipeline slot: its live lanes' first in the full build's order (the lowest object: one key, one k0 and k1), as
			// MakeViewBatches takes it; the base's own may be gone, and its slot another pipeline's since.
			auto& buckets = set->buckets[v];
			// Without base lanes (the decals': all volatile) the fresh lanes' sort took it as the full build's sort does.
			if (before && b.slotted[v]) {
				std::vector<std::uint32_t> best(buckets.pipeline.size(), ~0u);
				const auto take = [&](std::uint32_t a_bucket, std::uint32_t a_object, std::uint32_t a_flags, std::uint32_t a_slot) {
					if ((a_flags & Direct::kLaneCullOnly) || a_bucket >= best.size() || a_object >= best[a_bucket])
						return;
					best[a_bucket] = a_object;
					buckets.pipelineSlot[a_bucket] = a_slot;
				};
				for (std::size_t k = 0; k < before; ++k) {
					const auto& batch = list[k];
					for (std::uint32_t l = 0; l < batch.count; ++l)
						if (!((batch.dead[l >> 6] >> (l & 63)) & 1u))
							take(batch.pipelineBucket[l], batch.object[l], batch.flags[l], b.laneSlot[v][k * b.lanes + l]);
				}
				for (const auto& lane : lanesScratch[v])
					take(lane.bucketKey, lane.object, lane.flags, lane.pipelineSlot);
			}
		}
		// The base's movers still live in this set.
		for (const auto& ref : b.movers) {
			const std::size_t bit = (b.deadFirst[ref.view] + std::size_t(ref.batch) * b.WordsPerBatch()) * 64 + ref.lane;
			if (!((deadBits[bit >> 6] >> (bit & 63)) & 1u))
				set->moverLanes.push_back(ref);
		}
		// The decal list's groups by main range slot (its lanes are all fresh: decals are volatile).
		const auto& decals = lanesScratch[View(ViewType::Decal)];
		if (!decals.empty()) {
			auto& groupFirst = set->decalGroupFirst;
			groupFirst.assign(Extensions::kMainRangeSlots + 1, 0u);
			for (const auto& lane : decals)
				if (lane.decalGroup >= 1 && lane.decalGroup <= kDecalGroups)
					++groupFirst[lane.decalGroup];
			for (std::size_t g = 1; g < groupFirst.size(); ++g)
				groupFirst[g] += groupFirst[g - 1];
		}
		set->liveFilled[0].store(true, std::memory_order_relaxed);
		set->liveFilled[1].store(true, std::memory_order_relaxed);
		auto parts = std::make_shared<SharedParts>();
		parts->base = base;
		parts->args = args;
		parts->sequences = sequences;
		set->shared = std::move(parts);
		return set;
	}

	std::shared_ptr<Direct::ViewBatchSet> SceneLists::Make(const ViewBatchInputs& a_in, const std::shared_ptr<const Direct::SnapshotSpheres>& a_spheres, std::uint64_t a_write,
		SequencesUpload& a_upload)
	{
		ZoneScopedN("CS.DCLF.SceneLists");
		if (!a_in.tables || !a_in.payload || (!a_in.main && !a_in.shadow))
			return nullptr;
		const auto start = Clock::now();
		// A compaction done on the pool: its lists replace these (the window's counters kept), and this set's Update catches them up from
		// its snapshot (the journals' positions it was made at) to this one.
		if (compaction && compaction->done.load(std::memory_order_acquire)) {
			auto window = state->window;
			window.asyncCompactions += compaction->state->window.compactions;
			window.asyncMs += compaction->state->window.compactMs;
			state = std::move(compaction->state);
			state->window = window;
			compaction.reset();
		}
		auto& s = *state;
		const RowBases rows = RowsOf(a_in);
		const auto& geometries = GeometriesOf(a_in);
		SequenceSink sink{ s.newSequences, s.newArgs, geometries };
		ListBuilder builder(a_in, rows, sink, BuilderScratch());
		// What every lane depends on changed, or the journals no longer reach the last set: compacted here, before this set.
		if (s.Stale(a_in, builder) || !s.Update(a_in, a_spheres.get(), builder, sink)) {
			compaction.reset();
			s.Compact(a_in, a_spheres.get(), builder, sink);
		} else if (!compaction &&
				   (s.deltaLanes > std::max<std::size_t>(4096, s.base->laneCount / 4) || s.deadLanes > s.base->laneCount / 4 || s.deadSequences > s.persistent / 2)) {
			// Too much of the base gone, or too much added: compacted from this snapshot on the pool, while the sets go on from these lists.
			auto job = std::make_shared<Compaction>();
			job->in = a_in;
			job->spheres = a_spheres;
			const bool dispatched = SceneScheduler::Executor().Dispatch(SceneScheduler::Scope(), SceneExecutor::Preparation, org::async::TaskDispatch::Cpu,
				"dclf scene lists compaction", [job](const auto&) {
					auto made = std::make_unique<State>();
					Scratch scratch;
					const RowBases jobRows = RowsOf(job->in);
					SequenceSink jobSink{ made->newSequences, made->newArgs, GeometriesOf(job->in) };
					ListBuilder jobBuilder(job->in, jobRows, jobSink, scratch);
					made->Compact(job->in, job->spheres.get(), jobBuilder, jobSink);
					job->state = std::move(made);
					job->done.store(true, std::memory_order_release);
				});
			if (dispatched)
				compaction = std::move(job);
		}
		std::vector<Direct::DrawArgs> volatileArgs;
		SequenceSink volatileSink{ s.volatileSequences, volatileArgs, geometries };
		auto set = s.Assemble(a_in, a_spheres.get(), builder, volatileSink);
		set->sequenceArgs = std::move(volatileArgs);
		// The version: this set's writes are the store's appends since the last ready version and its volatile sequences.
		const std::uint32_t total = set->sequenceCount;
		const std::uint32_t first = s.posted ? std::min(s.postedPersistent, s.persistent) : 0u;
		auto volatileCopy = std::make_shared<const std::vector<DrawSequence>>(s.volatileSequences);
		a_upload.previous = s.posted;
		a_upload.first = first;
		a_upload.total = total;
		auto chunks = s.sequences;
		const std::uint32_t persistent = s.persistent;
		a_upload.emit = [chunks, volatileCopy, first, persistent](const std::function<void(const void*, std::size_t, std::uint64_t)>& a_write) {
			for (std::uint32_t at = first; at < persistent;) {
				const std::uint32_t end = std::min(persistent, (at | (SequenceChunks::kChunk - 1)) + 1);
				a_write(&chunks->At(at), std::size_t(end - at) * sizeof(DrawSequence), std::uint64_t(at) * sizeof(DrawSequence));
				at = end;
			}
			if (!volatileCopy->empty())
				a_write(volatileCopy->data(), volatileCopy->size() * sizeof(DrawSequence), std::uint64_t(persistent) * sizeof(DrawSequence));
		};
		// A backing at a version's content epoch agrees with the last ready one below both's persistent counts (the store is
		// append-only); past them, both versions' bytes may differ, as far as either's end.
		std::optional<State::Written> previous;
		for (const auto& w : s.written)
			if (s.posted && w.write == s.posted->writeSequence)
				previous = w;
		auto written = s.written;
		a_upload.changedSince = [written, previous](std::uint64_t a_from) -> std::optional<std::vector<org::services::BufferByteRange>> {
			if (!previous)
				return std::nullopt;
			for (const auto& w : written)
				if (w.write == a_from) {
					const std::uint64_t low = std::min(w.persistent, previous->persistent), high = std::max(w.total, previous->total);
					if (high <= low)
						return std::vector<org::services::BufferByteRange>{};
					return std::vector<org::services::BufferByteRange>{ { low * sizeof(DrawSequence), (high - low) * sizeof(DrawSequence) } };
				}
			return std::nullopt;
		};
		a_upload.keep = std::make_shared<std::pair<std::shared_ptr<const SequenceChunks>, std::shared_ptr<const std::vector<DrawSequence>>>>(chunks, volatileCopy);
		s.written.push_back({ a_write, s.persistent, total });
		if (s.written.size() > 64)
			s.written.erase(s.written.begin());
		++s.window.sets;
		s.window.uploaded += std::uint64_t(total - first) * sizeof(DrawSequence);
		s.window.setMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		return set;
	}

	void SceneLists::Posted(std::uint64_t a_write, std::shared_ptr<const org::services::PublishedBufferVersion> a_version)
	{
		auto& s = *state;
		if (!a_version) {
			Reset();
			return;
		}
		for (const auto& w : s.written)
			if (w.write == a_write && (!s.posted || a_write > s.posted->writeSequence)) {
				s.posted = std::move(a_version);
				s.postedPersistent = w.persistent;
				return;
			}
	}

	const DrawSequence* SceneLists::Sequence(const Direct::ViewBatchSet& a_set, std::uint32_t a_sequence) const
	{
		const auto* parts = static_cast<const SharedParts*>(a_set.shared.get());
		if (a_sequence < a_set.persistentSequences)
			return parts ? &parts->sequences->At(a_sequence) : nullptr;
		a_sequence -= a_set.persistentSequences;
		return a_sequence < state->volatileSequences.size() ? &state->volatileSequences[a_sequence] : nullptr;
	}

	std::string SceneLists::Explain(std::uint32_t a_object, const ViewBatchInputs& a_in) const
	{
		const auto& s = *state;
		std::string text;
		if (a_object < s.owned.size() && s.owned[a_object].present) {
			const auto& own = s.owned[a_object];
			std::uint32_t base = 0, delta = 0;
			for (const auto& at : own.lanes)
				(at.base ? base : delta) += 1;
			text = fmt::format("owned ({} base lanes, {} delta; in base {}), input geometry {} partitions {:x} lod {:x} record {:x} stream {:x}, slots [", base, delta, own.inBase,
				own.input.geometryIndex, own.input.partitions, own.input.lodPartitions, own.input.recordIndex, own.input.streamIndex);
			for (const std::uint32_t slot : own.slots)
				text += fmt::format("{} ", slot);
			text += "]";
		} else {
			text = "not owned";
		}
		const auto& region = RegionOf(a_in);
		for (std::size_t i = 0; i < region.Count(); ++i)
			if (const DrawInput* input = region.At(i); (input->objectIndex & kObjectIndexMask) == a_object)
				text += fmt::format("; region entry {}: geometry {} partitions {:x} lod {:x} record {:x} stream {:x} volatile {}", i, input->geometryIndex, input->partitions,
					input->lodPartitions, input->recordIndex, input->streamIndex, Volatile(*input));
		const auto& geometries = GeometriesOf(a_in);
		if (a_object < s.owned.size())
			for (const std::uint32_t slot : s.owned[a_object].slots)
				if (const GeometryDraw* g = geometries.slots.At(slot))
					text += fmt::format("; slot {} now: ib {:x} count {} first {} next {}", slot, g->indexBufferAddress, g->indexCount, g->firstIndex, g->nextPartition);
		text += fmt::format("; geometry epoch held {} now {}", s.geometryEpoch, geometries.ContentEpoch());
		return text;
	}

	std::string SceneLists::TakeReport()
	{
		auto& s = *state;
		const auto w = std::exchange(s.window, {});
		if (!w.sets)
			return {};
		return fmt::format("[DCLF] scene lists: {} sets ({:.2f} ms each, compactions on the builder included), {} compactions on the builder ({:.1f} ms each), {} on the "
						   "pool ({:.1f} ms each), {:.1f} objects re-emitted and {:.1f} re-laid (sphere) a set; base {} lanes ({} dead), delta {} lanes, {} persistent "
						   "sequences ({} dead); {:.1f} KB uploaded a set",
			w.sets, w.setMs / w.sets, w.compactions, w.compactions ? w.compactMs / w.compactions : 0.0, w.asyncCompactions,
			w.asyncCompactions ? w.asyncMs / w.asyncCompactions : 0.0, double(w.reemitted) / w.sets, double(w.resphered) / w.sets,
			s.base ? s.base->laneCount : 0, s.deadLanes, s.deltaLanes, s.persistent, s.deadSequences, double(w.uploaded) / w.sets / 1024.0);
	}

	namespace
	{
		/** @brief One lane as the equivalence check compares it (sorted, then compared in order). */
		struct LaneKey
		{
			std::uint32_t object, flags, fadeRoot, fadeDistance, partitions, lodPartitions, pipeline, slot, count;
			std::array<std::uint32_t, 4> sphere;
			std::uint64_t sequences;  // a hash of its sequences' bytes
			std::uint32_t firstSequence;  // not compared: for the log
			auto Tied() const { return std::tie(object, flags, fadeRoot, fadeDistance, partitions, lodPartitions, pipeline, slot, count, sphere, sequences); }
			bool operator<(const LaneKey& a_other) const { return Tied() < a_other.Tied(); }
		};

		template <class SequenceOf>
		void KeysOf(const Direct::ViewBatchSet& a_set, std::size_t a_view, SequenceOf&& a_sequence, std::vector<LaneKey>& a_keys)
		{
			a_keys.clear();
			const auto& buckets = a_set.buckets[a_view];
			for (const auto& batch : a_set.lists[a_view])
				for (std::uint32_t l = 0; l < batch.count; ++l) {
					if (batch.dead && ((batch.dead[l >> 6] >> (l & 63)) & 1u))
						continue;
					LaneKey key{};
					key.object = batch.object[l];
					key.flags = batch.flags[l];
					key.fadeRoot = batch.fadeRoot[l];
					key.fadeDistance = std::bit_cast<std::uint32_t>(batch.fadeDistance[l]);
					key.partitions = batch.partitions[l];
					key.lodPartitions = batch.lodPartitions[l];
					const std::uint32_t bucket = batch.pipelineBucket[l];
					const bool cullOnly = (batch.flags[l] & Direct::kLaneCullOnly) != 0;
					key.pipeline = !cullOnly && bucket < buckets.pipeline.size() ? buckets.pipeline[bucket] : ~0u;
					key.slot = !cullOnly && bucket < buckets.pipelineSlot.size() ? buckets.pipelineSlot[bucket] : ~0u;
					key.count = batch.sequenceCount[l];
					key.sphere = { std::bit_cast<std::uint32_t>(batch.cx[l]), std::bit_cast<std::uint32_t>(batch.cy[l]), std::bit_cast<std::uint32_t>(batch.cz[l]),
						std::bit_cast<std::uint32_t>(batch.r[l]) };
					std::uint64_t hash = 14695981039346656037ull;
					for (std::uint32_t q = 0; q < key.count; ++q) {
						const DrawSequence* sequence = a_sequence(batch.sequence[l] + q);
						const auto* bytes = reinterpret_cast<const unsigned char*>(sequence);
						for (std::size_t i = 0; sequence && i < sizeof(DrawSequence); ++i)
							hash = (hash ^ bytes[i]) * 1099511628211ull;
						hash = (hash ^ (sequence ? 1u : 0u)) * 1099511628211ull;
					}
					key.sequences = hash;
					key.firstSequence = batch.sequence[l];
					a_keys.push_back(key);
				}
			std::sort(a_keys.begin(), a_keys.end());
		}
	}

	std::size_t CompareSets(const SceneLists& a_lists, const Direct::ViewBatchSet& a_incremental, const Direct::ViewBatchSet& a_full,
		const std::vector<DrawSequence>& a_fullSequences, const ViewBatchInputs& a_in)
	{
		ZoneScopedN("CS.DCLF.SceneLists.Check");
		static std::atomic<std::uint32_t> logged{ 0 };
		std::vector<LaneKey> mine, theirs;
		std::size_t differ = 0;
		for (std::size_t v = 0; v < Direct::kViewTypes; ++v) {
			KeysOf(a_incremental, v, [&](std::uint32_t a_sequence) { return a_lists.Sequence(a_incremental, a_sequence); }, mine);
			KeysOf(a_full, v, [&](std::uint32_t a_sequence) { return a_sequence < a_fullSequences.size() ? &a_fullSequences[a_sequence] : nullptr; }, theirs);
			std::vector<LaneKey> onlyMine, onlyTheirs;
			std::set_difference(mine.begin(), mine.end(), theirs.begin(), theirs.end(), std::back_inserter(onlyMine));
			std::set_difference(theirs.begin(), theirs.end(), mine.begin(), mine.end(), std::back_inserter(onlyTheirs));
			differ += onlyMine.size() + onlyTheirs.size();
			if ((!onlyMine.empty() || !onlyTheirs.empty()) && logged.fetch_add(1) < 8) {
				const auto describe = [](const std::vector<LaneKey>& a_keys, auto&& a_sequence) {
					std::string text;
					for (std::size_t i = 0; i < std::min<std::size_t>(a_keys.size(), 3); ++i) {
						const auto& k = a_keys[i];
						text += fmt::format("{}{{object {} flags {:x} pipeline {} slot {} sequences {} ({:x}) sphere r {}", text.empty() ? "" : ", ", k.object, k.flags,
							k.pipeline, k.slot, k.count, k.sequences, std::bit_cast<float>(k.sphere[3]));
						if (const DrawSequence* q = k.count ? a_sequence(k.firstSequence) : nullptr)
							text += fmt::format(" [{}: pipeline {:x} rows {:x} {:x} object {:x} vb {:x}/{}/{} stream {:x}/{}/{} ib {:x}/{} count {} first {}]", k.firstSequence,
								q->pipelineIndex, q->pipelineRowAddress, q->materialRowAddress, q->objectIndex, q->vertexBufferAddress, q->vertexBufferSize, q->vertexStride,
								q->streamBufferAddress, q->streamBufferSize, q->streamStride, q->indexBufferAddress, q->indexBufferSize, q->indexCount, q->firstIndex);
						text += "}";
					}
					return text;
				};
				logger::warn("[DCLF] scene lists check: view {}: {} lanes only incremental ({}), {} only the full build's ({}) <- SCENE LISTS DIFFER", CpuViewName(v),
					onlyMine.size(), describe(onlyMine, [&](std::uint32_t a_q) { return a_lists.Sequence(a_incremental, a_q); }), onlyTheirs.size(),
					describe(onlyTheirs, [&](std::uint32_t a_q) { return a_q < a_fullSequences.size() ? &a_fullSequences[a_q] : nullptr; }));
				if (!onlyMine.empty())
					logger::warn("[DCLF] scene lists check: object {}: {}", onlyMine.front().object, a_lists.Explain(onlyMine.front().object, a_in));
			}
		}
		return differ;
	}
}
