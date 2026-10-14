// The kept stores' updates (KeptStores.h): extras rows, object records and geometry draws brought up to a frame's tables from
// their change logs. Run by MakeStreamViews, one caller at a time (the snapshot builder).

#include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Draws
{
	namespace
	{
		// A resync's records on the pool past this many slots (each record reads only the immutable tables).
		constexpr std::size_t kRecordsFanoutMin = 4096;
		constexpr std::size_t kRecordsGrain = 1024;

		/** @brief A kept array's elements made writable for pool chunks (the chunked copy-on-write's PrepareWrites when it has one). */
		template <class Kept>
		void PrepareKeptWrites(Kept& a_kept, std::size_t a_first, std::size_t a_count)
		{
			if constexpr (requires { a_kept.PrepareWrites(a_first, a_count); })
				a_kept.PrepareWrites(a_first, a_count);
			else
				(void)a_kept.Mutable();
		}
	}

	void UpdateExtras(ExtrasStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation, ExtrasOut& a_out)
	{
		ZoneScopedN("CS.DCLF.Build.UpdateExtras");
		a_out = {};
		a_out.extraRows = static_cast<std::uint32_t>(a_tables.extraRows.size() / 4);
		a_out.extras = a_tables.extraRows.data();
		if (!a_store)
			return;
		auto& s = *a_store;
		++s.updates;
		s.rows.BeginBuild(a_oldestContentEpoch);
		if (!s.cursor.Continues(a_tables.changeLog, a_generation)) {
			s.cursor.Restart(a_generation);
			s.rows.Resync();
			++s.resyncs;
		} else {
			for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
				const std::uint32_t o = change.slot;
				if ((change.causes & kChangeExtras) && o < a_tables.extraOffset.size() && a_tables.extraOffset[o] != kNoExtraRows)
					s.rows.MarkRange(a_tables.extraOffset[o], kExtraRows);
			}
		}
		s.cursor.Advance(a_tables.changeLog);
		a_out.changes = s.rows.Take();
	}

	void UpdateObjectRecords(ObjectRecordStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, ObjectRecordsOut& a_out)
	{
		ZoneScopedN("CS.DCLF.Build.UpdateObjectRecords");
		const std::size_t count = a_tables.objects.size();
		if (!a_store) {
			auto records = std::make_shared<std::vector<BindlessObject>>(count);
			for (std::size_t r = 0; r < count; ++r)
				BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), GeometryPort::kMainPassRenderFlags, (*records)[r]);
			a_out = {};
			a_out.elements = std::move(records);
			return;
		}
		auto& s = *a_store;
		if (s.busy.exchange(1, std::memory_order_acquire) != 0)
			++s.collisions;
		++s.updates;
		s.records.BeginBuild(a_oldestContentEpoch);
		TracyCZoneN(updateRecordsZone, "CS.DCLF.Build.Objects.ApplyChanges", true);
		if (!s.cursor.Continues(a_tables.changeLog, a_generation) || s.records.Size() > count) {
			// Every record again: the first build, new tables, or a log this store fell behind.
			auto& records = s.records.Mutable();
			records.resize(count);
			if (count >= kRecordsFanoutMin && !Policy::SerialFanout()) {
				PrepareKeptWrites(s.records, 0, count);
				SceneScheduler::Executor().ParallelFor("CS.DCLF.Build.Objects.Resync", count, kRecordsGrain, [&](std::size_t a_begin, std::size_t a_end) {
					ZoneScopedN("CS.DCLF.Build.Objects.ResyncShards");
					for (std::size_t r = a_begin; r < a_end; ++r)
						BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), GeometryPort::kMainPassRenderFlags, records[r]);
				});
			} else {
				for (std::size_t r = 0; r < count; ++r)
					BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), GeometryPort::kMainPassRenderFlags, records[r]);
			}
			s.records.Resync();
			s.cursor.Restart(a_generation);
			++s.resyncs;
		} else {
			// Slots the tables grew by since: records of their own (the log names them too).
			if (s.records.Size() < count) {
				auto& records = s.records.Mutable();
				const auto first = records.size();
				records.resize(count);
				for (std::size_t r = first; r < count; ++r) {
					BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), GeometryPort::kMainPassRenderFlags, records[r]);
					s.records.Mark(r);
				}
			}
			BindlessObject fresh;
			s.changedObjects.Clear();
			std::uint32_t causes = 0;
			for (const auto& change : s.cursor.Unread(a_tables.changeLog))
				causes |= change.causes;
			if (causes) {
				++s.structural;
				for (std::uint32_t c = 0; c < kChangeCauseCount; ++c)
					s.byCause[c] += (causes >> c) & 1u;
			} else {
				++s.streamOnly;
			}
			for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
				// Every event reads the final immutable table row, not an intermediate value.
				if (!(change.causes & kObjectRecordCauses) || change.slot >= count || (reuseKeptStorage && !s.changedObjects.Add(change.slot)))
					continue;
				BuildObjectRecord(a_tables, change.slot, GeometryPort::kMainPassRenderFlags, fresh);
				s.rewritten += s.records.Set(change.slot, fresh) ? 1u : 0u;
			}
		}
		s.cursor.Advance(a_tables.changeLog);
		TracyCZoneEnd(updateRecordsZone);
		Diagnostics::Notify(Diagnostics::StoreUpdated<ObjectRecordStore>{ s, a_tables, a_frame });
		{
			ZoneScopedN("CS.DCLF.Build.Objects.Snapshot");
			a_out = s.records.View();
		}
		s.busy.store(0, std::memory_order_release);
	}

	void TakeGeometryDraws(const StreamViews* a_streams, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame,
		GeometryDrawsOut& a_out)
	{
		if (!a_streams) {
			UpdateGeometryDraws(nullptr, 0, a_tables, a_generation, a_frame, a_out);
			return;
		}
		a_out = {};
		a_out.slots = a_streams->geometries;
	}

	void UpdateGeometryDraws(GeometryStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, GeometryDrawsOut& a_out)
	{
		ZoneScopedN("CS.DCLF.Build.UpdateGeometryDraws");
		a_out = {};
		const std::size_t count = a_tables.geometries.size();
		auto pack = [&](std::size_t a_slot) { return PackGeometryDraw(a_tables, static_cast<std::uint32_t>(a_slot), count); };
		if (!a_store) {
			auto packed = std::make_shared<std::vector<GeometryDraw>>(count);
			for (std::size_t g = 0; g < count; ++g)
				(*packed)[g] = pack(g);
			a_out.slots.elements = std::move(packed);
			return;
		}
		auto& s = *a_store;
		++s.updates;
		s.packed.BeginBuild(a_oldestContentEpoch);
		if (!s.cursor.Continues(a_tables.geometryLog, a_generation) || s.packed.Size() > count) {
			auto& packed = s.packed.Mutable();
			packed.resize(count);
			for (std::size_t g = 0; g < count; ++g)
				packed[g] = pack(g);
			s.packed.Resync();
			s.cursor.Restart(a_generation);
			++s.resyncs;
		} else {
			// Slots the tables grew by since (the log names them too), then the slots written.
			if (s.packed.Size() < count) {
				auto& packed = s.packed.Mutable();
				const auto first = packed.size();
				packed.resize(count);
				for (std::size_t g = first; g < count; ++g) {
					packed[g] = pack(g);
					s.packed.Mark(g);
				}
			}
			for (const std::uint32_t g : s.cursor.Unread(a_tables.geometryLog))
				if (g < count)
					s.rewritten += s.packed.Set(g, pack(g)) ? 1u : 0u;
		}
		s.cursor.Advance(a_tables.geometryLog);
		Diagnostics::Notify(Diagnostics::StoreUpdated<GeometryStore>{ s, a_tables, a_frame });
		a_out.slots = s.packed.View();
	}
}
