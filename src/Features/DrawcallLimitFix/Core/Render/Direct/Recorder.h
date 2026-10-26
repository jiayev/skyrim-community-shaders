#pragma once

// The CPU-driven pipeline's recorder (Direct.h; dclf-architecture.md, "The CPU-driven pipeline"): a CulledBatch's draws as direct draws
// onto an rhi::CommandList, DXVK's adopted buffer (Serial mode, inside a site's callback) or a secondary (Parallel mode, a per-snapshot
// view). One plain indexed draw a resident sequence: the lane's pulled pipeline bound when its bucket changes, the sequence's address
// and the draw's object word pushed (DrawPipelines.h, kDirectDrawPush*), its arguments from the set's CPU copy (ViewBatchSet::sequenceArgs).
// Recording reads only immutable inputs (the set, the culled batch, the frame's bindings), so any thread may record its own list.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

#include <rhi.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace DCLF
{
	struct IndirectState;
	struct ShadowIndirectState;
	struct BindlessPlacement;
}

namespace DCLF::Direct
{
	/** @brief Which pass of its site a recording is in: the pipeline a lane's bucket binds (ViewBuckets). */
	enum class RecordPass : std::uint8_t
	{
		Depth,       // the Z pipelines (IndirectState::zPipelines by zGroups): main depth, reflection's faces, first person's depth
		Colour,      // the plain colour pipelines (plainColour): main colour (EQUAL), the decals' second pass, forward
		DecalDepth,  // the opaque decals' depth pass (plainDepth)
		Shadow,      // a shadow view or occlusion map (ShadowIndirectState::pipelines)
		Refraction,  // the refraction normals (IndirectState::refraction)
	};

	/**
	 * @brief A draw family's main draw this frame (Api::DrawFamilyContribution::main.record's inputs, outside ORG): its pipelines as the
	 * frame's catalog has them (CatalogFamilyPipelines' payload, which the record callback reads as FamilyPipelines), its draw row's
	 * address (DrawFamilyDeclare::row) and its arguments buffer (DrawFamilyDeclare::arguments[0], resolved). Null pipelines: not drawn.
	 */
	struct FamilyDraw
	{
		std::shared_ptr<const void> pipelines;
		std::uint64_t row = 0;
		rhi::ResourceHandle arguments{};
	};

	/**
	 * @brief What a site's recordings bind, from the frame's adopted state (the sites fill it once a site and frame; immutable while any
	 * recording reads it). Handles stay valid while `main` / `shadow` hold their set versions.
	 */
	struct RecordBindings
	{
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		const IndirectState* main = nullptr;          // the frame's (FrameIndirectState or the revision's catalog's)
		const ShadowIndirectState* shadow = nullptr;  // the frame's shadow set (shadow views only)
		rhi::ResourceHandle indexPool{};              // the index pool's indices (R16, the whole buffer)
		// IndexPool::slotFirst as of the frame's first drawing commit (the site's per-frame copy): by geometry slot, the first index its
		// range starts at (IndexPool::kNoRange: none, not drawn).
		std::span<const std::uint32_t> poolFirsts;
		// The main layout's frame push words (FramePushWords of the view's frame constants: MainFramePushWords for the main camera).
		std::array<std::uint32_t, kFramePushWords> framePush{};
		// The shadow views' layout's pass words: a shadow view's own (ShadowFrameView::push: addresses of its latched blocks, so a
		// per-snapshot recording stays valid while the shadow shape does), the refraction's MainShadowLayoutPush.
		std::array<std::uint32_t, kShadowPushWords> shadowPush{};
		// A shadow view's map row (Lookups::ShadowMapRow of its mode and rasterizer state, the publication's lookups): by key slot (its
		// lanes' buckets), the ShadowIndirectState::pipelines index (Lookups::kNone: none yet, its draws skipped).
		std::span<const std::uint32_t> shadowMapRow;
		// The refraction draws' second address: the frame's LodFadeFrame (LodFadeFrameAddress), in the shadow layout's material rows' words.
		std::uint64_t lodFadeFrame = 0;
		// The frame's copy of the animated material rows (the latch's animatedRows, kDirectDrawPushAnimatedRows; 0: the installed rows).
		// Per frame: a per-snapshot recording's bindings keep 0.
		std::uint64_t animatedRows = 0;
		// Terrain Blending's depth offset on every draw (kZDrawPushOffsetDepth: a terrain group 1 twin's recording).
		bool offsetDepth = false;
		// RecordForward: the frame's placement rows (FrameValues::RowsIfDone, by object) and the view's ViewProj (row major, clip = M * p),
		// for each draw's sort key as BuildDrawsCS's StageForwardKey makes it.
		std::span<const BindlessPlacement> placements;
		std::array<float, 16> viewProj{};
		// RecordFamilyDraws: by draw family (Extensions::kDrawFamilies).
		std::span<const FamilyDraw> families;
	};

	/**
	 * @brief One recording's state: the list, the set, the view and its pass, the bindings; and what BeginList bound (a pipeline bound
	 * once per bucket change) and the draws recorded (drawn, added to counters' drawn[view] by each RecordBatch when counters is set).
	 */
	struct RecordContext
	{
		rhi::CommandList& list;
		const ViewBatchSet& set;
		ViewType view = ViewType::Main;
		std::uint32_t viewIndex = 0;
		Site site = Site::Depth;
		bool depthPass = false;  // the main list: depth (Z pipelines) or colour; with view Decal: the decal depth pass
		const RecordBindings& bindings;
		FrameCounters* counters = nullptr;  // drawn[view] (atomically: parallel recordings share it); null: not counted

		// Set by BeginList; RecordBatch's.
		RecordPass pass = RecordPass::Colour;
		std::uint32_t boundBucket = ~0u;
		rhi::PipelineHandle boundPipeline{};
		std::uint64_t boundVertexLayout = 0;  // a shadow bucket's (ShadowIndirectState::vertexLayouts), pushed with each of its draws
		std::uint32_t drawn = 0;
		// The list's calls (FrameCounters' binds, sameBinds, pushes, flushed with drawn): pipeline binds, those that bound the pipeline
		// already bound (another bucket of the same pipeline), and per-draw push constants.
		std::uint32_t binds = 0, sameBinds = 0, pushes = 0;
		std::uint32_t countedBinds = 0, countedSameBinds = 0, countedPushes = 0;
	};

	/** @brief The pass a view type's lanes draw in at a_depthPass (RecordContext::depthPass). */
	RecordPass PassOf(ViewType a_view, bool a_depthPass);

	/**
	 * @brief Binds what the list's draws share: the descriptor heaps, the pass's layout (the Z-prepass's for main-set pipelines, the
	 * shadow views' for shadow views and refraction), the topology, the frame (or view) push words and the index pool. In a secondary
	 * nothing is inherited, so each secondary begins with it (RecordSecondary does). False when the pass's state is not valid (no set
	 * version, no refraction pipeline): nothing recorded, record nothing.
	 */
	bool BeginList(RecordContext& a_context);

	/**
	 * @brief A_batch's draws in lane order: per kept lane, its sequences, the lane's bucket's pipeline bound when it changes. Main's colour
	 * pass leaves out the terrain groups' lanes (kLaneTerrainGroup1/2: RecordTerrainGroup's, after the decals); cull-only lanes draw nothing.
	 */
	void RecordBatch(RecordContext& a_context, const CulledBatch& a_batch);

	/**
	 * @brief A terrain group's colour draws (a_group 1 or 2: main's lanes with kLaneTerrainGroup1 or 2) from main's culled colour batches,
	 * in list order: the site calls it at the group's main range position in the second pass (Extensions::MainRanges' order), as the GPU
	 * path draws CS_DCLF_TERRAIN_PLAIN's buckets. Main's depth pass draws them with the rest (RecordBatch).
	 */
	void RecordTerrainGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches);

	/**
	 * @brief The forward list's draws in the engine's blended order (the refraction's are unordered, as the GPU path's: RecordBatch), merged
	 * across a_batches into the calling thread's buffer: each kept lane's sequences' sort key as
	 * StageForwardKey (the view depth of its bound centre, or of its BSOrderedNode's for a group's leaf), ordered as ForwardSortCS ranks
	 * them: farther first; at one depth a plain draw before a group's; then by group, ordinal, object and first index.
	 */
	void RecordForward(RecordContext& a_context, std::span<const CulledBatch* const> a_batches);

	/**
	 * @brief A second pass group's draws (ViewBatchSet::decalGroupFirst's group a_group, a main range slot) in the decal list's order,
	 * from whichever of a_batches hold its lanes: the site calls it per group at the colour pass's positions (Extensions::MainRanges'
	 * order; with RecordContext::depthPass, the groups that draw a depth prepass, in the decal depth pass).
	 */
	void RecordDecalGroup(RecordContext& a_context, std::uint32_t a_group, std::span<const CulledBatch* const> a_batches);

	/**
	 * @brief The draw families' instanced indirect draws (tree LOD's, grass's: their contribution's main.record, as the main pass records
	 * them) from RecordBindings::families: in a depth pass their depth draw (single phase: DrawFamilyPass::Depth), in the colour pass
	 * their colour draw under the Z-prepass layout and frame push words. Their cull is the site's pre-work. Leaves the list's pipeline
	 * and push data the families': call BeginList again before more DCLF draws.
	 */
	void RecordFamilyDraws(RecordContext& a_context);

	/**
	 * @brief Parallel mode and per-snapshot views: a_batch recorded into a_secondary (recycled by the caller from its allocator): begun
	 * with a_inheritance (the site's rendering: formats, samples, viewport), BeginList, RecordBatch, ended. A_context gives everything but
	 * the list (its own is not recorded to) and gets the draws counted; counters' secondaries counts it. False when it could not begin or
	 * BeginList had nothing valid (a_secondary is then ended with nothing in it: do not execute it).
	 */
	bool RecordSecondary(RecordContext& a_context, const rhi::SecondaryInheritance& a_inheritance, const CulledBatch& a_batch, rhi::CommandList& a_secondary);
}
