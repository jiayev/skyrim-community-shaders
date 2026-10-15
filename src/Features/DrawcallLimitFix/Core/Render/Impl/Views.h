#pragma once

// IndirectDraws::Impl's view state (ImplViews, a base of Impl): each view slot's frame state (ViewEpochState, which the view engine's
// calls hand the view features: IndirectDraws::ViewState), and the cube camera's LOD roots' residue watch (Core's while Lod is).
// Part of IndirectDraws' implementation: included by Impl.h, before the struct.

namespace DCLF
{
	/**
	 * @brief A view's frame state (ViewEpoch.cpp and the view engine, ViewEngine.cpp; render thread). The scene lane reads only the
	 * published LaneTargets and forwardCompatible, never `targets`. `covered` is decided at the frame's start, before the engine draws
	 * the view (ImplViews::ViewCovered reads it).
	 */
	struct ViewEpochState
	{
		ForwardTargets targets;
		std::atomic<std::uint64_t> laneColours{ 0 }, laneDepth{ 0 };  // colour in the low 32 bits, colour2 in the high
		void PublishTargets()
		{
			const std::uint64_t colours = std::uint64_t(static_cast<std::uint32_t>(targets.colour)) | (std::uint64_t(static_cast<std::uint32_t>(targets.colour2)) << 32);
			if (laneColours.load(std::memory_order_relaxed) != colours)
				laneColours.store(colours, std::memory_order_release);
			if (laneDepth.load(std::memory_order_relaxed) != static_cast<std::uint64_t>(targets.depth))
				laneDepth.store(static_cast<std::uint64_t>(targets.depth), std::memory_order_release);
		}
		ForwardTargets LaneTargets() const
		{
			const std::uint64_t colours = laneColours.load(std::memory_order_acquire);
			ForwardTargets out;
			out.colour = static_cast<DXGI_FORMAT>(colours & 0xFFFFFFFFu);
			out.colour2 = static_cast<DXGI_FORMAT>(colours >> 32);
			out.depth = static_cast<DXGI_FORMAT>(laneDepth.load(std::memory_order_acquire));
			return out;
		}
		std::vector<rhi::PipelineHandle> slotPipelines;
		std::shared_ptr<ViewResources> resources;
		bool setupFailed = false;
		// Until the first coverage decided it (coverageDecided), the view's coveredInitially stands for it (ImplViews::ViewCovered).
		bool covered = false;
		bool coverageDecided = false;
		// The engine's viewport depth range for the view (a revision input: the shapes' viewport); 0 until the view's work sets one
		// (IndirectDraws::SetViewDepthRange), the shape's (Api::ViewShape::depthRange) until then.
		float depthRange = 0.0f;
		// Whether the main set's forward pipelines can draw into the view's forward part (its depth format the main pass's), published
		// for the slot readiness (IndirectDraws::ViewSlotReady).
		std::atomic<bool> forwardCompatible{ false };
		// A trees view's tree LOD per face (Api::ViewShape::trees, the view engine's: IndirectDraws::ResolveViewTreeLod,
		// DecideViewTreeLod): its forward pipeline for the view's targets, and whether the faces' tree LOD is DCLF's this frame
		// (PassCapture::SetViewTreeLodOwned).
		rhi::PipelineHandle treePipeline{};
		bool treeReady = false;
		bool treeOwned = false;
	};

	// The views' frame state (ViewEpoch.cpp, the view engine): each view slot's, and their revision pipelines.
	struct ImplViews
	{
		// By view slot (the reflection's faces: the engine's cube target's format with DCLF's D24S8 depth; the first person's: kMAIN's
		// format, the motion vectors' (colour2) and kPOST_ZPREPASS_COPY's).
		std::array<ViewEpochState, kViews> viewStates;
		// The cube camera's LOD roots' residue watch: Policy::KeepReflectionRoots' updates (IndirectDraws::ViewLodRootsOwned).
		std::uint32_t reflectionResidueUpdates = 0;
		ViewEpochState& View(ViewKind a_kind) { return viewStates[ViewIndex(a_kind)]; }
		const ViewEpochState& View(ViewKind a_kind) const { return viewStates[ViewIndex(a_kind)]; }
		/** @brief A view's coverage: its last decision's, else its coveredInitially (before the first). */
		bool ViewCovered(ViewKind a_kind) const
		{
			const auto& view = View(a_kind);
			return view.coverageDecided ? view.covered : Extensions::ViewEntryOf(a_kind).coveredInitially;
		}
		void SetViewCovered(ViewKind a_kind, bool a_covered)
		{
			auto& view = View(a_kind);
			view.covered = a_covered;
			view.coverageDecided = true;
		}
		/** @brief A view's depth range: its state's once the view's work set one, else its shape's (Api::ViewShape::depthRange). */
		float DepthRangeOf(ViewKind a_kind) const
		{
			const float range = View(a_kind).depthRange;
			return range > 0.0f ? range : Extensions::ViewEntryOf(a_kind).shape.depthRange;
		}
		// Revision code, per view: each pipeline slot's forward pipeline from its request's catalog, the trees view's tree LOD pipeline,
		// the forward part's forward slots.
		std::array<std::vector<rhi::PipelineHandle>, kViews> viewRevisionPipelines;
		rhi::PipelineHandle viewRevisionTree{};
		std::vector<std::uint8_t> viewForwardSlots;
	};
}
