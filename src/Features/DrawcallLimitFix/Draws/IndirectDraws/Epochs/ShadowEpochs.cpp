// The shadow and occlusion epochs (render thread): view captures at the engine's shadow and occlusion hooks, shapes (bucket
// sizing, MakeShadowShape), the index pool, the epochs' commits, the shadow graph extension and the phases' readiness.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#	include "Features/DrawcallLimitFix/Engine/Cull/LocalLightCull.h"
#	include "Features/DrawcallLimitFix/Engine/Views/FocusViews.h"
#	include "Features/DrawcallLimitFix/Engine/Views/LightViews.h"
#	include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"
#	include "EngineFixes/ShadowmapCascadeRasterizerFix.h"
#	include "Features/Skylighting.h"

namespace DCLF
{
	namespace Draws
	{
		/**
		 * @brief A shadow view's common latch: a single phase over every input (no engine-visibility gate), the eye-folded
		 * view-projection and the frame's stamp. Culling flags and planes are the caller's.
		 */
		BuildDrawsLatch ShadowViewLatch(const PendingView& a_view, std::uint32_t a_inputs, std::uint32_t a_frame)
		{
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (a_inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = a_inputs;
			latch.visibilityStamp = a_frame & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			latch.fadeSeedsIndex = FrameValues::Get().FadeSeedsIndex();
			FoldEyeIntoViewProj(a_view.viewProj, a_view.eye, latch.viewProj);
			return latch;
		}

		/**
		 * @brief a_row within a_words buckets (the named count words): the buckets a_draws (the claims, by key slot) need first,
		 * then as many others as fit. The set the claims were made under holds their pipelines, so a dropped bucket with claimed
		 * draws is a defect, counted in a_lost.
		 */
		RowBuckets TrimmedRow(const RowBuckets& a_row, std::uint32_t a_words, std::span<const std::uint32_t> a_draws, std::uint64_t& a_lost)
		{
			if (a_row.pipelines.size() <= a_words)
				return a_row;
			std::vector<std::uint64_t> need(a_row.pipelines.size(), 0);
			for (std::size_t k = 0; k < a_row.bucketOfSlot.size() && k < a_draws.size(); ++k)
				if (a_row.bucketOfSlot[k] != Lookups::kNone)
					need[a_row.bucketOfSlot[k]] += a_draws[k];
			std::vector<std::uint32_t> order(a_row.pipelines.size());
			std::iota(order.begin(), order.end(), 0u);
			std::stable_partition(order.begin(), order.end(), [&](std::uint32_t a_bucket) { return need[a_bucket] != 0; });
			std::vector<std::uint32_t> newOf(a_row.pipelines.size(), Lookups::kNone);
			RowBuckets out;
			for (std::size_t n = 0; n < order.size(); ++n) {
				if (n >= a_words) {
					a_lost += need[order[n]];
					continue;
				}
				newOf[order[n]] = static_cast<std::uint32_t>(out.pipelines.size());
				out.pipelines.push_back(a_row.pipelines[order[n]]);
			}
			out.bucketOfSlot = a_row.bucketOfSlot;
			for (auto& bucket : out.bucketOfSlot)
				if (bucket != Lookups::kNone)
					bucket = newOf[bucket];
			return out;
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
		 * @brief A view's buckets, back to back in the slot's sequences, each sized for its key slots' draws. A bucket keeps its
		 * previous capacity while the draws fit and grows to a power of two past it, so recordings change only on growth. The
		 * slot holds twice the scene's draw bound (RequireShadowSequences); past that, exact sizes fit.
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

		/** @brief Writes a view's bucket table into its slot's latch region and points a_latch at it. */
		void WriteBucketTable(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, std::uint32_t a_latchSlot, std::uint32_t a_slot,
			std::span<const ShadowBucket> a_buckets, BuildDrawsLatch& a_latch)
		{
			// At most a bucket per key slot, which the layout reserves (ReserveShadowLatch).
			if (a_buckets.size() > a_layout.keySlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} shadow buckets past the latch's {} key slots", a_buckets.size(), a_layout.keySlots));
			std::vector<std::uint32_t> table;
			table.reserve(a_buckets.size() * 2);
			for (const auto& bucket : a_buckets) {
				table.push_back(bucket.first);
				table.push_back(bucket.capacity);
			}
			const std::uint32_t offset = a_layout.BucketOffset(a_slot);
			if (!table.empty())
				LatchWrite(a_latchBlock, "shadow bucket tables", a_latchSlot, offset, std::as_bytes(std::span(table)));
			a_latch.bucketTableOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchSlot)) + offset;
		}

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

		/**
		 * @brief Brings the index pool up to the tables, in the versions a_named (the adopted set) names: slots the geometry log named
		 * since the last read (every slot when it cannot read on) release their ranges and take new ones with a copy. Uploads the first
		 * indices and copies and writes the copy dispatch into the latch; IndexPoolPass runs it before the views draw.
		 */
		void UpdateIndexPool(const NamedVersions& a_named, IndexPool& a_pool, const SceneTables& a_tables, std::uint32_t a_generation, const org::LatchBlock& a_latch,
			std::uint32_t a_latchSlot, std::uint32_t a_poolOffset, CommitUploads& a_uploads)
		{
			auto& p = a_pool;
			const auto count = static_cast<std::uint32_t>(a_tables.geometries.size());
			// The capacities the revision producer required (Impl::RequireIndexPool), named by the adopted set: a range that does not
			// fit waits (only unclaimed geometry can: the claims' room was required).
			const auto& indices = a_named.Of(p.indices);
			const auto& firsts = a_named.Of(p.firsts);
			const std::uint64_t capacity = indices.units, firstsCapacity = firsts.units, copiesCapacity = a_named.Of(p.copies).units;
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
				if (std::uint64_t(p.end) + a_count > capacity)
					return IndexPool::kNoRange;
				const std::uint32_t first = p.end;
				p.end += a_count;
				return first;
			};
			auto copyOf = [&](const IndexPool::Range& a_range) {
				copies.push_back({ static_cast<std::uint32_t>(a_range.address), static_cast<std::uint32_t>(a_range.address >> 32), a_range.first,
					static_cast<std::uint32_t>((a_range.bytes + 3) / 4) });
			};
			// Lays every range out again in a new indices version (frames in flight read the old one) and copies it. Only between the
			// releases and the acquires, so every held range matches the tables.
			auto relayout = [&] {
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
			};
			// Each slot owns its range, copied whenever the log names it. Never shared by address: the device reuses a freed
			// buffer's address for the next one, which a cell load does while the unloaded cell's slots still hold theirs.
			auto acquire = [&](std::uint32_t a_slot) {
				const auto& geometry = a_tables.geometries[a_slot];
				const std::uint32_t slotIndices = PoolIndicesOf(a_tables, a_slot);
				if (!slotIndices)
					return;
				// Past the first-index table or this commit's copies, it waits.
				if (a_slot >= firstsCapacity || copies.size() >= copiesCapacity) {
					p.waiting.push_back(a_slot);
					return;
				}
				const std::uint32_t first = allocate(slotIndices);
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
				copyOf(range);
			};

			if (!p.cursor.Continues(a_tables.geometryLog, a_generation) || p.slots.size() > count) {
				// Every slot again, from an empty pool.
				p.free.clear();
				p.end = 0;
				p.indicesHeld = 0;
				p.waiting.clear();
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
				for (const std::uint32_t g : p.waiting)
					if (g < count)
						changed.push_back(g);
				p.waiting.clear();
				std::sort(changed.begin(), changed.end());
				changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
				// Every release before any acquire; an indices version this update did not lay out is laid out in between.
				for (const std::uint32_t g : changed)
					release(g);
				if (!p.indicesFilled.Holds(indices))
					relayout();
				for (const std::uint32_t g : changed)
					acquire(g);
			}
			p.cursor.Advance(a_tables.geometryLog);
			p.indicesFilled.Filled(indices);
			Diagnostics::Notify(Diagnostics::IndexPoolUpdated{ p, a_tables, count });

			// First indices: all of them into a version this update did not fill (a new version holds nothing), else only the changed
			// slots.
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
			// A copy per range, every range a slot of the tables the revision required the copies for: past them is a defect of that
			// requirement, never copies to drop.
			const auto copyCount = static_cast<std::uint32_t>(copies.size());
			if (copyCount > copiesCapacity)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the index pool's {} copies past the {} its buffer holds (the requirement missed them)", copyCount,
					copiesCapacity));
			if (copyCount)
				a_uploads(p.copies, copies.data(), std::size_t(copyCount) * sizeof(copies[0]), 0);
			const std::uint32_t dispatch[4] = { std::min(copyCount, kIndexPoolGroupsX), (copyCount + kIndexPoolGroupsX - 1) / kIndexPoolGroupsX, 1, copyCount };
			LatchWrite(a_latch, "shadow index-pool dispatch", a_latchSlot, a_poolOffset, std::as_bytes(std::span(dispatch)));
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
		 * @brief Points a_latch at its rasterizer state's pipeline-map row (each key slot's bucket), writing the row when a_write
		 * (once per state and frame). The offset is into the whole latch block, so it includes the frame slot's base.
		 */
		void UseShadowMapRow(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const RowBuckets& a_buckets, std::uint32_t a_latchSlot,
			std::uint32_t a_mode, std::uint32_t a_rasterState, bool a_write, BuildDrawsLatch& a_latch)
		{
			const auto& layout = a_layout;
			// ReserveShadowLatch gives every registered state a row in every mode: past it is a defect of that reserve.
			if (a_rasterState == 0 || a_rasterState > layout.rasterStates || a_mode >= kShadowModeCount)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: shadow view rasterizer state {} of mode {} past the latch's {} rows", a_rasterState, a_mode,
					layout.rasterStates));
			const std::uint32_t mapRowOffset = layout.MapOffset() + layout.MapRowOf(a_mode, a_rasterState) * layout.MapRowBytes();
			a_latch.pipelineMapOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchSlot)) + mapRowOffset;
			if (!a_write)
				return;
			// ReserveShadowLatch sized the row for every key slot.
			const auto& row = a_buckets.bucketOfSlot;
			if (row.size() > layout.keySlots)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} shadow key slots past the latch's {}", row.size(), layout.keySlots));
			if (!row.empty())
				LatchWrite(a_latchBlock, "shadow map rows", a_latchSlot, mapRowOffset, std::as_bytes(std::span(row)));
		}

		/** @brief Writes the sun's full-frustum processes into the frame slot's latch region; returns their offset into the block. */
		std::uint32_t WriteSunEntryRegion(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, std::uint32_t a_latchSlot,
			std::span<const SunEntryProcess> a_processes)
		{
			const auto& layout = a_layout;
			// ReserveShadowLatch sized the region for every process.
			if (a_processes.size() > layout.sunProcesses)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: {} sun full-frustum processes past the latch's {}", a_processes.size(), layout.sunProcesses));
			const std::uint32_t header[4] = { static_cast<std::uint32_t>(a_processes.size()), 0, 0, 0 };
			LatchWrite(a_latchBlock, "shadow sun processes", a_latchSlot, layout.SunEntryOffset(), std::as_bytes(std::span(header)));
			if (!a_processes.empty())
				LatchWrite(a_latchBlock, "shadow sun processes", a_latchSlot, layout.SunEntryOffset() + kSunRegionHeader, std::as_bytes(a_processes));
			return static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchSlot)) + layout.SunEntryOffset();
		}

		/** @brief Writes a view's view and per-frame blocks into its latch region, for ShadowLatchedCopiesPass to copy. */
		void WriteViewBlocks(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, std::uint32_t a_latchSlot, std::uint32_t a_slot,
			const PendingView& a_view)
		{
			const std::uint32_t offset = a_layout.ViewBlockOffset(a_slot);
			LatchWrite(a_latchBlock, "shadow view blocks", a_latchSlot, offset, std::as_bytes(std::span(a_view.viewBlock)));
			// PerTechnique c3, DCLFDepthRange (Utility.hlsl): the draws apply the depth range under a [0, 1] viewport, so a change
			// in it changes no recording.
			const float depthRange[4] = { a_view.minDepth, a_view.maxDepth - a_view.minDepth, 0.0f, 0.0f };
			LatchWrite(a_latchBlock, "shadow view blocks", a_latchSlot, offset + static_cast<std::uint32_t>(sizeof(a_view.viewBlock)), std::as_bytes(std::span(depthRange)));
			LatchWrite(a_latchBlock, "shadow per-frame view data", a_latchSlot, offset + static_cast<std::uint32_t>(kShadowPerFrameOffset), std::span(a_view.perFrame.data(), a_view.perFrameBytes));
		}

		/**
		 * @brief Where a view draws: its slot, viewport in its target's slice, and push data (kShadowPushWords): the frame record,
		 * its view blocks and the arena's fixed blocks (ShadowArenaBlocksOf).
		 */
		ShadowFrameView FrameViewOf(const ShadowViewLayout& a_view, std::uint32_t a_capacity, const ShadowResources& a_resources, const ShadowCapacity& a_sizing,
			std::uint64_t a_materialRows, std::uint64_t a_viewBlocks)
		{
			const std::uint32_t a_slot = a_view.slot;
			ShadowFrameView out{};
			out.slot = a_slot;
			out.modeIndex = a_view.modeIndex;
			out.capacity = a_capacity;
			out.x = a_view.x;
			out.y = a_view.y;
			out.width = a_view.width;
			out.height = a_view.height;
			// The depth range is applied by the draws (DCLFDepthRange).
			out.minDepth = 0.0f;
			out.maxDepth = 1.0f;
			out.target = a_view.target;
			out.slice = a_view.slice;
			out.rasterState = a_view.rasterState;
			out.materialRows = a_materialRows;
			out.sequenceDraws = a_sizing.sequenceDraws;
			const std::uint64_t base = a_resources.constantsAddress;
			const std::uint64_t viewBlock = a_viewBlocks + std::uint64_t(a_slot) * kShadowViewSlotBytes;
			auto push = [&](std::uint32_t a_word, std::uint64_t a_address) {
				out.push[a_word] = static_cast<std::uint32_t>(a_address);
				out.push[a_word + 1] = static_cast<std::uint32_t>(a_address >> 32);
			};
			push(kShadowPushFrameRecord, base + kShadowFrameRecordOffset);
			push(kShadowPushViewBlock, viewBlock);
			push(kShadowPushPerFrame, viewBlock + kShadowPerFrameOffset);
			const auto blocks = ShadowArenaBlocksOf();
			push(kShadowPushZeros, base + blocks.zeros);
			push(kShadowPushSharedData, base + blocks.sharedData);
			push(kShadowPushFeatureData, base + blocks.featureData);
			return out;
		}

		/** @brief A view's max count. The slot holds the scene's draw bound (RequireShadowSequences): past it is a defect, not a drop. */
		std::uint32_t ShadowViewCapacity(std::uint32_t a_previous, std::uint32_t a_draws, std::uint32_t a_slotDraws)
		{
			if (a_draws > a_slotDraws)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: a shadow view's {} draws past its sequence buffer's {} (the scene's draw bound missed them)",
					a_draws, a_slotDraws));
			return GrowCapacity(a_previous, a_draws, a_slotDraws);
		}

		/** @brief The revision's ShadowVariants index matching a_frame's shape, or SIZE_MAX. */
		std::size_t VariantOf(const org::async::RevisionFragment& a_shapes, const ShadowFrame& a_frame)
		{
			if (const auto variants = a_shapes.Value<ShadowVariants>())
				for (std::size_t v = 0; v < variants->shapes.size(); ++v)
					if (variants->shapes[v]->SameShape(a_frame))
						return v;
			return SIZE_MAX;
		}

		ShadowViewLayout LayoutOf(const PendingView& a_view, std::uint32_t a_slot, std::uint32_t a_mode, std::uint32_t a_target)
		{
			return { a_slot, a_mode, a_target, a_view.slice, a_view.x, a_view.y, a_view.width, a_view.height, a_view.rasterState };
		}
	}

	bool DrawBoundStore::ShadowShare::KeyOf(std::uint32_t a_mode, ShadowPipelineKey& a_key) const
	{
		// As BuildKeptShadow: an occlusion mode's own technique, else the caster's.
		std::uint32_t modeTechnique;
		if (IsOcclusionMode(a_mode)) {
			modeTechnique = occlusion[OcclusionOfMode(a_mode)];
			if (!modeTechnique)
				return false;
		} else {
			if (!caster)
				return false;
			modeTechnique = technique;
		}
		a_key = { modeTechnique, rasterFlags, vertexLayout };
		return true;
	}

	ShadowBounds Draws::ShadowBoundsOf(const DrawBoundStore& a_bound, const Lookups& a_lookups)
	{
		ShadowBounds out;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			std::uint64_t draws = 0;
			auto& slots = out.keySlotDraws[m];
			for (const auto& [key, keyDraws] : a_bound.modeKeyDraws[m]) {
				// A key with no slot draws nothing yet (its casters wait; the slot brings a new shape).
				const auto it = a_lookups.shadowSlots.find(key);
				if (it == a_lookups.shadowSlots.end())
					continue;
				if (it->second >= slots.size())
					slots.resize(std::size_t(it->second) + 1, 0u);
				slots[it->second] = static_cast<std::uint32_t>(std::min<std::uint64_t>(slots[it->second] + keyDraws, UINT32_MAX));
				draws += keyDraws;
			}
			out.modeDraws[m] = static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX));
		}
		return out;
	}

	std::vector<ShadowViewLayout> Draws::LayoutOf(const ShadowFrame& a_frame)
	{
		std::vector<ShadowViewLayout> out;
		out.reserve(a_frame.views.size());
		for (const auto& view : a_frame.views)
			out.push_back({ view.slot, view.modeIndex, view.target, view.slice, view.x, view.y, view.width, view.height, view.rasterState });
		return out;
	}

	std::shared_ptr<ShadowFrame> Draws::MakeShadowShape(const ShadowResources& a_resources, const ShadowShapeInputs& a_in)
	{
		// A revision's shape has no payload: the scene's bound sizes it.
		static const ShadowPayload kNoPayload;
		const auto& payload = a_in.payload ? *a_in.payload : kNoPayload;
		auto frame = std::make_shared<ShadowFrame>();
		frame->resourceHeap = a_in.resourceHeap;
		frame->samplerHeap = a_in.samplerHeap;
		frame->indirect = a_in.indirect;
		frame->latch = a_in.latch;
		frame->latchLayout = a_in.latchLayout;
		const ShadowCapacity& sizing = a_in.capacity;
		// Each view's row within the bucket counts' words.
		std::vector<RowBuckets> rows;
		rows.reserve(a_in.rows.size());
		std::uint64_t lost = 0;
		for (std::size_t v = 0; v < a_in.rows.size(); ++v) {
			const auto m = v < a_in.views.size() ? a_in.views[v].modeIndex : 0u;
			const auto& draws = a_in.payload || !a_in.bounds ? payload.keySlotDraws[m] : a_in.bounds->keySlotDraws[m];
			rows.push_back(TrimmedRow(a_in.rows[v], sizing.bucketCountWords, draws, lost));
		}
		if (lost) {
			static std::uint32_t reported = 0;
			if (reported++ < 8)
				logger::error("[DCLF] {} claimed shadow draws past the bucket counts' {} words: a claim past the set it was sealed with", lost,
					sizing.bucketCountWords);
		}
		frame->rows = std::make_shared<const std::vector<RowBuckets>>(rows);
		const std::uint64_t materialRows = a_in.materialRows, viewBlocks = a_in.viewBlocks;
		std::vector<std::uint32_t> slotDraws;
		for (std::size_t v = 0; v < a_in.views.size(); ++v) {
			const auto& view = a_in.views[v];
			const auto* previous = PreviousView(a_in.previous, view.slot);
			// Max count and buckets: the payload's draws and the scene's bound (the payload can exceed it for an object changed
			// since the bound was read), within the slot's sequences.
			const std::uint32_t m = view.modeIndex;
			const auto& own = payload.keySlotDraws[m];
			slotDraws.assign(own.begin(), own.end());
			std::uint32_t draws = payload.modeDraws[m];
			// The bound sizes ahead when it fits the slot; else the payload's own draws, which fit the set the claims were made under.
			if (a_in.bounds) {
				const auto& bound = a_in.bounds->keySlotDraws[m];
				std::vector<std::uint32_t> merged(slotDraws);
				if (bound.size() > merged.size())
					merged.resize(bound.size(), 0u);
				std::uint64_t total = 0;
				for (std::size_t k = 0; k < merged.size(); ++k) {
					if (k < bound.size())
						merged[k] = std::max(merged[k], bound[k]);
					total += merged[k];
				}
				const std::uint32_t boundDraws = std::max(draws, a_in.bounds->modeDraws[m]);
				if (boundDraws <= sizing.sequenceDraws && total <= std::uint64_t(kShadowClasses) * sizing.sequenceDraws) {
					slotDraws = std::move(merged);
					draws = boundDraws;
				}
			}
			const std::uint32_t capacity = ShadowViewCapacity(previous ? previous->capacity : 0, draws, sizing.sequenceDraws);
			auto& out = frame->views.emplace_back(FrameViewOf(view, capacity, a_resources, sizing, materialRows, viewBlocks));
			out.buckets = SizeShadowBuckets(rows[v], slotDraws, previous, std::uint64_t(kShadowClasses) * sizing.sequenceDraws);
		}
		// Latched copies per view: blocks from the latch, draw and bucket counts zeroed; then the commit's values.
		if (a_in.zeros) {
			auto& [fromLatch, fromZeros, commit] = frame->latched;
			fromLatch.latch = a_in.latch;
			fromZeros.latch = a_in.zeros;
			fromZeros.fixed = true;
			commit = a_in.latched;
			const auto blocksKey = a_resources.viewBlocks.buffer->Key();
			for (const auto& view : frame->views) {
				if (view.slot >= a_resources.count.size())
					continue;
				const auto slotBytes = static_cast<std::uint32_t>(kShadowViewSlotBytes);
				fromLatch.copies.push_back({ blocksKey, a_in.latchLayout.ViewBlockOffset(view.slot), slotBytes, std::uint64_t(view.slot) * slotBytes });
				fromZeros.copies.push_back({ a_resources.count[view.slot].get(), 0, static_cast<std::uint32_t>(sizeof(kZeroCounts)), 0 });
				fromZeros.copies.push_back({ a_resources.bucketCounts[view.slot]->Key(), 0,
					static_cast<std::uint32_t>(std::max<std::size_t>(view.buckets.size(), 1) * sizeof(std::uint32_t)), 0 });
			}
		}
		return frame;
	}

	ShadowArenaBlocks Draws::ShadowArenaBlocksOf()
	{
		ShadowArenaBlocks out;
		out.zeros = kShadowArenaBlocksOffset;
		const auto* state = globals::state;
		out.sharedBytes = state ? static_cast<std::uint32_t>(sizeof(State::SharedDataCB)) : 0u;
		out.sharedData = out.sharedBytes ? out.zeros + kShadowZeroBlockBytes : out.zeros;
		out.featureBytes = state ? state->featureDataBytes : 0u;
		out.featureData = out.featureBytes ? (out.zeros + kShadowZeroBlockBytes + out.sharedBytes + kConstantAlignment - 1) & ~(kConstantAlignment - 1) : out.zeros;
		return out;
	}

	std::vector<LatchedCopy> Draws::ShadowLatchedLayout(const ShadowResources& a_resources)
	{
		std::vector<LatchedCopy> out;
		const auto targets = a_resources.latchedTargets.load(std::memory_order_acquire);
		const void* key = a_resources.constants.get();
		if (!targets || std::find(targets->begin(), targets->end(), key) == targets->end())
			return out;
		// As the commit's writeBlock: SharedData, then FeatureData when CS has it.
		const auto blocks = ShadowArenaBlocksOf();
		std::size_t used = 0;
		for (const auto& [offset, bytes] : { std::pair{ blocks.sharedData, blocks.sharedBytes }, std::pair{ blocks.featureData, blocks.featureBytes } }) {
			if (!bytes)
				continue;
			const std::size_t at = (used + 15) & ~std::size_t(15);
			out.push_back({ key, static_cast<std::uint32_t>(at), bytes, offset });
			used = at + bytes;
		}
		return out;
	}

	namespace Draws
	{
		/** @brief DCLF's id for the engine rasterizer state at (fill, cull, bias, scissor); 0 when out of the table or empty. */
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

		/** @brief Reads the depth format, viewport and eye the engine just drew a view with from the renderer's state. */
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
		 * @brief Replaces the captured viewport and eye with DCLF's own (SunViews/LightViews/FocusViews, from the light's
		 * UpdateCamera): port times target size. False without DCLF's view this frame (captured values stand).
		 */
		bool ApplyOwnViewport(PendingView& a_view, const SunViews::Cascade* a_cascade, std::uint32_t a_descriptor, std::uint32_t a_target, Diagnostics::ShadowView a_kind)
		{
			Diagnostics::Notify(Diagnostics::OwnShadowViewport{ a_view, a_cascade, a_descriptor, a_target, a_kind });
			auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].texture;
			if (!a_cascade || !texture)
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const auto port = SunViews::Viewport(a_cascade->port, desc.Width, desc.Height);
			const RE::NiPoint3 eye{ a_cascade->eye[0], a_cascade->eye[1], a_cascade->eye[2] };
			a_view.x = port[0];
			a_view.y = port[1];
			a_view.width = port[2];
			a_view.height = port[3];
			a_view.minDepth = 0.0f;
			a_view.maxDepth = 1.0f;
			a_view.eye = eye;
			return true;
		}

		/**
		 * @brief Sets the view's VS_PerFrame block and caster planes from DCLF's own camera (SunViews::Block). a_engineBlock: the
		 * engine's block was captured too, for the observers.
		 */
		void ApplyOwnCamera(PendingView& a_view, const SunViews::Cascade& a_cascade, std::uint32_t a_descriptor, Diagnostics::ShadowView a_kind, bool a_engineBlock)
		{
			if (a_engineBlock)
				Diagnostics::Notify(Diagnostics::OwnShadowCamera{ a_view, a_cascade, a_descriptor, a_kind });
			std::array<std::byte, SunViews::kBlockBytes> block{};
			SunViews::Block(a_cascade, block.data());
			const auto* mine = reinterpret_cast<const float*>(block.data());
			std::memcpy(a_view.perFrame.data(), block.data(), block.size());
			a_view.perFrameBytes = static_cast<std::uint32_t>(block.size());
			std::memcpy(a_view.viewProj.data(), mine + 32, sizeof(float) * 16);
			a_view.hasViewProj = true;
			for (std::uint32_t q = 0; q < 6; ++q)
				for (std::uint32_t i = 0; i < 4; ++i)
					a_view.cullPlanes[q][i] = a_cascade.caster.plane[q][i];
			a_view.cullPlaneMask = a_cascade.caster.mask;
		}

		/**
		 * @brief Draws a focus view over its whole slice. The engine draws it into a sub-rectangle sized from the target's distance
		 * (BSShadowDirectionalLight::sub), new most frames, while DCLF's placements are per slice. The sub-rectangle's viewport
		 * transform moves into the projection (x' = x * sw/W + (sw + 2 sx)/W - 1, y' = y * sh/H + 1 - (sh + 2 sy)/H on c4, c8, c12
		 * and the culling view-projection), so casters land on the same pixels; the rest of the slice is sampled by no receiver.
		 */
		void FocusToSlice(PendingView& a_view, const SunViews::Cascade& a_cascade, std::uint32_t a_target)
		{
			auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].texture;
			if (!texture)
				return;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const float w = static_cast<float>(desc.Width), h = static_cast<float>(desc.Height);
			// Renderer::UpdateViewPort's rectangle, unrounded.
			const auto& port = a_cascade.port;
			const float sx = port[0] * w, sy = (1.0f - port[2]) * h, sw = (port[1] - port[0]) * w, sh = (port[2] - port[3]) * h;
			const float ax = sw / w, bx = (sw + 2.0f * sx) / w - 1.0f;
			const float ay = sh / h, by = 1.0f - (sh + 2.0f * sy) / h;
			auto* block = reinterpret_cast<float*>(a_view.perFrame.data());
			for (const std::uint32_t c : { 4u, 8u, 12u }) {
				float* row0 = block + c * 4;
				float* row1 = row0 + 4;
				const float* row3 = row0 + 12;
				for (std::uint32_t i = 0; i < 4; ++i) {
					row0[i] = ax * row0[i] + bx * row3[i];
					row1[i] = ay * row1[i] + by * row3[i];
				}
			}
			std::memcpy(a_view.viewProj.data(), block + 32, sizeof(float) * 16);
		}

		/**
		 * @brief Captures VS_PerFrame (b12) now, before the next view rewrites it: from the mirror, or CS's cached copy
		 * (CacheFramebuffer) until the mirror has seen a write. Its view-projection is the draws'.
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

	std::uint32_t IndirectDraws::ShadowViewCapacity()
	{
		// None when DCLF draws no shadows: the engine draws every view.
		if (failed || !impl->SetupShadow() || impl->shadowPlacements.empty())
			return 0;
		// Re-import shadow maps the engine replaced; a new import is built at the next build point.
		if (auto* renderer = globals::game::renderer) {
			using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
			for (const auto [index, target] : { std::pair{ 0u, T::kSHADOWMAPS_ESRAM }, std::pair{ 1u, T::kSHADOWMAPS }, std::pair{ 2u, T::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM } })
				if (renderer->GetDepthStencilData().depthStencils[target].texture)
					(void)impl->ImportShadowDepth(index, target);
		}
		// None while the built graph lacks a new import, or the frame has no claims.
		if (auto* host = RenderGraphRuntime::Get().Host(); !host || host->RebuildRequested() || Scene::FrameView::Get().SetWithdrawn())
			return 0;
		// Every view: each draws at its placement's slot.
		return UINT32_MAX;
	}

	void IndirectDraws::DecideShadowCoverage()
	{
		if (failed || !impl->shadow || impl->shadowPlacements.empty())
			return;
		auto& views = ShadowViews::Get();
		const auto all = views.All();
		if (std::none_of(all.begin(), all.end(), [](const ShadowViews::View& a_view) { return a_view.covered; }))
			return;
		// The views are DCLF's once the selected revision has the shadow epoch's recording (shaped for all placements) on the built
		// graph and the installed publication has a shadow payload; until then (startup, a toggle) the engine's. Both are trusted.
		if (!impl->EpochCovered(Epoch::Shadow) || !impl->installedDraws || !impl->installedDraws->shadow) {
			++impl->shadowUnrecorded;
			views.UncoverAll();
		}
	}

	void IndirectDraws::BeginShadowFrame()
	{
		impl->pendingViews.clear();
	}

	void IndirectDraws::CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode)
	{
		// Captures where the engine just drew the view and its constants, for ExecuteShadowFrame. Nothing is drawn here.
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
		// The tables are not read while the engine draws the shadow maps: ExecuteShadowFrame checks them.
		const auto* shadowView = ShadowViews::Get().At(a_viewId);
		if (!shadowView)
			return notReady(ShadowNotReady::Tables);
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + kShadowModeCount)
			return notReady(ShadowNotReady::Tables);
		// Not DCLF's this frame (no revision shape for the placements yet): the engine drew it whole.
		if (!shadowView->covered) {
			++shadowStats.uncovered;
			return;
		}
		// The target is DCLF's prediction (ShadowViews::PredictTargets); a cascade's first capture of the frame is its volumetric copy.
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t occurrence = ShadowViews::Get().NextCapture(a_viewId);
		const bool copyDraw = shadowView->volumetricCopy && occurrence == 0;
		const std::uint32_t target = copyDraw ? static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM) : shadowView->drawTarget;
		const std::uint32_t slice = shadowView->drawSlice;
		Diagnostics::Notify(Diagnostics::ShadowTargetPredicted{ a_viewId, shadowView, occurrence, target, slice });
		const std::uint32_t targetIndex = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM                     ? 0u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS                           ? 1u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM ? 2u :
		                                                                                                                     ~0u;
		// The volumetric copy holds only volumetric-only casters (batch group 15, all the engine's flag-0x100 draw renders): casterClass 1.
		const bool volumetricCopy = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;
		if (targetIndex == ~0u || !impl->ImportShadowDepth(targetIndex, target)) {
			// Logged once per target: a view the design has not met.
			if (target < 32 && !((impl->shadowLoggedTargets >> target) & 1)) {
				impl->shadowLoggedTargets |= 1u << target;
				logger::info("[DCLF] shadow view {} ({}, light {} descriptor {}, mode {:#x}) draws into depth target {} slice {}; it stays native",
					a_viewId, ShadowViews::KindName(shadowView->kind), shadowView->lightIndex, shadowView->descriptor, a_renderMode, target, slice);
			}
			return notReady(ShadowNotReady::Depth);
		}
		// The engine's rasterizer table entry for this view, read now: CS's ShadowmapCascadeRasterizerFix swaps per-cascade depth-bias
		// copies into the table for exactly this window. The cull mode is taken at 1, not the renderer's: the Utility shader sets it
		// per pass (0 for a two-sided property, 1 otherwise), so the renderer's is whatever drew last. Two-sided keys draw without
		// culling (kRasterTwoSided) as the engine's pass does, so the state changes only with the view. A view with no entry at 1
		// (the volumetric copy, culling off) takes its entry at 0.
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
		view.sunView = shadowView->kind == ShadowViews::Kind::Directional && !shadowView->focus;
		CaptureViewTarget(view, target);
		// The sun's, local lights' and focus views' viewport and eye are DCLF's; the renderer's are the observers'.
		const std::uint32_t sceneFrame = Scene::FrameView::Get().GetFrame();
		const SunViews::Cascade* ownView = nullptr;
		auto ownKind = Diagnostics::ShadowView::Sun;
		const bool localView = !view.sunView && !shadowView->focus &&
		                       (shadowView->kind == ShadowViews::Kind::Frustum || shadowView->kind == ShadowViews::Kind::Parabolic);
		if (view.sunView) {
			ownView = SunViews::Get().CascadeOf(shadowView->descriptor, sceneFrame);
		} else if (localView) {
			ownView = LightViews::Get().CascadeOf(shadowView->light, shadowView->descriptor, sceneFrame);
			ownKind = Diagnostics::ShadowView::Light;
		} else if (shadowView->focus) {
			ownView = FocusViews::Get().CascadeOf(shadowView->light, shadowView->descriptor, sceneFrame);
			ownKind = Diagnostics::ShadowView::Focus;
			view.focusTarget = FocusViews::Get().TargetOf(shadowView->descriptor, sceneFrame);
			view.focus = true;
		}
		const bool owned = (view.sunView || localView || shadowView->focus) && ApplyOwnViewport(view, ownView, shadowView->descriptor, target, ownKind);
		// A focus view draws over its whole slice (FocusToSlice).
		if (owned && shadowView->focus)
			if (auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[target].texture) {
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				view.x = 0;
				view.y = 0;
				view.width = desc.Width;
				view.height = desc.Height;
			}
		// The placement slot: foreseen by setup, under its state, the frame's only view there; else a defect (a hole).
		{
			const std::size_t placement = impl->PlacementOf(view);
			std::string defect;
			if (placement == SIZE_MAX)
				defect = "at no placement";
			else if (const auto& p = impl->shadowPlacements[placement]; p.rasterState != rasterState || p.modeIndex != view.modeIndex)
				defect = fmt::format("at placement {} (mode {}, state {}) with mode {}, state {}", placement, p.modeIndex, p.rasterState, view.modeIndex, rasterState);
			else if (std::any_of(impl->pendingViews.begin(), impl->pendingViews.end() - 1, [&](const PendingView& a_other) { return a_other.slot == p.slot; }))
				defect = fmt::format("at placement {}, which a view of the frame already drew at", placement);
			if (!defect.empty()) {
				if (impl->placementDefectsLogged++ < 16)
					logger::warn("[DCLF] shadow view {} ({}, mode {:#x}) drew into target {} slice {} at ({} {}) {}x{} {} <- PLACEMENT", a_viewId,
						ShadowViews::KindName(shadowView->kind), a_renderMode, targetIndex, slice, view.x, view.y, view.width, view.height, defect);
				impl->pendingViews.pop_back();
				return notReady(ShadowNotReady::Placement);
			}
			view.slot = impl->shadowPlacements[placement].slot;
		}
		// The engine's caster volume: BSShadowDirectionalLight::UpdateCamera builds it from the main frustum's corners and the light
		// direction into the descriptor's culling process, and the accumulation's cull (FUN_1414f0920) tests it on top of the shadow
		// frustum. Much tighter than the orthographic box, it keeps casters no visible receiver can see out of the far cascade.
		// A focus view has no culling process (its descriptor indexes the focus array).
		if (auto& lightData = const_cast<RE::BSShadowLight*>(shadowView->light)->GetRuntimeData();
			!shadowView->focus && shadowView->descriptor < lightData.shadowmapDescriptors.size()) {
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
		// PerTechnique (b0): HighDetailRange (LOD landscape only; zero until owned) and ParabolaParam from the engine's two globals as
		// SetupTechnique reads them.
		{
			static const REL::Relocation<float*> parabolaRadius{ REL::Offset(Engine::Address::kParabolaRadius) };
			static const REL::Relocation<float*> parabolaSide{ REL::Offset(Engine::Address::kParabolaSide) };
			const float radius = *parabolaRadius.get();
			view.viewBlock[4] = radius != 0.0f ? 1.0f / radius : 0.0f;
			view.viewBlock[5] = *parabolaSide.get();
			// c2 (DCLFEyeDelta) unused: records are absolute and Utility.hlsl subtracts the view's CameraPosAdjust.
			view.viewBlock[8] = view.viewBlock[9] = view.viewBlock[10] = 0.0f;
		}
		// DCLF's view-projection; the engine's block is captured only for the observers.
		const bool engineBlock = Policy::CaptureEngineShadowBlock();
		const bool capturePerFrame = !owned || engineBlock;
		if (capturePerFrame)
			CapturePerFrame(view);
		if (owned)
			ApplyOwnCamera(view, *ownView, shadowView->descriptor, ownKind, engineBlock);
		// After the observers compared the engine's block.
		if (owned && shadowView->focus)
			FocusToSlice(view, *ownView, target);
		// Capture check: the block's CameraPosAdjust (c40) against the renderer's eye for the view.
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

	void IndirectDraws::OcclusionView(std::uint32_t a_view, const RE::NiCamera& a_camera)
	{
		// DCLF's view from the camera Skylighting just set up for the map (Precipitation's occlusion camera; for the sky map a quarter
		// of its square, SetViewFrustum), before RenderMask draws it: SunViews' matrices and eye, port times map size; depth target 10
		// (Skylighting swaps its own texture in for the sky map), slice 0; back-face culling, solid fill, no bias or scissor (the
		// Utility shader's state but for two-sided keys). Computed every frame the map renders, drawn or not: next frame's build
		// prepares pipelines for its state and format.
		if (a_view >= kOcclusionViews || !ActiveToggles().shadows || failed || !OcclusionEnabled(a_view) || !impl->SetupShadow())
			return;
		constexpr auto target = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP);
		const std::uint32_t rasterState = EngineRasterStateId(0, 1, 0, 0, kOcclusionRenderMode);
		if (!rasterState || !impl->ImportShadowDepth(OcclusionDepthTarget(a_view), target))
			return;
		auto& occlusion = impl->occlusion[a_view];
		const std::uint32_t mode = OcclusionModeOf(a_view);
		if (!impl->readyModes[mode]) {
			impl->readyModes[mode] = true;
			impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("occlusion map {} set up before its setup", a_view));
		}
		if (const auto& states = impl->readyStates[mode].Of(false); !std::binary_search(states.begin(), states.end(), rasterState)) {
			impl->readyStates[mode].Add(rasterState, false);
			impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("occlusion map {} draws with rasterizer state {}, not in the catalog", a_view, rasterState));
		}
		const auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[target];
		if (!depth.texture || !depth.views[0])
			return;
		auto& view = occlusion.view;
		view = {};
		view.viewId = ~0u;
		view.renderMode = kOcclusionRenderMode;
		view.modeIndex = mode;
		view.targetIndex = OcclusionDepthTarget(a_view);
		view.slice = 0;
		view.rasterState = rasterState;
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		depth.views[0]->GetDesc(&dsvDesc);
		view.dsvFormat = dsvDesc.Format;
		D3D11_TEXTURE2D_DESC desc{};
		depth.texture->GetDesc(&desc);
		SunViews::Cascade camera;
		SunViews::CameraMatrices(a_camera, camera);
		const auto port = SunViews::Viewport(camera.port, desc.Width, desc.Height);
		view.x = port[0];
		view.y = port[1];
		view.width = port[2];
		view.height = port[3];
		view.minDepth = 0.0f;
		view.maxDepth = 1.0f;
		view.eye = { camera.eye[0], camera.eye[1], camera.eye[2] };
		SunViews::Block(camera, view.perFrame.data());
		view.perFrameBytes = SunViews::kBlockBytes;
		std::memcpy(view.viewProj.data(), reinterpret_cast<const float*>(view.perFrame.data()) + 32, sizeof(float) * 16);
		view.hasViewProj = true;
		occlusion.rasterState = rasterState;
		if (view.dsvFormat != occlusion.dsvFormat) {
			impl->NoteCapability(impl->shadowCatalogBuilt && occlusion.dsvFormat != DXGI_FORMAT_UNKNOWN,
				fmt::format("occlusion map {} draws into format {}, not {}", a_view, static_cast<int>(view.dsvFormat), static_cast<int>(occlusion.dsvFormat)));
			occlusion.dsvFormat = view.dsvFormat;
		}
		occlusion.capturedFrame = Scene::FrameView::Get().GetFrame();
		// A revision input (PredictedOcclusion).
		impl->PostRevisionInputs();
	}

	void IndirectDraws::CaptureOcclusion(std::uint32_t a_view)
	{
		// The engine's RenderMask just drew the map: for the observers only.
		Diagnostics::Notify(Diagnostics::OcclusionCaptured{ a_view });
	}

	std::vector<ShadowViewLayout> IndirectDraws::Impl::PredictedOcclusion() const
	{
		std::vector<ShadowViewLayout> out;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			const auto& o = occlusion[v];
			if (!OcclusionEnabled(v) || !o.rasterState || o.dsvFormat == DXGI_FORMAT_UNKNOWN)
				continue;
			out.push_back(LayoutOf(o.view, OcclusionSlot(v), OcclusionModeOf(v), OcclusionDepthTarget(v)));
		}
		return out;
	}

	bool IndirectDraws::OcclusionReady(std::uint32_t a_view) const
	{
		// Ready when this frame's shadow commit uploaded the occluders and the map's target is imported.
		if (a_view >= kOcclusionViews || failed || !ActiveToggles().shadows || !OcclusionEnabled(a_view) || !impl->shadow)
			return false;
		const auto& occlusion = impl->occlusion[a_view];
		// Not while the built graph lacks the map's import, nor without the selected revision's recorded shape for the maps' layout
		// (the engine draws the map meanwhile).
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || host->RebuildRequested())
			return false;
		const auto layouts = impl->PredictedOcclusion();
		bool shaped = false;
		if (impl->EpochCovered(Epoch::Occlusion))
			if (const auto& active = impl->revisions.active)
				if (const auto& fragment = active->Fragment(Impl::SceneRevisions::kShapeSlot + Epoch::Occlusion))
					if (const auto variants = fragment->Value<ShadowVariants>())
						for (const auto& shape : variants->shapes)
							shaped = shaped || (shape && LayoutOf(*shape) == layouts);
		if (!shaped) {
			++impl->occlusionUnrecorded;
			return false;
		}
		return occlusion.rasterState && occlusion.committedFrame == Scene::FrameView::Get().GetFrame() && impl->shadow->depth[OcclusionDepthTarget(a_view)];
	}

	std::uint32_t IndirectDraws::ExecuteOcclusion(std::uint32_t a_views)
	{
		ZoneScopedN("CS.DCLF.ExecuteOcclusion");
		const auto start = std::chrono::steady_clock::now();
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		// The requested views that are ready and captured this frame; a map the engine did not draw draws nothing.
		std::uint32_t drawable = 0;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (!(a_views & (1u << v)))
				continue;
			if (OcclusionReady(v) && impl->occlusion[v].capturedFrame == frameNumber)
				drawable |= 1u << v;
			else
				++shadowStats.occlusionNotReady[v];
		}
		const auto indirect = FrameShadowIndirectState();
		if (!drawable || !indirect.valid) {
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				shadowStats.occlusionNotReady[v] += (drawable & (1u << v)) ? 1 : 0;
			return 0;
		}
		ScopedPerfEvent event("CS DCLF: occlusion maps (CPU)");
		auto resources = impl->shadow;
		// The revision's variant for the drawable maps and the payload the shadow commit read: trusted, nothing is checked here.
		std::vector<ShadowViewLayout> layouts;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (drawable & (1u << v))
				layouts.push_back(LayoutOf(impl->occlusion[v].view, OcclusionSlot(v), OcclusionModeOf(v), OcclusionDepthTarget(v)));
		const auto revision = impl->RevisionOf(Epoch::Occlusion);
		std::shared_ptr<const ShadowFrame> revisionShape;
		std::size_t revisionVariant = 0;
		if (revision)
			if (const auto variants = revision.shape->Value<ShadowVariants>())
				for (std::size_t v = 0; v < variants->shapes.size() && !revisionShape; ++v)
					if (variants->shapes[v] && LayoutOf(*variants->shapes[v]) == layouts) {
						revisionShape = variants->shapes[v];
						revisionVariant = v;
					}
		const auto committed = impl->committedShadow;
		if (!revisionShape || !committed) {
			// A layout the revision lacks (one map drawn without the other): the engine draws them.
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				shadowStats.occlusionNotReady[v] += (drawable & (1u << v)) ? 1 : 0;
			return 0;
		}
		const auto& payload = *committed;
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::SkyOcclusion, [&](org::RenderGraph&) {
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			const ShadowFrame& target = *revisionShape;
			const auto& latchBlock = *target.latch;
			const auto& latchLayout = target.latchLayout;
			auto& scene = *resources->scene;
			std::size_t index = 0;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				if (!(drawable & (1u << v)))
					continue;
				const auto& view = impl->occlusion[v].view;
				const auto& shapeView = target.views[index];
				const auto& buckets = (*target.rows)[index++];
				const std::uint32_t slot = OcclusionSlot(v), mode = OcclusionModeOf(v);
				const auto inputCount = static_cast<std::uint32_t>(payload.Inputs());  // the whole shadow list
				// Occluders, material rows, frame record, objects and geometries are in the ring entry the shadow commit read.
				WriteViewBlocks(latchBlock, latchLayout, latchSlot, slot, view);
				// Frustum culling (near plane included), Skylighting::OcclusionTechnique's size test (bound radius above 32), and fade
				// roots as the engine's cull leaves them (kCullFadeOnVisible): never a root the main camera faded out or has not seen yet.
				auto latch = ShadowViewLatch(view, inputCount, frameNumber);
				static const REL::Relocation<const std::uint8_t*> fadesOn{ REL::Offset(Engine::Address::kFadesOn) };
				latch.cullFlags = 1u | kCullMinRadius | (*fadesOn.get() ? kCullFadeOnVisible : 0u) | KeyWordOf(mode);
				latch.viewBits = ShadowViewBits(mode, false);
				latch.fadeStatesIndex = scene.FadeStatesReadIndex(impl->Adopted(), frameNumber);
				Impl::ShadowRingLatch(impl->ringShadow, mode, latch);
				UseShadowMapRow(latchBlock, latchLayout, buckets, latchSlot, mode, view.rasterState, true, latch);
				WriteBucketTable(latchBlock, latchLayout, latchSlot, slot, shapeView.buckets, latch);
				LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				shadowStats.occlusionInputs[v] = inputCount;
				++shadowStats.occlusionDrawn[v];
			}
			impl->SubmitRevisionRecording(Epoch::Occlusion, *revision.recordings, revisionVariant);
		}, impl->shadowExecutionOwner);
		shadowStats.occlusionMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		++shadowStats.occlusionEpochs;
		if (!ok) {
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				if (drawable & (1u << v)) {
					--shadowStats.occlusionDrawn[v];
					++shadowStats.occlusionNotReady[v];
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
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !impl->shadow || impl->shadowPlacements.empty())
			return notReady(ShadowNotReady::Setup);
		const auto indirect = FrameShadowIndirectState();
		if (!indirect.valid) {
			impl->ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		const std::uint32_t frameNumber = Scene::FrameView::Get().GetFrame();
		auto resources = impl->shadow;
		std::array<bool, kShadowModeCount> modeUsed{};
		// Inputs and pipelines cover every catalog state per mode (UpdateShadowCapability), so which views a frame draws (cascades
		// alternate depth-bias states, point-light views come and go) changes nothing the build reads. An unforeseen mode or state
		// is a defect, added to the catalog now.
		for (const auto& view : pending) {
			modeUsed[view.modeIndex] = true;
			if (!impl->readyModes[view.modeIndex]) {
				impl->readyModes[view.modeIndex] = true;
				impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("a view of render mode {:#x}", view.renderMode));
			}
			if (const auto& states = impl->readyStates[view.modeIndex].Of(view.casterClass != 0); !std::binary_search(states.begin(), states.end(), view.rasterState)) {
				impl->readyStates[view.modeIndex].Add(view.rasterState, view.casterClass != 0);
				impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("view {} (mode {:#x}) draws with rasterizer state {}, not in the catalog", view.viewId, view.renderMode, view.rasterState));
			}
		}
		// The occlusion epoch draws later from this build, whether or not the engine draws the map this frame.
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (OcclusionEnabled(v) && impl->readyModes[OcclusionModeOf(v)] && impl->occlusion[v].dsvFormat != DXGI_FORMAT_UNKNOWN)
				modeUsed[OcclusionModeOf(v)] = true;
		std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			if (modeUsed[m])
				modeRasterStates[m] = impl->readyStates[m];
		// One pipeline set per shadow map format: every engine target is D16.
		const DXGI_FORMAT dsvFormat = pending.front().dsvFormat;
		if (dsvFormat != impl->shadowTargetFormat) {
			impl->shadowTargetFormat = dsvFormat;
			impl->NoteCapability(impl->shadowCatalogBuilt, fmt::format("a view draws into format {}", static_cast<int>(dsvFormat)));
		}
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0;

		ShadowInputs in = impl->PrepareShadowInputs(*resources, modeUsed, modeRasterStates);
		// The revision's shape and recording and the installed payload: trusted, nothing is checked here. The epoch covers every
		// placement; a slot without a view does no work (zero latch, counters zeroed, its render pass loads and stores the engine's).
		const std::vector<ShadowViewLayout>& layouts = impl->shadowPlacements;
		const auto revision = impl->RevisionOf(Epoch::Shadow);
		const std::shared_ptr<ShadowPayload> ahead = impl->installedDraws ? impl->installedDraws->shadow : nullptr;
		if (!revision || !ahead || ahead->inputs.addresses.identity != in.addresses.identity) {
			if (revision && impl->shadowUnbuilt++ == 0)
				logger::warn("[DCLF] the shadow epoch of frame {} has no payload for its resources in the installed publication: not submitted <- UNBUILT", frameNumber);
			return notReady(ShadowNotReady::Epoch);
		}
		const auto revisionShape = revision.shape->Value<ShadowVariants>()->shapes.front();
		const ShadowFrame& target = *revisionShape;
		// Clamped to the latch the producer sized (ReserveShadowLatch).
		if (in.sunEntryProcesses.size() > target.latchLayout.sunProcesses) {
			++impl->revisions.latchClamped[Epoch::Shadow];
			in.sunEntryProcesses.resize(target.latchLayout.sunProcesses);
		}
		ShadowPayload* committed = ahead.get();
		auto& async = stats.async[kAsyncShadow];
		const auto cleanup = RenderGraphRuntime::Get().Host()->ResourceCleanup();
		if (!cleanup)
			return;
		auto frameOwners = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::ShadowView, [&](org::RenderGraph&) {
			ZoneScopedN("CS.DCLF.ShadowInputs");
			struct BodyTimer
			{
				std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				double& out;
				~BodyTimer() { out = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
			} bodyTimer{ {}, bodyMs };
			bodyTimer.start = std::chrono::steady_clock::now();

			TracyCZoneN(shadowPrepareZone, "CS.DCLF.ShadowInputs.Prepare", true);
			const auto prepareStart = std::chrono::steady_clock::now();
			++async.used;
			// The publication was built before the frame's full-frustum cull.
			ahead->inputs.sunEntryProcesses = in.sunEntryProcesses;
			impl->committedShadow = ahead;
			ShadowPayload& payload = *ahead;
			// The payload is read from the frame's ring entry the producer filled; only the arena is uploaded here. The occlusion
			// epoch reads the same entry.
			impl->ringShadow = impl->ringFrame;
			impl->ringShadow.draws.reset();
			++impl->ringStats.shadowCommitted;
			// Copied: a frame that keeps the publication commits the payload again.
			*frameOwners = payload.bindingOwners;
			// The adopted snapshot lives until this execution (and the occlusion epoch's, which shares these owners) retires.
			if (impl->adoptedSnapshot)
				frameOwners->push_back(impl->adoptedSnapshot);
			// Cleared geometry slots' buffers, held until this execution retires.
			for (auto& owner : Scene::FrameView::Get().TakeRetiredImports())
				frameOwners->push_back(std::move(owner));
			impl->shadowExecutionOwner = frameOwners;
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
			TracyCZoneEnd(shadowPrepareZone);

			// The adopted set: the versions the commit writes into, and the SRV indices it writes.
			const auto& named = impl->Adopted();
			CommitUploads uploads(impl->commitStagedPool, named);
			auto& arena = payload.arena;
			TracyCZoneN(shadowCommitZone, "CS.DCLF.ShadowInputs.CommitShared", true);
			const auto inputsStart = std::chrono::steady_clock::now();
			// A revision input: the producer requires the rows for it (MakeRevision).
			if (std::exchange(impl->shadowRowsWanted, payload.rowsWanted) != payload.rowsWanted)
				impl->PostRevisionInputs();
			shadowStats.waitingRows = payload.waitingRows;
			auto& scene = *resources->scene;
			shadowStats.faceUploads += UploadFaceStreams(named, payload.faceStreams, scene.facePositions, scene.faceUploaded, uploads);
			UploadTrees(named, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetFrame(), scene, uploads, false);
			shadowStats.records = static_cast<std::uint32_t>(payload.materialRows.Count());
			shadowStats.skippedTexture = payload.skippedTexture;
			shadowStats.skippedPipeline = payload.skippedPipeline;
			shadowStats.deferredTextures = payload.deferredTextures;
			shadowStats.deferredPipelines = payload.deferredPipelines;
			std::size_t casterInputs = 0;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				if (modeUsed[m])
					casterInputs += payload.ModeInputs(m);
			shadowStats.inputs = static_cast<std::uint32_t>(casterInputs);
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
			TracyCZoneEnd(shadowCommitZone);

			TracyCZoneN(shadowViewsZone, "CS.DCLF.ShadowInputs.BuildViews", true);
			const auto blocksStart = std::chrono::steady_clock::now();
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			resources->labels.clear();
			const auto& latchBlock = *target.latch;
			const auto& latchLayout = target.latchLayout;
			// The index pool both this epoch and the occlusion epoch draw from.
			{
				ZoneScopedN("CS.DCLF.ShadowInputs.IndexPool");
				UpdateIndexPool(named, *resources->pool, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetTablesGeneration(), latchBlock, latchSlot,
					latchLayout.PoolOffset(), uploads);
			}
			std::vector<bool> mapRowsWritten(latchLayout.MapRows());  // per mode and state (MapRowOf): its row is in the latch
			std::uint32_t sunEntryOffset = 0;  // the slot's sun entry region, once written
			std::vector<bool> slotDrawn(layouts.size());
			for (const auto& view : pending) {
				ZoneScopedN("CS.DCLF.ShadowInputs.View");
				const std::uint32_t slot = view.slot;
				const std::uint32_t index = slot - kFirstShadowViewSlot;
				slotDrawn[index] = true;
				WriteViewBlocks(latchBlock, latchLayout, latchSlot, slot, view);
				// The whole shadow list: the view bits select the mode's inputs.
				const auto inputCount = static_cast<std::uint32_t>(payload.Inputs());
				// Frustum culling, no engine-visibility gate. Only class-split modes test the caster class (not the paraboloids).
				auto latch = ShadowViewLatch(view, inputCount, frameNumber);
				const std::uint32_t classFlags = view.casterClass ? kCullVolumetricOnly : view.modeIndex == kParabolicShadowMode ? 0u : kCullCastersOnly;
				latch.cullFlags = (view.hasViewProj ? (1u | (view.renderMode == 0xE ? kCullNoNearPlane : 0u)) : 0u) | classFlags |
				                  (view.sunView ? kCullSunEntry : 0u) | KeyWordOf(view.modeIndex);
				latch.viewBits = ShadowViewBits(view.modeIndex, view.sunView);
				// A focus view draws its target's members alone (root slot + 1; an unlisted target draws nothing).
				if (view.focus) {
					const auto& rootIndex = Scene::FrameView::Get().GetTables().fadeRootIndex;
					const auto root = view.focusTarget ? rootIndex.find(view.focusTarget) : rootIndex.end();
					latch.focusRoot = root != rootIndex.end() ? root->second + 1 : kFocusRootNone;
				}
				latch.cullPlaneMask = view.cullPlaneMask;
				std::memcpy(latch.cullPlanes, view.cullPlanes, sizeof(latch.cullPlanes));
				latch.fadeStatesIndex = resources->scene->FadeStatesReadIndex(named, frameNumber);
				Impl::ShadowRingLatch(impl->ringShadow, view.modeIndex, latch);
				// Sun entry rule: BuildDraws tests each input's entry sphere against the full-frustum processes (written once per frame).
				if (view.sunView) {
					if (!sunEntryOffset)
						sunEntryOffset = WriteSunEntryRegion(latchBlock, latchLayout, latchSlot, payload.inputs.sunEntryProcesses);
					latch.sunEntryOffset = sunEntryOffset;
				}
				const std::uint32_t mapRow = view.rasterState ? latchLayout.MapRowOf(view.modeIndex, view.rasterState) : ~0u;
				const bool rowWritten = mapRow < mapRowsWritten.size() && mapRowsWritten[mapRow];
				if (mapRow < mapRowsWritten.size())
					mapRowsWritten[mapRow] = true;
				UseShadowMapRow(latchBlock, latchLayout, (*target.rows)[index], latchSlot, view.modeIndex, view.rasterState, !rowWritten, latch);
				WriteBucketTable(latchBlock, latchLayout, latchSlot, slot, target.views[index].buckets, latch);
				LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				Diagnostics::Notify(Diagnostics::ShadowViewLatched{ view, latch, frameNumber, payload, shadowStats });
				resources->labels.push_back({ view.viewId, view.renderMode, slot });
			}
			// Placements without a view this frame: a zero latch, no work.
			for (std::size_t index = 0; index < layouts.size(); ++index) {
				if (slotDrawn[index])
					continue;
				LatchWriteValue(latchBlock, "shadow culling latches", latchSlot, layouts[index].slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), BuildDrawsLatch{});
				++shadowStats.retainedViews;
			}
			// The arena, with the frame record patched to this frame's per-frame buffers (the build ahead has another frame's).
			// Uploaded around the record, not over it: copies within one batch are unordered.
			if (const auto& bytes = arena.Bytes(); !bytes.empty()) {
				DrawBindings record = payload.frameRecord;
				record.textures[kObjectBufferRegister] = impl->ringShadow.objectsIndex;
				record.textures[kExtrasBufferRegister] = impl->ringShadow.extrasIndex;
				record.textures[kPlacementBufferRegister] = in.addresses.placementsIndex;
				record.textures[kPaletteBufferRegister] = in.addresses.palettesIndex;
				record.textures[kShadingBufferRegister] = in.addresses.shadingIndex;
				record.textures[kTreeWindRegister] = resources->scene->TreeWindReadIndex(named, frameNumber);
				constexpr std::size_t recordEnd = kShadowFrameRecordOffset + sizeof(DrawBindings);
				if (bytes.size() < recordEnd)
					stl::report_and_fail("Drawcall Limit Fix: a shadow arena without its frame record");
				if (kShadowFrameRecordOffset)
					uploads(resources->constants, bytes.data(), kShadowFrameRecordOffset, 0);
				uploads(resources->constants, &record, sizeof(record), kShadowFrameRecordOffset);
				if (bytes.size() > recordEnd)
					uploads(resources->constants, bytes.data() + recordEnd, bytes.size() - recordEnd, recordEnd);
			}
			// CS's SharedData and FeatureData as of now (the water reflection prepasses refreshed them after the build), latched: the
			// epoch's first pass copies them after the arena.
			auto latched = LatchedUploads(target.latched[ShadowFrame::kCommitList], uploads, latchSlot);
			// At their fixed places (ShadowArenaBlocksOf); a size mismatch with CS's block is a contract violation.
			const auto blocks = ShadowArenaBlocksOf();
			const auto writeBlock = [&](std::uint64_t a_offset, std::uint32_t a_size, const std::vector<std::byte>& a_bytes) {
				if (!a_size)
					return;
				if (a_bytes.size() != a_size)
					stl::report_and_fail(fmt::format("Drawcall Limit Fix: a shadow commit's CS block of {} bytes for its place of {}", a_bytes.size(), a_size));
				latched(resources->constants, a_bytes.data(), a_bytes.size(), a_offset);
			};
			writeBlock(blocks.sharedData, blocks.sharedBytes, in.sharedData);
			writeBlock(blocks.featureData, blocks.featureBytes, in.featureData);
			(void)latched.Finish();
			impl->revisions.latchedMisses += latched.Misses();
			impl->SubmitRevisionRecording(Epoch::Shadow, *revision.recordings, 0);
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			static std::uint32_t logged = 0;
			if (logged++ % 600 == 0) {
				std::string views;
				for (const auto& p : pending) {
					const auto& view = target.views[p.slot - kFirstShadowViewSlot];
					views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", p.viewId,
						p.renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.ModeInputs(view.modeIndex), view.capacity);
				}
				logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} material rows ({} waiting for the table to grow, {} held): {}",
					target.views.size(), shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, payload.waitingRows,
					resources->materialRows.RowsIn(named), views);
			}
			TracyCZoneEnd(shadowViewsZone);
		}, frameOwners);
		auto& payload = *committed;
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own compile, prepare and record
		if (ok) {
			++shadowStats.epochs;
			shadowStats.viewsDrawn += static_cast<std::uint32_t>(pending.size());
			// The occlusion maps' occluders are uploaded: each map can be DCLF's this frame.
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
				if (!modeUsed[OcclusionModeOf(v)])
					continue;
				auto& occlusion = impl->occlusion[v];
				occlusion.committedFrame = frameNumber;
				occlusion.inputs = static_cast<std::uint32_t>(payload.ModeInputs(OcclusionModeOf(v)));
			}
			impl->ReadShadowCullCounters(frameNumber, shadowStats);
			for (std::uint32_t m = 0; m < kFirstOcclusionMode && m < shadowStats.casters.size(); ++m)
				if (modeUsed[m])
					shadowStats.casters[m] = static_cast<std::uint32_t>(payload.ModeInputs(m));
			shadowStats.setWaiting += payload.setWaiting;
			if (shadowStats.setWaitingFirst.empty())
				shadowStats.setWaitingFirst = payload.setWaitingFirst;
			if (PassCapture::ShadowWithholdingEnabled()) {
				// The cascades' casters decide which sun entries the next frame's cascade culls may skip.
				if (modeUsed[kSunShadowMode])
					SunAccumulation::Get().PublishExclusion(payload.sunExclusion);
				// Likewise the paraboloids for point-light culls (none when no point light drew: a new light is the engine's).
				LocalLightCull::Publish(modeUsed[kParabolicShadowMode] ? payload.parabolicExclusion : nullptr);
			}
			pending.clear();
		} else {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			notReady(ShadowNotReady::Epoch);
		}
		shadowStats.cpuMs += totalMs;
	}

	const RowBuckets& IndirectDraws::Impl::ShadowRowBuckets(RowBucketsCache& a_cache, std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups,
		const ShadowIndirectState& a_indirect)
	{
		auto& cache = a_cache;
		if (cache.lookups != a_lookups.instance || cache.generation != a_lookups.shadowGeneration || cache.version != a_indirect.version) {
			cache.lookups = a_lookups.instance;
			cache.generation = a_lookups.shadowGeneration;
			cache.version = a_indirect.version;
			for (auto& rows : cache.rows)
				rows.clear();
		}
		auto& rows = cache.rows[a_mode < kShadowModeCount ? a_mode : 0];
		if (a_state >= rows.size())
			rows.resize(std::size_t(a_state) + 1);
		auto& row = rows[a_state];
		if (!row) {
			ZoneScopedN("CS.DCLF.ShadowInputs.RowBuckets");
			row = BucketsOfRow(a_lookups.ShadowMapRow(a_mode, a_state), a_indirect);
		}
		return *row;
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

	ShadowInputs IndirectDraws::Impl::PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const
	{
		ZoneScopedN("CS.DCLF.PrepareShadowInputs");
		ShadowInputs in;
		in.frameNumber = Scene::FrameView::Get().GetFrame();
		in.modeUsed = a_modeUsed;
		in.modeRasterStates = a_modeRasterStates;
		// The sun's full-frustum planes (kCullSunEntry), DCLF's as UpdateCamera set up each process (SunViews): this frame's at the
		// shadow epoch, last frame's for the build ahead (posted before this frame's UpdateCamera), as the engine held them there.
		if (const auto& sunViews = SunViews::Get().Current();
			sunViews.valid && (sunViews.sceneFrame == in.frameNumber || sunViews.sceneFrame + 1 == in.frameNumber) && !sunViews.fullFrusta.empty()) {
			for (const auto& mine : sunViews.fullFrusta) {
				auto& out = in.sunEntryProcesses.emplace_back();
				for (std::uint32_t p = 0; p < 6; ++p)
					for (std::uint32_t i = 0; i < 4; ++i)
						out.planes[p][i] = mine.plane[p][i];
				out.mask = mine.mask;
			}
			Diagnostics::Notify(Diagnostics::SunEntryPlanesTaken{ in.frameNumber, true });
		} else {
			Diagnostics::Notify(Diagnostics::SunEntryPlanesTaken{ in.frameNumber, false });
		}
		in.sunCandidates = Scene::FrameView::Get().GetSunCandidates();
		in.lightCandidates = Scene::FrameView::Get().GetLightCandidates();
		// The versioned buffers' addresses and SRV indices are the sealed set's (BuildSnapshotDraws: AddressesOf).
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.placementsIndex = FrameValues::Get().PlacementsIndex();
		in.addresses.palettesIndex = FrameValues::Get().PalettesIndex();
		in.addresses.shadingIndex = FrameValues::Get().ShadingIndex();
		in.addresses.identity = &a_resources;
		in.tablesGeneration = Scene::FrameView::Get().GetTablesGeneration();
		in.lookupGeneration = Scene::FrameView::Get().GetLookups().shadowGeneration;
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

	std::shared_ptr<ShadowPayload> IndirectDraws::Impl::AcquireShadowPayload()
	{
		// Idle payloads are reset so they hold nothing, as AcquirePayload does.
		std::shared_ptr<ShadowPayload> free;
		for (auto& payload : shadowPayloadPool)
			if (payload.use_count() == 1) {
				payload->Reset();
				if (!free)
					free = payload;
			}
		return free ? free : shadowPayloadPool.emplace_back(std::make_shared<ShadowPayload>());
	}

	void IndirectDraws::Impl::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
	{
		auto* context = globals::d3d::context;
		if (shadowCullReadback) {
			// Frames, not epochs: the GPU has finished the sampled epoch three Presents later.
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
				a_stats.cullRootFading = words[5];
				a_stats.cullClass = words[16];
				a_stats.cullSampledView = shadowCullReadback->view;
				a_stats.cullSampledMode = shadowCullReadback->mode;
				context->Unmap(shadowCullReadback->count.get(), 0);
			}
			shadowCullReadback.reset();
			return;
		}
		// One epoch in 127, cycling through the views.
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
		if (const auto set = Scene::FrameView::Get().FrameClaims(); set && (set->drawn & (kSetCaster | kSetCasterPoint)) && PassCapture::ShadowWithholdingEnabled())
			++a_stats.notReadyWithheld;
	}

	void IndirectDraws::Impl::BuildShadowCatalog()
	{
		// The engine's table is filled once the renderer is up; until then the catalog waits.
		auto& table = EngineRasterStates();
		std::vector<D3D11_RASTERIZER_DESC> engine, cascades;
		for (std::uint32_t bias = 0; bias < 12; ++bias) {
			// The entry at cull mode 1, else 0 (as CaptureShadowView). Fill and depth clip DCLF cannot express are skipped: such a
			// view stays the engine's.
			auto* entry = table[0][1][bias][0] ? table[0][1][bias][0] : table[0][0][bias][0];
			if (!entry)
				continue;
			D3D11_RASTERIZER_DESC desc{};
			entry->GetDesc(&desc);
			if (desc.FillMode != D3D11_FILL_SOLID || !desc.DepthClipEnable)
				continue;
			engine.push_back(desc);
			// The cascades draw with ShadowmapCascadeRasterizerFix's clones, each with the cascade's bias.
			for (std::uint32_t cascade = 0; cascade < ShadowmapRasterizerFix::CascadeCount(); ++cascade) {
				auto clone = desc;
				ShadowmapRasterizerFix::CascadeRasterDesc(clone, cascade);
				cascades.push_back(clone);
			}
		}
		if (engine.empty())
			return;
		auto& pipelines = DrawPipelines::Get();
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			const std::uint32_t renderMode = IsOcclusionMode(m) ? kOcclusionRenderMode : PassCapture::kFirstShadowMode + m;
			auto& states = readyStates[m];
			const auto add = [&](const D3D11_RASTERIZER_DESC& a_desc) {
				if (const std::uint32_t id = pipelines.ShadowRasterStateId(a_desc, renderMode)) {
					states.Add(id, false);
					// Only the sun's views have the volumetric class.
					if (m == kSunShadowMode)
						states.Add(id, true);
				}
			};
			for (const auto& desc : engine)
				add(desc);
			if (m == kSunShadowMode)
				for (const auto& desc : cascades)
					add(desc);
		}
		shadowCatalogBuilt = true;
		logger::info("[DCLF] shadow rasterizer state catalog: {} states ({} engine entries, {} cascade clones); sun {}, paraboloid {}, occlusion {}",
			pipelines.ShadowRasterStateCount(), engine.size(), cascades.size(), readyStates[kSunShadowMode].All().size(), readyStates[kParabolicShadowMode].All().size(),
			readyStates[kFirstOcclusionMode].All().size());
	}

	bool IndirectDraws::Impl::BuildShadowPlacements()
	{
		auto* renderer = globals::game::renderer;
		auto* plain = renderer ? EngineRasterStates()[0][1][0][0] : nullptr;
		if (!plain)
			return false;
		// Cascade c draws into slice c with its ShadowmapCascadeRasterizerFix clone; every other view with depth bias mode 0.
		const std::uint32_t plainState = EngineRasterStateId(0, 1, 0, 0, PassCapture::kFirstShadowMode + kSunShadowMode);
		(void)EngineRasterStateId(0, 1, 0, 0, PassCapture::kFirstShadowMode + kParabolicShadowMode);
		const auto cascadeState = [&](std::uint32_t a_cascade) {
			if (a_cascade >= ShadowmapRasterizerFix::CascadeCount())
				return plainState;
			D3D11_RASTERIZER_DESC desc{};
			plain->GetDesc(&desc);
			ShadowmapRasterizerFix::CascadeRasterDesc(desc, a_cascade);
			return DrawPipelines::Get().ShadowRasterStateId(desc, PassCapture::kFirstShadowMode + kSunShadowMode);
		};
		std::vector<ShadowViewLayout> placements;
		const auto add = [&](std::uint32_t a_mode, std::uint32_t a_target, std::uint32_t a_slice, std::uint32_t a_y, std::uint32_t a_width, std::uint32_t a_height,
							 std::uint32_t a_state) {
			placements.push_back({ kFirstShadowViewSlot + static_cast<std::uint32_t>(placements.size()), a_mode, a_target, a_slice, 0u, a_y, a_width, a_height, a_state });
		};
		using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
		for (const auto [index, target] : { std::pair{ 0u, T::kSHADOWMAPS_ESRAM }, std::pair{ 1u, T::kSHADOWMAPS }, std::pair{ 2u, T::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM } }) {
			auto* texture = renderer->GetDepthStencilData().depthStencils[target].texture;
			if (!texture)
				continue;
			// Imported now, before any view draws into it.
			(void)ImportShadowDepth(index, target);
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			for (std::uint32_t slice = 0; slice < desc.ArraySize; ++slice) {
				if (index == 0) {
					add(kSunShadowMode, index, slice, 0, desc.Width, desc.Height, cascadeState(slice));
				} else if (index == 2) {
					// The cascades' volumetric copies.
					add(kSunShadowMode, index, slice, 0, desc.Width, desc.Height, plainState);
				} else {
					// Lights' map: a spot light takes a slice, a point light's paraboloid pair its two halves.
					add(kSunShadowMode, index, slice, 0, desc.Width, desc.Height, plainState);
					add(kParabolicShadowMode, index, slice, 0, desc.Width, desc.Height / 2, plainState);
					add(kParabolicShadowMode, index, slice, desc.Height / 2, desc.Width, desc.Height / 2, plainState);
				}
			}
		}
		if (placements.empty() || !plainState)
			return false;
		shadowPlacements = std::move(placements);
		// A revision input: the next revision reserves the latch and slots (ReserveShadowLatch).
		PostRevisionInputs();
		logger::info("[DCLF] shadow view placements: {} (slots {} to {})", shadowPlacements.size(), shadowPlacements.front().slot, shadowPlacements.back().slot);
		return true;
	}

	std::size_t IndirectDraws::Impl::PlacementOf(const PendingView& a_view) const
	{
		for (std::size_t p = 0; p < shadowPlacements.size(); ++p) {
			const auto& placement = shadowPlacements[p];
			if (placement.target == a_view.targetIndex && placement.slice == a_view.slice && placement.x == a_view.x && placement.y == a_view.y &&
				placement.width == a_view.width && placement.height == a_view.height)
				return p;
		}
		return SIZE_MAX;
	}

	void IndirectDraws::Impl::NoteCapability(bool a_defect, std::string a_cause)
	{
		++capabilityStats.changes;
		if (a_defect) {
			++capabilityStats.defects;
			logger::warn("[DCLF] shadow capability changed: {} <- PHASES", a_cause);
		} else {
			logger::info("[DCLF] shadow capability: {}", a_cause);
		}
		capabilityStats.lastCause = std::move(a_cause);
	}

	void IndirectDraws::UpdateShadowCapability()
	{
		auto& i = *impl;
		std::uint8_t phases = 0;
		if (!failed && ActiveToggles().shadows && DrawPipelines::Get().Enabled() && globals::game::utilityShader && i.SetupShadow()) {
			if (!i.shadowCatalogBuilt) {
				i.BuildShadowCatalog();
				if (i.shadowCatalogBuilt) {
					// The pipelines' format is the depth-stencil view's, not the typeless texture's.
					if (auto* renderer = globals::game::renderer)
						if (auto* view = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].views[0]) {
							D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
							view->GetDesc(&desc);
							i.shadowTargetFormat = desc.Format;
						}
					// Caster modes: sun and spot (clamped) and point (paraboloid). No view uses the plain mode.
					i.readyModes[kSunShadowMode] = i.readyModes[kParabolicShadowMode] = true;
					// Both occlusion maps draw into the precipitation occlusion target (Skylighting swaps its own map in), in its format.
					if (auto* renderer = globals::game::renderer)
						if (auto* view = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP].views[0]) {
							D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
							view->GetDesc(&desc);
							for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
								i.occlusion[v].dsvFormat = desc.Format;
								i.readyModes[OcclusionModeOf(v)] = true;
							}
							// Imported now: a later import would rebuild the graph mid-play.
							using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
							(void)i.ImportShadowDepth(OcclusionDepthTarget(kOcclusionPrecipitation), T::kPRECIPITATION_OCCLUSION_MAP);
							if (auto* sky = globals::features::skylighting.texOcclusion)
								(void)i.ImportDepthTexture(OcclusionDepthTarget(kOcclusionSky), sky->resource.get(), sky->dsv.get(), T::kPRECIPITATION_OCCLUSION_MAP);
						}
					i.NoteCapability(false, "the casters' and occlusion maps' modes and the catalog set up");
				}
			}
			if (i.shadowCatalogBuilt && i.shadowPlacements.empty() && i.BuildShadowPlacements())
				i.NoteCapability(false, fmt::format("{} shadow view placements set up", i.shadowPlacements.size()));
			if (i.shadowCatalogBuilt && !i.shadowPlacements.empty() && i.shadowTargetFormat != DXGI_FORMAT_UNKNOWN) {
				for (std::uint32_t m = 0; m < kFirstOcclusionMode; ++m)
					phases |= i.readyModes[m] ? SetPhaseOfMode(m) : 0;
				for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
					phases |= i.readyModes[OcclusionModeOf(v)] && i.occlusion[v].dsvFormat != DXGI_FORMAT_UNKNOWN && OcclusionEnabled(v) ?
					              SetPhaseOfMode(OcclusionModeOf(v)) :
					              0;
			}
		}
		// What the builds ahead and the scene lane's shadow lookups are resolved for (PostLookupInputs).
		auto& views = i.lastShadow;
		views.known = phases != 0;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			views.modes[m] = phases && i.readyModes[m] && (!IsOcclusionMode(m) || OcclusionEnabled(OcclusionOfMode(m)));
			views.rasterStates[m] = views.modes[m] ? i.readyStates[m] : ModeRasterStates{};
		}
		views.dsvFormat = i.shadowTargetFormat;
		i.shadowCapability.store(phases, std::memory_order_release);
	}

	std::uint8_t IndirectDraws::ShadowCapability() const
	{
		return failed ? 0 : impl->shadowCapability.load(std::memory_order_acquire);
	}

	bool IndirectDraws::PhaseReady(const void* a_tables, const Lookups& a_lookups, std::uint32_t a_slot, std::uint8_t a_phase, std::uint32_t* a_why) const
	{
		auto no = [a_why](std::uint32_t a_reason) {
			if (a_why)
				*a_why = a_reason;
			return false;
		};
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		const auto& lookups = a_lookups;
		if (a_phase == kSetReflection)
			return impl->ReflectionPhaseReady(tables, a_slot) || no(8);
		if (a_slot >= tables.objects.size() || a_slot >= tables.shadowTechnique.size())
			return no(7);
		const auto& object = tables.objects[a_slot];
		if (object.geometryIndex >= tables.geometries.size())
			return no(7);
		// Past the scene buffers' fit there is no input until they grow.
		if (!FitsScene(&tables, a_slot, true))
			return no(12);
		// The modes and states the lane's lookups were resolved for (immutable), never the render thread's readyModes/readyStates,
		// which grow as it sees new ones. None yet: the caster waits.
		const auto* inputs = static_cast<const ShadowLookupInputs*>(lookups.shadowInputs.get());
		if (!inputs)
			return no(7);
		// Fade is culled on the GPU (FadedOutOfOcclusion), so nothing of the engine's cull is waited for. As BuildKeptShadow: every
		// state of the object's class in the phase's modes has its pipeline, and an alpha-tested technique's diffuse is imported.
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (SetPhaseOfMode(m) != a_phase || !inputs->modes[m])
				continue;
			const std::uint32_t technique = ModeTechnique(tables, m, a_slot);
			if (!technique)
				continue;
			const auto& states = inputs->rasterStates[m].Of(VolumetricClass(m, object.flags));
			if (states.empty())
				continue;
			const ShadowPipelineKey key{ BaseTechnique(tables, m, a_slot), (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
				VertexLayoutOf(tables.geometries[object.geometryIndex].vertexDesc) };
			const auto slot = lookups.shadowSlots.find(key);
			if (slot == lookups.shadowSlots.end())
				return no(7);
			for (const std::uint32_t state : states)
				if (lookups.ShadowMapPipeline(m, state, slot->second) == Lookups::kNone)
					return no(7);
			if (technique & 0x80) {
				auto* diffuse = a_slot < tables.shadowDiffuse.size() ? tables.shadowDiffuse[a_slot] : nullptr;
				const auto texture = diffuse ? lookups.shadowTextures.find(diffuse) : lookups.shadowTextures.end();
				if (texture == lookups.shadowTextures.end() || texture->second == Lookups::kNone)
					return no(13);
			}
		}
		return true;
	}
}

#endif
