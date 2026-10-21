#pragma once

// The shadow epoch's types (its views: the shadow maps' and the occlusion maps'): captured views (render thread hooks), shapes (revision
// producer), the index pool and the graph resources (render thread). Self-contained (Features/Shadows/Engine's passes include it);
// Internal.h includes it in its order.

// volk must precede every Vulkan header in this translation unit (Main/Resources.h first).
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/IndexPool.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"

#include <string>
// GrowableRows.h is not self-contained: Versioned.h and <string> above are what it names.
#include "Features/DrawcallLimitFix/Core/Render/Payload/GrowableRows.h"

#include "RenderGraph/ComputeProgram.h"
#include <Render/LatchBlock.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi.h>

#include <d3d11.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	/** @brief A view the hook captured for the frame's epoch (CaptureShadowView, ExecuteShadowFrame). */
	struct PendingView
	{
		std::uint32_t viewId = 0, renderMode = 0, modeIndex = 0, targetIndex = 0, slice = 0;
		std::uint32_t x = 0, y = 0, width = 0, height = 0;
		std::uint32_t slot = 0;  // its placement's (ShadowEngine::shadowPlacements)
		float minDepth = 0.0f, maxDepth = 1.0f;
		RE::NiPoint3 eye;
		bool hasViewProj = false;
		std::array<float, 16> viewProj{};
		// The engine's caster volume (NiCullingProcess::customCullPlanes), when its culling process has one.
		float cullPlanes[6][4] = {};
		std::uint32_t cullPlaneMask = 0;
		// ShadowRasterStateId of the engine's caster state at cull mode 1, which the Utility shader sets for every pass but a
		// two-sided property's (engine notes); two-sided keys draw unculled (kRasterTwoSided).
		std::uint32_t rasterState = 0;
		// 0: ordinary casters (kObjectVolumetricOnly inputs skipped); 1: the volumetric lighting copy (volumetric-only casters).
		std::uint32_t casterClass = 0;
		bool sunView = false;  // the directional light's: its casters are its full-frustum entries' (kCullSunEntry)
		bool focus = false;    // a focus shadow's: its casters are focusTarget's members (BuildDrawsLatch::focusRoot)
		const void* focusTarget = nullptr;
		std::uint32_t portalContext = 0;  // its source camera's (ShadowCameras::Camera::portalContext: BuildDrawsLatch::strictContext)
		float viewBlock[12] = {};  // PerTechnique: HighDetailRange, ParabolaParam, EyeDelta
		std::array<std::byte, 1024> perFrame{};
		std::uint32_t perFrameBytes = 0;
		DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
	};

	/**
	 * @brief One plain indirect draw with one pipeline over a range of a view slot's sequences; one per pipeline in the view's
	 * map row, in row order.
	 */
	struct ShadowBucket
	{
		std::uint32_t pipeline = 0;  // shadow set index (ShadowIndirectState::pipelines)
		std::uint32_t first = 0;     // range start, in sequences
		std::uint32_t capacity = 0;  // max draw count: every draw its key slots can produce, grown by doubling

		bool operator==(const ShadowBucket&) const = default;
	};

	/** @brief One captured view of the frame's shadow epoch: where it draws its render mode's inputs, with which blocks. */
	struct ShadowFrameView
	{
		std::uint32_t slot = 0;       // the view's sequence and count buffers, record region and latch
		std::uint32_t modeIndex = 0;  // the inputs buffer (render mode 0xD, 0xE, 0xF)
		std::uint32_t capacity = 0;   // ExecuteIndirect max count (GrowCapacity, per slot)
		std::uint32_t x = 0, y = 0, width = 0, height = 0;  // viewport in its slice
		float minDepth = 0.0f, maxDepth = 1.0f;
		std::uint32_t target = 0;  // index into ShadowResources::depth
		std::uint32_t slice = 0;
		std::uint32_t sequenceDraws = 0;  // its slot's sequence buffer, in draws
		std::array<std::uint32_t, kShadowPushWords> push{};
		// Depth-only class pipelines first (ShadowBuckets).
		std::vector<ShadowBucket> buckets;
		std::uint32_t rasterState = 0;  // whose pipeline map row the buckets follow (ShadowViewLayout)

		bool operator==(const ShadowFrameView&) const = default;
	};

	struct RowBuckets;
	/**
	 * @brief The frame's shadow epoch shape. All captured views draw in one graph execution at AfterShadowMaps (each execution has
	 * a fixed CPU cost): the placements (views[i] at slot kFirstShadowViewSlot + i), then the occlusion maps' views as last set up (their
	 * slots, OcclusionSlot). Per-view matrices live in the latch; the shape changes only with the view layout or the pipelines.
	 */
	struct ShadowFrame
	{
		std::uint64_t generation = 0;
		rhi::DescriptorHeapHandle resourceHeap{};
		rhi::DescriptorHeapHandle samplerHeap{};
		ShadowIndirectState indirect{};
		std::vector<ShadowFrameView> views;
		// The latch block (the one the revision's set names: a new one when the layout grows); pass preparation reads it from here.
		std::shared_ptr<const org::LatchBlock> latch;
		// Latched copies: view blocks, zeroed counts, and the commit's SharedData/FeatureData (kCommitList).
		std::array<LatchedList, 3> latched;
		static constexpr std::size_t kCommitList = 2;
		// Not compared (implied by the latch and the buckets): the latch layout and each view's map row a commit writes against.
		ShadowLatchLayout latchLayout;
		std::shared_ptr<const std::vector<RowBuckets>> rows;
		// Each key slot's entry (Lookups::shadowSlotKeys), for the latch's slot table. Slots are append-only per lookups instance, so a
		// shape's table only gains entries.
		std::shared_ptr<const std::vector<ShadowSlotEntry>> slotLayouts;
		// The views the recording draws, by index into views: a shadow epoch's variant for a set of placements the engine used and of
		// occlusion maps drawn (the others' draws, counted zero, would still cost their indirect draws' fetches, and an unused map its
		// target's load and store). All of them in the first variant, which the commit writes the latch against, and every variant shares.
		std::uint64_t activeViews = ~0ull;

		bool SameShape(const ShadowFrame& o) const
		{
			// The version covers the pipelines the buckets bind.
			return latch == o.latch && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
			       SameHandle(indirect.layout, o.indirect.layout) && indirect.version == o.indirect.version && views == o.views && activeViews == o.activeViews;
		}
	};

	// IndexPool: Resources/IndexPool.h (self-contained; included by Internal.h before this header).

	/** @brief What a shadow shape indexes by: the units the named versions of the shadow buffers hold (MakeRevision). */
	struct ShadowCapacity
	{
		// Every view slot's sequences in draws (the least slot's).
		std::uint32_t sequenceDraws = 0;
		// Bucket count words per view slot (the least slot's); a pipeline past them waits for the growth (TrimmedRow).
		std::uint32_t bucketCountWords = 0;
		// View slots the view-blocks buffer holds; a layout past them is not drawn.
		std::uint32_t viewSlots = 0;
	};

	/** @brief The shadow latch's first layout (CS_DCLF_TABLE_START=small: Skylighting's map, one view, a few key slots). */
	ShadowLatchLayout InitialShadowLatchLayout();

	/** @brief The shadow views' graph resources: the main path's set per view slot, without targets or an HZB. */
	struct ShadowResources
	{
		std::shared_ptr<org::Buffer> constants;
		Versioned visibility;
		// The views' inputs, geometries and material rows (ShadowMaterialRow) are the installed payload's (PayloadVersions).
		std::shared_ptr<SceneBuffers> scene;
		// Per view slot (occlusion views, then shadow views): sequence and count buffers.
		std::vector<Versioned> sequences;
		std::vector<std::shared_ptr<org::Buffer>> count;
		// Per view slot, one count word per bucket.
		std::vector<Versioned> bucketCounts;
		std::shared_ptr<IndexPool> pool;
		// The view slots' blocks (kShadowViewSlotBytes a row: b0, b12), written by each epoch's commit.
		GrowableRows viewBlocks;
		std::uint64_t constantsAddress = 0;
		// BuildDrawsCS's ShadowViews entry: every view of an epoch in one dispatch (ShadowBuildDrawsPass), dispatched through
		// dispatchSignature.
		std::shared_ptr<const ComputeProgram> buildDraws;
		// The engine's shadow map arrays, imported once each (again when the engine recreates them), a DSV per slice.
		std::array<std::shared_ptr<org::ExternalTextureResource>, kShadowDepthTargets> depth;
		std::array<ID3D11Texture2D*, kShadowDepthTargets> depthTexture{};
		std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};
		// The latch blocks are the revision's set's (NamedVersions::Latch). Targets the latched-copies pass declared:
		std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;  // LatchedTarget::key
		rhi::CommandSignaturePtr dispatchSignature;
		// This frame's engine views, for the culling readback's report.
		struct ViewLabel
		{
			std::uint32_t viewId = 0, renderMode = 0, slot = 0;
		};
		std::vector<ViewLabel> labels;
	};

	/** @brief The revision's shadow shape (its variant) a pass's preparation is for; null without one. */
	inline std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const org::PassPrepareContext& a_preparation)
	{
		if (const auto* shapes = RevisionShapesOf(a_preparation))
			return shapes->shadow;
		return nullptr;
	}

	/**
	 * @brief Whether the culling dispatch builds a_frame's view a_index, a row of ShadowBuildDrawsPass's views' table: a view of a shadow
	 * mode that the variant draws (ShadowFrame::activeViews; an unused placement's latch is zero). The pass also drops a view whose slot
	 * its bindings lack, so its rows are never more than ShadowBuildRows.
	 */
	inline bool ShadowBuildRow(const ShadowFrame& a_frame, std::size_t a_index)
	{
		return a_frame.views[a_index].modeIndex < kShadowModeCount && (a_index >= 64 || ((a_frame.activeViews >> a_index) & 1));
	}

	/** @brief The culling dispatch's y (one group row per view): a_frame's ShadowBuildRow views, which the commit writes with the groups. */
	inline std::uint32_t ShadowBuildRows(const ShadowFrame& a_frame)
	{
		std::uint32_t rows = 0;
		for (std::size_t i = 0; i < a_frame.views.size(); ++i)
			rows += ShadowBuildRow(a_frame, i) ? 1u : 0u;
		return rows;
	}
}
