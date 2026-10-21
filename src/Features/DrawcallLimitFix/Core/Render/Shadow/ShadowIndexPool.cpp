// The index pool every plain indexed draw binds: its creation, its update to the tables at a commit, and the producer's
// requirement of its capacities.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF
{
	namespace Draws
	{
		std::shared_ptr<IndexPool> EnsureIndexPool(SceneBuffers& a_scene, rhi::Device a_device, bool a_small)
		{
			if (a_scene.pool)
				return a_scene.pool;
			auto pool = std::make_shared<IndexPool>();
			pool->program = ComputeProgram::Load(a_device, { .source = kIndexPoolShader, .constantWords = kIndexPoolConstantWords });
			if (pool->program)
				pool->dispatchSignature = CreateDispatchSignature(a_device, pool->program->layout->GetHandle());
			if (!pool->dispatchSignature) {
				logger::warn("[DCLF] The index pool's copy program could not be created");
				return nullptr;
			}
			// 16-bit indices (a unit: an index), grown to twice what the ranges hold; a first index per geometry slot; a copy per range.
			pool->indices = MakeVersioned(CreateWords((a_small ? 4096u : kInitialPoolIndices) / 2, true, "cs.dclf.index-pool"),
				{ .extent = { 0, sizeof(std::uint16_t), 4 }, .policy = org::services::GrowthPolicy::Doubling(2) });
			pool->firsts = MakeVersioned(CreateWords(a_small ? 64u : kInitialGeometries, false, "cs.dclf.pool-firsts"), { .extent = { 0, 4, 4 } });
			auto copies = org::Buffer::CreateUnmaterializedStructuredBuffer(a_small ? 16u : 1024u, 4 * sizeof(std::uint32_t), false);
			copies->SetName("cs.dclf.pool-copies");
			copies->Materialize();
			pool->copies = MakeVersioned(std::move(copies), { .extent = { 0, 4 * sizeof(std::uint32_t), 4 * sizeof(std::uint32_t) } });
			a_scene.pool = pool;
			return pool;
		}

		/** @brief A geometry slot's indices in the pool: its index buffer's, rounded up to whole words (0: none). */
		std::uint32_t PoolIndicesOf(const SceneTables& a_tables, std::uint32_t a_slot)
		{
			const auto& geometry = a_tables.geometries[a_slot];
			if (!geometry.indexAddress || !geometry.indexBytes)
				return 0;
			return static_cast<std::uint32_t>(std::min<std::uint64_t>((geometry.indexBytes + 3) / 4 * 2, UINT32_MAX & ~1u));
		}

		namespace
		{
			/** @brief UpdateIndexPool's shared locals: the pool, the capacities the adopted set names, and the update's copies. */
			struct PoolUpdate
			{
				IndexPool& p;
				const SceneTables& tables;
				const org::services::NamedEntry& indices;
				std::uint64_t capacity, firstsCapacity, copiesCapacity;
				std::vector<std::array<std::uint32_t, 4>> copies{};
				std::vector<std::uint32_t> changed{};
				bool allFirsts = false;

				void Release(std::uint32_t a_slot)
				{
					auto& range = p.slots[a_slot];
					p.slotFirst[a_slot] = IndexPool::kNoRange;
					if (range.first == IndexPool::kNoRange) {
						range = {};
						return;
					}
					// Free at once: the graph orders this frame's copies after every earlier read of the pool.
					auto [at, inserted] = p.free.emplace(range.first, range.count);
					if (auto next = std::next(at); next != p.free.end() && at->first + at->second == next->first) {
						at->second += next->second;
						p.free.erase(next);
					}
					if (at != p.free.begin()) {
						if (auto previous = std::prev(at); previous->first + previous->second == at->first) {
							previous->second += at->second;
							p.free.erase(at);
						}
					}
					p.indicesHeld -= range.count;
					range = {};
				}

				std::uint32_t Allocate(std::uint32_t a_count)
				{
					for (auto it = p.free.begin(); it != p.free.end(); ++it) {
						if (it->second < a_count)
							continue;
						const std::uint32_t first = it->first, rest = it->second - a_count;
						p.free.erase(it);
						if (rest)
							p.free.emplace(first + a_count, rest);
						return first;
					}
					if (std::uint64_t(p.end) + a_count > capacity)
						return IndexPool::kNoRange;
					const std::uint32_t first = p.end;
					p.end += a_count;
					return first;
				}

				void CopyOf(const IndexPool::Range& a_range)
				{
					copies.push_back({ static_cast<std::uint32_t>(a_range.address), static_cast<std::uint32_t>(a_range.address >> 32), a_range.first,
						static_cast<std::uint32_t>((a_range.bytes + 3) / 4) });
				}

				// Lays every range out again in a new indices version (frames in flight read the old one) and copies it. Only between the
				// releases and the acquires, so every held range matches the tables.
				void Relayout()
				{
					p.free.clear();
					p.end = 0;
					copies.clear();
					for (auto& range : p.slots) {
						if (range.first == IndexPool::kNoRange)
							continue;
						range.first = p.end;
						p.end += range.count;
						CopyOf(range);
					}
					for (std::size_t g = 0; g < p.slots.size(); ++g)
						p.slotFirst[g] = p.slots[g].first;
					allFirsts = true;
				}

				// Each slot owns its range, copied whenever the log names it. Never shared by address: the device reuses a freed
				// buffer's address for the next one, which a cell load does while the unloaded cell's slots still hold theirs.
				void Acquire(std::uint32_t a_slot)
				{
					const auto& geometry = tables.geometries[a_slot];
					const std::uint32_t slotIndices = PoolIndicesOf(tables, a_slot);
					if (!slotIndices)
						return;
					// Past the first-index table or this commit's copies, it waits.
					if (a_slot >= firstsCapacity || copies.size() >= copiesCapacity) {
						p.waiting.push_back(a_slot);
						return;
					}
					const std::uint32_t first = Allocate(slotIndices);
					if (first == IndexPool::kNoRange) {
						// No room though the version holds twice what the ranges do: fragmented. The producer requires a newer version
						// (laid out again whole by the update that first writes it).
						if ((std::uint64_t(p.indicesHeld) + slotIndices) * 2 <= capacity)
							p.fragmentedAt.store(indices.serial, std::memory_order_relaxed);
						p.waiting.push_back(a_slot);
						return;
					}
					auto& range = p.slots[a_slot];
					range = { first, slotIndices, geometry.indexAddress, geometry.indexBytes };
					p.slotFirst[a_slot] = first;
					p.indicesHeld += slotIndices;
					CopyOf(range);
				}

				/** @brief The slots' ranges brought up to the tables: every slot from an empty pool, else the changed slots'. */
				void UpdateRanges(std::uint32_t a_count, std::uint32_t a_generation)
				{
					const auto count = a_count;
					if (!p.cursor.Continues(tables.geometryLog, a_generation) || p.slots.size() > count) {
						// Every slot again, from an empty pool.
						p.free.clear();
						p.end = 0;
						p.indicesHeld = 0;
						p.waiting.clear();
						p.slots.assign(count, {});
						p.slotFirst.assign(count, IndexPool::kNoRange);
						for (std::uint32_t g = 0; g < count; ++g)
							Acquire(g);
						p.cursor.Restart(a_generation);
						allFirsts = true;
					} else {
						const auto first = static_cast<std::uint32_t>(p.slots.size());
						p.slots.resize(count);
						p.slotFirst.resize(count, IndexPool::kNoRange);
						for (std::uint32_t g = 0; g < count; ++g)
							if (g >= first)
								changed.push_back(g);
						for (const std::uint32_t g : p.cursor.Unread(tables.geometryLog))
							if (g < first)
								changed.push_back(g);
						for (const std::uint32_t g : p.waiting)
							if (g < count)
								changed.push_back(g);
						p.waiting.clear();
						std::sort(changed.begin(), changed.end());
						changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
						// Every release before any acquire; an indices version this update did not lay out is laid out in between.
						for (const std::uint32_t g : changed)
							Release(g);
						if (!p.indicesFilled.Holds(indices))
							Relayout();
						for (const std::uint32_t g : changed)
							Acquire(g);
					}
				}

				// First indices: all of them into a version this update did not fill (a new version holds nothing), else only the changed
				// slots.
				void UploadFirsts(const org::services::NamedEntry& a_firsts, std::uint32_t a_count, CommitUploads& a_uploads)
				{
					const auto& firsts = a_firsts;
					const auto count = a_count;
					if (!p.firstsFilled.Holds(firsts))
						allFirsts = true;
					p.firstsFilled.Filled(firsts);
					const auto held = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, firstsCapacity));
					if (allFirsts) {
						if (held)
							a_uploads(p.firsts, p.slotFirst.data(), std::size_t(held) * sizeof(std::uint32_t), 0);
					} else {
						for (std::size_t i = 0; i < changed.size() && changed[i] < held;) {
							std::size_t j = i + 1;
							while (j < changed.size() && changed[j] == changed[j - 1] + 1 && changed[j] < held)
								++j;
							a_uploads(p.firsts, &p.slotFirst[changed[i]], (j - i) * sizeof(std::uint32_t), std::uint64_t(changed[i]) * sizeof(std::uint32_t));
							i = j;
						}
					}
				}

				// A copy per range, every range a slot of the tables the revision required the copies for: past them is a defect of that
				// requirement, never copies to drop.
				std::uint32_t UploadCopies(const org::LatchBlock& a_latch, std::uint32_t a_latchSlot, std::uint32_t a_poolOffset, CommitUploads& a_uploads)
				{
					const auto copyCount = static_cast<std::uint32_t>(copies.size());
					if (copyCount > copiesCapacity)
						stl::report_and_fail(fmt::format("Drawcall Limit Fix: the index pool's {} copies past the {} its buffer holds (the requirement missed them)", copyCount,
							copiesCapacity));
					if (copyCount)
						a_uploads(p.copies, copies.data(), std::size_t(copyCount) * sizeof(copies[0]), 0);
					const std::uint32_t dispatch[4] = { std::min(copyCount, kIndexPoolGroupsX), (copyCount + kIndexPoolGroupsX - 1) / kIndexPoolGroupsX, 1, copyCount };
					LatchWrite(a_latch, "shadow index-pool dispatch", a_latchSlot, a_poolOffset, std::as_bytes(std::span(dispatch)));
					return copyCount;
				}
			};
		}

		/**
		 * @brief Brings the index pool up to the tables, in the versions a_sealed (the adopted set) names: slots the geometry log named
		 * since the last read (every slot when it cannot read on) release their ranges and take new ones with a copy. Uploads the first
		 * indices and copies and writes the copy dispatch into the latch; IndexPoolPass runs it before the views draw. Returns the copies.
		 */
		std::uint32_t UpdateIndexPool(const NamedVersions& a_sealed, IndexPool& a_pool, const SceneTables& a_tables, std::uint32_t a_generation, const org::LatchBlock& a_latch,
			std::uint32_t a_latchSlot, std::uint32_t a_poolOffset, CommitUploads& a_uploads)
		{
			auto& p = a_pool;
			const auto count = static_cast<std::uint32_t>(a_tables.geometries.size());
			// The capacities the revision producer required (Impl::RequireIndexPool), named by the adopted set: a range that does not
			// fit waits (only unclaimed geometry can: the claims' room was required).
			const auto& indices = a_sealed.Of(p.indices);
			const auto& firsts = a_sealed.Of(p.firsts);
			PoolUpdate update{ p, a_tables, indices, indices.units, firsts.units, a_sealed.Of(p.copies).units };
			update.UpdateRanges(count, a_generation);
			p.cursor.Advance(a_tables.geometryLog);
			p.indicesFilled.Filled(indices);
			Diagnostics::Notify(Diagnostics::IndexPoolUpdated{ p, a_tables, count });

			update.UploadFirsts(firsts, count, a_uploads);
			return update.UploadCopies(a_latch, a_latchSlot, a_poolOffset, a_uploads);
		}
	}

	void IndirectDraws::Impl::RequireIndexPool(GrowthBuild& a_build, SceneBuffers& a_scene, const SceneTables& a_tables, std::uint32_t a_generation)
	{
		if (!a_scene.pool)
			return;
		auto& p = *a_scene.pool;
		// The producer's bound: every slot's indices, kept from the geometry log by the revision code alone.
		auto& b = producer.poolBound;
		const auto count = static_cast<std::uint32_t>(a_tables.geometries.size());
		auto take = [&](std::uint32_t a_slot) {
			const std::uint32_t indices = PoolIndicesOf(a_tables, a_slot);
			b.total = b.total + indices - b.indices[a_slot];
			b.indices[a_slot] = indices;
		};
		if (!b.cursor.Continues(a_tables.geometryLog, a_generation) || b.indices.size() > count) {
			b.indices.assign(count, 0u);
			b.total = 0;
			for (std::uint32_t g = 0; g < count; ++g)
				take(g);
			b.cursor.Restart(a_generation);
		} else {
			const auto first = static_cast<std::uint32_t>(b.indices.size());
			b.indices.resize(count, 0u);
			for (std::uint32_t g = first; g < count; ++g)
				take(g);
			for (const std::uint32_t g : b.cursor.Unread(a_tables.geometryLog))
				if (g < first)
					take(g);
		}
		b.cursor.Advance(a_tables.geometryLog);
		// Room for twice the indices (a relayout needs no more), a first index per slot and a copy per range, so together with the
		// claims; and a version newer than one the frame found fragmented (laid out again at the same size).
		auto& buffers = SceneBufferSet::Get();
		buffers.Require(a_build, p.indices, std::uint64_t(b.total) * 2);
		buffers.RequireNewer(a_build, p.indices, p.fragmentedAt.load(std::memory_order_relaxed));
		UnitGroup{}.Add(p.firsts).Add(p.copies).Require(a_build, std::max(count, 1u));
	}
}
