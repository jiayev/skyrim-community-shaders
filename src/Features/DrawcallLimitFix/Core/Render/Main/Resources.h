#pragma once

// The main epochs' graph resources, bindings, bucket plans, shape inputs and pass statistics. Render thread's; their buffers grow
// to the revision producer's requirements (SceneBufferSet), its layout (MainLayout) derived from the scene's draw bound.
// Self-contained (features include it); Internal.h includes it in its order.

// volk must precede every Vulkan header in this translation unit (FrameLayout.h first, in its own include block).
#include "Features/DrawcallLimitFix/Core/Render/Main/FrameLayout.h"

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Draws.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"

#include "RenderGraph/ComputeProgram.h"
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/PixelBuffer.h>
#include <rhi.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Draws
{
	struct ViewResources;  // Epochs/ViewEpoch.h
	struct ShadowResources;  // Shadow/ShadowFrame.h

	/**
	 * @brief How the main epochs lay out the sequences and the Z-prepass buckets (the revision producer's, Impl::RequireMain): derived
	 * from the scene's draw bound, grown only when exceeded (powers of two), so the recorded calls change only then. Every buffer it
	 * indexes is required to hold it, and a set sealed without that is never named, so a shape made from this layout and the named
	 * buffers always agree.
	 */
	struct MainLayout
	{
		// Z-prepass plain draws (MainOpaquePass, DCLF_PULLED): per pipeline slot, its bucket's range of each phase's sequences
		// (every draw the slot can produce; zBucketsLayout moves with them), and the count words the buckets take (slots, then a
		// twin per terrain group 1 slot), zeroed by the depth commit.
		std::vector<std::uint32_t> zBucketCapacity, zBucketFirst;
		std::uint64_t zBucketsLayout = 0;
		std::uint32_t buckets = 0;
		// Sequence buffer ranges (GpuLayouts.h, SequenceSlots): draws per draw range and per decal sub-range, and the decal sub-ranges
		// the buffer holds (one per decal group and state class present: SetClassPlan::decalRanges; doubled past them).
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
		std::uint32_t decalRanges = kDecalGroups;
		/*
		 * Per pipeline slot, the draws its objects can produce (DrawBoundStore::PipelineDraws: decals count toward none), kept while they
		 * fit and grown to a power of two past them; slotsLayout moves with them. A state class's sub-range of a range holds the
		 * capacities of the slots its pipelines draw there (PlanSetClasses), so the colour calls change only when a slot grows or the
		 * slots' classes move.
		 */
		std::vector<std::uint32_t> slotCapacity;
		std::uint64_t slotsLayout = 0;
		// Within sequenceDraws, the ranges in phase 2 (GpuLayouts.h): the terrain groups' (kTerrainGroups), then each forward half's
		// (kForwardSortShader), each the sum of its slots' capacities (its classes' sub-ranges within it), then refraction's (a power of two).
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
		std::uint32_t forwardCapacity = 0;
		std::uint32_t refractionCapacity = 0;
		bool operator==(const MainLayout&) const = default;
	};
	/** @brief The main layout's first sequence ranges (CS_DCLF_TABLE_START=small: a few draws, to test growth). */
	MainLayout InitialMainLayout();

	struct Resources
	{
		std::vector<FrameBuffer> frameBuffers;
		// Shared by every epoch.
		std::shared_ptr<SceneBuffers> scene;
		// BuildDraws' out (its inputs, geometries and rows are the installed payload's: PayloadVersions).
		Versioned sequences;
		std::shared_ptr<org::Buffer> count;
		// The colour segment's state class sub-ranges' counts, a word per (class, range) (GpuLayouts.h, ClassCountWord): BuildDraws
		// appends through them, the colour and forward calls read them. Zeroed by the colour commit.
		Versioned classCounts;
		std::array<Versioned, 2> zBucketCounts;
		std::shared_ptr<IndexPool> pool;
		std::shared_ptr<const ComputeProgram> buildDraws, clearPortalQueries, preparePortalQueries;
		rhi::CommandSignaturePtr portalQuerySignature;
		winrt::com_ptr<ID3D11Buffer> countD3D11;  // the culling counters' readback
		// The foliage record (Policy::RecordFoliagePixels): per frame parity, each pixel's object and colours, and the compare
		// pass's results (Diagnostics/Draws/Foliage.cpp), read back per frame slot.
		struct FoliageRecord
		{
			std::array<std::shared_ptr<org::Buffer>, 2> ids, colours;
			std::array<std::uint64_t, 2> idsAddress{}, coloursAddress{};
			std::shared_ptr<org::Buffer> results;
			std::uint64_t resultsAddress = 0;
			// The Z-prepass's alpha-tested fragment owner per pixel (GpuLayouts.h, kFoliageOwned) and its UAV index.
			std::shared_ptr<org::Buffer> owners;
			std::uint32_t ownersIndex = 0;
			std::uint32_t epoch = 0;  // colour commits so far: buffer parity and tag (the scene frame can repeat or skip)
			std::shared_ptr<const ComputeProgram> program;
			std::vector<std::shared_ptr<org::Buffer>> readback;
			std::vector<std::uint32_t> readbackFrame;  // 1 where the slot's copy holds a frame's results
			std::uint32_t width = 0, height = 0;
		};
		std::shared_ptr<FoliageRecord> foliage;
		// The per-frame constant blocks at fixed slots (FrameSlotOffset), so a build can name them before their contents exist.
		std::shared_ptr<org::Buffer> frameConstants;
		// Targets declared for the latched copies (LatchedCopiesPass); anything else is staged.
		std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;  // LatchedTarget::key
		std::uint64_t frameConstantsAddress = 0;
		// The zeroed StrictLightData every bindless draw's b3 reads: constant, staged once by the first commit.
		bool sharedLightZeroed = false;
		// The main pass's imported targets and depth.
		std::uint32_t targetCount = 0;
		std::array<std::shared_ptr<org::ExternalTextureResource>, kColorTargets> native;
		std::shared_ptr<org::ExternalTextureResource> nativeDepth;
		// Terrain Blending's terrain depth, which the Z-prepass draws near terrain into. Null without that feature.
		std::shared_ptr<org::ExternalTextureResource> terrainDepth;
		// The engine's refraction normals (kREFRACTION_NORMALS), for RefractionDrawPass. Null without the target.
		std::shared_ptr<org::ExternalTextureResource> refractionNormals;
		DXGI_FORMAT refractionFormat = DXGI_FORMAT_UNKNOWN;
		// The engine's screen-space shadow mask (kSHADOW_MASK, made UAV-capable by Community Shaders' creation hook), into which
		// LocalShadowMaskCS resolves the local shadow lights' channels for DCLF's receivers, and that program. Null without either.
		std::shared_ptr<org::ExternalTextureResource> shadowMask;
		std::uint32_t shadowMaskWidth = 0, shadowMaskHeight = 0;
		std::shared_ptr<const ComputeProgram> localShadowMasks;
		// The shadow engine's resources (their shadow map imports, which the masks sample), once both exist (Impl::ShadowResourcesMade).
		std::shared_ptr<ShadowResources> shadow;
		Versioned visibility;  // per-object verdict, published by the depth segment
		// Per object, the last frame stamp at which main depth phase 1 found its bound inside the frustum, occlusion aside (the
		// engine's OnVisible semantics). Never cleared: a stale stamp is simply not this frame's.
		Versioned frustum;
		// Depth phase 1's retest list: the inputs it left OccludedRetest (an input index a word, kCountRetestsWord long), which phase 2
		// runs over. An object slot a word, with the per-object buffers (RequireSceneTables): an object has one input, so the list never
		// holds more entries than the visibility words its inputs name.
		Versioned retests;
		// TEMP (CS_DCLF_SET_PARITY): per object, who wrote its visibility word (BuildDrawsCS.hlsl, RecordWriter; 32 bytes). Null otherwise.
		Versioned writers;
		// Phase 2's dispatch arguments (groups x, y, z, a pad word): the depth commit latches kRetestDispatchReset, phase 1 adds a group
		// per 64 retest entries, phase 2's ExecuteIndirect reads them.
		std::shared_ptr<org::Buffer> retestDispatch;
		std::shared_ptr<PassStats> passStats;  // CS_DCLF_PASS_STATS
		// Explicit DGC preprocess state lists (colour segment, forward epoch), and the forward range's sort.
		std::shared_ptr<PreprocessStates> preprocessMain;
		std::shared_ptr<PreprocessStates> preprocessForward;
		std::shared_ptr<const ComputeProgram> forwardSort;
		// The HZB, the depth pyramid's (Api/DepthPyramid.h; null without its producer). nativeDepth is imported with SRV and DSV so the
		// graph sees one depth resource for draws and build.
		std::shared_ptr<org::PixelBuffer> hzb;
		// The producer's state (its setup's result), handed to its pass factory.
		std::shared_ptr<const void> hzbState;
		// The producer's scratch words (the downsample's finished-group count): zeroed by the first depth commit, then by the producer.
		std::shared_ptr<org::Buffer> hzbScratch;
		bool hzbScratchZeroed = false;
		std::uint32_t hzbWidth = 0, hzbHeight = 0, hzbMips = 0;
		std::uint32_t width = 0, height = 0;
		// The frame graph buffers registered on the graph (Api::FrameGraphBuffers, a bit per registration: Integrations/LightLimitFix's,
		// t35-t37), read at their registers instead of the captured frame buffers.
		std::uint32_t graphBuffers = 0;
		rhi::CommandSignaturePtr dispatchSignature;
		// The pipeline sort of phase 1's and the colour segment's sequences; null when off.
		std::shared_ptr<DrawSort> sort;
		// The last depth commit. A view drawing this frame's depth commit (the reflection's faces: TakeViewInputs) draws only on its frame
		// and while the adopted set is the one it was committed against (nothing has grown since).
		struct Committed
		{
			std::uint32_t frame = ~0u, inputs = 0;
			std::uint64_t adoptedSerial = 0;  // the adopted set's serial
		};
		Committed depthCommitted;
		// The views drawing from these resources (ViewResources::main), by view slot: set by SetupView, and by Setup for the views made
		// before. What a main epoch hosting views' epochs (Api::EpochDef::hostedBy: the depth epoch, the reflection's and first person's
		// depth) makes each epoch's passes with, at the graph's build (render thread, as the writes). Weak: a view holds its main resources.
		std::array<std::weak_ptr<ViewResources>, static_cast<std::size_t>(Api::EngineView::Count)> views;
	};

	struct PassBindings
	{
		std::array<org::DeclaredViewToken, kColorTargets> targets{};
		org::DeclaredViewToken depth;
		org::DeclaredViewToken terrainDepth;  // the Z-prepass's, with Terrain Blending (Resources::terrainDepth)
		org::ResourceBindingToken sequences, count;
		org::ResourceBindingToken classCounts;                                             // the colour and forward calls' counts (Resources::classCounts)
		org::ResourceBindingToken pool, bucketCounts;                                      // the depth segment's plain draws
		std::array<org::ResourceBindingToken, Extensions::kDrawFamilies> familyArguments;  // each draw family's arguments (DrawFamilyDeclare)
		std::array<std::uint64_t, Extensions::kDrawFamilies> familyRows{};                 // and its draw row's address (DrawFamilyDeclare::row)
	};

	/*
	 * The main epochs' shape producers (Revision/Producer.cpp; dclf-architecture.md, "Revisions and recordings"). A segment's shape (PassFrame) is a function
	 * of what a scene revision names (resource versions, draw bound, pipeline set, lookups) and of engine setup (viewport, frame
	 * block sizes); never of a frame's values, which the commit writes into the latch.
	 */

	/**
	 * @brief The Z-prepass's plain draws: one bucket per group of pipeline slots sharing a depth pipeline (IndirectState::zGroups),
	 * in first-use order. map: each slot's bucket (kNoBucket without a published pipeline: BuildDraws drops its draws). The calls
	 * are the shape's; map and tables go into the latch.
	 */
	struct ZBucketPlan
	{
		std::vector<std::uint32_t> map;    // per pipeline slot
		std::vector<std::uint32_t> table;  // phase 1's (first, capacity) per bucket, then phase 2's
		std::vector<PassFrame::ZCall> calls;
		std::uint32_t Buckets() const { return static_cast<std::uint32_t>(calls.size()); }
	};
	void PlanZBuckets(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out);

	/**
	 * @brief The colour segment's state class sub-ranges (GpuLayouts.h, kClassRangeMain): per class of the revision's set version
	 * (IndirectState::classes), each range's sub-range from the capacities of the slots whose set index is the class's
	 * (MainLayout::slotCapacity), by the slot's key: a decal group's (sequenceDecals each, one per group and class present), the forward
	 * range's (not refraction's: a plain draw), a terrain group's, else the main one. The main sub-ranges from slot 0, the terrain groups'
	 * and the forward pairs' (staging, sorted) in phase 2's range where MainLayout places those ranges, the decal ones from the decal
	 * region. classes are the shape's (compared); table and map the latch's words (BuildDrawsLatch::classTableOffset, classMapOffset).
	 */
	struct SetClassPlan
	{
		std::vector<SetClassRanges> classes;
		std::vector<std::uint32_t> table;                                                                  // [classes, kClassRanges, 0, 0], then kClassWords per class
		// [set indices, forward buckets, main buckets, terrain buckets, decal buckets, 0, 0, 0], then IndirectState::classOf; with forward
		// buckets, then each set index's forward bucket (kNoBucket: none) and each bucket's (staging first, capacity); with main buckets, then
		// likewise each set index's main bucket and each bucket's (first, capacity); with terrain buckets, and then decal buckets, likewise.
		std::vector<std::uint32_t> map;
		std::uint32_t decalRanges = 0;  // the (group, class) decal sub-ranges present
		/**
		 * The forward range's plain draws (CS_DCLF_FORWARD_PLAIN; ForwardDrawPass): a bucket per set index with forward slots, its staging
		 * half then its sorted half (each of its slots' capacities), laid out over the forward range the classes' pairs would take. Empty
		 * when off, or while a forward set index has no plain colour pipeline (IndirectState::plainColour): the classes' calls draw it.
		 */
		struct PlainBucket
		{
			std::uint32_t set = 0, first = 0, capacity = 0;
			std::uint32_t group = 0;  // a terrain bucket's terrain group (1, 2: its range's), 0 for any other
		};
		std::vector<PlainBucket> forward;
		/**
		 * The main range's plain draws (CS_DCLF_MAIN_PLAIN; the colour pass): a bucket per set index with main slots (its slots' capacities),
		 * laid out over the main range the classes' sub-ranges would take, drawn in any order (EQUAL against the Z-prepass). Empty when off, or
		 * while a main set index has no plain colour pipeline: the classes' calls draw it.
		 */
		std::vector<PlainBucket> main;
		/**
		 * The terrain groups' plain draws (CS_DCLF_TERRAIN_PLAIN; the colour segment's second pass): a bucket per set index with terrain
		 * slots (a set index's key has one terrain group), laid out over its group's range, drawn in any order (each group appends
		 * unsorted). Group 1's buckets first. Empty when off, or while a terrain set index has no plain colour pipeline.
		 */
		std::vector<PlainBucket> terrain;
		/**
		 * The decal groups' plain draws (CS_DCLF_DECAL_PLAIN; the colour segment's decal depth and second passes): a bucket per set index
		 * with decal slots (a set index's key has one decal group), its staging half then its sorted half (ForwardSortCS ranks its decals by
		 * their engine ordinal: the same order every frame), each half a decal sub-range's size, past the classes' decal sub-ranges. Group by
		 * group, set index by set index: drawn in that order. Empty when off, or while a decal set index has no plain colour pipeline (or,
		 * in a group with a depth prepass, no Z-prepass one).
		 */
		std::vector<PlainBucket> decals;
		// classCounts' words: a class's per range, then a forward bucket's, then a main bucket's, then a terrain bucket's, then a decal bucket's.
		std::uint32_t Words() const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(forward.size() + main.size() + terrain.size() + decals.size());
		}
		std::uint32_t DecalCountWord(std::size_t a_bucket) const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(forward.size() + main.size() + terrain.size() + a_bucket);
		}
		std::uint32_t TerrainCountWord(std::size_t a_bucket) const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(forward.size() + main.size() + a_bucket);
		}
		std::uint32_t ForwardCountWord(std::size_t a_bucket) const { return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(a_bucket); }
		std::uint32_t MainCountWord(std::size_t a_bucket) const
		{
			return static_cast<std::uint32_t>(classes.size()) * kClassRanges + static_cast<std::uint32_t>(forward.size() + a_bucket);
		}
	};
	void PlanSetClasses(const MainLayout& a_layout, const Lookups& a_lookups, const SceneTables& a_tables, const IndirectState& a_indirect, SetClassPlan& a_out);

	/** @brief A main epoch's frame block sizes (FrameBlocks) per stage and register, which its latched copies take. */
	struct FrameBlockSizes
	{
		std::array<std::uint32_t, kConstantBufferRegisters> vs{}, ps{};
		bool operator==(const FrameBlockSizes&) const = default;
	};

	/** @brief A main commit's latched copies, in write order and packed as LatchedUploads packs them (a_classWords: the colour commit's classCounts zeros). */
	std::vector<LatchedCopy> MainLatchedLayout(const Resources& a_resources, bool a_depthOnly, const FrameBlockSizes& a_blocks, std::uint32_t a_buckets,
		std::uint32_t a_classWords);
	/** @brief The bytes a layout takes of its latch block's slot. */
	std::size_t LatchedBytes(const std::vector<LatchedCopy>& a_copies);

	/** @brief The main pass's render area and depth range, as a segment's last capture had them. */
	struct MainViewport
	{
		std::uint32_t width = 0, height = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
		bool operator==(const MainViewport&) const = default;
	};

	/** @brief What a main segment's shape is made from (MakeMainShape). */
	struct MainShapeInputs
	{
		bool depthOnly = false;
		std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // MainLayout's ranges
		std::shared_ptr<const SetClassPlan> classPlan;        // the colour segment's (PlanSetClasses)
		std::array<std::uint32_t, kTerrainGroups> terrainCapacity{};
		std::uint32_t forwardCapacity = 0;
		std::uint32_t refractionCapacity = 0;
		MainViewport viewport;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};
		std::uint32_t cullMode = 0;
		std::shared_ptr<const org::LatchBlock> latch;  // the one the revision's set names (RequireLatches)
		MainLatchLayout latchLayout;
		std::vector<PassFrame::ZCall> zCalls;  // the depth segment's (PlanZBuckets)
		std::shared_ptr<const ZBucketPlan> zPlan;
		LatchedList latched;  // MainLatchedLayout, in its reserved block
	};
	/**
	 * @brief A segment's shape inputs from the layout and the versions a_sealed names (ranges, a_cullMode). The caller fills
	 * the latch and the descriptor heaps.
	 */
	MainShapeInputs MainShapeInputsOf(const Resources& a_resources, const MainLayout& a_layout, const NamedVersions& a_sealed, bool a_depthOnly, std::uint32_t a_cullMode);
	std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in);

	/**
	 * @brief CS_DCLF_PASS_STATS=1: the main draw passes' pipeline statistics per frame slot, read back when the slot comes round.
	 */
	struct PassStats
	{
		enum Kind : std::uint32_t
		{
			kDepth,
			kDepthPhaseTwo,
			kColour,
			kDecals,  // the colour segment's second pass
			kKinds
		};
		static constexpr std::uint32_t kFields = 4;  // IA vertices, IA primitives, VS invocations, PS invocations
		rhi::QueryPoolPtr pool;
		std::vector<std::shared_ptr<org::Buffer>> readback;  // per frame slot, kKinds results
		std::vector<std::array<bool, kKinds>> written;
		std::mutex mutex;
		std::array<std::array<std::uint64_t, kFields>, kKinds> totals{};
		std::array<std::uint32_t, kKinds> samples{};

		static bool Enabled()
		{
			return Diagnostics::Observe().passStats;
		}

		/** @brief The slot's results from its last use, then its query reset; before the pass begins. */
		void Start(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
		{
			if (a_slot >= readback.size())
				return;
			const std::lock_guard lock(mutex);
			if (written[a_slot][a_kind]) {
				auto resource = readback[a_slot]->GetAPIResource();
				void* mapped = nullptr;
				resource.Map(&mapped);
				if (mapped) {
					const auto* values = static_cast<const std::uint64_t*>(mapped) + std::size_t(a_kind) * kFields;
					for (std::uint32_t f = 0; f < kFields; ++f)
						totals[a_kind][f] += values[f];
					++samples[a_kind];
					resource.Unmap(0, 0);
				}
			}
			if (a_kind == kColour && samples[kColour] >= Reports::Interval()) {
				Reports::PassStatsLine(totals, samples);
				totals = {};
				samples = {};
			}
			a_commands.ResetQueries(pool->GetHandle(), a_slot * kKinds + a_kind, 1);
		}
		void Begin(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
		{
			if (a_slot < readback.size())
				a_commands.BeginQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
		}
		void End(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
		{
			if (a_slot < readback.size())
				a_commands.EndQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
		}
		/** @brief After the pass ends. */
		void Resolve(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
		{
			if (a_slot >= readback.size())
				return;
			a_commands.ResolveQueryData(pool->GetHandle(), a_slot * kKinds + a_kind, 1, readback[a_slot]->GetAPIResource().GetHandle(),
				std::uint64_t(a_kind) * kFields * sizeof(std::uint64_t));
			const std::lock_guard lock(mutex);
			written[a_slot][a_kind] = true;
		}
	};

	// Frame push: the pass-wide constant buffers' addresses, as the layout's push address ranges read them.
	inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants);

	/** @brief What a main pass execution draws a draw family with (its slot's pipelines while it is set up: null for nothing) and its row's address. */
	struct FamilyPrepared
	{
		std::shared_ptr<const void> drawn;
		std::uint64_t row = 0;
	};

	struct PreparedDraws
	{
		PassStats* stats = nullptr;              // CS_DCLF_PASS_STATS
		PreprocessStates* preprocess = nullptr;  // the frame slot's explicit DGC preprocess state list
		std::array<std::uint32_t, kFramePushWords> framePushWords{};
		std::shared_ptr<const PassFrame> frame;
		std::array<org::PreparedDescriptorReference, kColorTargets> targetViews{};
		org::PreparedDescriptorReference depthView{};
		org::PreparedDescriptorReference terrainDepthView{};  // Terrain Blending's terrain depth (the Z-prepass's)
		bool terrainDepth = false;
		std::uint32_t targetCount = 0;
		bool phaseTwo = false;
		bool zPrepass = false;
		// The draw families' draws (Api::DrawFamilyContribution::main), by family.
		std::array<FamilyPrepared, Extensions::kDrawFamilies> families{};
	};
}
