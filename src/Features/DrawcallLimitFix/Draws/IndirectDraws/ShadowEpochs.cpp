#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Internal.h"
#	include "Features/DrawcallLimitFix/Engine/LocalLightCull.h"

namespace DCLF
{
	namespace Draws
	{
		/**
		 * @brief A shadow view's latch as every view has it: the dispatch over its inputs, every one drawn (a single phase,
		 * no engine-visibility gate), its view-projection with the eye folded in, and the frame's stamp. The culling's
		 * flags and planes are the caller's.
		 */
		BuildDrawsLatch ShadowViewLatch(const PendingView& a_view, std::uint32_t a_inputs, std::uint32_t a_frame)
		{
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (a_inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = a_inputs;
			latch.visibilityStamp = a_frame & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
			FoldEyeIntoViewProj(a_view.viewProj, a_view.eye, latch.viewProj);
			return latch;
		}

		RowBuckets BucketsOfRow(std::span<const std::uint32_t> a_row, const ShadowIndirectState& a_indirect)
		{
			RowBuckets out;
			const auto published = static_cast<std::uint32_t>(a_indirect.pipelines.size());
			for (const auto pipeline : a_row)
				if (pipeline != Lookups::kNone && pipeline < published)
					out.pipelines.push_back(pipeline);
			std::sort(out.pipelines.begin(), out.pipelines.end());
			out.pipelines.erase(std::unique(out.pipelines.begin(), out.pipelines.end()), out.pipelines.end());
			std::stable_partition(out.pipelines.begin(), out.pipelines.end(), [&](std::uint32_t a_pipeline) { return a_indirect.discards[a_pipeline] == 0; });
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> bucketOf;
			for (std::uint32_t b = 0; b < out.pipelines.size(); ++b)
				bucketOf.emplace(out.pipelines[b], b);
			out.bucketOfSlot.assign(a_row.size(), Lookups::kNone);
			for (std::size_t k = 0; k < a_row.size(); ++k)
				if (const auto it = bucketOf.find(a_row[k]); it != bucketOf.end())
					out.bucketOfSlot[k] = it->second;
			return out;
		}

		/**
		 * @brief A view's buckets: each sized for every draw its key slots' inputs can produce (ShadowPayload::keySlotDraws), and
		 * laid out back to back in the slot's sequences. A bucket keeps the capacity its slot's previous view gave it while the
		 * draws fit, and grows to a power of two past it, so that the recorded draws (their offsets and max counts) change only
		 * when one grows. The slot holds twice the scene's draw bound (ReserveShadowSequences); past that, the exact sizes,
		 * which add up to the mode's draws, fit.
		 */
		std::vector<ShadowBucket> SizeShadowBuckets(const RowBuckets& a_row, std::span<const std::uint32_t> a_slotDraws, const ShadowFrameView* a_previous,
			std::uint64_t a_slotSequences)
		{
			std::vector<std::uint64_t> need(a_row.pipelines.size(), 0);
			for (std::size_t k = 0; k < a_row.bucketOfSlot.size() && k < a_slotDraws.size(); ++k)
				if (a_row.bucketOfSlot[k] != Lookups::kNone)
					need[a_row.bucketOfSlot[k]] += a_slotDraws[k];
			std::vector<ShadowBucket> out(need.size());
			const bool samePipelines = a_previous && a_previous->buckets.size() == out.size() &&
			                           std::equal(a_previous->buckets.begin(), a_previous->buckets.end(), a_row.pipelines.begin(),
										   [](const ShadowBucket& a_bucket, std::uint32_t a_pipeline) { return a_bucket.pipeline == a_pipeline; });
			std::uint64_t total = 0, exact = 0;
			for (std::size_t b = 0; b < out.size(); ++b) {
				const std::uint64_t previous = samePipelines ? a_previous->buckets[b].capacity : 0;
				const std::uint64_t capacity = need[b] <= previous ? previous : std::bit_ceil(need[b]);
				out[b].pipeline = a_row.pipelines[b];
				out[b].capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity, UINT32_MAX));
				total += out[b].capacity;
				exact += need[b];
			}
			if (exact > a_slotSequences)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: a shadow view's {} draws past its slot's {} sequences (the scene's draw bound missed them)",
					exact, a_slotSequences));
			if (total > a_slotSequences)
				for (std::size_t b = 0; b < out.size(); ++b)
					out[b].capacity = static_cast<std::uint32_t>(need[b]);
			std::uint32_t first = 0;
			for (auto& bucket : out) {
				bucket.first = first;
				first += bucket.capacity;
			}
			return out;
		}

		/** @brief Writes a view's bucket table into its slot's region of the frame slot's latch, and points a_latch at it. */
		void WriteBucketTable(ShadowResources& a_resources, std::uint32_t a_latchSlot, std::uint32_t a_slot, std::span<const ShadowBucket> a_buckets,
			BuildDrawsLatch& a_latch)
		{
			const auto& layout = a_resources.latchLayout;
			// A view has at most a bucket per key slot, and the layout a table's worth per key slot (ReserveShadowLatch).
			if (a_buckets.size() > layout.keySlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} shadow buckets past the latch's {} key slots", a_buckets.size(), layout.keySlots));
			std::vector<std::uint32_t> table;
			table.reserve(a_buckets.size() * 2);
			for (const auto& bucket : a_buckets) {
				table.push_back(bucket.first);
				table.push_back(bucket.capacity);
			}
			const std::uint32_t offset = layout.BucketOffset(a_slot);
			if (!table.empty())
				a_resources.latch->Write(a_latchSlot, offset, std::as_bytes(std::span(table)));
			a_latch.bucketTableOffset = static_cast<std::uint32_t>(a_resources.latch->Offset(a_latchSlot)) + offset;
		}

		/**
		 * @brief Brings the index pool (IndexPool) up to the tables: the slots the geometry log names since it last read it
		 * (every slot when it cannot read on) let go of their ranges and take their buffers' ranges, a buffer with no range yet
		 * getting one and a copy. The commit uploads the slots' first indices and the copies, and writes the copies' dispatch into
		 * a_latchSlot's region of the latch; the epoch's IndexPoolPass runs them before the views draw.
		 */
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
			pool->capacity = a_small ? 4096u : kInitialPoolIndices;
			pool->indices = MakeVersioned(CreateWords(pool->capacity / 2, true, "cs.dclf.index-pool"));
			pool->firstsCapacity = a_small ? 64u : kInitialGeometries;
			pool->firsts = MakeVersioned(CreateWords(pool->firstsCapacity, false, "cs.dclf.pool-firsts"));
			pool->copiesCapacity = a_small ? 16u : 1024u;
			auto copies = org::Buffer::CreateUnmaterializedStructuredBuffer(pool->copiesCapacity, 4 * sizeof(std::uint32_t), false);
			copies->SetName("cs.dclf.pool-copies");
			copies->Materialize();
			pool->copies = MakeVersioned(std::move(copies));
			a_scene.pool = pool;
			return pool;
		}

		void UpdateIndexPool(IndexPool& a_pool, const SceneStore::Tables& a_tables, std::uint32_t a_generation, org::LatchBlock& a_latch,
			std::uint32_t a_latchSlot, std::uint32_t a_poolOffset, CommitUploads& a_uploads)
		{
			auto& p = a_pool;
			const auto count = static_cast<std::uint32_t>(a_tables.geometries.size());
			std::vector<std::array<std::uint32_t, 4>> copies;
			std::vector<std::uint32_t> changed;
			bool allFirsts = false;
			auto release = [&](std::uint32_t a_slot) {
				auto& range = p.slots[a_slot];
				p.slotFirst[a_slot] = IndexPool::kNoRange;
				if (range.first == IndexPool::kNoRange) {
					range = {};
					return;
				}
				// Back on the free list at once: this frame's copies are ordered after every earlier read of the pool (the
				// graph's barrier between the views' index reads and the copy's writes).
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
			};
			auto allocate = [&](std::uint32_t a_count) -> std::uint32_t {
				for (auto it = p.free.begin(); it != p.free.end(); ++it) {
					if (it->second < a_count)
						continue;
					const std::uint32_t first = it->first, rest = it->second - a_count;
					p.free.erase(it);
					if (rest)
						p.free.emplace(first + a_count, rest);
					return first;
				}
				if (std::uint64_t(p.end) + a_count > p.capacity)
					return IndexPool::kNoRange;
				const std::uint32_t first = p.end;
				p.end += a_count;
				return first;
			};
			auto copyOf = [&](const IndexPool::Range& a_range) {
				copies.push_back({ static_cast<std::uint32_t>(a_range.address), static_cast<std::uint32_t>(a_range.address >> 32), a_range.first,
					static_cast<std::uint32_t>((a_range.bytes + 3) / 4) });
			};
			// A full pool: twice what it must hold, every range laid out again from the start and copied again (a new backing,
			// so frames in flight keep reading the old one). Only between the releases and the acquires below, so every range
			// held is its slot's buffer as the tables have it now.
			auto grow = [&](std::uint32_t a_more) {
				std::uint64_t needed = std::uint64_t(p.indicesHeld) + a_more;
				std::uint64_t capacity = std::max<std::uint64_t>(p.capacity, 2);
				while (capacity < needed * 2)
					capacity *= 2;
				logger::info("[DCLF] index pool: {} indices grown to {} ({} MB)", p.capacity, capacity, capacity * 2 >> 20);
				p.capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity, UINT32_MAX & ~1u));
				// The new version's layout: every range laid out again from its start, and copied into it.
				Adopt(std::move(Growth{}.Structured(p.indices, p.capacity / 2)), [&] {
					++p.layout;
					p.free.clear();
					p.end = 0;
					copies.clear();
					for (auto& range : p.slots) {
						if (range.first == IndexPool::kNoRange)
							continue;
						range.first = p.end;
						p.end += range.count;
						copyOf(range);
					}
					for (std::size_t g = 0; g < p.slots.size(); ++g)
						p.slotFirst[g] = p.slots[g].first;
					allFirsts = true;
				});
			};
			// A slot's own range of its index buffer, copied whenever the log names the slot: a range is never shared, because
			// a buffer address is no identity - the device hands a freed buffer's address to the next buffer it creates, which a
			// cell's load does while the unloaded cell's slots still hold theirs.
			auto acquire = [&](std::uint32_t a_slot) {
				const auto& geometry = a_tables.geometries[a_slot];
				if (!geometry.indexAddress || !geometry.indexBytes)
					return;
				const std::uint32_t indices = static_cast<std::uint32_t>(std::min<std::uint64_t>((geometry.indexBytes + 3) / 4 * 2, UINT32_MAX & ~1u));
				std::uint32_t first = allocate(indices);
				if (first == IndexPool::kNoRange) {
					grow(indices);
					first = allocate(indices);
				}
				auto& range = p.slots[a_slot];
				range = { first, indices, geometry.indexAddress, geometry.indexBytes };
				p.slotFirst[a_slot] = first;
				p.indicesHeld += indices;
				copyOf(range);
			};

			if (!p.cursor.Continues(a_tables.geometryLog, a_generation) || p.slots.size() > count) {
				// Every slot again, from an empty pool.
				p.free.clear();
				p.end = 0;
				p.indicesHeld = 0;
				p.slots.assign(count, {});
				p.slotFirst.assign(count, IndexPool::kNoRange);
				for (std::uint32_t g = 0; g < count; ++g)
					acquire(g);
				p.cursor.Restart(a_generation);
				allFirsts = true;
			} else {
				const auto first = static_cast<std::uint32_t>(p.slots.size());
				p.slots.resize(count);
				p.slotFirst.resize(count, IndexPool::kNoRange);
				for (std::uint32_t g = 0; g < count; ++g)
					if (g >= first)
						changed.push_back(g);
				for (const std::uint32_t g : p.cursor.Unread(a_tables.geometryLog))
					if (g < first)
						changed.push_back(g);
				std::sort(changed.begin(), changed.end());
				changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
				// Every release before any acquire.
				for (const std::uint32_t g : changed)
					release(g);
				for (const std::uint32_t g : changed)
					acquire(g);
			}
			p.cursor.Advance(a_tables.geometryLog);
			// Build parity: every range against its slot's buffer as the tables have it now (a slot whose buffer changed with no
			// log entry would keep drawing the old buffer's indices).
			if (SwitchEnabled(Switch::BuildParity) && (++p.checks % 300) == 0) {
				std::uint32_t held = 0, stale = 0, unheld = 0, firstStale = IndexPool::kNoRange;
				for (std::uint32_t g = 0; g < count; ++g) {
					const auto& geometry = a_tables.geometries[g];
					const auto& range = p.slots[g];
					const bool wants = geometry.indexAddress && geometry.indexBytes;
					if (range.first == IndexPool::kNoRange) {
						unheld += wants ? 1 : 0;
						continue;
					}
					++held;
					if (!wants || range.address != geometry.indexAddress || range.bytes != geometry.indexBytes) {
						if (stale++ == 0)
							firstStale = g;
					}
				}
				logger::info("[DCLF] index pool parity: {} slots, {} ranges ({} indices held of {}), {} stale, {} without a range{}{}", count, held, p.indicesHeld,
					p.capacity, stale, unheld, stale || unheld ? " <- INDEX POOL" : " <- OK",
					firstStale != IndexPool::kNoRange ? fmt::format("; first stale: slot {}", firstStale) : std::string());
			}

			// The slots' first indices: every one into a new backing, else the slots changed.
			if (count > p.firstsCapacity) {
				while (p.firstsCapacity < count)
					p.firstsCapacity *= 2;
				Adopt(std::move(Growth{}.Structured(p.firsts, p.firstsCapacity)), [&] {
					++p.layout;
					allFirsts = true;
				});
			}
			if (allFirsts) {
				if (count)
					a_uploads(p.firsts, p.slotFirst.data(), std::size_t(count) * sizeof(std::uint32_t), 0);
			} else {
				for (std::size_t i = 0; i < changed.size();) {
					std::size_t j = i + 1;
					while (j < changed.size() && changed[j] == changed[j - 1] + 1)
						++j;
					a_uploads(p.firsts, &p.slotFirst[changed[i]], (j - i) * sizeof(std::uint32_t), std::uint64_t(changed[i]) * sizeof(std::uint32_t));
					i = j;
				}
			}
			// The copies, and their dispatch.
			const auto copyCount = static_cast<std::uint32_t>(copies.size());
			if (copyCount > p.copiesCapacity) {
				while (p.copiesCapacity < copyCount)
					p.copiesCapacity *= 2;
				Adopt(std::move(Growth{}.Structured(p.copies, p.copiesCapacity)), [&] { ++p.layout; });
			}
			if (copyCount)
				a_uploads(p.copies, copies.data(), copies.size() * sizeof(copies[0]), 0);
			const std::uint32_t dispatch[4] = { std::min(copyCount, kIndexPoolGroupsX), (copyCount + kIndexPoolGroupsX - 1) / kIndexPoolGroupsX, 1, copyCount };
			a_latch.Write(a_latchSlot, a_poolOffset, std::as_bytes(std::span(dispatch)));
		}

		/** @brief The previous shape's view of a slot, if it had one. */
		const ShadowFrameView* PreviousView(const std::shared_ptr<const ShadowFrame>& a_previous, std::uint32_t a_slot)
		{
			if (a_previous)
				for (const auto& previous : a_previous->views)
					if (previous.slot == a_slot)
						return &previous;
			return nullptr;
		}

		/**
		 * @brief Points a_latch at its rasterizer state's row of the pipeline map, and writes the row into the frame slot's
		 * latch block when a_write (once per state and frame): each key slot's bucket (RowBuckets). The shader reads the row at
		 * an offset into the whole latch block, so the offset carries the frame slot's base: a slot-relative one read slot 0's
		 * rows, which async epochs never write (every draw got bucket 0).
		 */
		void UseShadowMapRow(ShadowResources& a_resources, const RowBuckets& a_buckets, std::uint32_t a_latchSlot, std::uint32_t a_rasterState, bool a_write,
			BuildDrawsLatch& a_latch)
		{
			const auto& layout = a_resources.latchLayout;
			// Every registered state has its row (ReserveShadowLatch, before the epoch): past it is a defect of that reserve.
			if (a_rasterState == 0 || a_rasterState > layout.rasterStates)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: shadow view rasterizer state {} past the latch's {} rows", a_rasterState, layout.rasterStates));
			const std::uint32_t mapRowOffset = layout.MapOffset() + (a_rasterState - 1) * layout.MapRowBytes();
			a_latch.pipelineMapOffset = static_cast<std::uint32_t>(a_resources.latch->Offset(a_latchSlot)) + mapRowOffset;
			if (!a_write)
				return;
			// Every key slot fits its row (ReserveShadowLatch, before the epoch): past it is a defect of that reserve.
			const auto& row = a_buckets.bucketOfSlot;
			if (row.size() > layout.keySlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} shadow key slots past the latch's {}", row.size(), layout.keySlots));
			if (!row.empty())
				a_resources.latch->Write(a_latchSlot, mapRowOffset, std::as_bytes(std::span(row)));
		}

		/**
		 * @brief Writes the sun's full-frustum processes into a frame slot's region of the shadow latch block (ShadowLatchLayout::
		 * SunEntryOffset) and returns where, in bytes into the block, as a sun view's latch names it (BuildDrawsLatch::sunEntryOffset).
		 */
		std::uint32_t WriteSunEntryRegion(ShadowResources& a_resources, std::uint32_t a_latchSlot, std::span<const SunEntryProcess> a_processes)
		{
			const auto& layout = a_resources.latchLayout;
			// Every process fits (ReserveShadowLatch, before the epoch): past it is a defect of that reserve.
			if (a_processes.size() > layout.sunProcesses)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} sun full-frustum processes past the latch's {}", a_processes.size(), layout.sunProcesses));
			const std::uint32_t header[4] = { static_cast<std::uint32_t>(a_processes.size()), 0, 0, 0 };
			a_resources.latch->Write(a_latchSlot, layout.SunEntryOffset(), std::as_bytes(std::span(header)));
			if (!a_processes.empty())
				a_resources.latch->Write(a_latchSlot, layout.SunEntryOffset() + kSunRegionHeader, std::as_bytes(a_processes));
			return static_cast<std::uint32_t>(a_resources.latch->Offset(a_latchSlot)) + layout.SunEntryOffset();
		}

		/**
		 * @brief Writes a view's blocks (its view block, its per-frame block) into its slot's region of the frame slot's latch, where
		 * the epoch's latched copies (ShadowLatchedCopiesPass) take them to the view-blocks buffer, and points the frame there.
		 */
		void WriteViewBlocks(const ShadowResources& a_resources, std::uint32_t a_latchSlot, std::uint32_t a_slot, const PendingView& a_view, ShadowFrame& a_frame)
		{
			const std::uint32_t offset = a_resources.latchLayout.ViewBlockOffset(a_slot);
			a_resources.latch->Write(a_latchSlot, offset, std::as_bytes(std::span(a_view.viewBlock)));
			// PerTechnique's c3, DCLFDepthRange (Utility.hlsl): the view's viewport depth range, which its draws apply under a [0, 1]
			// viewport (FrameViewOf), so the engine changing it changes no recording.
			const float depthRange[4] = { a_view.minDepth, a_view.maxDepth - a_view.minDepth, 0.0f, 0.0f };
			a_resources.latch->Write(a_latchSlot, offset + static_cast<std::uint32_t>(sizeof(a_view.viewBlock)), std::as_bytes(std::span(depthRange)));
			a_resources.latch->Write(a_latchSlot, offset + static_cast<std::uint32_t>(kShadowPerFrameOffset), std::span(a_view.perFrame.data(), a_view.perFrameBytes));
			a_frame.viewBlocksOffset = a_resources.latchLayout.ViewBlockOffset(0);
			a_frame.zeros = a_resources.zeros;
		}

		/**
		 * @brief Where a view draws: its slot's buffers, its viewport and depth range in its target's slice, and its push data
		 * (DrawPipelines.h, kShadowPushWords): the frame record, its own blocks at its slot's row of the view blocks, and the build's.
		 */
		ShadowFrameView FrameViewOf(const PendingView& a_view, std::uint32_t a_slot, std::uint32_t a_mode, std::uint32_t a_target, std::uint32_t a_capacity,
			const ShadowResources& a_resources, const ShadowPayload& a_payload)
		{
			ShadowFrameView out{};
			out.slot = a_slot;
			out.modeIndex = a_mode;
			out.capacity = a_capacity;
			out.x = a_view.x;
			out.y = a_view.y;
			out.width = a_view.width;
			out.height = a_view.height;
			// [0, 1]: the view's depth range is its draws' (DCLFDepthRange, WriteViewBlocks).
			out.minDepth = 0.0f;
			out.maxDepth = 1.0f;
			out.target = a_target;
			out.slice = a_view.slice;
			out.materialRows = a_resources.materialRows.address;
			out.sequenceDraws = a_resources.sequenceDraws[a_slot];
			const std::uint64_t base = a_resources.constantsAddress;
			const std::uint64_t viewBlock = a_resources.viewBlocks.address + std::uint64_t(a_slot) * kShadowViewSlotBytes;
			auto push = [&](std::uint32_t a_word, std::uint64_t a_address) {
				out.push[a_word] = static_cast<std::uint32_t>(a_address);
				out.push[a_word + 1] = static_cast<std::uint32_t>(a_address >> 32);
			};
			push(kShadowPushFrameRecord, base + kShadowFrameRecordOffset);
			push(kShadowPushViewBlock, viewBlock);
			push(kShadowPushPerFrame, viewBlock + kShadowPerFrameOffset);
			push(kShadowPushZeros, a_payload.zerosAddress);
			push(kShadowPushSharedData, a_payload.sharedDataAddress);
			push(kShadowPushFeatureData, a_payload.featureDataAddress);
			return out;
		}

		/**
		 * @brief A view's max count: its mode's draws, within its slot's sequence buffer, which holds every draw the scene can
		 * produce (ReserveShadowSequences, before the epoch). A mode past it is a defect of that bound, never draws to drop.
		 */
		std::uint32_t ShadowViewCapacity(std::uint32_t a_previous, std::uint32_t a_draws, std::uint32_t a_slotDraws)
		{
			if (a_draws > a_slotDraws)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: a shadow view's {} draws past its sequence buffer's {} (the scene's draw bound missed them)",
					a_draws, a_slotDraws));
			return GrowCapacity(a_previous, a_draws, a_slotDraws);
		}

		/**
		 * @brief The id DCLF's pipelines give the engine's rasterizer state at (fill, cull, bias, scissor) for a_renderMode: 0
		 * when an index is out of the engine's table or its entry is empty.
		 */
		std::uint32_t EngineRasterStateId(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_bias, std::uint32_t a_scissor, std::uint32_t a_renderMode)
		{
			if (a_fill >= 2 || a_cull >= 3 || a_bias >= 12 || a_scissor >= 2)
				return 0;
			auto* engineState = EngineRasterStates()[a_fill][a_cull][a_bias][a_scissor];
			if (!engineState)
				return 0;
			D3D11_RASTERIZER_DESC desc{};
			engineState->GetDesc(&desc);
			return DrawPipelines::Get().ShadowRasterStateId(desc, a_renderMode);
		}

		/** @brief Where the engine has just drawn a view, from the renderer's state: the depth target's format, the viewport and the eye. */
		void CaptureViewTarget(PendingView& a_view, std::uint32_t a_target)
		{
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsv->GetDesc(&dsvDesc);
				a_view.dsvFormat = dsvDesc.Format;
			}
			a_view.x = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftX));
			a_view.y = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftY));
			a_view.width = static_cast<std::uint32_t>(shadowState.viewPort.Width);
			a_view.height = static_cast<std::uint32_t>(shadowState.viewPort.Height);
			a_view.minDepth = shadowState.viewPort.MinDepth;
			a_view.maxDepth = shadowState.viewPort.MaxDepth;
			a_view.eye = shadowState.posAdjust.getEye();
		}

		/**
		 * @brief VS_PerFrame (b12) as the engine wrote it for the view, taken now because the next view rewrites it: from the
		 * mirror, or from Community Shaders' copy of the same buffer (Globals: CacheFramebuffer) until the mirror has seen a
		 * write. Its view-projection is what the view's draws use.
		 */
		void CapturePerFrame(PendingView& a_view)
		{
			auto& mirror = ConstantMirror::Get();
			if (auto* perFrame = *globals::game::perFrame.get()) {
				mirror.Watch(perFrame);
				const auto contents = mirror.Contents(perFrame);
				if (contents.size() >= 48 * sizeof(float)) {
					a_view.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), a_view.perFrame.size()));
					std::memcpy(a_view.perFrame.data(), contents.data(), a_view.perFrameBytes);
				}
			}
			if (!a_view.perFrameBytes) {
				const auto& cached = globals::game::frameBufferCached.data;
				static_assert(sizeof(cached) >= 48 * sizeof(float) && sizeof(cached) <= 1024);
				a_view.perFrameBytes = sizeof(cached);
				std::memcpy(a_view.perFrame.data(), &cached, sizeof(cached));
			}
			std::memcpy(a_view.viewProj.data(), reinterpret_cast<const float*>(a_view.perFrame.data()) + 32, sizeof(float) * 16);
			a_view.hasViewProj = true;
		}
	}

	namespace
	{
		/** @brief BuildDrawsCS's Culled, on the CPU: the latch's planes, then its view-projection's frustum over the bound's box. */
		bool CulledByLatch(const BuildDrawsLatch& a_latch, const RE::NiBound& a_bound, std::string* a_why = nullptr)
		{
			const float c[3] = { a_bound.center.x, a_bound.center.y, a_bound.center.z };
			const float r = a_bound.radius;
			for (std::uint32_t p = 0; p < 6; ++p)
				if ((a_latch.cullPlaneMask & (1u << p)) &&
					a_latch.cullPlanes[p][0] * c[0] + a_latch.cullPlanes[p][1] * c[1] + a_latch.cullPlanes[p][2] * c[2] - a_latch.cullPlanes[p][3] < -r) {
					if (a_why)
						*a_why = fmt::format("cull plane {} of mask {:#x}, distance {:.0f}", p, a_latch.cullPlaneMask,
							a_latch.cullPlanes[p][0] * c[0] + a_latch.cullPlanes[p][1] * c[1] + a_latch.cullPlanes[p][2] * c[2] - a_latch.cullPlanes[p][3]);
					return true;
				}
			const bool noNear = (a_latch.cullFlags & kCullNoNearPlane) != 0;
			bool out[6] = { true, true, true, true, true, true };
			for (std::uint32_t corner = 0; corner < 8; ++corner) {
				const float x = c[0] + ((corner & 1) ? r : -r), y = c[1] + ((corner & 2) ? r : -r), z = c[2] + ((corner & 4) ? r : -r);
				float clip[4];
				for (std::uint32_t row = 0; row < 4; ++row)
					clip[row] = a_latch.viewProj[row * 4 + 0] * x + a_latch.viewProj[row * 4 + 1] * y + a_latch.viewProj[row * 4 + 2] * z + a_latch.viewProj[row * 4 + 3];
				out[0] = out[0] && clip[0] < -clip[3];
				out[1] = out[1] && clip[0] > clip[3];
				out[2] = out[2] && clip[1] < -clip[3];
				out[3] = out[3] && clip[1] > clip[3];
				out[4] = out[4] && clip[2] < 0 && !noNear;
				out[5] = out[5] && clip[2] > clip[3];
			}
			if (a_why)
				for (std::uint32_t side = 0; side < 6; ++side)
					if (out[side]) {
						*a_why = fmt::format("clip side {} (near plane {})", side, noNear ? "off" : "on");
						break;
					}
			return out[0] || out[1] || out[2] || out[3] || out[4] || out[5];
		}
	}

	/**
	 * CS_DCLF_SET_PARITY: a sun view's culling against the engine's own cascade cull of the same frame - every caster of the set the
	 * engine's cull of the view's cascade reached (SunAccumulation::ClaimedRegistrations) must pass the view's latch, or it is
	 * drawn by nobody: the engine skips it as the set's, and DCLF's view rejects it.
	 */
	void IndirectDraws::Impl::CheckCascadeCulling(const PendingView& a_view, const BuildDrawsLatch& a_latch, std::uint32_t a_frame, const ShadowPayload& a_payload)
	{
		static std::uint64_t checks = 0, registered = 0, rejected = 0, offscreen = 0;
		static std::string first;
		if (!a_view.sunView || a_view.casterClass != 0)
			return;
		std::uint32_t frame = 0;
		const auto& registrations = SunAccumulation::Get().ClaimedRegistrations(frame);
		if (frame != a_frame)
			return;
		const auto* shadowView = ShadowViews::Get().At(a_view.viewId);
		if (!shadowView)
			return;
		++checks;
		for (const auto& registration : registrations) {
			if (registration.descriptor != shadowView->descriptor || !registration.geometry)
				continue;
			++registered;
			std::string why;
			if (CulledByLatch(a_latch, registration.geometry->worldBound, &why)) {
				// Wholly past a side of the view's own projection (the bound's box against its clip x or y): the engine's looser sphere
				// cull registers it, but its draw would write no texel either. Not a caster nobody draws.
				if (why.starts_with("clip side 0") || why.starts_with("clip side 1") || why.starts_with("clip side 2") || why.starts_with("clip side 3")) {
					++offscreen;
					continue;
				}
				++rejected;
				if (first.empty()) {
					const auto& b = registration.geometry->worldBound;
					first = fmt::format("'{}' in cascade {} (view {}), bound ({:.0f} {:.0f} {:.0f}) r {:.0f}: {}", registration.geometry->name.c_str() ? registration.geometry->name.c_str() : "?",
						shadowView->descriptor, a_view.viewId, b.center.x, b.center.y, b.center.z, b.radius, why);
				}
			}
		}
		// The other direction: what the view draws that covers most of the map at the light's near plane (a caster in front of
		// everything), and anything drawn whose world transform is far from its own bound. From the records the GPU reads.
		{
			const auto& records = objectStore.records.Get();
			const auto& tablesNow = SceneStore::Get().GetTables();
			ankerl::unordered_dense::set<const RE::BSGeometry*> reached;
			for (const auto& registration : registrations)
				if (registration.descriptor == shadowView->descriptor)
					reached.insert(registration.geometry);
			struct Cover
			{
				float area;
				std::uint32_t object;
				float maxZ;
			};
			std::vector<Cover> covers;
			std::uint32_t drawn = 0, farWorld = 0, staleBuffers = 0;
			std::string farFirst, staleFirst;
			a_payload.ForEachInput(a_view.modeIndex, [&](const DrawInput& a_input) {
				const std::uint32_t object = a_input.objectIndex;
				if (object >= records.size() || (a_input.flags & kObjectVolumetricOnly) || OutsideSunEntry(a_payload.inputs, tablesNow, object))
					return;
				const auto& record = records[object];
				RE::NiBound bound;
				bound.center = { record.bound[0], record.bound[1], record.bound[2] };
				bound.radius = record.bound[3];
				if (CulledByLatch(a_latch, bound))
					return;
				++drawn;
				// The geometry row the draw pulls from against the buffers the object's TriShape holds now.
				if (const auto* geometry = object < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[object] : nullptr;
					geometry && !geometry->GetGeometryRuntimeData().skinInstance && a_input.geometryIndex < tablesNow.geometries.size() &&
					a_input.geometryIndex < tablesNow.geometryLayerKey.size() && !tablesNow.geometryLayerKey[a_input.geometryIndex]) {
					const auto* triShape = geometry->GetGeometryRuntimeData().rendererData;
					const auto& row = tablesNow.geometries[a_input.geometryIndex];
					if (triShape && (reinterpret_cast<ID3D11Buffer*>(triShape->vertexBuffer) != row.vertexBuffer || reinterpret_cast<ID3D11Buffer*>(triShape->indexBuffer) != row.indexBuffer)) {
						if (staleBuffers++ == 0)
							staleFirst = fmt::format("'{}' (object {}, slot {}): row vertex {} index {}, TriShape vertex {} index {}", geometry->name.c_str() ? geometry->name.c_str() : "?",
								object, a_input.geometryIndex, static_cast<const void*>(row.vertexBuffer), static_cast<const void*>(row.indexBuffer),
								static_cast<const void*>(triShape->vertexBuffer), static_cast<const void*>(triShape->indexBuffer));
					}
				}
				const float dx = record.world[3] - bound.center.x, dy = record.world[7] - bound.center.y, dz = record.world[11] - bound.center.z;
				if (std::sqrt(dx * dx + dy * dy + dz * dz) > 4.0f * bound.radius + 512.0f) {
					if (farWorld++ == 0) {
						const auto* geometry = object < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[object] : nullptr;
						farFirst = fmt::format("'{}' world at ({:.0f} {:.0f} {:.0f}), bound ({:.0f} {:.0f} {:.0f}) r {:.0f}", geometry && geometry->name.c_str() ? geometry->name.c_str() : "?",
							record.world[3], record.world[7], record.world[11], bound.center.x, bound.center.y, bound.center.z, bound.radius);
					}
				}
				float minX = 1, maxX = -1, minY = 1, maxY = -1, maxZ = -1e30f;
				for (std::uint32_t corner = 0; corner < 8; ++corner) {
					const float p[3] = { bound.center.x + ((corner & 1) ? bound.radius : -bound.radius), bound.center.y + ((corner & 2) ? bound.radius : -bound.radius),
						bound.center.z + ((corner & 4) ? bound.radius : -bound.radius) };
					float clip[4];
					for (std::uint32_t row = 0; row < 4; ++row)
						clip[row] = a_latch.viewProj[row * 4] * p[0] + a_latch.viewProj[row * 4 + 1] * p[1] + a_latch.viewProj[row * 4 + 2] * p[2] + a_latch.viewProj[row * 4 + 3];
					const float w = clip[3] != 0.0f ? clip[3] : 1.0f;
					minX = std::min(minX, clip[0] / w);
					maxX = std::max(maxX, clip[0] / w);
					minY = std::min(minY, clip[1] / w);
					maxY = std::max(maxY, clip[1] / w);
					maxZ = std::max(maxZ, clip[2] / w);
				}
				const float area = (std::clamp(maxX, -1.0f, 1.0f) - std::clamp(minX, -1.0f, 1.0f)) * (std::clamp(maxY, -1.0f, 1.0f) - std::clamp(minY, -1.0f, 1.0f)) / 4.0f;
				if (area > 0.25f && maxZ < 0.02f)
					covers.push_back({ area, object, maxZ });
			});
			std::sort(covers.begin(), covers.end(), [](const Cover& a, const Cover& b) { return a.area > b.area; });
			if (!covers.empty() || farWorld || staleBuffers) {
				std::string text;
				for (std::size_t i = 0; i < covers.size() && i < 5; ++i) {
					const auto& c = covers[i];
					const auto* geometry = c.object < tablesNow.objectGeometry.size() ? tablesNow.objectGeometry[c.object] : nullptr;
					const auto& r = records[c.object];
					text += fmt::format("; '{}' (object {}) {:.0f}% of the map, depth at most {:.4f}, bound ({:.0f} {:.0f} {:.0f}) r {:.0f}, the engine's ({:.0f} {:.0f} {:.0f}) r {:.0f}, {}",
						geometry && geometry->name.c_str() ? geometry->name.c_str() : "?", c.object, 100.0f * c.area, c.maxZ, r.bound[0], r.bound[1], r.bound[2], r.bound[3],
						geometry ? geometry->worldBound.center.x : 0.0f, geometry ? geometry->worldBound.center.y : 0.0f, geometry ? geometry->worldBound.center.z : 0.0f,
						geometry ? geometry->worldBound.radius : 0.0f, reached.contains(geometry) ? "the engine's cull reached it" : "the engine's cull did not reach it");
					{
						const auto& t = r.tree;
						const std::uint32_t treeSlot = std::bit_cast<std::uint32_t>(t.windTimers[2]);
						text += fmt::format(", tree params ({} {} {} {}), wind slot {:#x} generation {:#x}", t.treeParams[0], t.treeParams[1], t.treeParams[2], t.treeParams[3],
							treeSlot, std::bit_cast<std::uint32_t>(t.windTimers[3]));
						if (treeSlot < tablesNow.trees.size()) {
							const auto& row = tablesNow.trees[treeSlot];
							text += fmt::format(", tree row: model amplitude {}, amplitude {}, leaf frequency {}, animated {}, generation {:#x}, at ({:.0f} {:.0f} {:.0f})", row.modelAmplitude,
								row.amplitude, row.leafFrequency, row.animated, row.generation, row.position[0], row.position[1], row.position[2]);
						}
					}
				}
				static std::uint32_t logged = 0;
				if (logged++ < 40)
					logger::warn("[DCLF] cascade cover: view {} (cascade {}) frame {}: {} drawn, {} cover over a quarter of the map at the near plane, {} with the world far from the bound{}, {} pulling from buffers their TriShape no longer holds{}{}",
						a_view.viewId, shadowView->descriptor, a_frame, drawn, covers.size(), farWorld, farFirst.empty() ? "" : " (first " + farFirst + ")", staleBuffers,
						staleFirst.empty() ? "" : " (first " + staleFirst + ")", text);
			}
		}
		if (checks == 20) {
			logger::info("[DCLF] cascade culling parity: {} sun views checked, {} claimed casters the engine's cascade culls reached, {} of them outside the view's viewport (no texels), {} rejected by DCLF's view{}{}",
				checks, registered, offscreen, rejected, rejected ? " <- CASCADE CULL" : " <- OK", first.empty() ? "" : "; first " + first);
			checks = registered = rejected = offscreen = 0;
			first.clear();
		}
	}

	void IndirectDraws::BeginShadowFrame()
	{
		impl->pendingViews.clear();
	}

	void IndirectDraws::CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode)
	{
		// The hook's half: capture the view - where the engine has just drawn, and the constants it drew
		// with - for the frame's single epoch (ExecuteShadowFrame). Nothing is drawn here.
		if (!ActiveToggles().shadows || failed)
			return;
		ScopedPerfEvent event("CS DCLF: shadow view capture");
		const auto start = std::chrono::steady_clock::now();
		++shadowStats.views;
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		auto notReady = [&](ShadowNotReady a_reason) {
			++shadowStats.notReady;
			++shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)];
			if (a_renderMode >= PassCapture::kFirstShadowMode)
				impl->ShadowsNotDrawn(shadowStats);
		};
		if (!pipelines.Enabled() || !utility || !impl->SetupShadow())
			return notReady(ShadowNotReady::Setup);
		// The tables are not read here: under CS_DCLF_ASYNC the scene walk is still writing them while the
		// engine draws the shadow maps. ExecuteShadowFrame checks them after the join.
		const auto* shadowView = ShadowViews::Get().At(a_viewId);
		if (!shadowView)
			return notReady(ShadowNotReady::Tables);
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + kShadowModeCount)
			return notReady(ShadowNotReady::Tables);
		// A focus shadow holds one actor's casters, not the scene's, and DCLF has no caster set for it: it stays native.
		if (shadowView->focus) {
			++shadowStats.focusSkipped;
			return;
		}
		// Where the engine has just drawn: the target and slice come from the renderer's state, because the
		// descriptor's own fields are filled only when the draw allocates them (engine notes: shadow maps).
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t target = shadowState.depthStencil;
		const std::uint32_t slice = shadowState.depthStencilSlice;
		const std::uint32_t targetIndex = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM                     ? 0u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS                           ? 1u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM ? 2u :
		                                                                                                                     ~0u;
		// The volumetric lighting copy holds only the volumetric-only casters (batch group 15, which is all the
		// engine's flag-0x100 draw of the view renders): the view draws those alone (casterClass 1).
		const bool volumetricCopy = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;
		if (targetIndex == ~0u || !impl->ImportShadowDepth(targetIndex, target)) {
			// Which target, once per target: anything here is a view the design has not met.
			if (target < 32 && !((impl->shadowLoggedTargets >> target) & 1)) {
				impl->shadowLoggedTargets |= 1u << target;
				logger::info("[DCLF] shadow view {} ({}, light {} descriptor {}, mode {:#x}) draws into depth target {} slice {}; it stays native",
					a_viewId, ShadowViews::KindName(shadowView->kind), shadowView->lightIndex, shadowView->descriptor, a_renderMode, target, slice);
			}
			return notReady(ShadowNotReady::Depth);
		}
		// The rasterizer state the engine draws this view with: its table entry for the renderer's modes, read
		// now, while the view is drawn - Community Shaders' ShadowmapCascadeRasterizerFix swaps per-cascade
		// copies with their own depth bias into that table for exactly this window, and the volumetric copy
		// draws with culling off. DCLF's pipelines for the view are built with it. The cull mode is the entry's at 1, not the
		// renderer's: the Utility shader sets it per pass (0 for a two-sided property, 1 otherwise; engine notes, shadow maps),
		// so the renderer's is the view's last pass's, which changes from frame to frame with whatever drew last. A two-sided
		// caster's key draws without culling (kRasterTwoSided) whatever the state, as the engine's pass does - and the view's
		// state, and so its recording, only changes with the view. A view whose table has no entry at 1 (the volumetric copy,
		// which draws with culling off) takes its entry at 0.
		std::uint32_t rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 1, shadowState.rasterStateDepthBiasMode,
			shadowState.rasterStateScissorMode, a_renderMode);
		if (rasterState == 0)
			rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 0, shadowState.rasterStateDepthBiasMode, shadowState.rasterStateScissorMode,
				a_renderMode);
		if (rasterState == 0)
			return notReady(ShadowNotReady::Pipelines);

		auto& view = impl->pendingViews.emplace_back();
		view.viewId = a_viewId;
		view.renderMode = a_renderMode;
		view.modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		view.targetIndex = targetIndex;
		view.slice = slice;
		view.rasterState = rasterState;
		view.casterClass = volumetricCopy ? 1u : 0u;
		view.sunView = shadowView->kind == ShadowViews::Kind::Directional;
		CaptureViewTarget(view, target);
		// The caster volume the engine culled this view's casters against: BSShadowDirectionalLight::UpdateCamera
		// builds it from the main camera frustum's corners and the light direction into the descriptor's culling
		// process, and the accumulation's cull (FUN_1414f0920) tests it on top of the shadow camera's frustum. It
		// is much tighter than the orthographic box: without it DCLF drew into the far cascade the casters of a
		// whole slice's worth of terrain that no visible receiver can see a shadow from.
		if (auto& lightData = const_cast<RE::BSShadowLight*>(shadowView->light)->GetRuntimeData(); shadowView->descriptor < lightData.shadowmapDescriptors.size()) {
			const auto* process = lightData.shadowmapDescriptors[shadowView->descriptor].cullingProcess;
			if (process && process->doCustomCullPlanes) {
				const auto& planes = process->customCullPlanes;
				for (std::uint32_t p = 0; p < RE::NiFrustumPlanes::Planes::kTotal; ++p) {
					view.cullPlanes[p][0] = planes.cullingPlanes[p].normal.x;
					view.cullPlanes[p][1] = planes.cullingPlanes[p].normal.y;
					view.cullPlanes[p][2] = planes.cullingPlanes[p].normal.z;
					view.cullPlanes[p][3] = planes.cullingPlanes[p].constant;
				}
				view.cullPlaneMask = planes.activePlanes.underlying() & 0x3Fu;
			}
		}
		// The view's PerTechnique block (b0): HighDetailRange (LOD landscape only; zero until owned),
		// ParabolaParam from the engine's two globals as its SetupTechnique reads them, and the eye delta
		// from the frame's reference eye to this view's.
		{
			static const REL::Relocation<float*> parabolaRadius{ REL::Offset(0x2035df8) };
			static const REL::Relocation<float*> parabolaSide{ REL::Offset(0x2035dfc) };
			const float radius = *parabolaRadius.get();
			view.viewBlock[4] = radius != 0.0f ? 1.0f / radius : 0.0f;
			view.viewBlock[5] = *parabolaSide.get();
			// c2 (DCLFEyeDelta) is not read: the records are absolute and Utility.hlsl subtracts the view's own
			// CameraPosAdjust. Left zero.
			view.viewBlock[8] = view.viewBlock[9] = view.viewBlock[10] = 0.0f;
		}
		CapturePerFrame(view);
		// The capture check: the block's CameraPosAdjust (c40) against the eye the renderer's state has for the view now.
		{
			static std::uint64_t views = 0, fallback = 0, differ = 0;
			static float largest = 0.0f;
			static std::string first;
			++views;
			const auto* block = reinterpret_cast<const float*>(view.perFrame.data());
			if (view.perFrameBytes >= 164 * sizeof(float)) {
				const float d = std::max({ std::abs(block[160] - view.eye.x), std::abs(block[161] - view.eye.y), std::abs(block[162] - view.eye.z) });
				if (d > 0.01f) {
					++differ;
					if (first.empty())
						first = fmt::format("view {} mode {:#x}: block ({:.1f} {:.1f} {:.1f}), renderer ({:.1f} {:.1f} {:.1f})", a_viewId, a_renderMode, block[160], block[161],
							block[162], view.eye.x, view.eye.y, view.eye.z);
				}
				largest = std::max(largest, d);
			}
			if (!ConstantMirror::Get().Contents(*globals::game::perFrame.get()).size())
				++fallback;
			if (views == 1000) {
				logger::info("[DCLF] shadow view capture: {} views, {} from the cached copy, {} whose b12 eye is not the renderer's (largest {:.2f}){}{}", views, fallback, differ,
					largest, differ ? " <- CAPTURE" : " <- OK", first.empty() ? "" : "; first " + first);
				views = fallback = differ = 0;
				largest = 0.0f;
				first.clear();
			}
		}
		shadowStats.captureMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void IndirectDraws::CaptureOcclusion(std::uint32_t a_view)
	{
		// The hook's half, as CaptureShadowView: where the engine's RenderMask has just drawn an occlusion map (the clear, and
		// whatever SetupMask registered), with which camera and state. Taken whether or not DCLF draws the map this frame: the
		// state and format are what next frame's build prepares the pipelines for.
		if (a_view >= kOcclusionViews || !ActiveToggles().shadows || failed || !SceneStore::OcclusionEnabled(a_view) || !impl->SetupShadow())
			return;
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t target = shadowState.depthStencil;
		// The renderer's state at this hook is what the view's last pass left, or whatever came before when nothing
		// drew; the Utility shader sets the cull mode per pass (engine notes, shadow maps: 0 for a two-sided
		// property, 1 otherwise). So the view's state is back-face culling at the renderer's fill, bias and scissor
		// modes, and a two-sided occluder's key draws without culling, as a two-sided caster's does.
		const std::uint32_t rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 1, shadowState.rasterStateDepthBiasMode,
			shadowState.rasterStateScissorMode, kOcclusionRenderMode);
		if (!rasterState || !impl->ImportShadowDepth(OcclusionDepthTarget(a_view), target))
			return;
		auto& occlusion = impl->occlusion[a_view];
		auto& view = occlusion.view;
		view = {};
		view.viewId = ~0u;
		view.renderMode = kOcclusionRenderMode;
		view.modeIndex = OcclusionModeOf(a_view);
		view.targetIndex = OcclusionDepthTarget(a_view);
		view.slice = shadowState.depthStencilSlice;
		view.rasterState = rasterState;
		CaptureViewTarget(view, target);
		CapturePerFrame(view);
		occlusion.rasterState = rasterState;
		occlusion.dsvFormat = view.dsvFormat;
		occlusion.capturedFrame = SceneStore::Get().GetFrame();
	}

	bool IndirectDraws::OcclusionReady(std::uint32_t a_view) const
	{
		// This frame's shadow commit uploaded the view's occluders (the set's, of its phase), and the map's target is imported.
		if (a_view >= kOcclusionViews || failed || !ActiveToggles().shadows || !SceneStore::OcclusionEnabled(a_view) || !impl->shadow)
			return false;
		const auto& occlusion = impl->occlusion[a_view];
		return occlusion.rasterState && occlusion.committedFrame == SceneStore::Get().GetFrame() && impl->shadow->depth[OcclusionDepthTarget(a_view)];
	}

	std::uint32_t IndirectDraws::ExecuteOcclusion(std::uint32_t a_views)
	{
		ZoneScopedN("CS.DCLF.ExecuteOcclusion");
		const auto start = std::chrono::steady_clock::now();
		auto& store = SceneStore::Get();
		const std::uint32_t frameNumber = store.GetFrame();
		// The views asked for that can be drawn: ready, captured this frame, under the state their pipelines were built for.
		std::uint32_t drawable = 0;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (!(a_views & (1u << v)))
				continue;
			const auto& occlusion = impl->occlusion[v];
			if (OcclusionReady(v) && occlusion.capturedFrame == frameNumber && occlusion.view.rasterState == occlusion.rasterState)
				drawable |= 1u << v;
			else
				++shadowStats.occlusionNotReady[v];
			// Drawn or not, the next commit takes the map's occluders into the set only after a frame that drew it.
			impl->occlusionDrawable[v] = (drawable >> v) & 1;
		}
		const auto indirect = GetShadowIndirectState();
		if (!drawable || !indirect.valid) {
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				shadowStats.occlusionNotReady[v] += (drawable & (1u << v)) ? 1 : 0;
			return 0;
		}
		ScopedPerfEvent event("CS DCLF: occlusion maps (CPU)");
		auto resources = impl->shadow;
		auto& payload = impl->shadowPayload;
		// The views' states' map rows: a state first seen by this frame's capture has none yet.
		impl->ReserveShadowLatch(0, resources->latchLayout.keySlots, DrawPipelines::Get().ShadowRasterStateCount(), resources->latchLayout.sunProcesses);
		impl->ReserveShadowSequences(store.GetTables(), kOcclusionViews, OcclusionSlot(0));
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::SkyOcclusion, [&](org::RenderGraph&) {
			resources->occlusionFrame.store(nullptr, std::memory_order_release);
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			auto frame = std::make_shared<ShadowFrame>();
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = indirect;
			frame->latch = resources->latch;
			const auto& previousShape = resources->occlusionPublished;
			const auto& lookups = store.GetLookups();
			auto& scene = *resources->scene;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				if (!(drawable & (1u << v)))
					continue;
				const auto& view = impl->occlusion[v].view;
				const std::uint32_t slot = OcclusionSlot(v), mode = OcclusionModeOf(v);
				const auto inputCount = static_cast<std::uint32_t>(payload.ModeInputs(mode));
				// The view slot's blocks, which its push data names, into the latch (its counters are zeroed by the latched copies).
				// The occluders, material rows, frame record, objects and geometries were uploaded by this frame's shadow commit.
				WriteViewBlocks(*resources, latchSlot, slot, view, *frame);
				// Its latch: frustum culling alone, near plane included as the rasterizer clips, every input drawn, and the rule's
				// size test (Skylighting::OcclusionTechnique's bound radius above 32) on the record's bound, and the fade roots as the
				// engine's cull of the map leaves them (kCullFadeOnVisible): it never draws a root the main camera has faded out, nor
				// the roots of a new cell before the main camera has seen them.
				auto latch = ShadowViewLatch(view, inputCount, frameNumber);
				static const REL::Relocation<const std::uint8_t*> fadesOn{ REL::Offset(0x2032dfd) };
				latch.cullFlags = 1u | kCullMinRadius | (*fadesOn.get() ? kCullFadeOnVisible : 0u);
				latch.fadeStatesIndex = scene.FadeStatesReadIndex(frameNumber);
				const auto& buckets = impl->ShadowRowBuckets(view.rasterState, lookups, indirect);
				UseShadowMapRow(*resources, buckets, latchSlot, view.rasterState, true, latch);
				const auto* previous = PreviousView(previousShape, slot);
				auto viewBuckets = SizeShadowBuckets(buckets, payload.keySlotDraws[mode], previous, std::uint64_t(kShadowClasses) * resources->sequenceDraws[slot]);
				WriteBucketTable(*resources, latchSlot, slot, viewBuckets, latch);
				resources->latch->WriteValue(latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				const std::uint32_t capacity = ShadowViewCapacity(previous ? previous->capacity : 0, payload.modeDraws[mode], resources->sequenceDraws[slot]);
				frame->views.push_back(FrameViewOf(view, slot, mode, OcclusionDepthTarget(v), capacity, *resources, payload));
				frame->views.back().buckets = std::move(viewBuckets);
				shadowStats.occlusionInputs[v] = inputCount;
				++shadowStats.occlusionDrawn[v];
			}
			PublishShape(std::move(frame), resources->occlusionPublished, resources->occlusionFrame, resources->shapeGenerations, resources->recentOcclusionShapes);
		}, impl->shadowExecutionOwner);
		shadowStats.occlusionMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++shadowStats.occlusionEpochs;
		if (!ok) {
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				if (drawable & (1u << v)) {
					--shadowStats.occlusionDrawn[v];
					++shadowStats.occlusionNotReady[v];
					impl->occlusionDrawable[v] = false;
				}
			return 0;
		}
		return drawable;
	}

	void IndirectDraws::ExecuteShadowFrame()
	{
		ZoneScopedN("CS.DCLF.ExecuteShadowFrame");
		auto& pending = impl->pendingViews;
		if (!ActiveToggles().shadows || failed || pending.empty()) {
			pending.clear();
			impl->DropShadowJob(stats);
			return;
		}
		ScopedPerfEvent event("CS DCLF: shadow views (CPU)");
		const auto start = std::chrono::steady_clock::now();
		auto notReady = [&](ShadowNotReady a_reason) {
			shadowStats.notReady += static_cast<std::uint32_t>(pending.size());
			shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)] += static_cast<std::uint32_t>(pending.size());
			if (!pending.empty())
				impl->ShadowsNotDrawn(shadowStats);
			pending.clear();
			impl->DropShadowJob(stats);
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !impl->shadow)
			return notReady(ShadowNotReady::Setup);
		const auto indirect = GetShadowIndirectState();
		if (!indirect.valid) {
			impl->ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		const std::uint32_t frameNumber = store.GetFrame();
		auto resources = impl->shadow;
		std::array<bool, kShadowModeCount> modeUsed{};
		// Per mode and caster class, every rasterizer state its views have drawn with (DrawPipelines' ids, which only
		// grow): the build's inputs and the shadow pipelines are for all of them, so which of its views a frame draws -
		// the sun's cascades alternate their depth-bias states frame by frame, a local light's culling-off view comes and
		// goes - changes nothing the build reads. It grows when a state first appears. Each view's latch row is its own.
		for (const auto& view : pending) {
			modeUsed[view.modeIndex] = true;
			impl->shadowStatesSeen[view.modeIndex].Add(view.rasterState, view.casterClass != 0);
		}
		std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			if (modeUsed[m])
				modeRasterStates[m] = impl->shadowStatesSeen[m];
		// The occlusion maps are drawn later in the frame, by their own epoch, from this build: their occluders under the state
		// each view drew with last (known once the engine's own draw of the map has been captured).
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			const auto& occlusion = impl->occlusion[v];
			if (!SceneStore::OcclusionEnabled(v) || !occlusion.rasterState || occlusion.dsvFormat == DXGI_FORMAT_UNKNOWN)
				continue;
			const std::uint32_t m = OcclusionModeOf(v);
			modeUsed[m] = true;
			modeRasterStates[m] = {};
			modeRasterStates[m].Add(occlusion.rasterState, false);
		}
		// One pipeline set per shadow map format: every target the engine has is D16 (engine notes), and a
		// view whose target differed would rebuild the set on every epoch, so the first view's is taken.
		const DXGI_FORMAT dsvFormat = pending.front().dsvFormat;
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0;

		impl->ReserveSceneTables(tables);
		impl->ReserveShadowRows();
		ShadowInputs in = impl->PrepareShadowInputs(store, *resources, modeUsed, modeRasterStates);
		impl->shadowJob.modes = modeUsed;
		// What the set's caster readiness reads (CasterReady): a new kind of view seen is a readiness event.
		if (impl->readyModes != modeUsed || impl->readyStates != modeRasterStates) {
			impl->readyModes = modeUsed;
			impl->readyStates = modeRasterStates;
			++impl->shadowReadinessSerial;
		}
		impl->shadowJob.rasterStates = modeRasterStates;
		impl->shadowJob.modesKnown = true;
		impl->shadowJob.views = static_cast<std::uint32_t>(pending.size());
		auto& payload = impl->shadowPayload;
		auto& async = stats.async[kAsyncShadow];
		bool usedWorkerBuild = false;
		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup)
			return;
		auto frameOwners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();

		// The view slots and the key slots this epoch can name: the lookups' keys, and every key a used mode may add to them
		// when the body refreshes them.
		{
			const auto& lookups = store.GetLookups();
			std::size_t keys = lookups.shadowSlotKeys.size();
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				if (modeUsed[m])
					keys += IsOcclusionMode(m) ? tables.occlusionKeysUsed[OcclusionOfMode(m)].size() : tables.shadowKeysUsed.size();
			impl->ReserveShadowLatch(static_cast<std::uint32_t>(pending.size()), static_cast<std::uint32_t>(keys), DrawPipelines::Get().ShadowRasterStateCount(),
				static_cast<std::uint32_t>(in.sunEntryProcesses.size()));
		}
		impl->ReserveShadowSequences(tables, static_cast<std::uint32_t>(pending.size()), kFirstShadowViewSlot);
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::ShadowView, [&](org::RenderGraph&) {
			ZoneScopedN("CS.DCLF.ShadowInputs");
			struct BodyTimer
			{
				std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				double& out;
				~BodyTimer() { out = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
			} bodyTimer{ {}, bodyMs };
			bodyTimer.start = std::chrono::steady_clock::now();

			// The worker's build if one was kicked and it was built for exactly these inputs, else the build
			// here. Joined before the lookups are refreshed: the worker reads them until it is done. The job read them as the
			// last epoch left them; they are refreshed after it is taken (what changed reaches the next frame's build), or
			// before the build here. Lookups that are behind (shadowRefreshDue) are refreshed first either way.
			bool useAsync = false;
			TracyCZoneN(shadowPrepareZone, "CS.DCLF.ShadowInputs.Prepare", true);
			const auto prepareStart = std::chrono::steady_clock::now();
			auto& job = impl->shadowJob;
			const auto joined = JoinJob(job.handle);
			auto& lookups = store.MutableLookups();
			auto refresh = [&] {
				RefreshMaterialLookups(store, tables, false, store.GetProjectedTextures(), lookups);
				RefreshShadowLookups(store, tables, modeUsed, modeRasterStates, dsvFormat, impl->OcclusionFormats(), lookups);
			};
			bool refreshed = false;
			if (lookups.shadowRefreshDue || !job.handle) {
				refresh();
				refreshed = true;
			}
			in.lookupGeneration = lookups.shadowGeneration;
			if (job.handle) {
				useAsync = TakeJob(
					joined, async, [&] { return SameShadowInputs(job.inputs, in); },
					[&] {
						if (job.inputs.lookupGeneration != in.lookupGeneration)
							++async.staleLookups;
						if (const auto logged = job.loggedStale++; logged < 4 || logged % 64 == 0) {
							const auto& k = job.inputs;
							logger::info("[DCLF] async shadow: the job's inputs are stale (frame {} vs {}, modes {}{}{} vs {}{}{}, raster states {}, sun planes {} ({} vs {}), sun candidates {}, shared data {}, feature data {}, tables {} vs {}, lookups {} vs {}, resources {})",
								k.frameNumber, in.frameNumber, int(k.modeUsed[0]), int(k.modeUsed[1]), int(k.modeUsed[2]),
								int(in.modeUsed[0]), int(in.modeUsed[1]), int(in.modeUsed[2]), k.modeRasterStates == in.modeRasterStates ? "same" : "differ",
								k.sunEntryProcesses == in.sunEntryProcesses ? "same" : "differ", k.sunEntryProcesses.size(), in.sunEntryProcesses.size(),
								k.sunCandidates == in.sunCandidates ? "same" : "differ", k.sharedData == in.sharedData ? "same" : "differs",
								k.featureData == in.featureData ? "same" : "differs", k.tablesGeneration, in.tablesGeneration, k.lookupGeneration,
								in.lookupGeneration, k.addresses == in.addresses ? "same" : "changed");
						}
						// Which views' states moved: per mode, the casters' and the volumetric copies' state ids, the job's and the epoch's.
						if (job.inputs.modeRasterStates != in.modeRasterStates && job.loggedStates++ < 40) {
							auto ids = [](const std::vector<std::uint32_t>& a_ids) {
								std::string out;
								for (const auto id : a_ids)
									out += (out.empty() ? "" : ",") + std::to_string(id);
								return out;
							};
							std::string text;
							for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
								if (job.inputs.modeRasterStates[m] != in.modeRasterStates[m])
									text += fmt::format(" mode {}: casters [{}] -> [{}], volumetric [{}] -> [{}];", m, ids(job.inputs.modeRasterStates[m].casters),
										ids(in.modeRasterStates[m].casters), ids(job.inputs.modeRasterStates[m].volumetric), ids(in.modeRasterStates[m].volumetric));
							logger::info("[DCLF] async shadow: raster states moved at frame {} ({} views):{}", in.frameNumber, pending.size(), text);
						}
					});
				job.handle = {};
			}
			usedWorkerBuild = useAsync;
			if (useAsync) {
				++async.used;
				// The epoch's full-frustum planes, for its latch (the build may have been kicked before the cull that sets them).
				payload.inputs.sunEntryProcesses = in.sunEntryProcesses;
				ProbeWorkerBuild(payload, impl->shadowProbePayload, async, "shadow",
					[&](ShadowPayload& a_probe) { BuildShadowPayload(job.inputs, tables, lookups, a_probe); });
				if (!refreshed)
					refresh();
			} else {
				++async.builtInline;
				if (!refreshed) {
					refresh();
					in.lookupGeneration = lookups.shadowGeneration;
				}
				BuildShadowPayload(in, tables, lookups, payload, impl->SceneObjects(), impl->SceneBones(), impl->ShadowKeptState(), impl->SceneGeometries());
			}
			*frameOwners = std::move(payload.bindingOwners);
			// The cleared geometry slots' buffers, held until this execution retires (SceneStore::TakeRetiredImports).
			for (auto& owner : store.TakeRetiredImports())
				frameOwners->push_back(std::move(owner));
			impl->shadowExecutionOwner = frameOwners;
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
			TracyCZoneEnd(shadowPrepareZone);

			// Nothing to draw until this commit publishes the shape again (so a failed one draws nothing, rather
			// than a reused recording reading latch values this execution never wrote).
			resources->frame.store(nullptr, std::memory_order_release);
			CommitUploads uploads(impl->commitStagedPool);
			// ---- The commit: the shared uploads (the material rows among them), the per-mode inputs, then per view its blocks
			// at its slot of the arena's head, its count buffer zeroed, and the view.
			auto& arena = payload.arena;
			bool staged = false;
			TracyCZoneN(shadowCommitZone, "CS.DCLF.ShadowInputs.CommitShared", true);
			const auto inputsStart = std::chrono::steady_clock::now();
			// The worker's build staged what does not depend on the views (StageShadowPayload): one submission,
			// ahead of this commit's own uploads. A build made here, or staged against resources since recreated,
			// is uploaded from its vectors.
			staged = useAsync && payload.staged && payload.stagedFor == resources.get();
			if (staged) {
				SubmitWorkerBatch(std::move(payload.staged));
			} else {
				auto& scene = *resources->scene;
				EmitGeometryDraws(payload.geometries, scene.held.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(scene.geometries, a_data, a_bytes, a_offset);
				});
				payload.materialRows.Emit(resources->materialRowsHeld, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->materialRows.buffer, a_data, a_bytes, a_offset);
				});
			}
			// The rows the table holds (none past its capacity, which the build left waiting), and what the next frame's
			// Reserve grows it to. A build without the kept state wrote them whole: the table holds no journal version.
			resources->materialRowsHeld = payload.kept ? payload.materialRows.Version() : 0;
			impl->shadowRowsWanted = payload.rowsWanted;
			shadowStats.waitingRows = payload.waitingRows;
			// Either path uploaded the geometry slots' draws the buffers did not hold. The object records and the bone rows are
			// the streams, as the tables hold them now (CommitSceneStreams).
			auto& scene = *resources->scene;
			if (payload.geometries.Version())
				scene.held.geometries = payload.geometries.Version();
			impl->CommitSceneStreams(scene, store.GetTables(), store.GetFrame(), store.GetTablesGeneration(), uploads);
			shadowStats.faceUploads += UploadFaceStreams(payload.faceStreams, scene.facePositions, scene.faceUploaded, uploads);
			ZeroFrameAheadOutputs(scene, uploads);
			UploadTrees(store.GetTables(), store.GetFrame(), scene, uploads, false);
			shadowStats.records = static_cast<std::uint32_t>(payload.materialRows.Count());
			shadowStats.skippedTexture = payload.skippedTexture;
			shadowStats.skippedPipeline = payload.skippedPipeline;
			shadowStats.deferredTextures = payload.deferredTextures;
			shadowStats.deferredPipelines = payload.deferredPipelines;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!modeUsed[m])
					continue;
				if (!staged)
					EmitShadowInputs(payload, m, payload.kept ? resources->inputsUploaded[m] : 0,
						[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { uploads(resources->inputs[m], a_data, a_bytes, a_offset); });
				// Either path wrote what the buffer did not hold; a build without the kept state wrote it whole, which no
				// version of the kept state is.
				resources->inputsUploaded[m] = payload.kept ? payload.regionInputs[m].Version() : 0;
				shadowStats.inputs = static_cast<std::uint32_t>(payload.ModeInputs(m));
			}
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
			TracyCZoneEnd(shadowCommitZone);

			TracyCZoneN(shadowViewsZone, "CS.DCLF.ShadowInputs.BuildViews", true);
			const auto blocksStart = std::chrono::steady_clock::now();
			auto frame = std::make_shared<ShadowFrame>();
			const auto& previousShape = resources->published;
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			// The index pool, which this epoch's views and the occlusion epoch's after it draw from.
			{
				ZoneScopedN("CS.DCLF.ShadowInputs.IndexPool");
				UpdateIndexPool(*resources->pool, store.GetTables(), store.GetTablesGeneration(), *resources->latch, latchSlot, resources->latchLayout.PoolOffset(), uploads);
			}
			resources->labels.clear();
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = indirect;
			frame->latch = resources->latch;
			std::vector<bool> mapRowsWritten(std::size_t(DrawPipelines::Get().ShadowRasterStateCount()) + 1);  // per state: its row is in the latch
			auto bucketsOf = [&](std::uint32_t a_state) -> const RowBuckets& { return impl->ShadowRowBuckets(a_state, store.GetLookups(), indirect); };
			std::uint32_t sunEntryOffset = 0;  // the slot's sun entry region, once written
			for (std::uint32_t index = 0; index < pending.size(); ++index) {
				ZoneScopedN("CS.DCLF.ShadowInputs.View");
				const auto& view = pending[index];
				const std::uint32_t slot = kFirstShadowViewSlot + index;
				// Its blocks into the latch; the epoch's latched copies take them to the view-blocks buffer and zero its counters.
				WriteViewBlocks(*resources, latchSlot, slot, view, *frame);
				const auto inputCount = static_cast<std::uint32_t>(payload.ModeInputs(view.modeIndex));
				// The view's values into its latch: frustum culling alone (mode 1), the single phase, and no
				// engine-visibility gate - a caster is drawn whether or not the main camera kept it.
				auto latch = ShadowViewLatch(view, inputCount, frameNumber);
				latch.cullFlags = (view.hasViewProj ? (1u | (view.renderMode == 0xE ? kCullNoNearPlane : 0u)) : 0u) |
				                  (view.casterClass ? kCullVolumetricOnly : kCullCastersOnly) | (view.sunView ? kCullSunEntry : 0u);
				latch.cullPlaneMask = view.cullPlaneMask;
				std::memcpy(latch.cullPlanes, view.cullPlanes, sizeof(latch.cullPlanes));
				latch.fadeStatesIndex = resources->scene->FadeStatesReadIndex(frameNumber);
				// A sun view's entry rule on the GPU: the slot's region of the frame's full-frustum processes (written once, below the
				// loop's first sun view), which BuildDraws tests every input's entry sphere against (SetSunEntryRow).
				if (view.sunView) {
					if (!sunEntryOffset)
						sunEntryOffset = WriteSunEntryRegion(*resources, latchSlot, payload.inputs.sunEntryProcesses);
					latch.sunEntryOffset = sunEntryOffset;
					// CS_DCLF_PERSISTENT_PARITY: the test BuildDraws makes on the object record's entry sphere (the kept records the
					// epoch uploads), against the CPU's verdict from the tables' entry, per input.
					if (PersistentParityEnabled() && ParityDue(frameNumber)) {
						const auto& tablesNow = store.GetTables();
						const auto& records = impl->objectStore.records.Get();
						for (const auto& input : payload.Flat(view.modeIndex)) {
							if (input.objectIndex >= records.size())
								continue;
							const bool cpu = OutsideSunEntry(payload.inputs, tablesNow, input.objectIndex);
							++shadowStats.sunEntryChecks;
							shadowStats.sunEntryMismatches += cpu != OutsideSunEntryProcesses(payload.inputs.sunEntryProcesses, records[input.objectIndex].sunEntry) ? 1 : 0;
						}
					}
				}
				const bool rowWritten = view.rasterState < mapRowsWritten.size() && mapRowsWritten[view.rasterState];
				if (view.rasterState < mapRowsWritten.size())
					mapRowsWritten[view.rasterState] = true;
				const auto& buckets = bucketsOf(view.rasterState);
				UseShadowMapRow(*resources, buckets, latchSlot, view.rasterState, !rowWritten, latch);
				const auto* previous = PreviousView(previousShape, slot);
				TracyCZoneN(viewBucketsZone, "CS.DCLF.ShadowInputs.View.Buckets", true);
				auto viewBuckets = SizeShadowBuckets(buckets, payload.keySlotDraws[view.modeIndex], previous,
					std::uint64_t(kShadowClasses) * resources->sequenceDraws[slot]);
				WriteBucketTable(*resources, latchSlot, slot, viewBuckets, latch);
				TracyCZoneEnd(viewBucketsZone);
				resources->latch->WriteValue(latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				impl->CheckCascadeCulling(view, latch, frameNumber, payload);
				resources->labels.push_back({ view.viewId, view.renderMode, slot });
				frame->views.push_back(FrameViewOf(view, slot, view.modeIndex, view.targetIndex,
					ShadowViewCapacity(previous ? previous->capacity : 0, payload.modeDraws[view.modeIndex], resources->sequenceDraws[slot]),
					*resources, payload));
				frame->views.back().buckets = std::move(viewBuckets);
				if (impl->shadowSlotDrawn.size() <= slot)
					impl->shadowSlotDrawn.resize(std::size_t(slot) + 1, 0u);
				impl->shadowSlotDrawn[slot] = frameNumber;
			}
			// The view slots the previous shape had past this frame's views (a local light's paraboloid pair, which comes and goes
			// with the light): kept in the shape, as they were, with no work - a zero latch (no dispatch, no draw), their counters
			// zeroed by the latched copies, and their render passes loading and storing what the engine left - so a view's
			// coming and going changes no shape and the epoch's ticket stays current. One whose light comes back finds its
			// capacity and buckets. A slot not drawn for kRetainedViewFrames frames leaves the shape.
			for (std::uint32_t index = static_cast<std::uint32_t>(pending.size()); previousShape && index < previousShape->views.size(); ++index) {
				const auto& retained = previousShape->views[index];
				const std::uint32_t slot = kFirstShadowViewSlot + index;
				if (retained.slot != slot || slot >= impl->shadowSlotDrawn.size() || frameNumber - impl->shadowSlotDrawn[slot] > Impl::kRetainedViewFrames)
					break;
				resources->latch->WriteValue(latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), BuildDrawsLatch{});
				frame->views.push_back(retained);
				++shadowStats.retainedViews;
			}
			// The arena (the frame record and the blocks), when the worker did not stage it.
			if (const auto& bytes = arena.Bytes(); !staged && !bytes.empty())
				uploads(resources->constants, bytes.data(), bytes.size(), 0);
			// CS's SharedData and FeatureData as the frame has them now, over the build's copies (it may have been kicked before
			// the water reflections' prepasses refreshed them).
			// Latched (LatchedUploads): the epoch's first pass copies them, after the arena a build here staged above.
			LatchedUploads latched(resources->latchedTargets, uploads, resources->latchedBlock, latchSlot, RenderGraphRuntime::Get().Host()->FrameSlots());
			const auto writeBlock = [&](std::uint64_t a_address, const std::vector<std::byte>& a_bytes) {
				if (a_address && a_address != payload.zerosAddress && !a_bytes.empty() && a_address >= in.addresses.constants)
					latched(resources->constants, a_bytes.data(), a_bytes.size(), a_address - in.addresses.constants);
			};
			writeBlock(payload.sharedDataAddress, in.sharedData);
			writeBlock(payload.featureDataAddress, in.featureData);
			frame->latched = latched.Finish();
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			static std::uint32_t logged = 0;
			if (logged++ % 600 == 0) {
				std::string views;
				for (std::size_t v = 0; v < frame->views.size() && v < pending.size(); ++v) {
					const auto& view = frame->views[v];
					views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", pending[v].viewId,
						pending[v].renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.ModeInputs(view.modeIndex), view.capacity);
				}
				logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} material rows ({} waiting for the table to grow, {} held): {}",
					frame->views.size(), shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, payload.waitingRows,
					resources->materialRows.capacity, views);
			}
			{
				ZoneScopedN("CS.DCLF.ShadowInputs.Publish");
				PublishShape(std::move(frame), resources->published, resources->frame, resources->shapeGenerations, resources->recentShapes);
			}
			TracyCZoneEnd(shadowViewsZone);
		}, frameOwners);
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own compile, prepare and record
		if (ok) {
			++shadowStats.epochs;
			shadowStats.viewsDrawn += static_cast<std::uint32_t>(pending.size());
			// The occlusion maps' occluders are uploaded: each map can be DCLF's this frame if none was left out.
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				if (!modeUsed[OcclusionModeOf(v)])
					continue;
				auto& occlusion = impl->occlusion[v];
				occlusion.committedFrame = frameNumber;
				occlusion.inputs = static_cast<std::uint32_t>(payload.ModeInputs(OcclusionModeOf(v)));
			}
			impl->ReadShadowCullCounters(frameNumber, shadowStats);
			// The views are drawn: the set's casters stay DCLF's (SetPhasesDrawn).
			impl->shadowsDrawable = true;
			for (std::uint32_t m = 0; m < kFirstOcclusionMode && m < shadowStats.casters.size(); ++m)
				if (modeUsed[m])
					shadowStats.casters[m] = static_cast<std::uint32_t>(payload.ModeInputs(m));
			shadowStats.setWaiting += payload.setWaiting;
			if (shadowStats.setWaitingFirst.empty())
				shadowStats.setWaitingFirst = payload.setWaitingFirst;
			if (PassCapture::ShadowWithholdingEnabled()) {
				// The cascades' casters decide which sun entries the next frame's cascade culls may skip.
				if (modeUsed[kSunShadowMode])
					SunAccumulation::Get().PublishExclusion(usedWorkerBuild ? payload.sunExclusion :
					                                                          BuildSunExclusion(payload.inputs.sunCandidates, payload, kSunShadowMode, SceneStore::Get().GetTables(), &impl->sunExclusionCache));
				// The paraboloid views' casters decide which entries the next frame's point-light culls may skip (none when no
				// point light was drawn: a light new next frame is culled by the engine).
				LocalLightCull::Publish(!modeUsed[kParabolicShadowMode] ? nullptr :
				                        usedWorkerBuild                ? payload.parabolicExclusion :
				                                                         BuildSunExclusion(payload.inputs.lightCandidates, payload, kParabolicShadowMode, SceneStore::Get().GetTables(), &impl->parabolicExclusionCache));
			}
			pending.clear();
		} else {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			notReady(ShadowNotReady::Epoch);
		}
		shadowStats.cpuMs += totalMs;
	}

	const RowBuckets& IndirectDraws::Impl::ShadowRowBuckets(std::uint32_t a_state, const Lookups& a_lookups, const ShadowIndirectState& a_indirect)
	{
		auto& cache = shadowRowBuckets;
		if (cache.lookups != a_lookups.instance || cache.generation != a_lookups.shadowGeneration || cache.version != a_indirect.version) {
			cache.lookups = a_lookups.instance;
			cache.generation = a_lookups.shadowGeneration;
			cache.version = a_indirect.version;
			cache.rows.clear();
		}
		if (a_state >= cache.rows.size())
			cache.rows.resize(std::size_t(a_state) + 1);
		auto& row = cache.rows[a_state];
		if (!row) {
			ZoneScopedN("CS.DCLF.ShadowInputs.RowBuckets");
			row = BucketsOfRow(a_lookups.ShadowMapRow(a_state), a_indirect);
		}
		return *row;
	}

	void IndirectDraws::Impl::ReserveShadowRows()
	{
		// What the last build wanted, with a quarter more: the table grows ahead of the scene, not a frame behind it.
		if (!shadow || !shadowRowsWanted)
			return;
		shadow->materialRows.Reserve(shadowRowsWanted + shadowRowsWanted / 4, [state = shadow.get()] { state->materialRowsHeld = 0; });  // a new version holds nothing
	}

	ShadowInputs IndirectDraws::Impl::PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const
	{
		ZoneScopedN("CS.DCLF.PrepareShadowInputs");
		ShadowInputs in;
		in.frameNumber = a_store.GetFrame();
		in.modeUsed = a_modeUsed;
		in.modeRasterStates = a_modeRasterStates;
		// The sun's full-frustum planes, read on the render thread after the full-frustum cull has run
		// (NiCamera::CalculateAndDrawShadowCasterLights precedes both the build's kick and the views).
		if (auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr)
			if (auto* sun = node->GetRuntimeData().sunShadowDirLight)
				for (const auto& process : sun->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray) {
					if (!process)
						continue;
					const auto& planes = process->planes;
					auto& out = in.sunEntryProcesses.emplace_back();
					for (std::uint32_t p = 0; p < 6; ++p) {
						out.planes[p][0] = planes.cullingPlanes[p].normal.x;
						out.planes[p][1] = planes.cullingPlanes[p].normal.y;
						out.planes[p][2] = planes.cullingPlanes[p].normal.z;
						out.planes[p][3] = planes.cullingPlanes[p].constant;
					}
					out.mask = planes.activePlanes.underlying() & 0x3Fu;
				}
		in.sunCandidates = a_store.GetSunCandidates();
		in.lightCandidates = a_store.GetLightCandidates();
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.records = a_resources.materialRows.address;
		in.addresses.objectsIndex = a_resources.scene->objectsIndex;
		in.addresses.bonesIndex = a_resources.scene->bonesIndex;
		in.addresses.treeWindIndex = a_resources.scene->TreeWindReadIndex(a_store.GetFrame());
		in.addresses.facePositions = FaceSnapshots::Enabled() ? a_resources.scene->facePositionsAddress : 0;
		in.addresses.recordCapacity = a_resources.materialRows.capacity;
		in.addresses.identity = &a_resources;
		in.tablesGeneration = a_store.GetTablesGeneration();
		in.lookupGeneration = a_store.GetLookups().shadowGeneration;
		in.tablesHeld = a_resources.scene->held;
		in.inputsHeld = a_resources.inputsUploaded;
		in.materialRowsHeld = a_resources.materialRowsHeld;
		if (auto* csState = globals::state) {
			const auto* shared = reinterpret_cast<const std::byte*>(&csState->lastSharedData);
			in.sharedData.assign(shared, shared + sizeof(State::SharedDataCB));
			if (!csState->lastFeatureData.empty()) {
				const auto* feature = reinterpret_cast<const std::byte*>(csState->lastFeatureData.data());
				in.featureData.assign(feature, feature + csState->lastFeatureData.size());
			}
		}
		return in;
	}

	void IndirectDraws::KickShadowBuildEarly()
	{
		// The shadow build's inputs are final from the end of the scene phase (dclf-gpu-driven-frame.md: nothing between
		// it and the shadow maps writes what the build reads, but the change notes the witness watches and CS's shared data,
		// which the water reflections' prepasses refresh): kicked here, behind the placement job, and kept at BeforeShadowMaps
		// when nothing it read has moved.
		impl->shadowEarly = false;
		KickShadowBuild();
		if (impl->shadowJob.handle) {
			impl->shadowEarly = true;
			impl->shadowWitness = SceneStore::Get().ShadowInputsWitness();
			++stats.async[kAsyncShadow].earlyKicked;
		}
	}

	void IndirectDraws::BeforePlacementJoin()
	{
		auto& job = impl->shadowJob;
		if (!impl->shadowEarly || !job.handle)
			return;
		if (AsyncWorker::Get().Wait(job.handle, AsyncWaitBudget()) != AsyncWorker::WaitResult::Done) {
			impl->DropShadowJob(stats);
			impl->shadowEarly = false;
			++stats.async[kAsyncShadow].earlyRekicked;
			++stats.async[kAsyncShadow].earlyRekickedBy[2];
		}
	}

	bool IndirectDraws::KeepEarlyShadowBuild()
	{
		auto& job = impl->shadowJob;
		if (!std::exchange(impl->shadowEarly, false) || !job.handle || !impl->shadow)
			return false;
		auto& async = stats.async[kAsyncShadow];
		auto& store = SceneStore::Get();
		// What the build read, read again: the change logs (the witness), then the inputs the epoch compares.
		if (store.ShadowInputsWitness() != impl->shadowWitness) {
			++async.earlyRekicked;
			++async.earlyRekickedBy[0];
			return false;
		}
		const auto now = impl->PrepareShadowInputs(store, *impl->shadow, job.modes, job.rasterStates);
		if (!SameShadowInputs(job.inputs, now)) {
			++async.earlyRekicked;
			++async.earlyRekickedBy[now.sharedData.size() != job.inputs.sharedData.size() || now.featureData.size() != job.inputs.featureData.size() ? 1 : 2];
			return false;
		}
		++async.earlyKept;
		return true;
	}

	void IndirectDraws::KickShadowBuild()
	{
		// The shadow epoch's inputs are final from here to AfterShadowMaps: the scene phase has just built the
		// tables and the frame's reference eye is set. The build runs on the worker while the engine draws
		// the shadow maps, for last frame's render modes (a change is stale, and built inline). A build kicked at the end of
		// the scene phase is kept when nothing it read has moved since (KeepEarlyShadowBuild).
		if (KeepEarlyShadowBuild())
			return;
		impl->DropShadowJob(stats);
		// The scene stores are the main jobs' too; last frame's were joined by their epochs or dropped at EndFrame.
		for (std::size_t j = 0; j < impl->mainJobs.size(); ++j)
			impl->DropMainJob(j, stats);
		if (!ActiveToggles().shadows || failed || !AsyncEnabled())
			return;
		auto& async = stats.async[kAsyncShadow];
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		auto& job = impl->shadowJob;
		const bool tablesReady = !tables.objects.empty() && tables.shadowTechnique.size() == tables.objects.size();
		if (!impl->shadow || !job.modesKnown || !DrawPipelines::Get().Enabled() || !globals::game::utilityShader || !tablesReady) {
			++async.notKicked;
			return;
		}
		// The material rows' table grows here, on the render thread before the worker reads it, if the last build wanted more.
		impl->ReserveSceneTables(tables);
		impl->ReserveShadowRows();
		job.inputs = impl->PrepareShadowInputs(store, *impl->shadow, job.modes, job.rasterStates);
		++async.kicked;
		const auto* tablesPtr = &tables;
		const auto* lookups = &store.GetLookups();
		auto* payload = &impl->shadowPayload;
		auto* pool = &job.stagedPool;
		auto* objects = impl->SceneObjects();
		auto* bonesStore = impl->SceneBones();
		auto* geometriesStore = impl->SceneGeometries();
		auto* exclusionCache = &impl->sunExclusionCache;
		auto* parabolicCache = &impl->parabolicExclusionCache;
		auto* kept = impl->ShadowKeptState();
		const ShadowInputs inputs = job.inputs;
		const bool exclusions = PassCapture::ShadowWithholdingEnabled();
		const rhi::Device device = RecordingDevice();
		job.handle = AsyncWorker::Get().Submit("shadow", [inputs, tablesPtr, lookups, payload, pool, objects, bonesStore, kept, geometriesStore, exclusionCache, parabolicCache, target = impl->shadow,
																exclusions, device](std::stop_token) {
			BuildShadowPayload(inputs, *tablesPtr, *lookups, *payload, objects, bonesStore, kept, geometriesStore);
			StageShadowPayload(*payload, *target, *pool, device);
			if (exclusions) {
				ZoneScopedN("CS.DCLF.BuildShadow.Exclusions");
				if (inputs.modeUsed[kSunShadowMode])
					payload->sunExclusion = BuildSunExclusion(inputs.sunCandidates, *payload, kSunShadowMode, *tablesPtr, exclusionCache);
				if (inputs.modeUsed[kParabolicShadowMode])
					payload->parabolicExclusion = BuildSunExclusion(inputs.lightCandidates, *payload, kParabolicShadowMode, *tablesPtr, parabolicCache);
			}
		});
	}

	void IndirectDraws::Impl::DropShadowJob(IndirectDraws::Stats& a_stats)
	{
		if (!shadowJob.handle)
			return;
		AsyncWorker::Get().Cancel(shadowJob.handle);
		++a_stats.async[kAsyncShadow].dropped;
		shadowJob.handle = {};
	}

	void IndirectDraws::Impl::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
	{
		auto* context = globals::d3d::context;
		if (shadowCullReadback) {
			// Frames, not epochs: several views run per frame, and the copy needs the GPU to have finished
			// the sampled view's epoch, which three Presents later it has.
			if (a_frame - shadowCullReadback->copiedFrame < 3)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(shadowCullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullSunEntryOut = words[3];
				a_stats.cullMinRadius = words[4];
				a_stats.cullStoodInFading = words[5];
				a_stats.cullClass = words[16];
				a_stats.cullSampledView = shadowCullReadback->view;
				a_stats.cullSampledMode = shadowCullReadback->mode;
				context->Unmap(shadowCullReadback->count.get(), 0);
			}
			shadowCullReadback.reset();
			return;
		}
		// One epoch in 127, and within it the views in turn, so every view of the frame gets sampled.
		const auto epoch = shadowCullEpochs++;
		if ((epoch % 127) != 0 || !shadow || shadow->labels.empty())
			return;
		const auto& view = shadow->labels[(epoch / 127) % shadow->labels.size()];
		if (view.slot >= shadow->countD3D11.size() || !shadow->countD3D11[view.slot])
			return;
		D3D11_BUFFER_DESC desc{};
		shadow->countD3D11[view.slot]->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ShadowCullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		ScopedPerfEvent event("CS DCLF: shadow culling readback");
		context->CopyResource(readback.count.get(), shadow->countD3D11[view.slot].get());
		readback.copiedFrame = a_frame;
		readback.view = view.viewId;
		readback.mode = view.renderMode;
		shadowCullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats)
	{
		if (const auto set = PassCapture::Get().CurrentSet(); set && (set->drawn & (kSetCaster | kSetCasterPoint)) && PassCapture::ShadowWithholdingEnabled())
			++a_stats.notReadyWithheld;
		shadowsDrawable = false;
	}

	std::uint8_t IndirectDraws::ShadowPhasesDrawn() const
	{
		if (!impl->shadowsDrawable || failed)
			return 0;
		std::uint8_t phases = 0;
		for (std::uint32_t m = 0; m < kFirstOcclusionMode; ++m)
			phases |= impl->readyModes[m] ? SetPhaseOfMode(m) : 0;
		// An occlusion map's once its last ExecuteOcclusion drew it.
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			phases |= impl->readyModes[OcclusionModeOf(v)] && impl->occlusionDrawable[v] && SceneStore::OcclusionEnabled(v) ? SetPhaseOfMode(OcclusionModeOf(v)) : 0;
		return phases;
	}

	std::uint64_t IndirectDraws::ShadowReadinessSerial() const
	{
		return impl->shadowReadinessSerial;
	}

	bool IndirectDraws::PhaseReady(std::uint32_t a_slot, std::uint8_t a_phase) const
	{
		const auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		const auto& lookups = store.GetLookups();
		if (a_phase == kSetReflection)
			return impl->ReflectionPhaseReady(a_slot);
		if (a_slot >= tables.objects.size() || a_slot >= tables.shadowTechnique.size())
			return false;
		const auto& object = tables.objects[a_slot];
		if (object.geometryIndex >= tables.geometries.size())
			return false;
		// An occluder under a fade node: the engine's cull of the map goes into it only as the node's fade allows
		// (BuildDrawsCS, FadedOutOfOcclusion), which DCLF knows for a root it services (kFadeRootOwned, FadeStateCS) and no other.
		if (a_phase & (kSetOccluderSky | kSetOccluderPrecipitation)) {
			const auto* geometry = a_slot < tables.objectGeometry.size() ? tables.objectGeometry[a_slot] : nullptr;
			const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			if (const auto* node = property ? property->fadeNode : nullptr) {
				const std::uint32_t root = a_slot < tables.objectFadeRoot.size() ? tables.objectFadeRoot[a_slot] : kNoFadeRoot;
				if (root >= tables.fadeRoots.size() || tables.fadeRootNode[root] != node || !(tables.fadeRoots[root].bits & kFadeRootOwned))
					return false;
			}
		}
		// The shadow build's rule (BuildKeptShadow's evaluate): every view of the object's class in each of the phase's modes drawn
		// has its pipeline, and an alpha-tested technique's diffuse is imported.
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (SetPhaseOfMode(m) != a_phase || !impl->readyModes[m])
				continue;
			const std::uint32_t technique = ModeTechnique(tables, m, a_slot);
			if (!technique)
				continue;
			const auto& states = impl->readyStates[m].Of(VolumetricClass(m, object.flags));
			if (states.empty())
				continue;
			const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u, VertexLayoutOf(tables.geometries[object.geometryIndex].vertexDesc) };
			const auto slot = lookups.shadowSlots.find(key);
			if (slot == lookups.shadowSlots.end())
				return false;
			for (const std::uint32_t state : states)
				if (lookups.ShadowMapPipeline(state, slot->second) == Lookups::kNone)
					return false;
			if (technique & 0x80) {
				auto* diffuse = a_slot < tables.shadowDiffuse.size() ? tables.shadowDiffuse[a_slot] : nullptr;
				const auto texture = diffuse ? lookups.shadowTextures.find(diffuse) : lookups.shadowTextures.end();
				if (texture == lookups.shadowTextures.end() || texture->second == Lookups::kNone)
					return false;
			}
		}
		return true;
	}
}

#endif
