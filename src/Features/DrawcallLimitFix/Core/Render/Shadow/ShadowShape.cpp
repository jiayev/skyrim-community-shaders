// The shadow epochs' shapes: the scene's bounds, bucket sizing per view, MakeShadowShape and its frame views, the arena's
// fixed blocks and latched layout, and the map rows' buckets.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"

namespace DCLF
{
	namespace Draws
	{
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
		 * @brief Where a view draws: its slot, viewport in its target's slice, and push data (kShadowPushWords): the frame record,
		 * its view blocks and the arena's fixed blocks (ShadowArenaBlocksOf).
		 */
		ShadowFrameView FrameViewOf(const ShadowViewLayout& a_view, std::uint32_t a_capacity, const ShadowResources& a_resources, const ShadowCapacity& a_sizing,
			std::uint64_t a_viewBlocks)
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
		frame->slotLayouts = a_in.slotLayouts;
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
		const std::uint64_t viewBlocks = a_in.viewBlocks;
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
			auto& out = frame->views.emplace_back(FrameViewOf(view, capacity, a_resources, sizing, viewBlocks));
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

	const RowBuckets& ShadowEngine::ShadowRowBuckets(RowBucketsCache& a_cache, std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups,
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
}
