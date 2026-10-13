#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsState.h"
#	include "Events.h"
#	include "Observers.h"

// CS_DCLF_PERSISTENT_PARITY: the stores the builds keep against a rebuild from the tables (the object records, the geometry table, the
// extras rows as each buffer holds them, the draw bound, the kept shadow build, the exclusions' reuse). Each runs on its store's thread.

namespace DCLF::Diagnostics
{
	namespace
	{
		/** @brief One buffer's rows as uploaded (a_mirror) against the view it was sent from. A mirror shorter than the view's rows differs (rows the buffer was never sent). a_first, when given, gets the first difference. */
		bool CompareExtras(const std::vector<float>& a_mirror, const ExtrasOut& a_out, std::string* a_first)
		{
			const std::uint32_t rows = a_out.Rows();
			if (a_mirror.size() < std::size_t(rows) * 4) {
				if (a_first)
					*a_first = fmt::format("{} rows (version {}), the mirror holds {}: rows never sent", rows, a_out.Version(), a_mirror.size() / 4);
				return false;
			}
			for (std::uint32_t row = 0; row < rows; ++row) {
				const float* u = &a_mirror[std::size_t(row) * 4];
				const float* t = a_out.Row(row);
				if (std::memcmp(u, t, 16) == 0)
					continue;
				if (a_first)
					*a_first = fmt::format("row {} of {} (version {}): uploaded ({} {} {} {}), tables ({} {} {} {})", row, rows, a_out.Version(), u[0], u[1], u[2], u[3],
						t[0], t[1], t[2], t[3]);
				return false;
			}
			return true;
		}
	}

	void MirrorExtras(const ExtrasSending& a_sending)
	{
		auto& kept = DrawsState::Get().kept;
		a_sending.mirror = a_sending.entry == ExtrasSending::kSceneBuffers ? &kept.sceneExtras : &kept.ringExtras[a_sending.entry % kept.ringExtras.size()];
	}

	void CheckExtras(const ExtrasSent& a_sent)
	{
		auto& kept = DrawsState::Get().kept;
		if (a_sent.entry == ExtrasSending::kSceneBuffers) {
			// The scene buffers, on the parity's frames, when they hold every row.
			if (!ParityDue(a_sent.frame) || a_sent.rows.Rows() > a_sent.bufferRows)
				return;
			std::string first;
			const bool same = CompareExtras(kept.sceneExtras, a_sent.rows, kept.extrasFirst.empty() ? &first : nullptr);
			if (!same && kept.extrasFirst.empty())
				kept.extrasFirst = "scene buffers: " + first;
			kept.extras.Check(same);
			return;
		}
		// A payload ring entry, over kPayloadRing consecutive frames from each parity frame, so every entry is checked (the entries rotate
		// one a frame, and the parity's period is a multiple of their count: one frame alone would check the same entry each time).
		bool due = false;
		for (std::uint32_t k = 0; k < DrawsView::Impl::kPayloadRing && !due; ++k)
			due = ParityDue(a_sent.frame, k);
		if (!due)
			return;
		auto& ring = kept.ring;
		const bool wantFirst = ring.firstState.load(std::memory_order_relaxed) == 0;
		std::string first;
		const bool same = CompareExtras(kept.ringExtras[a_sent.entry % kept.ringExtras.size()], a_sent.rows, wantFirst ? &first : nullptr);
		ring.checks.fetch_add(1, std::memory_order_relaxed);
		if (same)
			return;
		ring.mismatches.fetch_add(1, std::memory_order_relaxed);
		std::uint32_t none = 0;
		if (wantFirst && ring.firstState.compare_exchange_strong(none, 1, std::memory_order_acquire)) {
			ring.first = fmt::format("payload ring entry {}: {}", a_sent.entry, first);
			ring.firstState.store(2, std::memory_order_release);
		}
	}

	void CheckObjectRecords(const ObjectRecordsUpdated& a_updated)
	{
		// Every record against one built from the tables now.
		if (!ParityDue(a_updated.frame))
			return;
		const auto& a_tables = a_updated.tables;
		auto& parity = DrawsState::Get().kept.objects;
		BindlessObject fresh;
		const auto& records = a_updated.store.records.Get();
		const std::size_t count = a_tables.objects.size();
		for (std::size_t r = 0; r < count; ++r) {
			BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), GeometryPort::kMainPassRenderFlags, fresh);
			parity.Check(std::memcmp(&fresh, &records[r], sizeof(fresh)) == 0, [&] {
				const auto* geometry = r < a_tables.objectGeometry.size() ? a_tables.objectGeometry[r] : nullptr;
				return fmt::format("record {} '{}'", r, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?");
			});
		}
	}

	void CheckGeometryDraws(const GeometryDrawsUpdated& a_updated)
	{
		// Every slot against one packed from the tables now.
		if (!ParityDue(a_updated.frame))
			return;
		const auto& a_tables = a_updated.tables;
		const std::size_t count = a_tables.geometries.size();
		const auto& packed = a_updated.store.packed.Get();
		std::size_t differs = count;
		for (std::size_t g = 0; g < count && differs == count; ++g) {
			const GeometryDraw fresh = PackGeometryDraw(a_tables, static_cast<std::uint32_t>(g), count);
			if (std::memcmp(&fresh, &packed[g], sizeof(fresh)) != 0)
				differs = g;
		}
		DrawsState::Get().kept.geometry.Check(differs == count, [&] { return fmt::format("slot {}", differs); });
	}

	void CheckDrawBound(const DrawBoundUpdated& a_updated)
	{
		// The kept bound against a scan.
		if (!ParityDue(a_updated.frame))
			return;
		const auto& s = a_updated.store;
		const auto scan = ScanDrawBound(a_updated.tables);
		const auto& perPipeline = scan.perPipeline;
		const std::uint32_t scanned = scan.draws;
		bool same = scanned == s.Draws();
		std::size_t differs = perPipeline.size();
		for (std::size_t p = 0; same && p < perPipeline.size(); ++p)
			if (perPipeline[p] != s.PipelineDraws(p)) {
				same = false;
				differs = p;
			}
		// And the casting draws per mode and key.
		const auto& modeKeys = scan.modeKeys;
		std::uint32_t modeDiffers = kShadowModeCount;
		for (std::uint32_t m = 0; same && m < kShadowModeCount; ++m)
			if (modeKeys[m] != s.modeKeyDraws[m]) {
				same = false;
				modeDiffers = m;
			}
		DrawsState::Get().kept.drawBound.Check(same, [&] {
			return differs < perPipeline.size()     ? fmt::format("pipeline slot {}: {} kept, {} scanned", differs, s.PipelineDraws(differs), perPipeline[differs]) :
			       modeDiffers < kShadowModeCount ? fmt::format("shadow mode {}: {} keys kept, {} scanned", modeDiffers, s.modeKeyDraws[modeDiffers].size(), modeKeys[modeDiffers].size()) :
			                                        fmt::format("{} draws kept, {} scanned", s.Draws(), scanned);
		});
	}

	void CheckKeptShadow(const ShadowBuilt& a_built)
	{
		// A kept shadow build against the same build made the per-frame way (BuildShadowPayload without kept state: the reference).
		// Per used mode, the same casters with the same inputs (the record's number apart), records binding the same textures and
		// samplers with the same texcoord values.
		const auto& a_in = a_built.in;
		const auto& a_tables = a_built.tables;
		const auto& a_kept = a_built.payload;
		const auto& k = a_built.kept;
		if (!ParityDue(a_in.frameNumber))
			return;
		auto& kept = DrawsState::Get().kept;
		auto& parity = kept.shadow;
		ShadowPayload reference;
		BuildShadowPayload(a_in, a_tables, a_built.lookups, reference);
		auto fail = [&](std::uint32_t a_object, const std::string& a_what) {
			if (parity.mismatches++ == 0)
				parity.first = fmt::format("object {}: {}", a_object, a_what);
		};
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_in.modeUsed[m])
				continue;
			ankerl::unordered_dense::map<std::uint32_t, DrawInput> keptInputs;
			a_kept.ForEachInput(m, [&](const DrawInput& a_input) { keptInputs.emplace(a_input.objectIndex, a_input); });
			std::size_t matched = 0;
			reference.ForEachInput(m, [&](const DrawInput& input) {
				++parity.checks;
				const auto it = keptInputs.find(input.objectIndex);
				if (it == keptInputs.end()) {
					// Why the kept build lacks it: no record, waiting on a pipeline or texture, in the frame's face list, or
					// held nowhere (an object no change named since it became a caster).
					const std::uint32_t o = input.objectIndex;
					const auto& mode = k.modes[m];
					const std::uint32_t record = o < k.objectRecord.size() ? k.objectRecord[o] : ShadowKept::kNoRecord;
					const char* why = record == ShadowKept::kNoRecord                        ? "no record" :
					                  o < mode.waitingMark.size() && mode.waitingMark[o]      ? "waiting" :
					                  o < mode.faceMark.size() && mode.faceMark[o]            ? "a face" :
					                  k.Holds(m, o)                                           ? "held" :
					                                                                            "not held";
					auto& count = kept.shadowMissingBy[why];
					if (!count++) {
						const auto* geometry = o < a_tables.objectGeometry.size() ? a_tables.objectGeometry[o] : nullptr;
						kept.shadowMissingFirst[why] = fmt::format("'{}' (object {}, flags {:#x}, technique {:#x}, record {} against {})",
							geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", o, a_tables.objects[o].flags, a_tables.shadowTechnique[o], record,
							input.recordIndex);
					}
					fail(o, fmt::format("mode {}: an input of the per-frame build only ({})", m, why));
					return;
				}
				++matched;
				// What the shadow views read of the two (a scene entry's main part is the main build's).
				const DrawInput x = ShadowPartOf(it->second), y = ShadowPartOf(input);
				const std::uint32_t xr = it->second.shadowRow, yr = input.shadowRow;
				if (std::memcmp(&x, &y, sizeof(DrawInput)) != 0) {
					fail(input.objectIndex, fmt::format("mode {}: the input differs (mask {:#x} against {:#x}, flags {:#x} against {:#x})", m, x.view.mask, y.view.mask,
												x.flags, y.flags));
					return;
				}
				// The rows by value: the two builds number them differently.
				const auto* a = a_kept.materialRows.At(xr);
				const auto* b = reference.materialRows.At(yr);
				if (!a || !b) {
					fail(input.objectIndex, "a material row out of range");
					return;
				}
				if (!(*a == *b))
					fail(input.objectIndex, fmt::format("mode {}: its material row differs (kept {} against {})", m, xr, yr));
			});
			if (matched != keptInputs.size())
				fail(~0u, fmt::format("mode {}: {} inputs of the kept build only", m, keptInputs.size() - matched));
		}
	}

	void CheckExclusionReuse(const ExclusionRebuilt& a_rebuilt)
	{
		auto& kept = DrawsState::Get().kept;
		(a_rebuilt.parabolic ? kept.parabolicExclusion : kept.sunExclusion).Check(a_rebuilt.same);
	}
}

#endif
