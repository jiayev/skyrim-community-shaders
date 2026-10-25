#pragma once

// The CPU-driven pipeline's view sites (Sites.h, SiteFrame.h): the views drawn by epochs of their own in the GPU path (ViewEpoch.h), drawn
// here with direct draws at the same triggers.
// - The depth site's hosted views (depth.committed): the reflection's faces and first person's depth. Their commits run inside the depth
//   commit, before its site opens (IndirectDraws::SubmitViewEpoch's CPU branch: CommitHostedView as in the GPU path, which latches them,
//   then HostedViewCommitted); the depth site takes them when it opens (AttachHostedViews: their shapes into its pre-work's, so the main
//   depth's latched copies copy theirs and tree LOD's faces cull runs; their views into its run, culled with its own) and draws them after
//   main depth and the families (RecordHostedDepthViews), each into its own target as the GPU path's view passes do.
// - The first-person colour site (fp.colour-render, Site::FirstPerson): its epoch's commit (FirstPersonColourReached, in
//   Direct::ExecuteEpoch) draws what this frame's first-person depth culled, with each bucket's first-person forward pipeline into kMAIN
//   and the motion vectors against kPOST_ZPREPASS_COPY, then its forward part sorted back to front with the main set's.
//
// A view's recording reads only its shape (its pipelines, its push words: addresses of its latched copies' targets), the batch set and
// the frame's index pool, so it could be recorded once per snapshot.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"

#include <rhi.h>

#include <cstdint>
#include <memory>
#include <span>

namespace DCLF::Draws
{
	struct RevisionShapes;
	struct ViewFrame;
	struct ViewResources;
}

namespace DCLF::Direct
{
	/** @brief A face's draw family draw (a trees view's: the reflection's tree LOD), as the GPU path's faces' draw records it. */
	struct HostedFaceDraw
	{
		std::uint32_t family = 0;  // Api::DrawFamily
		std::uint32_t face = 0;
		std::uint64_t row = 0;          // the face's row (ViewResources::faceRowsAddress)
		rhi::ResourceHandle list{};     // the face's list, the adopted set's version (its cull is the site's pre-work)
		rhi::PipelineHandle pipeline{};  // the shape's (ViewFrame::faces)
		rhi::CommandSignatureHandle signature{};
	};

	/** @brief A hosted view's commit (CommitHostedView, inside the depth commit): what its site draws from. */
	struct HostedViewCommit
	{
		ViewType type = ViewType::Reflection;  // Reflection or FirstPerson
		std::shared_ptr<const Draws::ViewFrame> shape;  // the revision's shape it committed against
		std::shared_ptr<Draws::ViewResources> resources;
		std::span<const CullView> faces;  // per drawn face, from the latch its commit wrote (viewIndex: the face)
		std::span<const HostedFaceDraw> families;
		std::shared_ptr<const void> keep;  // what the families' handles are of (their lists' versions)
	};

	/**
	 * @brief Render thread, in a hosted view's commit inside the depth commit (CPU mode): the view posted for the depth site, which takes it
	 * when it opens (AttachHostedViews). Nothing without a frame slot or a batch set.
	 */
	void HostedViewCommitted(const HostedViewCommit& a_commit);

	/**
	 * @brief Render thread, inside the first-person colour epoch's body (Direct::ExecuteEpoch): the first-person colour site opened, from
	 * this frame's first-person depth (its culled batches); its callback queued after the body. False: no first-person depth this frame.
	 */
	bool FirstPersonColourReached();
}

namespace DCLF::Direct::detail
{
	struct FrameSlot;
	struct SiteRun;

	/**
	 * @brief Render thread, in DepthCommitted after NewRun and before its pre-work and culls: the hosted views this depth commit committed
	 * (HostedViewCommitted) attached to its run: their shapes into a_shapes (RevisionShapes::views: the pre-work's latched copies and
	 * faces' cull of each), a view of their list per face in a_depth (dispatched with its own by DepthCommitted's OnCullFrame), their
	 * targets and bindings in a_depth.viewState.
	 */
	void AttachHostedViews(FrameSlot& a_slot, SiteRun& a_depth, Draws::RevisionShapes& a_shapes);
}
