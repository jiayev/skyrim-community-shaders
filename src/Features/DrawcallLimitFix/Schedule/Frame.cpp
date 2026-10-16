// The frame's schedule: every epoch DCLF adds passes to, and its passes in the order the graph adds them.
//
// How to read it:
// - An epoch is a RenderGraphRuntime::Segment and the shape its passes are made from: Main (the main extension's resources), Shadow (the
//   shadow extension's) or View (a view's extension, for the Api::EngineView it names). ORG orders the epochs; within one, its passes
//   run in this order.
// - A pass is a key (Api::PassDef), registered at install by its owner, whom the key's prefix names: core.* (the main view's culling and
//   copies), main-view.* (its draws), forward.*, shadows.*, view.* (Core's, through Core/Render/Passes/CorePasses.cpp), or a feature's
//   (Features/<Name>/): tree-wind.*, fades.*, texture-transforms.*, tree-lod.*, gpu-occlusion.* (the depth pyramid's producer).
// - A pass's own conditions decide whether it is added (no sort, no forward range, no HZB, no state from its setup: the pass is skipped,
//   not the key).
// - Anchors exist only for diagnostics (diag.*): each adds the diagnostics' observers' passes at that point, any number of them.
// A key nobody registered fails the install (Extensions::InstallFeatures).

#include "Features/DrawcallLimitFix/Schedule/Schedule.h"
#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include "RenderGraph/RenderGraphRuntime.h"

namespace DCLF::Schedule
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		constexpr std::uint32_t SegmentOf(Segment a_segment) { return static_cast<std::uint32_t>(a_segment); }
		constexpr std::uint32_t ViewOf(Api::EngineView a_view) { return static_cast<std::uint32_t>(a_view); }

		// The depth (Z-prepass) epoch: the latched copies; the next frame's state (tree wind, on the compute queue beside the epoch's raster
		// work, which nothing of this frame reads) and cull state (the fade roots' states, then their write-back copy); the texture
		// transforms (the latch's stage word, before anything of the frame draws); phase 1 (build, index pool, tree LOD's cull, depth draw);
		// the two-phase tail (only with the HZB: the pyramid, the retest build, tree LOD's retest, the rescues' depth draw); then what reads
		// both phases (tree LOD's counts, the visibility events).
		constexpr std::string_view kMainDepth[] = {
			"core.z.latched-copies",
			"tree-wind.next-frame",
			"fades.state",
			"fades.events",
			"texture-transforms.apply",
			"core.z.build-draws",
			"core.z.index-pool",
			"tree-lod.cull",
			"main-view.z.depth",
			"gpu-occlusion.hzb",
			"core.build-draws-phase2",
			"tree-lod.cull-phase2",
			"main-view.depth-phase2",
			"tree-lod.counts",
			"core.z.visibility-events",
			"core.z.visibility-readback",
			"diag.after-depth",
		};
		// The colour epoch: the build, the sort by pipeline, the forward range's sort, the colour draws.
		constexpr std::string_view kMainColour[] = {
			"core.latched-copies",
			"core.build-draws",
			"core.sort-scan",
			"core.sort-scatter",
			"core.forward-sort",
			"diag.before-colour",
			"main-view.colour",
			"diag.after-colour",
		};
		// The forward epoch: the forward range's blended draws, then the refraction normals.
		constexpr std::string_view kForward[] = {
			"forward.draw",
			"forward.refraction",
		};
		constexpr std::string_view kShadow[] = {
			"shadows.latched-copies",
			"shadows.index-pool",
			"shadows.build-draws",
			"shadows.view",
			"shadows.cull-counters",
		};
		// Skylighting's occlusion map: the shadow epoch's passes in its own epoch after RenderMask.
		constexpr std::string_view kOcclusion[] = {
			"shadows.sky.latched-copies",
			"shadows.sky.build-draws",
			"shadows.sky.view",
		};
		// The reflection's faces: one epoch, the faces drawn with their draw families (tree LOD's) culled per face.
		constexpr std::string_view kReflection[] = {
			"view.latched-copies",
			"view.build-draws",
			"tree-lod.faces-cull",
			"view.faces",
		};
		// First person: the depth epoch (and the forward range's sort), then the colour epoch.
		constexpr std::string_view kFirstPersonDepth[] = {
			"view.latched-copies",
			"view.build-draws",
			"tree-lod.faces-cull",
			"view.depth",
			"view.forward-sort",
		};
		constexpr std::string_view kFirstPersonColour[] = {
			"view.colour",
			"view.forward",
		};

		constexpr Api::EpochDef kEpochs[] = {
			{ "main.depth", SegmentOf(Segment::ZPrepass), Api::EpochShape::Main, 0, kMainDepth },
			{ "main.colour", SegmentOf(Segment::MainOpaque), Api::EpochShape::Main, 0, kMainColour },
			{ "forward", SegmentOf(Segment::Forward), Api::EpochShape::Main, 0, kForward },
			{ "shadow", SegmentOf(Segment::ShadowView), Api::EpochShape::Shadow, 0, kShadow },
			{ "occlusion", SegmentOf(Segment::SkyOcclusion), Api::EpochShape::Shadow, 0, kOcclusion },
			{ "reflection", SegmentOf(Segment::Reflection), Api::EpochShape::View, ViewOf(Api::EngineView::Reflection), kReflection },
			{ "fp.depth", SegmentOf(Segment::FirstPersonDepth), Api::EpochShape::View, ViewOf(Api::EngineView::FirstPerson), kFirstPersonDepth },
			{ "fp.colour", SegmentOf(Segment::FirstPerson), Api::EpochShape::View, ViewOf(Api::EngineView::FirstPerson), kFirstPersonColour },
		};
	}

	const Api::FrameSchedule& Frame()
	{
		static constexpr Api::FrameSchedule kFrame{ kEpochs };
		return kFrame;
	}
}
