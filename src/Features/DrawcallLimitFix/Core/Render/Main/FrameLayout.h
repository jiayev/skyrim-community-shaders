#pragma once

// A main segment's shape (PassFrame), the frame buffers, the sorts' constants and programs, the DGC preprocess state lists.
// Made by the commits and the shape producer, recorded against on the render thread. Self-contained (features include it); Internal.h
// includes it in its order.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/PrefixSum.h"
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct ZBucketPlan;
	struct SetClassPlan;
	/**
	 * @brief A state class's sub-ranges of the colour segment's ranges (GpuLayouts.h, kClassRangeMain; SetClassPlan): per range its first
	 * sequence slot and capacity (0: the class draws nothing there, and no call is made), and its sort keys (its first, and its size: the
	 * pipelines the revision's set version holds of it).
	 */
	struct SetClassRanges
	{
		struct Range
		{
			std::uint32_t first = 0, capacity = 0;
			bool operator==(const Range&) const = default;
		};
		std::array<Range, kClassRanges> ranges{};
		std::uint32_t keyBase = 0, size = 0;
		bool operator==(const SetClassRanges&) const = default;
	};
	/**
	 * @brief What one segment's passes record against: everything their recorded commands depend on,
	 * and nothing that changes every frame. The frame's own values (the counts, the view-projection, the
	 * culling stamp) are in the segment's BuildDrawsLatch, and the draw counts on the GPU. Published by
	 * the commit before the epoch prepares and replaced only when it changes, so its identity is what
	 * the passes' invocation revisions carry.
	 */
	struct PassFrame
	{
		std::uint64_t generation = 0;
		// The colour segment's state classes' sub-ranges (SetClassPlan), by class in creation order: what its calls draw, each with its
		// class's signature, and their max counts. None in the depth segment (its draws are bucketed plain draws).
		std::vector<SetClassRanges> classes;
		// Each terrain group's range (the colour segment's; MainLayout::terrainCapacity), group 1 from phase 2's base, group 2 after it,
		// holding its classes' sub-ranges.
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
		// Each half of the forward range in all (the colour segment's and the forward epoch's; MainLayout::forwardCapacity), after the
		// terrain groups' ranges: per class a staging half BuildDraws appends to, then a sorted half the forward epoch draws.
		std::uint32_t forwardCapacity = 0;
		// The refraction range (the colour segment's and the forward epoch's; MainLayout::refractionCapacity), after the
		// forward halves (RefractionSlot).
		std::uint32_t refractionCapacity = 0;
		// The sequence buffer's ranges as the epoch sized it (MainLayout::sequenceDraws, sequenceDecals): its layout.
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
		std::uint32_t cullMode = 0;
		std::uint32_t width = 0, height = 0;  // render area: the main pass viewport
		float minDepth = 0.0f, maxDepth = 1.0f;  // its depth range (the engine uses [0, 0.999998])
		rhi::DescriptorHeapHandle resourceHeap{};
		rhi::DescriptorHeapHandle samplerHeap{};
		IndirectState indirect{};
		// The latch block the epoch wrote (the one the revision's set names): a layout grown (ReserveMainLatch) is a new block of the
		// next set, and a frame in flight keeps reading its own.
		std::shared_ptr<const org::LatchBlock> latch;
		// The Z-prepass's plain draws (MainOpaquePass): a call per bucket, a group of pipeline slots sharing a depth pipeline. Phase
		// 2's ranges are the same past sequenceDraws.
		struct ZCall
		{
			std::uint32_t bucket = 0, first = 0, capacity = 0;
			rhi::PipelineHandle pipeline{};
			// A terrain group 1 bucket's twin, its near blocks (kBucketTerrainPair): drawn into the main depth with Terrain
			// Blending's depth offset (kZDrawPushOffsetDepth), then into its terrain depth without (Resources::terrainDepth).
			bool terrainNear = false;
			bool operator==(const ZCall& o) const
			{
				return bucket == o.bucket && first == o.first && capacity == o.capacity && SameHandle(pipeline, o.pipeline) && terrainNear == o.terrainNear;
			}
		};
		std::vector<ZCall> zCalls;
		// The commit's per-frame values the epoch's first pass copies from the latch (LatchedCopiesPass).
		LatchedList latched;
		// Not compared (the latch block's layout, and the bucket plan zCalls are the calls of, the class plan classes are the sub-ranges
		// of): what a commit writing its values into this shape writes them against - a scene revision's shape, which that commit did not
		// make.
		MainLatchLayout latchLayout;
		std::shared_ptr<const ZBucketPlan> zPlan;
		std::shared_ptr<const SetClassPlan> classPlan;

		bool SameShape(const PassFrame& o) const
		{
			return zCalls == o.zCalls && latched == o.latched && classes == o.classes &&
			       terrainCapacity == o.terrainCapacity && forwardCapacity == o.forwardCapacity && refractionCapacity == o.refractionCapacity &&
			       sequenceDraws == o.sequenceDraws &&
			       sequenceDecals == o.sequenceDecals && cullMode == o.cullMode &&
			       width == o.width && height == o.height &&
			       minDepth == o.minDepth && maxDepth == o.maxDepth && SameHandle(resourceHeap, o.resourceHeap) &&
			       SameHandle(samplerHeap, o.samplerHeap) && SameIndirect(indirect, o.indirect) && latch == o.latch;
		}
	};


	// A CPU-written buffer the main pass reads as a structured buffer SRV (t16 and up): the graph reads a
	// copy, refilled every epoch from the buffer's CPU mirror.
	struct FrameBuffer
	{
		std::uint32_t textureRegister = 0;
		std::uint32_t stride = 0;
		std::uint32_t firstElement = 0;
		std::uint32_t elements = 0;
		std::shared_ptr<org::Buffer> copy;

		bool SameShape(const FrameBuffer& a_other) const
		{
			return textureRegister == a_other.textureRegister && stride == a_other.stride && firstElement == a_other.firstElement && elements == a_other.elements;
		}
	};

	/**
	 * @brief A content epoch per kept table (ObjectRecordStore, ExtrasStore, GeometryStore): what a build trims each one's
	 * journal to (PayloadBuilder::OldestContentEpoch of its part).
	 */
	struct TablesContentEpochs
	{
		std::uint64_t objects = 0, extras = 0, geometries = 0;
		bool operator==(const TablesContentEpochs&) const = default;
	};

	struct SortSequencesConstants
	{
		std::uint32_t stagingIndex;
		std::uint32_t ranksIndex;
		std::uint32_t offsetsIndex;
		std::uint32_t countIndex;
		std::uint32_t sequencesIndex;
		std::uint32_t sequenceStride;  // bytes (sizeof(DrawSequence))
		std::uint32_t drawLimit;       // the draws' range (BuildDrawsConstants::phaseTwoBase)
		std::uint32_t padding;
	};
	constexpr std::uint32_t kSortSequencesConstantWords = sizeof(SortSequencesConstants) / 4;
	/**
	 * @brief ForwardSortCS.hlsl's push constants: a state class's forward staging half (BuildDraws appended its visible forward draws
	 * there, each with its view depth in its firstInstance word) and its sorted half, which the forward epoch draws.
	 */
	struct ForwardSortConstants
	{
		std::uint32_t sequencesIndex;  // RWByteAddressBuffer: the main sequences (or a view's)
		std::uint32_t countIndex;      // ByteAddressBuffer: the count words (the classCounts words, a view's bucket counts)
		std::uint32_t stagingBase;     // the staging half's first slot
		std::uint32_t capacity;        // each half's slots (the class's forward sub-range's)
		std::uint32_t sequenceStride;  // bytes (sizeof(DrawSequence))
		// With kCountOffset in options, the count is the word at countOffset bytes (the class's: ClassCountWord, a view's forward entry).
		std::uint32_t countOffset = 0;
		std::uint32_t options = 0;
		std::uint32_t padding = 0;
		static constexpr std::uint32_t kCountOffset = 1;
		// The sorted draw's firstInstance is its rank: a forward bucket's plain draw finds its sequence by its instance (DCLF_PULLED).
		static constexpr std::uint32_t kRankInstance = 2;
	};
	constexpr std::uint32_t kForwardSortConstantWords = sizeof(ForwardSortConstants) / 4;
	constexpr std::uint32_t kForwardSortGroup = 64;  // ForwardSortCS.hlsl's
	/** @brief The forward range's first slot in a colour shape's sequences (after phase 2's terrain ranges): its first class's staging half. */
	inline std::uint64_t ForwardStagingSlot(const PassFrame& a_frame)
	{
		return std::uint64_t(a_frame.sequenceDraws) + a_frame.terrainCapacity[0] + a_frame.terrainCapacity[1];
	}
	/** @brief The refraction range's first slot in a colour shape's sequences (after the forward range's two halves). */
	inline std::uint64_t RefractionSlot(const PassFrame& a_frame)
	{
		return ForwardStagingSlot(a_frame) + 2ull * a_frame.forwardCapacity;
	}
	constexpr std::uint32_t kSortSequencesGroup = 64;  // SortSequencesCS.hlsl's
	// A sort's rank word holds the sort key in its top 12 bits and the rank below (BuildDrawsCS.hlsl, kSortRankBits): a
	// draw range past this is a hard failure until the ranks get a word of their own.
	constexpr std::uint32_t kSortRankLimit = 1u << 20;
	static_assert(DrawPipelines::kMaxPipelines <= (1u << 12));

	/**
	 * @brief The sort by pipeline (DrawSort) of the colour segment's sequences. BuildDraws stages the sequences and counts them per
	 * sort key (a state class's keys contiguous: its first key plus the pipeline's index in it), PrefixSum turns the counts into each
	 * key's first slot, and SortSequencesPass (SortSequencesCS.hlsl) writes each class's sequences grouped by pipeline into its main
	 * sub-range. The counts are zeroed once, by the first commit (countsZeroed), and after that by the scan that reads them.
	 *
	 * Not the shadow views': sorting them (a part of these buffers per view slot, one scan over all of them) gained no measurable
	 * draw time, and the scan over every view's counts costs more than it saves.
	 */
	struct DrawSort
	{
		std::shared_ptr<org::Buffer> counts, offsets, blockSums;
		Versioned staging, ranks;
		std::shared_ptr<const PrefixSum::Programs> prefixSum;
		std::shared_ptr<const ComputeProgram> scatter;
		bool countsZeroed = false;  // render thread

		static constexpr std::uint32_t kKeys = DrawPipelines::kMaxPipelines;

		/** @brief Null, logged, when either program could not be created. Staging and ranks for a_draws (they grow with the sequences). */
		static std::shared_ptr<DrawSort> Create(rhi::Device a_device, std::uint32_t a_draws)
		{
			auto sort = std::make_shared<DrawSort>();
			sort->prefixSum = PrefixSum::Load(a_device);
			sort->scatter = ComputeProgram::Load(a_device, { .source = kSortSequencesShader, .constantWords = kSortSequencesConstantWords });
			if (!sort->prefixSum || !sort->scatter) {
				logger::warn("[DCLF] The draws are not sorted by pipeline: its programs could not be created");
				return {};
			}
			sort->counts = CreateWords(kKeys, true, "cs.dclf.sort-counts");
			sort->offsets = CreateWords(kKeys, true, "cs.dclf.sort-offsets");
			sort->blockSums = CreateWords(PrefixSum::Blocks(kKeys), true, "cs.dclf.sort-block-sums");
			// Sized to the main layout's draws exactly (its ranges grow by doubling: Impl::RequireMain).
			sort->staging = MakeVersioned(CreateWords(std::uint64_t(a_draws) * sizeof(DrawSequence) / 4, true, "cs.dclf.sort-staging"),
				{ .extent = { 0, sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
			sort->ranks = MakeVersioned(CreateWords(a_draws, true, "cs.dclf.sort-ranks"), { .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
			return sort;
		}

		void Register(org::RenderGraph& a_graph) const
		{
			// Not the staging or the ranks: versioned, their passes declare them (Register, Passes/MainExtension.cpp).
			for (const auto& buffer : { counts, offsets, blockSums })
				a_graph.RegisterResource(org::ResourceIdentifier(buffer->GetName()), buffer);
		}

		/**
		 * @brief The scan, which runs in every execution of its epoch it is active in (a_active; null: every one), so the counts it clears
		 * stay zero whether or not a build ran. An inactive execution must follow one whose counts nothing raised since (the last scan
		 * cleared them).
		 */
		std::shared_ptr<org::RenderPass> ScanPass(std::function<bool(const org::PassPrepareContext&)> a_active = {}) const
		{
			PrefixSum::Desc scan{};
			scan.programs = prefixSum;
			scan.counts = counts;
			scan.offsets = offsets;
			scan.blockSums = blockSums;
			scan.elements = kKeys;
			scan.clearCounts = true;
			scan.active = std::move(a_active);
			return PrefixSum::CreatePass(std::move(scan));
		}

		template <class Uploads>
		void ZeroCountsOnce(Uploads& a_uploads)
		{
			if (countsZeroed)
				return;
			static const std::array<std::uint32_t, kKeys> zeros{};
			a_uploads(counts, zeros.data(), sizeof(zeros), 0);
			countsZeroed = true;
		}
	};

	struct PassStats;

	/**
	 * @brief The state lists of one recording site's explicit DGC preprocesses (DrawPipelines.h), one per frame slot:
	 * graphics lists that are never submitted. vkCmdPreprocessGeneratedCommandsEXT is recorded outside any pass but
	 * generates for the state of another list, render pass included, which the execution must then match exactly; so
	 * a site begins each of its passes on its slot's list and sets it up as the execution will (heaps, layout, topology,
	 * push data) before preprocessing that pass's calls against it. A site of its own for every pass that can record at
	 * the same time as another.
	 */
	class PreprocessStates
	{
	public:
		static std::shared_ptr<PreprocessStates> Create(rhi::Device a_device, std::uint32_t a_frameSlots, const char* a_site)
		{
			auto states = std::make_shared<PreprocessStates>();
			states->slots.resize(std::max<std::uint32_t>(a_frameSlots, 1u));
			for (auto& slot : states->slots) {
				if (rhi::Failed(a_device.CreateCommandAllocator(rhi::QueueKind::Graphics, slot.allocator)) || !slot.allocator ||
					rhi::Failed(a_device.CreateCommandList(rhi::QueueKind::Graphics, slot.allocator.Get(), slot.list)) || !slot.list) {
					logger::error("[DCLF] DGC preprocess: no state list for {}; its draws cannot execute", a_site);
					return nullptr;
				}
			}
			return states;
		}

		// The frame slot's list, reset and recording.
		rhi::CommandList& Begin(std::uint32_t a_frameSlot)
		{
			ZoneScopedN("CS.DCLF.Record.RecyclePreprocessState");
			auto& slot = slots[a_frameSlot % slots.size()];
			slot.list->Recycle(slot.allocator.Get());
			return slot.list.Get();
		}

	private:
		struct Slot
		{
			rhi::CommandAllocatorPtr allocator;
			rhi::CommandListPtr list;
		};
		std::vector<Slot> slots;
	};

	struct IndexPool;
}
