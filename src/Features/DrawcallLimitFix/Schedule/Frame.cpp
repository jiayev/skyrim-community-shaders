// The frame's schedule: the engine points DCLF's epochs are submitted at, every epoch DCLF adds passes to, and its passes in the order
// the graph adds them.
//
// How to read it:
// - A trigger (Api::TriggerDef) is an engine point an epoch is submitted at: the hook that owns it marks it active on the render thread
//   while its DCLF work runs (Core's FrameDriver points; a feature's through InstallContext::FindTrigger and Extensions::TriggerScope).
//   Every epoch names its trigger; every trigger is named. An epoch submitted outside its trigger is counted and logged once.
// - An epoch is a RenderGraphRuntime::Segment and the shape its passes are made from: Main (the main extension's resources), Shadow (the
//   shadow extension's) or View (a view's extension, for the Api::EngineView it names). ORG orders the epochs; within one, its passes
//   run in this order.
// - An epoch's row is its definition (Api::EpochDef): its trigger, its index in its shape, the epoch whose shape it shares, the
//   class gating it and whether the frame's claims need it. The rows' order is the scene revisions' slot order and the reports'; the frame's order is
//   kFrameOrder, the host's async order kAsyncOrder (the revision-driven epochs are those with revision; the foreign ones never are).
// - A pass is a key (Api::PassDef), registered at install by its owner, whom the key's prefix names: core.* (the main view's culling and
//   copies), view.* (Core's, through Core/Render/Passes/CorePasses.cpp), or a feature's (Features/<Name>/): main-view.* (the main view's
//   draws: Features/MainView), shadows.* (the shadow and occlusion epochs': Features/Shadows/Engine), forward.*, tree-wind.*, fades.*,
//   texture-transforms.*, tree-lod.*, gpu-occlusion.* (the depth pyramid's producer).
// - A pass's own conditions decide whether it is added (no sort, no forward range, no HZB, no state from its setup: the pass is skipped,
//   not the key).
// - Anchors exist only for diagnostics (diag.*): each adds the diagnostics' observers' passes at that point, any number of them.
// A key nobody registered fails the install (Extensions::InstallFeatures).

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Schedule/Schedule.h"

#include "RenderGraph/RenderGraphRuntime.h"

namespace DCLF::Schedule
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		constexpr std::uint32_t SegmentOf(Segment a_segment) { return static_cast<std::uint32_t>(a_segment); }
		constexpr std::uint32_t ViewOf(Api::EngineView a_view) { return static_cast<std::uint32_t>(a_view); }
		constexpr std::uint32_t MainOf(Api::MainShape a_shape) { return static_cast<std::uint32_t>(a_shape); }

		// The engine points the epochs are submitted at, in the order a frame reaches them (kFrameOrder's).
		constexpr Api::TriggerDef kTriggers[] = {
			{ .id = "shadow-maps.after", .owner = "Features/Shadows/Engine", .where = "Main::RenderShadowMaps returned, the shadow views drawn (Deferred's Main_RenderShadowMaps hook)" },
			{ .id = "depth.committed", .owner = "Core", .where = "Main::RenderDepth's world draw done (the shell's Main_RenderDepth_WorldDrawn hook), the views' work there with it" },
			{ .id = "fp.depth-draw", .owner = "Features/FirstPerson", .where = "RenderDepth's first-person draw (the accumulator render's call there, FirstPersonHooks.cpp)" },
			{ .id = "occlusion.draw", .owner = "Features/Shadows/Engine", .where = "Skylighting's occlusion mask rendered (its DrawOcclusion call, Skylighting.cpp)" },
			{ .id = "opaque.after", .owner = "Features/MainView", .where = "the main pass's opaque batches done (Deferred's Main_RenderWorld_BlendedDecals hook)" },
			{ .id = "blended.draw", .owner = "Features/Forward", .where = "the main pass's blended draw for the main accumulator (both its call sites, ForwardHooks.cpp)" },
			{ .id = "fp.colour-render", .owner = "Features/FirstPerson", .where = "Main::RenderFirstPersonView's accumulator render (FirstPersonHooks.cpp)" },
		};

		// The depth (Z-prepass) epoch: the latched copies; the texture transforms (the latch's stage word, before anything of the frame
		// draws); the member portals' queries (on the graphics queue: phase 1's build reads their results at once, so on the compute queue
		// they were a round trip with nothing to overlap); phase 1 (build, index pool, tree LOD's cull, depth draw); the next frame's state
		// on the compute queue beside phase 1's depth draw and the two-phase tail (tree wind, then the fade roots' portal batch and states:
		// nothing of this frame reads them); the two-phase tail (only with the HZB: the pyramid, the retest build, tree LOD's retest, the
		// rescues' depth draw); then what reads both phases (tree LOD's counts, the visibility events) and the fade states' write-back copy,
		// last: ORG starts a graphics batch at the visibility events, the first pass that needs the compute queue's state, so the graphics
		// queue waits for it only there.
		constexpr std::string_view kMainDepth[] = {
			"core.z.latched-copies",
			"texture-transforms.apply",
			"core.z.portal-queries",
			"portals.member-batch",
			"portals.member-evaluate",
			"core.z.build-draws",
			"core.z.index-pool",
			"tree-lod.cull",
			"tree-wind.next-frame",
			"fades.portal-queries",
			"fades.portal-batch",
			"fades.portal-evaluate",
			"fades.state",
			"main-view.z.depth",
			"gpu-occlusion.hzb",
			"core.build-draws-phase2",
			"tree-lod.cull-phase2",
			"main-view.depth-phase2",
			"tree-lod.counts",
			"core.z.visibility-events",
			"core.z.visibility-readback",
			"fades.events",
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

		// In the scene revisions' slot order (an epoch is its row's index); the frame runs them in kFrameOrder. The main epochs decide the
		// frame's claims (the forward epoch's too while its class is on: its members' passes are withheld with the claims, and only it
		// draws them).
		constexpr Api::EpochDef kEpochs[] = {
			{ .id = "main.depth", .label = "Z-prepass", .trigger = "depth.committed", .segment = SegmentOf(Segment::ZPrepass), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Depth), .claims = true, .passes = kMainDepth },
			{ .id = "main.colour", .label = "colour", .trigger = "opaque.after", .segment = SegmentOf(Segment::MainOpaque), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Colour), .claims = true, .passes = kMainColour },
			{ .id = "shadow", .label = "shadow", .trigger = "shadow-maps.after", .segment = SegmentOf(Segment::ShadowView), .shape = Api::EpochShape::Shadow, .shapeIndex = 0, .passes = kShadow },
			{ .id = "occlusion", .label = "occlusion", .trigger = "occlusion.draw", .segment = SegmentOf(Segment::SkyOcclusion), .shape = Api::EpochShape::Shadow, .shapeIndex = 1, .passes = kOcclusion },
			{ .id = "reflection", .label = "reflection", .trigger = "depth.committed", .segment = SegmentOf(Segment::Reflection), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::Reflection), .passes = kReflection },
			{ .id = "forward", .label = "forward", .trigger = "blended.draw", .segment = SegmentOf(Segment::Forward), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Colour), .sharesWith = "main.colour", .gate = Api::ObjectClass::Forward, .claims = true, .passes = kForward },
			{ .id = "fp.depth", .label = "first-person depth", .trigger = "fp.depth-draw", .segment = SegmentOf(Segment::FirstPersonDepth), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::FirstPerson), .passes = kFirstPersonDepth },
			{ .id = "fp.colour", .label = "first-person colour", .trigger = "fp.colour-render", .segment = SegmentOf(Segment::FirstPerson), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::FirstPerson), .sharesWith = "fp.depth", .passes = kFirstPersonColour },
		};

		// Light Limit Fix's light culling: live (its ticket prepared on the host's thread), not a revision's.
		constexpr Api::ForeignEpoch kForeign[] = {
			{ "light-culling", SegmentOf(Segment::LightCulling) },
		};
		// The order a frame runs them: the shadow views at AfterShadowMaps, the Z-prepass at the depth pass, the water reflection's faces
		// after it, first person's depth, Skylighting's map, Light Limit Fix's culling at Prepass, the colour pass where the main pass's
		// opaque batches end, the forward epoch at the engine's blended draw after the deferred composite, first person's colour.
		constexpr std::string_view kFrameOrder[] = {
			"shadow",
			"main.depth",
			"reflection",
			"fp.depth",
			"occlusion",
			"light-culling",
			"main.colour",
			"forward",
			"fp.colour",
		};
		// The host's async epochs, in its ticket loop's order; the revision-driven ones are those with revision.
		constexpr std::string_view kAsyncOrder[] = {
			"reflection",
			"shadow",
			"main.depth",
			"occlusion",
			"light-culling",
			"main.colour",
			"forward",
			"fp.depth",
			"fp.colour",
		};
	}

	const Api::FrameSchedule& Frame()
	{
		static constexpr Api::FrameSchedule kFrame{ kTriggers, kEpochs, kForeign, kFrameOrder, kAsyncOrder };
		return kFrame;
	}
}
