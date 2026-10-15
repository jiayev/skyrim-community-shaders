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
			std::uint64_t checks = 0, entries = 0, differ = 0;
			std::string first;
		} parity;
		std::uint64_t lineFrames = 0, lineEntries = 0, lineChanged = 0, lineRefused = 0;
	}

	// Every entry read again at the producer's end against what it sends. The frame's buffer stands all frame (the controllers write the
	// other), so a difference is a write to the buffer the frame's shaders read, or a sampling defect.
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
			if (row.rowOffset == entry.rowOffset && row.mask == entry.mask && std::memcmp(values, row.values, sizeof(values)) == 0)
				continue;
			++p.differ;
			if (p.first.empty())
				p.first = fmt::format("entry {} (material {}, row byte {}): sent ({}, {}, {}, {}) mask {:x}, now ({}, {}, {}, {}) in buffer {}", i,
					static_cast<const void*>(entry.material), entry.rowOffset, row.values[0], row.values[1], row.values[2], row.values[3], row.mask, values[0],
					values[1], values[2], values[3], a_buffer);
		}
		if (p.checks >= 5 || p.differ) {
			logger::info("[DCLF] frame values' texture transforms (parity): {} checks, {} entries read again, {} differ{}{}", p.checks, p.entries, p.differ,
				p.differ ? " <- DIFFER; first: " : " <- OK", p.first);
			p = {};
		}
	}

	// What the rows carrying TexcoordOffset cost a frame and how many scrolled.
	void CountPoolLine(std::size_t a_count, std::uint64_t a_changed, std::uint64_t a_refused)
	{
		++lineFrames;
		lineEntries += a_count;
		lineChanged += a_changed;
		lineRefused += a_refused;
		if (lineFrames < 300)
			return;
		if (Diagnostics::Observe().stats) {
			const double n = static_cast<double>(lineFrames);
			// The list's stages since the last line: built by the builder (rows left out by reason), carried by a payload, installed.
			namespace stats = TextureTransformStats;
			const auto take = [](std::atomic<std::uint64_t>& a_counter) { return a_counter.exchange(0, std::memory_order_relaxed); };
			const auto built = take(stats::built), builtEntries = take(stats::builtEntries), unpacked = take(stats::rowsUnpacked),
					   noTexcoord = take(stats::rowsNoTexcoord), freeSlot = take(stats::rowsFreeSlot), stale = take(stats::rowsStale);
			const auto published = take(stats::published), publishedEntries = take(stats::publishedEntries), publishedNone = take(stats::publishedNone);
			const auto adopted = take(stats::adopted), adoptedEntries = take(stats::adoptedEntries), adoptedNone = take(stats::adoptedNone);
			const auto per = [](std::uint64_t a_sum, std::uint64_t a_count) { return a_count ? static_cast<double>(a_sum) / static_cast<double>(a_count) : 0.0; };
			logger::info("[DCLF] frame values' texture transforms: {} frames; a frame {:.0f} material rows sampled, {:.1f} changed (scrolled) and sent, {} unread "
						 "(refused leases, a producer a frame behind); lists built {} ({:.0f} entries; a build left out {:.0f} unpacked, {:.0f} without TexcoordOffset, "
						 "{:.0f} free slots, {:.0f} stale), carried {} ({:.0f} entries, {} main payloads without one{}), installed {} ({:.0f} entries, {} none)",
				lineFrames, lineEntries / n, lineChanged / n, lineRefused, built, per(builtEntries, built), per(unpacked, built),
				per(noTexcoord, built), per(freeSlot, built), per(stale, built), published, per(publishedEntries, published), publishedNone,
				publishedNone ? " <- LIST LOST" : "", adopted, per(adoptedEntries, adopted), adoptedNone);
		}
		lineFrames = lineEntries = lineChanged = lineRefused = 0;
	}
}
