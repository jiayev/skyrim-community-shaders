// The texture transforms' frame-value pool: every installed material row's TexcoordOffset sampled each frame by FrameValues' producer
// (TextureTransformRow: the header, then entry i at i + 1), the rows that moved sent into the frame's backing, with its parity and its
// stats line.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Scene/Ports/FrameGlobals.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// Render thread: the installed payload's list (InstallList), the most rows a list named (the header and its entries: the rows
		// never shrink), and the pool (install).
		std::shared_ptr<const TextureTransformList> installed;
		std::uint64_t rowsNeeded = 1;
		Api::FrameValuePoolId pool;

		// The producers', one at a time in frame order.
		struct Produced
		{
			// The rows, and per entry the material its values were last read from (a refused read keeps them only for the same material).
			KeptWholeArray<TextureTransformRow> transforms;
			std::vector<const RE::BSShaderMaterial*> materials;
			// The parity (Policy::SampleEveryMover's frames), and the line's counts, since its last line.
			struct Parity
			{
				std::uint64_t checks = 0, entries = 0, differ = 0;
				std::string first;
			} parity;
			std::uint64_t lineFrames = 0, lineEntries = 0, lineChanged = 0, lineRefused = 0;
		};
		Produced produced;

		// A read only while this producer's Kick is the newest: its list's materials are held by the publication its frame installed, which
		// is let go no earlier than the next frame's start (released at that frame's Present, after the window closed).
		bool Readable(const Api::FrameValuePoolProducer& a_producer, EngineReadWindow::Lease& a_lease) { return a_lease && a_producer.Newest(); }

		void Read(const TextureTransformList::Entry& a_entry, std::size_t a_buffer, float (&a_values)[4])
		{
			const auto* material = a_entry.material;
			a_values[0] = material->texCoordOffset[a_buffer].x;
			a_values[1] = material->texCoordOffset[a_buffer].y;
			a_values[2] = material->texCoordScale[a_buffer].x;
			a_values[3] = material->texCoordScale[a_buffer].y;
		}

		// Render thread, at Kick: the installed payload's TexcoordOffset rows for the frame's producer.
		Api::FrameValueInput Kick(const Api::FrameValuePoolKick&)
		{
			rowsNeeded = std::max<std::uint64_t>(rowsNeeded, (installed ? installed->entries.size() : 0u) + 1u);
			return { installed, rowsNeeded };
		}

		// Sample's stages: the entries read across the pool (the rows that changed marked, in order), the parity, the line.
		struct Sampled
		{
			std::uint64_t changed = 0, refused = 0;
		};

		Sampled SampleEntries(const Api::FrameValuePoolProducer& a_producer, const TextureTransformList* a_list, std::size_t a_count, std::size_t a_buffer,
			std::vector<TextureTransformRow>& a_out)
		{
			auto& p = produced;
			constexpr std::size_t kGrain = 256;
			struct Chunk
			{
				std::vector<std::uint32_t> changed;
				std::uint64_t refused = 0;
			};
			std::vector<Chunk> chunks((a_count + kGrain - 1) / kGrain);
			SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.TextureTransforms", a_count, kGrain, [&](std::size_t a_begin, std::size_t a_end) {
				ZoneScopedN("CS.DCLF.FrameValues.TextureTransformsShards");
				auto& mine = chunks[a_begin / kGrain];
				for (std::size_t i = a_begin; i < a_end; ++i) {
					const auto& entry = a_list->entries[i];
					auto& row = a_out[i + 1];
					TextureTransformRow next = row;
					next.rowOffset = entry.rowOffset;
					next.mask = entry.mask;
					EngineReadWindow::Lease lease;
					if (Readable(a_producer, lease)) {
						Read(entry, a_buffer, next.values);
						p.materials[i] = entry.material;
					} else {
						// Unread: the values stand for the material they were read from, else the row keeps its own (no component patched).
						++mine.refused;
						if (p.materials[i] != entry.material)
							next.mask = 0;
					}
					if (std::memcmp(&next, &row, sizeof(row)) != 0) {
						row = next;
						mine.changed.push_back(static_cast<std::uint32_t>(i + 1));
					}
				}
			});
			Sampled out;
			for (const auto& chunk : chunks) {
				for (const auto at : chunk.changed)
					p.transforms.Mark(at);
				out.changed += chunk.changed.size();
				out.refused += chunk.refused;
			}
			return out;
		}

		// Parity (CS_DCLF_PERSISTENT_PARITY, the moved-only parity's frames): every entry read again at the producer's end against what it
		// sends. The frame's buffer stands all frame (the controllers write the other), so a difference is a write to the buffer the
		// frame's shaders read, or a sampling defect.
		void CheckParity(const Api::FrameValuePoolProducer& a_producer, const TextureTransformList* a_list, std::size_t a_count, std::size_t a_buffer,
			const std::vector<TextureTransformRow>& a_out)
		{
			ZoneScopedN("CS.DCLF.FrameValues.TextureTransformsParity");
			auto& p = produced.parity;
			++p.checks;
			for (std::size_t i = 0; i < a_count; ++i) {
				const auto& entry = a_list->entries[i];
				const auto& row = a_out[i + 1];
				EngineReadWindow::Lease lease;
				if (!Readable(a_producer, lease) || produced.materials[i] != entry.material)
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

		// The line (CS_DCLF_STATS): what the rows carrying TexcoordOffset cost a frame and how many scrolled.
		void CountLine(std::size_t a_count, const Sampled& a_sampled)
		{
			auto& p = produced;
			++p.lineFrames;
			p.lineEntries += a_count;
			p.lineChanged += a_sampled.changed;
			p.lineRefused += a_sampled.refused;
			if (p.lineFrames < 300)
				return;
			if (Diagnostics::Observe().stats) {
				const double n = static_cast<double>(p.lineFrames);
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
					p.lineFrames, p.lineEntries / n, p.lineChanged / n, p.lineRefused, built, per(builtEntries, built), per(unpacked, built),
					per(noTexcoord, built), per(freeSlot, built), per(stale, built), published, per(publishedEntries, published), publishedNone,
					publishedNone ? " <- LIST LOST" : "", adopted, per(adoptedEntries, adopted), adoptedNone);
			}
			p.lineFrames = p.lineEntries = p.lineChanged = p.lineRefused = 0;
		}

		void Sample(Api::FrameValuePoolProducer& a_producer)
		{
			// Every installed material row's TexcoordOffset as this frame's shaders read it: the buffer Main::Update made current, which the
			// controllers leave alone until the next flip (they write the other), so a read anywhere in the window is the frame's value.
			// TextureTransformsCS stores the entries into the payload's rows before the frame's draws.
			ZoneScopedN("CS.DCLF.FrameValues.TextureTransforms");
			auto& p = produced;
			a_producer.BeginBuild(p.transforms);
			auto& out = p.transforms.Mutable();
			const auto* list = a_producer.Input<TextureTransformList>();
			const std::size_t count = list ? list->entries.size() : 0;
			if (out.size() < count + 1) {
				const std::size_t first = out.size();
				out.resize(count + 1, TextureTransformRow{});
				p.transforms.MarkRange(first, count + 1 - first);
			}
			if (p.materials.size() < count)
				p.materials.resize(count, nullptr);
			if (out[0].rowOffset != count) {
				out[0] = TextureTransformRow{};
				out[0].rowOffset = static_cast<std::uint32_t>(count);
				p.transforms.Mark(0);
			}
			// The texture-transform buffer the frame's shaders read (the frame's capture of it, after Main::Update's flip; the controllers
			// write the other one).
			const std::size_t buffer = a_producer.globals ? a_producer.globals->material.vanilla.textureTransformBuffer & 1u : 0u;
			const auto sampled = SampleEntries(a_producer, list, count, buffer, out);
			if (a_producer.parity)
				CheckParity(a_producer, list, count, buffer, out);
			CountLine(count, sampled);
		}

		void Send(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(produced.transforms);
		}
	}

	void RegisterPool(Api::InstallContext& a_context)
	{
		pool = a_context.RegisterFrameValuePool({ .name = "texture-transforms", .stride = sizeof(TextureTransformRow), .kick = &Kick, .sample = &Sample, .send = &Send });
	}

	// Render thread, when a payload is installed (before Kick): the main material rows that carry TexcoordOffset (null: none). From the next
	// Kick each producer samples every entry's material at the frame's buffer into the rows, which TextureTransformsCS stores into the
	// installed payload's rows before the frame's draws.
	void InstallList(std::shared_ptr<const void> a_list)
	{
		auto list = std::static_pointer_cast<const TextureTransformList>(std::move(a_list));
		if (list) {
			TextureTransformStats::adopted.fetch_add(1, std::memory_order_relaxed);
			TextureTransformStats::adoptedEntries.fetch_add(list->entries.size(), std::memory_order_relaxed);
		} else {
			TextureTransformStats::adoptedNone.fetch_add(1, std::memory_order_relaxed);
		}
		installed = std::move(list);
	}

	std::uint32_t RowsIndex()
	{
		return FrameValues::Get().PoolIndex(pool);
	}
}
