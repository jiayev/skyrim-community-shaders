#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"

namespace DCLF::Draws
{
	void UpdateBones(BonesStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation, BonesOut& a_out)
	{
		ZoneScopedN("CS.DCLF.Build.UpdateBones");
		a_out = {};
		a_out.capacity = a_tables.BoneCapacity();
		a_out.extraRows = static_cast<std::uint32_t>(a_tables.extraRows.size() / 4);
		a_out.bones = a_tables.bones.data();
		a_out.previous = a_tables.previousBones.data();
		a_out.extras = a_tables.extraRows.data();
		if (!a_store)
			return;
		auto& s = *a_store;
		++s.updates;
		s.rows.BeginBuild(a_uploaded);
		if (!s.cursor.Continues(a_tables.changeLog, a_generation) || s.capacity != a_out.capacity) {
			s.cursor.Restart(a_generation);
			s.capacity = a_out.capacity;
			s.rows.Resync();
			++s.resyncs;
		} else {
			for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
				const std::uint32_t o = change.slot;
				if ((change.causes & (kChangePalette | kChangeSkin)) && o < a_tables.boneRows.size() && a_tables.boneRows[o]) {
					s.rows.MarkRange(a_tables.boneOffset[o], a_tables.boneRows[o]);
					s.rows.MarkRange(std::uint64_t(a_out.capacity) + a_tables.boneOffset[o], a_tables.boneRows[o]);
				}
				if ((change.causes & kChangeExtras) && o < a_tables.extraOffset.size() && a_tables.extraOffset[o] != kNoExtraRows)
					s.rows.MarkRange(2ull * a_out.capacity + a_tables.extraOffset[o], kExtraRows);
			}
		}
		s.cursor.Advance(a_tables.changeLog);
		a_out.changes = s.rows.Take();
	}

	void UpdateObjectRecords(ObjectRecordStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, ObjectRecordsOut& a_out)
	{
		ZoneScopedN("CS.DCLF.Build.UpdateObjectRecords");
		const std::size_t count = std::min<std::size_t>(a_tables.objects.size(), kMaxObjects);
		if (!a_store) {
			auto records = std::make_shared<std::vector<BindlessObject>>(count);
			for (std::size_t r = 0; r < count; ++r)
				BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), SceneStore::kMainPassRenderFlags, (*records)[r]);
			a_out = {};
			a_out.elements = std::move(records);
			return;
		}
		auto& s = *a_store;
		if (s.busy.exchange(1, std::memory_order_acquire) != 0)
			++s.collisions;
		++s.updates;
		s.records.BeginBuild(a_uploaded);
		TracyCZoneN(updateRecordsZone, "CS.DCLF.Build.Objects.ApplyChanges", true);
		if (!s.cursor.Continues(a_tables.changeLog, a_generation) || s.records.Size() > count) {
			// Every record again: the first build, new tables, or a log this store fell behind.
			auto& records = s.records.Mutable();
			records.resize(count);
			for (std::size_t r = 0; r < count; ++r)
				BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), SceneStore::kMainPassRenderFlags, records[r]);
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
					BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), SceneStore::kMainPassRenderFlags, records[r]);
					s.records.Mark(r);
				}
			}
			BindlessObject fresh;
			s.changedObjects.Clear();
			for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
				// Every event reads the final immutable table row, not an intermediate value.
				if (!(change.causes & kObjectRecordCauses) || change.slot >= count || (reuseKeptStorage && !s.changedObjects.Add(change.slot)))
					continue;
				BuildObjectRecord(a_tables, change.slot, SceneStore::kMainPassRenderFlags, fresh);
				s.rewritten += s.records.Set(change.slot, fresh) ? 1u : 0u;
			}
		}
		s.cursor.Advance(a_tables.changeLog);
		// CS_DCLF_PERSISTENT_PARITY: every record against one built from the tables now.
		TracyCZoneEnd(updateRecordsZone);
		if (PersistentParityEnabled() && ParityDue(a_frame)) {
			BindlessObject fresh;
			const auto& records = s.records.Get();
			for (std::size_t r = 0; r < count; ++r) {
				BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), SceneStore::kMainPassRenderFlags, fresh);
				s.parity.Check(std::memcmp(&fresh, &records[r], sizeof(fresh)) == 0, [&] {
					const auto* geometry = r < a_tables.objectGeometry.size() ? a_tables.objectGeometry[r] : nullptr;
					return fmt::format("record {} '{}'", r, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?");
				});
			}
		}
		{
			ZoneScopedN("CS.DCLF.Build.Objects.Snapshot");
			a_out = s.records.View();
		}
		s.busy.store(0, std::memory_order_release);
	}

	void UpdateGeometryDraws(GeometryStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, GeometryDrawsOut& a_out)
	{
		ZoneScopedN("CS.DCLF.Build.UpdateGeometryDraws");
		a_out = {};
		const std::size_t count = std::min<std::size_t>(a_tables.geometries.size(), kMaxGeometries);
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
		s.packed.BeginBuild(a_uploaded);
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
		// CS_DCLF_PERSISTENT_PARITY: every slot against one packed from the tables now.
		if (PersistentParityEnabled() && ParityDue(a_frame)) {
			const auto& packed = s.packed.Get();
			std::size_t differs = count;
			for (std::size_t g = 0; g < count && differs == count; ++g) {
				const GeometryDraw fresh = pack(g);
				if (std::memcmp(&fresh, &packed[g], sizeof(fresh)) != 0)
					differs = g;
			}
			s.parity.Check(differs == count, [&] { return fmt::format("slot {}", differs); });
		}
		a_out.slots = s.packed.View();
	}
}

#endif
