#pragma once

// IndirectDraws::Impl's view state (ImplViews, a base of Impl): each view slot's frame state (ViewEpochState, Epochs/ViewEpoch.h, which
// the view engine's calls hand the view features: IndirectDraws::ViewState).
// Part of IndirectDraws' implementation: included by Impl.h, before the struct.

namespace DCLF
{
	// The views' frame state (ViewEpoch.cpp, the view engine): each view slot's, and their revision pipelines.
	struct ImplViews
	{
		// By view slot (the reflection's faces: the engine's cube target's format with DCLF's D24S8 depth; the first person's: kMAIN's
		// format, the motion vectors' (colour2) and kPOST_ZPREPASS_COPY's).
		std::array<ViewEpochState, kViews> viewStates;
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
		// The view epochs a main epoch hosts (Api::EpochDef::hostedBy: the depth epoch, the reflection's faces), committed into the host's
		// commit (render thread). hostedWork: the views' work the depth commit runs inside its epoch's execution, before its recording is
		// submitted (IndirectDraws::CaptureDepthPass's: FrameDriver's ViewPoint::DepthCommitted work), set around its RunEpoch.
		// hostedCommit: the host's commit while it runs that work - its epoch, the owners its execution holds, its payload (held already),
		// and the hosted epochs committed into it (a bit each), which pick its recording with their passes' work (Impl::CommitHostedView).
		const std::function<void()>* hostedWork = nullptr;
		struct HostedCommit
		{
			std::uint32_t host = kNoEpoch;
			std::vector<std::shared_ptr<const void>>* owners = nullptr;
			const MainPayload* payload = nullptr;
			std::uint32_t drawn = 0;
		};
		HostedCommit hostedCommit;
		// Revision code, per view: each pipeline slot's forward pipeline from its request's catalog, the trees view's draw families'
		// pipelines (by family), the forward part's forward slots.
		std::array<std::vector<rhi::PipelineHandle>, kViews> viewRevisionPipelines;
		std::array<rhi::PipelineHandle, Extensions::kDrawFamilies> viewRevisionFaces{};
		std::vector<std::uint8_t> viewForwardSlots;
	};
}
