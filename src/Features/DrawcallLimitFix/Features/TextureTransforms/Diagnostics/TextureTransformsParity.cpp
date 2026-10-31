// The texture transforms' frame-value pool's parity (CS_DCLF_PERSISTENT_PARITY, the moved-only parity's frames) and its stats line
// (CS_DCLF_STATS), called by the frame's producer (TextureTransformsPool.cpp).

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransformsPool.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <cstring>
#include <string>

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// The producers', one at a time in frame order: the parity (Policy::SampleEveryMover's frames), and the line's counts, since its
		// last line.
		struct Parity
		{
			std::uint64_t checks = 0, entries = 0, variables = 0, differ = 0;
			std::string first;
		} parity;
		std::uint64_t lineFrames = 0, lineEntries = 0, lineChanged = 0, lineRefused = 0, lineVariables = 0, lineVariablesChanged = 0;
	}

	// Every entry read again at the producer's end against what it sends. The frame's buffer stands all frame (the controllers write the
	// other), so a difference is a write to the buffer the frame's shaders read, a write to a declared frame variable inside the window, or
	// a sampling defect.
	void CheckPoolParity(const Api::FrameValuePoolProducer& a_producer, const TextureTransformList* a_list, std::size_t a_count, std::size_t a_buffer,
		const std::vector<TextureTransformRow>& a_out, const std::vector<const RE::BSShaderMaterial*>& a_materials)
	{
		ZoneScopedN("CS.DCLF.FrameValues.TextureTransformsParity");
		auto& p = parity;
		++p.checks;
		for (std::size_t i = 0; i < a_count; ++i) {
			const auto& entry = a_list->entries[i];
			const auto& row = a_out[i + 1];
			EngineReadWindow::Lease lease;
			if (!Readable(a_producer, lease) || a_materials[i] != entry.material)
				continue;
			float values[4];
			Read(entry, a_buffer, values);
			++p.entries;
			p.variables += entry.components ? 1u : 0u;
			if (row.rowOffset == entry.rowOffset && row.mask == entry.mask && std::memcmp(values, row.values, sizeof(values)) == 0)
				continue;
			++p.differ;
			if (p.first.empty())
				p.first = fmt::format("entry {} ({}, material {}, row byte {}): sent ({}, {}, {}, {}) mask {:x}, now ({}, {}, {}, {}) in buffer {}", i,
					entry.components ? "frame variable" : "TexcoordOffset", static_cast<const void*>(entry.material), entry.rowOffset, row.values[0], row.values[1],
					row.values[2], row.values[3], row.mask, values[0], values[1], values[2], values[3], a_buffer);
		}
		if (p.checks >= 5 || p.differ) {
			logger::info("[DCLF] frame values' texture transforms (parity): {} checks, {} entries read again ({} frame variables), {} differ{}{}", p.checks,
				p.entries, p.variables, p.differ, p.differ ? " <- DIFFER; first: " : " <- OK", p.first);
			p = {};
		}
	}

	// The builder's (one build at a time): a list made from its previous one and the moved slots against the list made whole. A difference is
	// a move the builder was not told of (a row's state written unmarked, a material slot keyed or let go with no log entry).
	void CheckListParity(const TextureTransformList& a_built, const TextureTransformList& a_whole)
	{
		static std::uint64_t checks = 0, differ = 0;
		static std::string first;
		++checks;
		if (a_built.entries != a_whole.entries || a_built.entrySlots != a_whole.entrySlots || a_built.slotVerdicts != a_whole.slotVerdicts ||
			a_built.leftOut != a_whole.leftOut || a_built.variables != a_whole.variables) {
			++differ;
			if (first.empty()) {
				std::size_t at = 0;
				while (at < a_built.entries.size() && at < a_whole.entries.size() && a_built.entries[at] == a_whole.entries[at] && a_built.entrySlots[at] == a_whole.entrySlots[at])
					++at;
				std::size_t slot = 0;
				while (slot < a_built.slotVerdicts.size() && slot < a_whole.slotVerdicts.size() && a_built.slotVerdicts[slot] == a_whole.slotVerdicts[slot])
					++slot;
				first = fmt::format("{} entries against {} whole, first differing entry {} (slot {}), first differing verdict at slot {} ({} slots against {})",
					a_built.entries.size(), a_whole.entries.size(), at, at < a_whole.entrySlots.size() ? a_whole.entrySlots[at] : ~0u, slot, a_built.slotVerdicts.size(),
					a_whole.slotVerdicts.size());
			}
		}
		if (checks >= 5 || differ) {
			logger::info("[DCLF] texture transforms' list (parity): {} builds checked, {} differ{}{}", checks, differ, differ ? " <- DIFFER; first: " : " <- OK", first);
			checks = differ = 0;
			first.clear();
		}
	}

	// What the rows carrying TexcoordOffset or a declared frame variable cost a frame and how many moved.
	void CountPoolLine(std::size_t a_count, std::uint64_t a_changed, std::uint64_t a_refused, std::size_t a_variables, std::uint64_t a_variablesChanged)
	{
		++lineFrames;
		lineEntries += a_count;
		lineChanged += a_changed;
		lineRefused += a_refused;
		lineVariables += a_variables;
		lineVariablesChanged += a_variablesChanged;
		if (lineFrames < 300)
			return;
		if (Diagnostics::Observe().stats) {
			const double n = static_cast<double>(lineFrames);
			// The list's stages since the last line: built by the builder (rows left out by reason), carried by a payload, installed.
			namespace stats = TextureTransformStats;
			const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
			const auto built = take(stats::built), builtEntries = take(stats::builtEntries), builtVariables = take(stats::builtVariables), unpacked = take(stats::rowsUnpacked),
					   noTexcoord = take(stats::rowsNoTexcoord), freeSlot = take(stats::rowsFreeSlot), stale = take(stats::rowsStale);
			const auto published = take(stats::published), publishedEntries = take(stats::publishedEntries), publishedNone = take(stats::publishedNone);
			const auto adopted = take(stats::adopted), adoptedEntries = take(stats::adoptedEntries), adoptedNone = take(stats::adoptedNone);
			const auto per = [](std::uint64_t a_sum, std::uint64_t a_count) { return a_count ? static_cast<double>(a_sum) / static_cast<double>(a_count) : 0.0; };
			logger::info("[DCLF] frame values' texture transforms: {} frames; a frame {:.0f} material rows sampled, {:.1f} changed (scrolled) and sent, {} unread "
						 "(refused leases, a producer a frame behind); of them frame variables {:.0f} sampled, {:.1f} changed; lists built {} ({:.0f} entries, "
						 "{:.0f} frame variables; a build left out {:.0f} unpacked, {:.0f} without TexcoordOffset or a frame variable, {:.0f} free slots, {:.0f} stale), "
						 "carried {} ({:.0f} entries, {} main payloads without one{}), installed {} ({:.0f} entries, {} none)",
				lineFrames, lineEntries / n, lineChanged / n, lineRefused, lineVariables / n, lineVariablesChanged / n, built, per(builtEntries, built),
				per(builtVariables, built), per(unpacked, built), per(noTexcoord, built), per(freeSlot, built), per(stale, built), published,
				per(publishedEntries, published), publishedNone, publishedNone ? " <- LIST LOST" : "", adopted, per(adoptedEntries, adopted), adoptedNone);
		}
		lineFrames = lineEntries = lineChanged = lineRefused = lineVariables = lineVariablesChanged = 0;
	}
}
