#include "FaceSnapshots.h"

#include "Switches.h"
#include "VolumetricProbe.h"

#include <array>
#include <cstring>
#include <vector>

namespace DCLF
{
	namespace
	{
		// The table's capacity (a power of two): far above the heads a scene loads at once.
		constexpr std::uint32_t kCapacity = 2048;
		// latest: the slot index in bits 0-1, and whether it holds a snapshot the reader has not taken.
		constexpr std::uint32_t kSlotMask = 3;
		constexpr std::uint32_t kFresh = 4;
		// A key whose record was retired: probing continues past it, and an insert may reuse it.
		const RE::BSFaceGenNiNode* const kTombstone = reinterpret_cast<const RE::BSFaceGenNiNode*>(std::uintptr_t{ 1 });
		constexpr std::uint32_t kStatsInterval = 600;

		bool hooksInstalled = false;

		std::uint32_t HashOf(const void* a_pointer)
		{
			auto value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_pointer));
			value ^= value >> 33;
			value *= 0xff51afd7ed558ccdull;
			value ^= value >> 33;
			return static_cast<std::uint32_t>(value) & (kCapacity - 1);
		}

		/** @brief Whether the instruction at a_address is a CALL rel32 to a_target: the patch site is what we reverse engineered. */
		bool CallsTo(std::uintptr_t a_address, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			if (bytes[0] != 0xE8)
				return false;
			std::int32_t displacement = 0;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
		}
	}

	struct FaceSnapshots::Impl
	{
		struct Record
		{
			struct Shape
			{
				RE::NiPointer<RE::BSDynamicTriShape> shape;
				std::uint32_t offset = 0;  // in floats, within a slot
				std::uint32_t vertexCount = 0;
			};

			RE::NiPointer<RE::BSFaceGenNiNode> head;
			std::vector<Shape> shapes;  // immutable once the record is in the table
			std::uint32_t floats = 0;   // a slot's size: four per vertex of every shape
			std::unique_ptr<float[]> storage;
			// Written by the writer for its slot before it publishes the slot, read by the reader after it takes it.
			std::array<std::uint64_t, 3> slotGeneration{};
			std::atomic<std::uint32_t> latest{ 1 };
			std::uint32_t writeSlot = 0;  // the writer's (one at a time: a job, then the stage's join)
			std::uint32_t readSlot = 2;   // the walk's
			// A first snapshot is wanted from the stage's join; set until any capture publishes.
			std::atomic<bool> wantsCapture{ true };
			// A capture found a shape whose vertex count or data changed: the walk rebuilds the record.
			std::atomic<bool> stale{ false };
			// The walk's.
			std::uint32_t seenWalk = 0;
			std::uint64_t retiredAtStage = 0;

			float* Slot(std::uint32_t a_slot) { return storage.get() + std::size_t(a_slot) * floats; }
		};

		struct Entry
		{
			std::atomic<const RE::BSFaceGenNiNode*> key{ nullptr };
			std::atomic<Record*> record{ nullptr };
		};

		std::array<Entry, kCapacity> table;
		std::atomic<std::uint64_t> generation{ 0 };
		std::atomic<std::uint64_t> stagesCompleted{ 0 };
		std::atomic<std::uint32_t> jobCaptures{ 0 }, stageCaptures{ 0 }, mismatches{ 0 }, stages{ 0 };

		// The walk's.
		std::vector<Record*> live;
		std::vector<Record*> retired;
		std::uint32_t walk = 0;
		Stats stats;

		Entry* Find(const RE::BSFaceGenNiNode* a_head)
		{
			for (std::uint32_t i = 0, h = HashOf(a_head); i < kCapacity; ++i, h = (h + 1) & (kCapacity - 1)) {
				const auto* key = table[h].key.load(std::memory_order_acquire);
				if (!key)
					return nullptr;
				if (key == a_head)
					return &table[h];
			}
			return nullptr;
		}

		Record* Lookup(const RE::BSFaceGenNiNode* a_head)
		{
			auto* entry = Find(a_head);
			return entry ? entry->record.load(std::memory_order_acquire) : nullptr;
		}

		// The walk's: the record goes in before the key, so a writer that finds the key finds the record.
		bool Insert(const RE::BSFaceGenNiNode* a_head, Record* a_record)
		{
			Entry* free = nullptr;
			for (std::uint32_t i = 0, h = HashOf(a_head); i < kCapacity; ++i, h = (h + 1) & (kCapacity - 1)) {
				const auto* key = table[h].key.load(std::memory_order_relaxed);
				if (key == a_head) {
					table[h].record.store(a_record, std::memory_order_release);
					return true;
				}
				if (!key) {
					if (!free)
						free = &table[h];
					break;
				}
				if (key == kTombstone && !free)
					free = &table[h];
			}
			if (!free)
				return false;
			free->record.store(a_record, std::memory_order_release);
			free->key.store(a_head, std::memory_order_release);
			return true;
		}

		void Remove(const RE::BSFaceGenNiNode* a_head)
		{
			if (auto* entry = Find(a_head)) {
				entry->record.store(nullptr, std::memory_order_release);
				entry->key.store(kTombstone, std::memory_order_release);
			}
		}

		// The writer's: every registered shape of the head into the write slot, published as one snapshot. Only
		// ever called by the thread that has just morphed the head, or after the stage's join.
		bool Capture(Record& a_record)
		{
			float* out = a_record.Slot(a_record.writeSlot);
			for (const auto& shape : a_record.shapes) {
				auto* geometry = shape.shape.get();
				const auto& dynamic = geometry->GetDynamicTrishapeRuntimeData();
				const std::uint32_t count = geometry->GetTrishapeRuntimeData().vertexCount;
				if (!dynamic.dynamicData || count != shape.vertexCount || dynamic.dataSize < count * 16u) {
					mismatches.fetch_add(1, std::memory_order_relaxed);
					a_record.stale.store(true, std::memory_order_relaxed);
					return false;
				}
				std::memcpy(out + shape.offset, dynamic.dynamicData, std::size_t(count) * 16);
			}
			a_record.slotGeneration[a_record.writeSlot] = generation.fetch_add(1, std::memory_order_relaxed) + 1;
			const auto previous = a_record.latest.exchange(a_record.writeSlot | kFresh, std::memory_order_acq_rel);
			a_record.writeSlot = previous & kSlotMask;
			a_record.wantsCapture.store(false, std::memory_order_relaxed);
			return true;
		}

		// The face shapes of a head as the walk sees it now: its direct children that are dynamic shapes, which
		// are the ones its morph job writes.
		static void ShapesOf(RE::BSFaceGenNiNode& a_head, std::vector<Record::Shape>& a_out)
		{
			a_out.clear();
			std::uint32_t offset = 0;
			for (const auto& child : a_head.GetChildren()) {
				auto* geometry = child ? child->AsGeometry() : nullptr;
				if (!geometry || geometry->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape)
					continue;
				auto* shape = static_cast<RE::BSDynamicTriShape*>(geometry);
				const std::uint32_t count = shape->GetTrishapeRuntimeData().vertexCount;
				a_out.push_back({ RE::NiPointer<RE::BSDynamicTriShape>(shape), offset, count });
				offset += count * 4;
			}
		}

		bool Matches(const Record& a_record, const std::vector<Record::Shape>& a_shapes) const
		{
			if (a_record.stale.load(std::memory_order_relaxed) || a_record.shapes.size() != a_shapes.size())
				return false;
			for (std::size_t i = 0; i < a_shapes.size(); ++i)
				if (a_record.shapes[i].shape != a_shapes[i].shape || a_record.shapes[i].vertexCount != a_shapes[i].vertexCount)
					return false;
			return true;
		}

		void Retire(Record* a_record)
		{
			Remove(a_record->head.get());
			a_record->retiredAtStage = stagesCompleted.load(std::memory_order_acquire);
			retired.push_back(a_record);
			++stats.retired;
		}

		// A retired record is freed once a morph stage has completed after its retirement: every job that could
		// have found it belonged to a stage that has joined since.
		void FreeRetired()
		{
			const auto completed = stagesCompleted.load(std::memory_order_acquire);
			std::erase_if(retired, [&](Record* a_record) {
				if (a_record->retiredAtStage >= completed)
					return false;
				delete a_record;
				++stats.freed;
				return true;
			});
		}

		std::vector<Record::Shape> scratch;
	};

	FaceSnapshots::FaceSnapshots() :
		impl(std::make_unique<Impl>())
	{}

	FaceSnapshots::~FaceSnapshots() = default;

	FaceSnapshots& FaceSnapshots::Get()
	{
		static FaceSnapshots instance;
		return instance;
	}

	bool FaceSnapshots::Enabled()
	{
		static const bool enabled = SwitchValue("CS_DCLF_FACEGEN") != "0";
		return enabled && hooksInstalled;
	}

	/** @brief After a head's morph job has reset and morphed its shapes (FUN_140432550), on that job's thread. */
	struct FaceSnapshots::MorphHook
	{
		static void thunk(RE::BSFaceGenNiNode* a_head, std::uint8_t a_flag)
		{
			func(a_head, a_flag);
			if (!a_head)
				return;
			auto& impl = *Get().impl;
			if (auto* record = impl.Lookup(a_head); record && impl.Capture(*record))
				impl.jobCaptures.fetch_add(1, std::memory_order_relaxed);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief After the face morphing stage's JobList::Finish: no morph job runs until the next stage. */
	struct FaceSnapshots::StageJoinHook
	{
		static void thunk(void* a_jobList)
		{
			func(a_jobList);
			auto& impl = *Get().impl;
			for (auto& entry : impl.table) {
				auto* record = entry.record.load(std::memory_order_acquire);
				if (record && record->wantsCapture.load(std::memory_order_relaxed) && impl.Capture(*record))
					impl.stageCaptures.fetch_add(1, std::memory_order_relaxed);
			}
			impl.stages.fetch_add(1, std::memory_order_relaxed);
			impl.stagesCompleted.fetch_add(1, std::memory_order_release);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void FaceSnapshots::Install()
	{
		if (hooksInstalled || !REL::Module::IsAE())
			return;
		// AE 1.6.1170: the per-head job FUN_1404334e0 calls FUN_140432550 at +0x13; Job_Face_morphing
		// (RELOCATION_ID 38139, 39096) calls JobList::Finish (FUN_140cf6810) at +0x4D. Checked before patching:
		// another executable leaves the face shapes to the engine.
		const auto morphCall = REL::Offset(0x4334f3).address();
		const auto joinCall = REL::Offset(0x6d36fd).address();
		if (!CallsTo(morphCall, REL::Offset(0x432550).address()) || !CallsTo(joinCall, REL::Offset(0xcf6810).address())) {
			logger::warn("[DCLF] face snapshots: the face morphing calls are not where expected; face shapes stay the engine's");
			return;
		}
		stl::write_thunk_call<MorphHook>(morphCall);
		stl::write_thunk_call<StageJoinHook>(joinCall);
		hooksInstalled = true;
		logger::info("[DCLF] face snapshots installed on the face morphing stage");
	}

	void FaceSnapshots::BeginWalk()
	{
		auto& d = *impl;
		++d.walk;
		d.FreeRetired();
		for (auto* record : d.live) {
			if (record->latest.load(std::memory_order_relaxed) & kFresh) {
				const auto previous = record->latest.exchange(record->readSlot, std::memory_order_acq_rel);
				record->readSlot = previous & kSlotMask;
				++d.stats.acquired;
			}
		}
		d.stats.heads = static_cast<std::uint32_t>(d.live.size());
	}

	FaceSnapshots::ShapeView FaceSnapshots::Shape(RE::BSDynamicTriShape& a_shape, RE::BSFaceGenNiNode& a_head)
	{
		auto& d = *impl;
		auto* record = d.Lookup(&a_head);
		if (!record || record->seenWalk != d.walk) {
			// The first shape of the head this walk: its record against the head's shapes as they are now.
			Impl::ShapesOf(a_head, d.scratch);
			if (!record || !d.Matches(*record, d.scratch)) {
				if (d.scratch.empty()) {
					if (record) {
						std::erase(d.live, record);
						d.Retire(record);
					}
					return {};
				}
				auto* rebuilt = new Impl::Record();
				rebuilt->head.reset(&a_head);
				rebuilt->shapes = d.scratch;
				for (const auto& shape : rebuilt->shapes)
					rebuilt->floats += shape.vertexCount * 4;
				rebuilt->storage = std::make_unique<float[]>(std::size_t(rebuilt->floats) * 3);
				if (record) {
					std::erase(d.live, record);
					d.Retire(record);
					++d.stats.rebuilt;
				}
				if (!d.Insert(&a_head, rebuilt)) {
					delete rebuilt;
					return {};
				}
				d.live.push_back(rebuilt);
				record = rebuilt;
			}
			d.scratch.clear();  // its references are the record's to hold
			record->seenWalk = d.walk;
			if (record->slotGeneration[record->readSlot] == 0)
				++d.stats.withoutSnapshot;
		}
		const auto generation = record->slotGeneration[record->readSlot];
		if (generation == 0)
			return {};
		for (const auto& shape : record->shapes) {
			if (shape.shape.get() != &a_shape)
				continue;
			const float* positions = record->Slot(record->readSlot) + shape.offset;
			// [TEMP] The snapshot against the engine's positions now. A probe's read, outside the writer's phase: under
			// the engine's schedule nothing writes them during the walk, which is what this measures.
			if (VolumetricProbe::Enabled()) {
				const auto* live = a_shape.GetDynamicTrishapeRuntimeData().dynamicData;
				++(live && std::memcmp(live, positions, std::size_t(shape.vertexCount) * 16) == 0 ? d.stats.parityEqual : d.stats.parityDiffer);
			}
			return { positions, shape.vertexCount, generation };
		}
		return {};
	}

	void FaceSnapshots::EndWalk()
	{
		auto& d = *impl;
		std::erase_if(d.live, [&](Impl::Record* a_record) {
			if (a_record->seenWalk == d.walk)
				return false;
			d.Retire(a_record);
			return true;
		});
		if (d.walk % kStatsInterval == 0 && (!d.live.empty() || d.stats.retired)) {
			const auto stats = TakeStats();
			const auto writer = TakeWriterStats();
			logger::info("[DCLF] face snapshots: {} heads, {} taken fresh, {} seen without one, {} rebuilt, {} retired, {} freed; "
			             "captured by jobs {}, at the join {}, {} mismatches, over {} stages; parity with dynamicData {} equal, {} differ",
				stats.heads, stats.acquired, stats.withoutSnapshot, stats.rebuilt, stats.retired, stats.freed, writer.jobCaptures, writer.stageCaptures,
				writer.mismatches, writer.stages, stats.parityEqual, stats.parityDiffer);
		}
	}

	void FaceSnapshots::RetireAll()
	{
		auto& d = *impl;
		for (auto* record : d.live)
			d.Retire(record);
		d.live.clear();
		d.FreeRetired();
	}

	FaceSnapshots::Stats FaceSnapshots::TakeStats()
	{
		auto& d = *impl;
		auto stats = d.stats;
		stats.heads = static_cast<std::uint32_t>(d.live.size());
		d.stats = {};
		return stats;
	}

	FaceSnapshots::WriterStats FaceSnapshots::TakeWriterStats()
	{
		auto& d = *impl;
		return { d.jobCaptures.exchange(0), d.stageCaptures.exchange(0), d.mismatches.exchange(0), d.stages.exchange(0) };
	}
}
