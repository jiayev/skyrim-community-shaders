// The CPU-driven pipeline's recorder (Recorder.h): a culled batch's direct draws, one plain indexed draw a resident sequence.

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Recorder.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <rhi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace DCLF::Direct
{
	namespace
	{
		// The words a direct draw pushes per draw: its sequence's address through kDirectDrawPushFlags (BeginList zeroes the rest once).
		constexpr std::uint32_t kDirectDrawWords = kDirectDrawPushFlags + 1;
		static_assert(kZDrawPushSequences == 0 && kShadowDrawPushSequences == 0 && kDirectDrawWords <= kDrawPushWords);

		void Split(std::uint32_t* a_words, std::uint64_t a_value)
		{
			a_words[0] = static_cast<std::uint32_t>(a_value);
			a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
		}

		/**
		 * @brief Binds the list's layout and the push data its draws keep: the frame (or view) words, then for every layout but the main
		 * one the draw range zeroed, the set's row bases and the frame's animated rows (each draw pushes its first kDirectDrawWords). a_main:
		 * the main layout, a pushed colour draw's (kExpPushedColour), whose draws push their whole range.
		 */
		void BindListLayout(RecordContext& a_context, bool a_main)
		{
			const auto& bindings = a_context.bindings;
			auto& list = a_context.list;
			const bool shadowLayout = a_context.pass == RecordPass::Shadow || a_context.pass == RecordPass::Refraction;
			list.BindLayout(a_main                                     ? bindings.main->layout :
			                a_context.pass == RecordPass::Shadow       ? bindings.shadow->layout :
			                a_context.pass == RecordPass::Refraction   ? bindings.main->refractionLayout :
			                                                             bindings.main->zLayout);
			if (shadowLayout)
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kShadowPushWords, bindings.shadowPush.data());
			else
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, bindings.framePush.data());
			a_context.mainLayoutBound = a_main;
			if (a_main)
				return;
			// The draw range whole, once: each draw then pushes its first kDirectDrawWords.
			const std::uint32_t zeros[kDrawPushWords]{};
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, zeros);
			// The set's row bases (its sequences' rows are relative), kept by every draw's push of its first words.
			std::uint32_t bases[4]{};
			const auto& set = a_context.set;
			if (a_context.pass == RecordPass::Shadow) {
				Split(bases + 2, set.shadowRowsBase);
			} else {
				Split(bases, set.pipelineRowsBase);
				Split(bases + 2, set.materialRowsBase);
			}
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, kDirectDrawPushRowBases, 4, bases);
			// The main rows' animated rows read from the frame's copy (the texture transforms' TexcoordOffset).
			if (a_context.pass != RecordPass::Shadow && bindings.animatedRows) {
				std::uint32_t animated[2]{};
				Split(animated, bindings.animatedRows);
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, kDirectDrawPushAnimatedRows, 2, animated);
			}
		}

		/** @brief The key's pipeline under the main layout a pushed list draws set index a_index with (null: its pulled one). */
		rhi::PipelineHandle PushedPipeline(const RecordContext& a_context, std::uint32_t a_index)
		{
			if (!a_context.pushedColour)
				return {};
			const auto& main = *a_context.bindings.main;
			const auto& pipelines = a_context.pass == RecordPass::Depth ? main.depth : main.colour;
			return a_index < pipelines.size() ? pipelines[a_index] : rhi::PipelineHandle{};
		}

		/** @brief The pulled pipeline bucket a_bucket binds in a_pass (ViewBuckets), and a shadow view's its vertex layout; null: none. */
		rhi::PipelineHandle PulledBucketPipeline(const ViewBatchSet& a_set, ViewType a_view, RecordPass a_pass, const RecordBindings& a_bindings, std::uint32_t a_bucket, std::uint64_t& a_vertexLayout)
		{
			a_vertexLayout = 0;
			const auto& bindings = a_bindings;
			if (a_pass == RecordPass::Refraction)
				return bindings.main->refraction;
			const auto& buckets = a_set.buckets[static_cast<std::size_t>(a_view)].pipeline;
			if (a_bucket >= buckets.size())
				return {};
			std::uint32_t index = buckets[a_bucket];
			switch (a_pass) {
			case RecordPass::Depth:
				{
					const auto& main = *bindings.main;
					return index < main.zGroups.size() && main.zGroups[index] < main.zPipelines.size() ? main.zPipelines[main.zGroups[index]] : rhi::PipelineHandle{};
				}
			case RecordPass::Colour:
				{
					const auto& main = *bindings.main;
					return index < main.plainColour.size() ? main.plainColour[index] : rhi::PipelineHandle{};
				}
			case RecordPass::DecalDepth:
				return index < bindings.main->plainDepth.size() ? bindings.main->plainDepth[index] : rhi::PipelineHandle{};
			case RecordPass::Shadow:
				{
					// The bucket is a key slot: its pipeline is the view's map row's (kNone, past the set: none yet).
					const auto& shadow = *bindings.shadow;
					if (index >= bindings.shadowMapRow.size())
						return {};
					index = bindings.shadowMapRow[index];
					if (index >= shadow.pipelines.size())
						return {};
					a_vertexLayout = index < shadow.vertexLayouts.size() ? shadow.vertexLayouts[index] : 0;
					return shadow.pipelines[index];
				}
			default:
				return {};
			}
		}

		/** @brief The pipeline bucket a_bucket binds in the context's pass: a pushed list's own, else its pulled one. */
		rhi::PipelineHandle BucketPipeline(const RecordContext& a_context, std::uint32_t a_bucket, std::uint64_t& a_vertexLayout)
		{
			if (a_context.pushedColour && (a_context.pass == RecordPass::Depth || a_context.pass == RecordPass::Colour)) {
				const auto& buckets = a_context.set.buckets[static_cast<std::size_t>(a_context.view)].pipeline;
				if (a_bucket < buckets.size())
					if (const auto pushed = PushedPipeline(a_context, buckets[a_bucket]); pushed.valid()) {
						a_vertexLayout = 0;
						return pushed;
					}
			}
			return PulledBucketPipeline(a_context.set, a_context.view, a_context.pass, a_context.bindings, a_bucket, a_vertexLayout);
		}

		/**
		 * @brief Binds a_bucket's pipeline unless it is bound (a shadow bucket's vertex layout with it); false: the bucket has none (its
		 * draws are skipped, as a GPU bucket without one).
		 */
		bool BindBucket(RecordContext& a_context, std::uint32_t a_bucket)
		{
			if (a_context.pass == RecordPass::Refraction || a_bucket == a_context.boundBucket)
				return a_context.boundPipeline.valid();  // the refraction's: BeginList's
			a_context.boundBucket = a_bucket;
			const rhi::PipelineHandle previous = a_context.boundPipeline;
			a_context.boundPipeline = BucketPipeline(a_context, a_bucket, a_context.boundVertexLayout);
			if (a_context.boundPipeline.valid()) {
				if (a_context.pushedColour) {
					const auto& buckets = a_context.set.buckets[static_cast<std::size_t>(a_context.view)].pipeline;
					const bool main = a_bucket < buckets.size() && PushedPipeline(a_context, buckets[a_bucket]).valid();
					if (main != a_context.mainLayoutBound)
						BindListLayout(a_context, main);
				}
				a_context.list.BindPipeline(a_context.boundPipeline);
				++a_context.binds;
				if (previous.index == a_context.boundPipeline.index && previous.generation == a_context.boundPipeline.generation)
					++a_context.sameBinds;
			}
			return a_context.boundPipeline.valid();
		}

		/**
		 * @brief One resident sequence's draw: its address, the layout's second address and vertex layout word as the GPU path's plain
		 * draws push them per call (RecordBucketDraw, ShadowViewPass, RefractionDrawPass), the draw's object word with
		 * kDirectFlagObjectWord, then its arguments with the index pool's first of its geometry slot.
		 */
		bool DrawSequenceDirect(RecordContext& a_context, std::uint32_t a_sequence, std::uint32_t a_objectWord)
		{
			const auto& set = a_context.set;
			const auto& bindings = a_context.bindings;
			const DrawArgs* found = set.Args(a_sequence);
			if (!found)
				return false;
			const DrawArgs& args = *found;
			if (!args.indexCount || args.geometrySlot >= bindings.poolFirsts.size())
				return false;
			const std::uint32_t poolFirst = bindings.poolFirsts[args.geometrySlot];
			if (poolFirst == Draws::IndexPool::kNoRange)
				return false;
			auto& list = a_context.list;
			if (a_context.mainLayoutBound) {
				// A device-generated draw's Constant argument (its sequence's bytes 4-99, BuildDrawsCS's StoreSequence): the rows placed at
				// the set's bases (an animated material row at its row of the frame's copy), their blocks from them, the draw's object word.
				const DrawSequence* source = set.Sequence(a_sequence);
				if (!source)
					return false;
				DrawSequence s = *source;
				s.pipelineRowAddress += set.pipelineRowsBase;
				const std::uint64_t row = s.materialRowAddress / kMaterialRowBytes;
				const std::uint32_t animated = bindings.animatedRows && row < set.animatedRowOf.size() ? set.animatedRowOf[row] : ~0u;
				s.materialRowAddress = animated != ~0u ? bindings.animatedRows + std::uint64_t(animated) * kMaterialRowBytes : s.materialRowAddress + set.materialRowsBase;
				s.objectIndex = a_objectWord;
				SetSequenceBlocks(s);
				std::uint32_t pushed[kDrawPushArgumentWords];
				std::memcpy(pushed, reinterpret_cast<const std::byte*>(&s) + 4, sizeof(pushed));
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushArgumentWords, pushed);
				++a_context.pushes;
				a_context.indices += args.indexCount;
				list.DrawIndexed(args.indexCount, 1, poolFirst + args.firstIndex, 0, 0);
				return true;
			}
			a_context.runWordsPushed = false;
			std::uint32_t words[kDirectDrawWords]{};
			Split(words + kZDrawPushSequences, set.sequencesAddress + std::uint64_t(a_sequence) * sizeof(DrawSequence));
			switch (a_context.pass) {
			case RecordPass::Shadow:
				// The layout's rows word 0, as ShadowViewPass's (each draw's row is its sequence's); its bucket's vertex layout.
				Split(words + kShadowDrawPushVertexLayout, a_context.boundVertexLayout);
				break;
			case RecordPass::Refraction:
				Split(words + kShadowDrawPushMaterialRows, bindings.lodFadeFrame);
				break;
			default:
				words[kZDrawPushOffsetDepth] = bindings.offsetDepth ? 1u : 0u;
				break;
			}
			words[kDirectDrawPushObjectWord] = a_objectWord;
			words[kDirectDrawPushFlags] = kDirectFlagObjectWord | kDirectFlagRelativeRows;
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDirectDrawWords, words);
			++a_context.pushes;
			a_context.indices += args.indexCount;
			list.DrawIndexed(args.indexCount, 1, poolFirst + args.firstIndex, 0, 0);
			return true;
		}

		// The cross-fade copy's object word bit (Records.h, kObjectCrossfadeCopy; DCLFAlphaFade's a_copy).
		constexpr std::uint32_t kCrossfadeCopyBit = 1u << 30;
		// CulledBatch::drawMasks: every sequence, none as the copy.
		constexpr std::uint32_t kDrawAll = 0xFFFFu;

		/**
		 * @brief Whether a lane's sequence a_sequence (its index among the lane's) is drawn under a_mask (CulledBatch::drawMasks), and its
		 * object word. A lane's sequences past the mask's 16 are drawn when it draws all of them, never as the copy.
		 */
		bool SequenceDrawn(std::uint32_t a_mask, std::uint32_t a_sequence, std::uint32_t a_objectWord, std::uint32_t& a_word)
		{
			a_word = a_objectWord;
			if (a_sequence >= 16)
				return (a_mask & kDrawAll) == kDrawAll;
			if (!((a_mask >> a_sequence) & 1u))
				return false;
			if ((a_mask >> (16 + a_sequence)) & 1u)
				a_word |= kCrossfadeCopyBit;
			return true;
		}

		/** @brief A kept lane's draws: its consecutive sequences (its partitions) a_mask draws, with its bucket's pipeline and its object word. */
		void DrawLane(RecordContext& a_context, const ViewBatch& a_batch, std::uint32_t a_lane, std::uint32_t a_objectWord, std::uint32_t a_mask)
		{
			// A cull-only lane (kLaneCullOnly: no sequences, its bucket unread) draws nothing.
			const std::uint32_t count = a_batch.sequenceCount ? a_batch.sequenceCount[a_lane] : 1u;
			if (!(a_mask & kDrawAll) || !count)
				return;
			const std::uint32_t bucket = a_batch.pipelineBucket ? a_batch.pipelineBucket[a_lane] : 0u;
			if (!BindBucket(a_context, bucket))
				return;
			const std::uint32_t first = a_batch.sequence[a_lane];
			for (std::uint32_t s = 0; s < count; ++s) {
				std::uint32_t word = 0;
				if (SequenceDrawn(a_mask, s, a_objectWord, word))
					a_context.drawn += DrawSequenceDirect(a_context, first + s, word) ? 1u : 0u;
			}
		}

		std::uint32_t MaskOf(const CulledBatch& a_batch, std::uint32_t a_kept) { return a_batch.drawMasks ? a_batch.drawMasks[a_kept] : kDrawAll; }

		constexpr std::uint32_t kTerrainLanes = kLaneTerrainGroup1 | kLaneTerrainGroup2;
		std::uint32_t FlagsOf(const ViewBatch& a_batch, std::uint32_t a_lane) { return a_batch.flags ? a_batch.flags[a_lane] : 0u; }

		void CountDrawn(RecordContext& a_context, std::uint32_t a_drawn)
		{
			if (!a_context.counters)
				return;
			auto& counters = *a_context.counters;
			if (a_drawn)
				std::atomic_ref<std::uint32_t>(counters.drawn[static_cast<std::size_t>(a_context.view)]).fetch_add(a_drawn, std::memory_order_relaxed);
			const auto flush = [](std::uint32_t& a_counter, std::uint32_t a_value, std::uint32_t& a_counted) {
				if (a_value != a_counted)
					std::atomic_ref<std::uint32_t>(a_counter).fetch_add(a_value - std::exchange(a_counted, a_value), std::memory_order_relaxed);
			};
			flush(counters.binds, a_context.binds, a_context.countedBinds);
			flush(counters.sameBinds, a_context.sameBinds, a_context.countedSameBinds);
			flush(counters.pushes, a_context.pushes, a_context.countedPushes);
			flush(counters.indirects, a_context.indirects, a_context.countedIndirects);
			if (a_context.indices != a_context.countedIndices) {
				const std::uint64_t added = a_context.indices - std::exchange(a_context.countedIndices, a_context.indices);
				std::atomic_ref<std::uint64_t>(counters.indices[static_cast<std::size_t>(a_context.site)]).fetch_add(added, std::memory_order_relaxed);
				std::atomic_ref<std::uint64_t>(counters.viewIndices[static_cast<std::size_t>(a_context.view)]).fetch_add(added, std::memory_order_relaxed);
			}
		}

		/** @brief A forward draw's sort key (ForwardSortCS.hlsl, SortKey) and what records it. */
		struct ForwardDraw
		{
			float depth = 0.0f;
			std::uint32_t group = 0;
			std::uint32_t ordinal = 0;  // 0: a plain draw
			std::uint32_t object = 0;
			std::uint32_t first = 0;
			std::uint32_t sequence = 0;
			std::uint32_t bucket = 0;
			std::uint32_t objectWord = 0;
		};

		// ForwardSortCS.hlsl's Before: whether a draws before b in the blended order.
		bool Before(const ForwardDraw& a, const ForwardDraw& b)
		{
			if (a.depth != b.depth)
				return a.depth > b.depth;
			if ((a.ordinal == 0) != (b.ordinal == 0))
				return a.ordinal == 0;
			if (a.group != b.group)
				return a.group < b.group;
			if (a.ordinal != b.ordinal)
				return a.ordinal < b.ordinal;
			if (a.object != b.object)
				return a.object < b.object;
			return a.first < b.first;
		}

		/** @brief A batch's indirect draws of a part (CulledBatch::runs), one a run, each binding its pipeline unless it is bound. */
		void RecordRuns(RecordContext& a_context, const CulledBatch& a_batch, std::uint32_t a_part)
		{
			auto& list = a_context.list;
			const IndirectBuffer& buffer = *a_context.indirect;
			const auto sameHandle = [](rhi::PipelineHandle a, rhi::PipelineHandle b) { return a.index == b.index && a.generation == b.generation; };
			std::uint32_t drawn = 0;
			for (std::uint32_t r = a_batch.runStart[a_part]; r < a_batch.runStart[a_part + 1]; ++r) {
				const IndirectRun& run = a_batch.runs[r];
				const bool bind = !sameHandle(run.pipeline, a_context.boundPipeline);
				if (bind) {
					list.BindPipeline(run.pipeline);
					++a_context.binds;
				}
				// A direct draw's words, a shadow pipeline's layout, or the list's first run: the run's words.
				if (bind || !a_context.runWordsPushed) {
					std::uint32_t words[kDirectDrawWords]{};
					Split(words + kZDrawPushSequences, a_context.set.sequencesAddress);
					if (a_context.pass == RecordPass::Shadow)
						Split(words + kShadowDrawPushVertexLayout, run.vertexLayout);
					else
						words[kZDrawPushOffsetDepth] = a_context.bindings.offsetDepth ? 1u : 0u;
					words[kDirectDrawPushFlags] = kDirectFlagDrawRecords | kDirectFlagRelativeRows;
					list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDirectDrawWords, words);
					++a_context.pushes;
					a_context.runWordsPushed = true;
				}
				// BindBucket's state: a later direct draw binds its bucket again.
				a_context.boundBucket = ~0u;
				a_context.boundPipeline = run.pipeline;
				a_context.boundVertexLayout = run.vertexLayout;
				list.ExecuteIndirect(buffer.signature, buffer.buffer, std::uint64_t(run.first) * sizeof(IndirectDraw), {}, 0, run.count);
				++a_context.indirects;
				drawn += run.count;
			}
			a_context.drawn += drawn;
			CountDrawn(a_context, drawn);
		}

		// The recording thread's merge buffer: reused, grown to the largest forward list it has sorted (never shared, never capped).
		thread_local std::vector<ForwardDraw> forwardDraws;
	}

	RecordPass PassOf(ViewType a_view, bool a_depthPass)
	{
		switch (a_view) {
		case ViewType::Main:
		case ViewType::Reflection:
		case ViewType::FirstPerson:
			return a_depthPass ? RecordPass::Depth : RecordPass::Colour;
		case ViewType::Decal:
			return a_depthPass ? RecordPass::DecalDepth : RecordPass::Colour;
		case ViewType::Forward:
			return RecordPass::Colour;
		case ViewType::Refraction:
			return RecordPass::Refraction;
		default:
			return RecordPass::Shadow;  // Sun, Spot, Point, SkyOccluder, PrecipOccluder
		}
	}

	bool BeginList(RecordContext& a_context)
	{
		a_context.pass = PassOf(a_context.view, a_context.depthPass);
		a_context.boundBucket = ~0u;
		a_context.boundPipeline = {};
		a_context.boundVertexLayout = 0;
		const auto& bindings = a_context.bindings;
		if (a_context.pass == RecordPass::Shadow ? !bindings.shadow || !bindings.shadow->valid : !bindings.main || !bindings.main->valid)
			return false;
		const rhi::PipelineLayoutHandle layout = a_context.pass == RecordPass::Shadow ? bindings.shadow->layout :
		                                         a_context.pass == RecordPass::Refraction ? bindings.main->refractionLayout :
		                                                                                    bindings.main->zLayout;
		if (!layout.valid() || (a_context.pass == RecordPass::Refraction && !bindings.main->refraction.valid()))
			return false;
		auto& list = a_context.list;
		list.SetDescriptorHeaps(bindings.resourceHeap, bindings.samplerHeap);
		list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
		// A colour list binds the main layout at its first bucket with a colour pipeline (BindBucket). The pushed pipelines are the main
		// set's, built for the main pass's formats: the reflection's faces draw into the cube's (R16G16B16A16 against the main target's
		// R11G11B10), so they keep their own view pipelines (VUID-vkCmdDrawIndexed-dynamicRenderingUnusedAttachments-08911).
		a_context.pushedColour = !a_context.indirect && bindings.main && bindings.main->layout.valid() &&
		                         ((a_context.pass == RecordPass::Colour && a_context.view != ViewType::Reflection && ExperimentOn(kExpPushedColour) &&
		                              !bindings.main->colour.empty()) ||
		                          (a_context.pass == RecordPass::Depth && ExperimentOn(kExpPushedDepth) && !bindings.offsetDepth && !bindings.main->depth.empty()));
		BindListLayout(a_context, false);
		a_context.runWordsPushed = false;
		if (a_context.indirect) {
			std::uint32_t records[2]{};
			Split(records, a_context.indirect->address);
			list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, kDirectDrawPushDrawRecords, 2, records);
		}
		list.SetIndexBuffer(rhi::IndexBufferView{ bindings.indexPool, 0, 0, rhi::Format::R16_UInt });  // the whole buffer
		if (a_context.pass == RecordPass::Refraction) {
			a_context.boundBucket = 0;
			a_context.boundPipeline = bindings.main->refraction;
			list.BindPipeline(a_context.boundPipeline);
		}
		return true;
	}

	std::uint64_t WriteIndirectDraws(CulledBatch& a_batch, const ViewBatchSet& a_set, ViewType a_view, bool a_depthPass, const RecordBindings& a_bindings, const IndirectBuffer& a_buffer, CullArena& a_arena)
	{
		a_batch.hasRuns = false;
		std::uint64_t indices = 0;
		if (!a_batch.batch || !a_batch.count)
			return indices;
		const ViewBatch& batch = *a_batch.batch;
		const RecordPass pass = PassOf(a_view, a_depthPass);
		// Main's colour pass: the terrain groups apart (RecordTerrainGroup draws them in the second pass).
		const bool parts = a_view == ViewType::Main && pass == RecordPass::Colour;
		const auto partOf = [&](std::uint32_t a_lane) -> std::uint32_t {
			if (!parts)
				return 0u;
			const std::uint32_t flags = FlagsOf(batch, a_lane);
			return (flags & kLaneTerrainGroup1) ? 1u : (flags & kLaneTerrainGroup2) ? 2u : 0u;
		};
		// The most it can write, by part, and the most runs: each part's draws are consecutive in the buffer.
		std::array<std::uint32_t, 3> mostDraws{}, mostRuns{}, lastBucket{ ~0u, ~0u, ~0u };
		for (std::uint32_t k = 0; k < a_batch.count; ++k) {
			const std::uint32_t lane = a_batch.lanes[k];
			const std::uint32_t mask = MaskOf(a_batch, k);
			const std::uint32_t count = batch.sequenceCount ? batch.sequenceCount[lane] : 1u;
			if (!(mask & kDrawAll) || !count)
				continue;
			const std::uint32_t part = partOf(lane);
			mostDraws[part] += (mask & kDrawAll) == kDrawAll ? count : static_cast<std::uint32_t>(std::popcount(mask & kDrawAll));
			const std::uint32_t bucket = batch.pipelineBucket ? batch.pipelineBucket[lane] : 0u;
			if (bucket != lastBucket[part]) {
				lastBucket[part] = bucket;
				++mostRuns[part];
			}
		}
		const std::uint32_t reserve = mostDraws[0] + mostDraws[1] + mostDraws[2];
		if (!reserve)
			return indices;
		const std::uint32_t first = a_buffer.cursor->fetch_add(reserve, std::memory_order_relaxed);
		if (first > a_buffer.capacity || reserve > a_buffer.capacity - first)
			return indices;
		auto* runs = a_arena.Make<IndirectRun>(mostRuns[0] + mostRuns[1] + mostRuns[2]);
		std::array<std::uint32_t, 3> cursor{ first, first + mostDraws[0], first + mostDraws[0] + mostDraws[1] };
		const std::array<std::uint32_t, 3> runBase{ 0u, mostRuns[0], mostRuns[0] + mostRuns[1] };
		std::array<std::uint32_t, 3> runEnd = runBase;
		lastBucket = { ~0u, ~0u, ~0u };
		std::array<rhi::PipelineHandle, 3> pipeline{};
		std::array<std::uint64_t, 3> layout{};
		const auto sameHandle = [](rhi::PipelineHandle a, rhi::PipelineHandle b) { return a.index == b.index && a.generation == b.generation; };
		IndirectDraw* const mapped = a_buffer.mapped;
		const auto poolFirsts = a_bindings.poolFirsts;
		for (std::uint32_t k = 0; k < a_batch.count; ++k) {
			const std::uint32_t lane = a_batch.lanes[k];
			const std::uint32_t mask = MaskOf(a_batch, k);
			const std::uint32_t count = batch.sequenceCount ? batch.sequenceCount[lane] : 1u;
			if (!(mask & kDrawAll) || !count)
				continue;
			const std::uint32_t part = partOf(lane);
			const std::uint32_t bucket = batch.pipelineBucket ? batch.pipelineBucket[lane] : 0u;
			if (bucket != lastBucket[part]) {
				lastBucket[part] = bucket;
				pipeline[part] = PulledBucketPipeline(a_set, a_view, pass, a_bindings, bucket, layout[part]);
			}
			if (!pipeline[part].valid())
				continue;
			const std::uint32_t objectWord = a_batch.objectWords[k];
			const std::uint32_t sequence = batch.sequence[lane];
			for (std::uint32_t s = 0; s < count; ++s) {
				std::uint32_t word = 0;
				if (!SequenceDrawn(mask, s, objectWord, word))
					continue;
				const DrawArgs* args = a_set.Args(sequence + s);
				if (!args || !args->indexCount || args->geometrySlot >= poolFirsts.size())
					continue;
				const std::uint32_t poolFirst = poolFirsts[args->geometrySlot];
				if (poolFirst == Draws::IndexPool::kNoRange)
					continue;
				const std::uint32_t at = cursor[part]++;
				// A run goes on while its pipeline does: buckets sharing one (the Z groups') make one run.
				IndirectRun* open = runEnd[part] > runBase[part] ? &runs[runEnd[part] - 1] : nullptr;
				if (!open || !sameHandle(open->pipeline, pipeline[part]) || open->vertexLayout != layout[part])
					open = new (&runs[runEnd[part]++]) IndirectRun{ pipeline[part], layout[part], at, 0u };
				++open->count;
				// Write-combined memory: written in order, never read.
				IndirectDraw& draw = mapped[at];
				draw.indexCount = args->indexCount;
				indices += args->indexCount;
				draw.instanceCount = 1;
				draw.firstIndex = poolFirst + args->firstIndex;
				draw.vertexOffset = 0;
				draw.firstInstance = at;
				draw.sequence = sequence + s;
				draw.objectWord = word;
			}
		}
		// The parts' runs, closed up.
		std::uint32_t written = runEnd[0];
		a_batch.runStart[0] = 0;
		a_batch.runStart[1] = written;
		for (std::uint32_t part = 1; part < 3; ++part) {
			for (std::uint32_t r = runBase[part]; r < runEnd[part]; ++r)
				runs[written++] = runs[r];
			a_batch.runStart[part + 1] = written;
		}
		a_batch.runs = runs;
		a_batch.hasRuns = true;
		return indices;
	}

	void RecordBatch(RecordContext& a_context, const CulledBatch& a_batch)
	{
		if (!a_batch.batch || !a_batch.count)
			return;
		if (a_context.indirect && a_batch.hasRuns) {
			RecordRuns(a_context, a_batch, 0);
			return;
		}
		const std::uint32_t before = a_context.drawn;
		const ViewBatch& batch = *a_batch.batch;
		// Main's colour pass: the terrain groups draw in the second pass (RecordTerrainGroup).
		const bool skipTerrain = a_context.view == ViewType::Main && a_context.pass == RecordPass::Colour;
		for (std::uint32_t k = 0; k < a_batch.count; ++k) {
			const std::uint32_t lane = a_batch.lanes[k];
			if (!skipTerrain || !(FlagsOf(batch, lane) & kTerrainLanes))
				DrawLane(a_context, batch, lane, a_batch.objectWords[k], MaskOf(a_batch, k));
		}
		CountDrawn(a_context, a_context.drawn - before);
	}

	void RecordTerrainGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches)
	{
		const std::uint32_t flag = a_group == 1 ? kLaneTerrainGroup1 : a_group == 2 ? kLaneTerrainGroup2 : 0u;
		if (!flag)
			return;
		const std::uint32_t before = a_context.drawn;
		for (const CulledBatch* culled : a_batches) {
			if (!culled || !culled->batch)
				continue;
			if (a_context.indirect && culled->hasRuns) {
				RecordRuns(a_context, *culled, a_group);
				continue;
			}
			const ViewBatch& batch = *culled->batch;
			for (std::uint32_t k = 0; k < culled->count; ++k) {
				const std::uint32_t lane = culled->lanes[k];
				if (FlagsOf(batch, lane) & flag)
					DrawLane(a_context, batch, lane, culled->objectWords[k], MaskOf(*culled, k));
			}
		}
		CountDrawn(a_context, a_context.drawn - before);
	}

	void RecordForward(RecordContext& a_context, std::span<const CulledBatch* const> a_batches)
	{
		const auto& bindings = a_context.bindings;
		const auto& set = a_context.set;
		const auto& m = bindings.viewProj;
		auto& draws = forwardDraws;
		draws.clear();
		for (const CulledBatch* culled : a_batches) {
			if (!culled || !culled->batch)
				continue;
			const ViewBatch& batch = *culled->batch;
			for (std::uint32_t k = 0; k < culled->count; ++k) {
				const std::uint32_t lane = culled->lanes[k];
				const std::uint32_t object = batch.object[lane];
				// StageForwardKey: a BSOrderedNode group's leaf at its node's centre (its ordinal in forwardSort.w), else at its bound's.
				ForwardDraw draw{};
				draw.object = object;
				draw.bucket = batch.pipelineBucket ? batch.pipelineBucket[lane] : 0u;
				const std::uint32_t laneWord = culled->objectWords[k];
				const std::uint32_t mask = MaskOf(*culled, k);
				if (object < bindings.placements.size()) {
					const BindlessPlacement& placement = bindings.placements[object];
					draw.ordinal = std::bit_cast<std::uint32_t>(placement.forwardSort[3]);
					const float* centre = draw.ordinal != 0 ? placement.forwardSort : placement.bound;
					const float clipW = m[12] * centre[0] + m[13] * centre[1] + m[14] * centre[2] + m[15];
					draw.depth = std::isnan(clipW) ? 0.0f : clipW;  // a NaN would break the order
					if (draw.ordinal != 0)
						draw.group = (std::bit_cast<std::uint32_t>(centre[0]) * 0x9E3779B1u) ^ (std::bit_cast<std::uint32_t>(centre[1]) * 0x85EBCA77u) ^
						             (std::bit_cast<std::uint32_t>(centre[2]) * 0xC2B2AE3Du);
				}
				// Each of its sequences (partitions) is a staged draw of its own, as the GPU path stages them.
				const std::uint32_t count = batch.sequenceCount ? batch.sequenceCount[lane] : 1u;
				for (std::uint32_t s = 0; s < count; ++s) {
					if (!SequenceDrawn(mask, s, laneWord, draw.objectWord))
						continue;
					draw.sequence = batch.sequence[lane] + s;
					const DrawArgs* args = set.Args(draw.sequence);
					draw.first = args ? args->firstIndex : 0u;
					const std::uint32_t slot = args ? args->geometrySlot : ~0u;
					if (slot < bindings.poolFirsts.size() && bindings.poolFirsts[slot] != Draws::IndexPool::kNoRange)
						draw.first += bindings.poolFirsts[slot];
					draws.push_back(draw);
				}
			}
		}
		std::sort(draws.begin(), draws.end(), Before);
		std::uint32_t drawn = 0;
		for (const ForwardDraw& draw : draws) {
			if (BindBucket(a_context, draw.bucket))
				drawn += DrawSequenceDirect(a_context, draw.sequence, draw.objectWord) ? 1u : 0u;
		}
		a_context.drawn += drawn;
		CountDrawn(a_context, drawn);
	}

	void RecordDecalGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches)
	{
		const auto& set = a_context.set;
		if (std::size_t(a_group) + 1 >= set.decalGroupFirst.size())
			return;
		const std::uint64_t from = set.decalGroupFirst[a_group], to = set.decalGroupFirst[a_group + 1];
		if (from >= to)
			return;
		const std::uint32_t before = a_context.drawn;
		// In list order: the batches are, and each one's kept lanes are in lane order.
		for (const CulledBatch* culled : a_batches) {
			if (!culled || !culled->batch)
				continue;
			const ViewBatch& batch = *culled->batch;
			const std::uint64_t base = std::uint64_t(batch.index) * set.lanes;
			if (base >= to || base + batch.count <= from)
				continue;
			for (std::uint32_t k = 0; k < culled->count; ++k) {
				const std::uint32_t lane = culled->lanes[k];
				if (base + lane >= from && base + lane < to)
					DrawLane(a_context, batch, lane, culled->objectWords[k], MaskOf(*culled, k));
			}
		}
		CountDrawn(a_context, a_context.drawn - before);
	}

	void RecordFamilyDraws(RecordContext& a_context)
	{
		const auto& bindings = a_context.bindings;
		const bool colour = a_context.pass == RecordPass::Colour;
		if ((a_context.pass != RecordPass::Depth && !colour) || !bindings.main || !bindings.main->valid)
			return;
		const auto& families = Extensions::DrawFamilies();
		auto& list = a_context.list;
		std::uint32_t drawn = 0;
		for (std::size_t f = 0; f < bindings.families.size() && f < Extensions::kDrawFamilies; ++f) {
			const FamilyDraw& draw = bindings.families[f];
			const auto& family = families[f];
			if (!draw.pipelines || !draw.arguments.valid() || !family.taken || !family.contribution.main.record)
				continue;
			// A family's colour tests EQUAL against its depth draw, under the Z-prepass layout and frame push data (MainViewPasses).
			if (colour) {
				list.BindLayout(bindings.main->zLayout);
				list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, bindings.framePush.data());
			}
			// One phase: no HZB in the CPU pipeline, so the depth draw is phase 1's (its single-phase cull writes those arguments).
			family.contribution.main.record({ list, colour ? Api::DrawFamilyPass::Colour : Api::DrawFamilyPass::Depth, draw.pipelines, draw.row, draw.arguments, {}, {} });
			++drawn;
		}
		// Each family's draw (its indirect executions) counted as one of the main view's: a site that drew only families drew something.
		a_context.drawn += drawn;
		CountDrawn(a_context, drawn);
		// The families bound their own pipelines and draw words: the next lane binds its bucket again (and pushes its own words), and
		// a pushed colour lane the main layout.
		a_context.boundBucket = ~0u;
		a_context.boundPipeline = {};
		a_context.mainLayoutBound = false;
	}

	bool RecordSecondary(RecordContext& a_context, const rhi::SecondaryInheritance& a_inheritance, const CulledBatch& a_batch, rhi::CommandList& a_secondary)
	{
		if (a_secondary.BeginSecondary(a_inheritance) != rhi::Result::Ok)
			return false;
		RecordContext context{ a_secondary, a_context.set, a_context.view, a_context.viewIndex, a_context.site, a_context.depthPass, a_context.bindings, a_context.counters };
		const bool begun = BeginList(context);
		if (begun)
			RecordBatch(context, a_batch);
		a_secondary.End();
		a_context.drawn += context.drawn;
		if (begun && a_context.counters)
			std::atomic_ref<std::uint32_t>(a_context.counters->secondaries).fetch_add(1, std::memory_order_relaxed);
		return begun;
	}
}
