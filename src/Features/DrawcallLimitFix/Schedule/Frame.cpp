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
// - A hosted epoch (hostedBy: the reflection's and first person's depth, in main.depth) is a shape and its passes without an epoch of its
//   own: its keys are in its host's list, prefixed with its id ("reflection/view.faces"), made from its own shape's resources; the host's
//   recordings carry them (each hosted epoch with a shape a dimension of them: RevisionRecordings::hosted) and its commit commits it. It
//   is in neither order. Another epoch may share its shape (fp.colour, fp.depth's).
// - A pass is a key (Api::PassDef), registered at install by its owner, whom the key's prefix names: core.* (the main view's culling and
//   copies), view.* (Core's, through Core/Render/Passes/CorePasses.cpp), or a feature's (Features/<Name>/): main-view.* (the main view's
//   draws: Features/MainView), shadows.* (the shadow epoch's, the occlusion maps' with it: Features/Shadows/Engine), forward.*,
//   tree-wind.*, fades.*, texture-transforms.*, tree-lod.*, grass.*, gpu-occlusion.* (the depth pyramid's producer).
// - A pass's own conditions decide whether it is added (no sort, no forward range, no HZB, no state from its setup, the graph's
//   conditions: an interior leaves grass and tree LOD out, a cull mode without occlusion the phase 2 block; the pass is skipped, not the key).
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
			{ .id = "shadow-maps.after", .owner = "Features/Shadows/Engine", .where = "Main::RenderShadowMaps returned, the shadow views drawn and the occlusion maps set up before it (Deferred's Main_RenderShadowMaps hook)" },
			{ .id = "depth.committed", .owner = "Core", .where = "Main::RenderDepth's world draw done (the shell's Main_RenderDepth_WorldDrawn hook), the views' work there with it" },
			{ .id = "opaque.after", .owner = "Features/MainView", .where = "the main pass's opaque batches done (Deferred's Main_RenderWorld_BlendedDecals hook)" },
			{ .id = "blended.draw", .owner = "Features/Forward", .where = "the main pass's blended draw for the main accumulator (both its call sites, ForwardHooks.cpp)" },
			{ .id = "fp.colour-render", .owner = "Features/FirstPerson", .where = "Main::RenderFirstPersonView's accumulator render (FirstPersonHooks.cpp)" },
		};

		// The depth (Z-prepass) epoch: the latched copies (the reflection's faces' too); the texture transforms (the latch's stage word,
		// before anything of the frame draws); the member portals' queries (on the graphics queue: phase 1's build reads their results at
		// once, so on the compute queue they were a round trip with nothing to overlap); phase 1 (build, index pool, tree LOD's and grass's
		// culls, grass's draw arguments, depth draw); the next frame's state on the compute queue beside phase 1's depth draw and the
		// two-phase tail (tree wind, then the fade roots' portal batch and states: nothing of this frame reads them); the reflection's faces'
		// cull (its build and tree LOD's per face) once phase 1's depth is drawn, beside the depth pyramid (both read what phase 1 left;
		// neither holds the depth draw back); the two-phase tail (only with the HZB: the pyramid, the retest build, tree LOD's and grass's
		// retests, the rescues' depth draw; then grass's colour arguments, both phases' counted); first person's cull once both phases'
		// verdicts are published (the frame's, as its own epoch at RenderDepth's first-person draw read them); the faces' draws (the
		// reflection's cube, from this commit's index pool) after the main view's depth, between first person's cull and its depth draw,
		// which they keep apart; first person's depth into the main depth after the HZB and both phases' depth (the HZB leaves the arms out;
		// nothing of the world draws depth between this commit and RenderDepth's first-person draw, and the depth epoch is submitted before
		// the engine takes kPOST_ZPREPASS_COPY at the pass's end, which the first-person colour epoch tests against) and its forward range's
		// sort; then what reads both phases (tree LOD's and grass's counts, the visibility events) and the fade states' write-back copy,
		// last: ORG starts a graphics batch at the visibility events, the first pass that needs the compute queue's state, so the graphics
		// queue waits for it only there. The reflection's and first person's passes are their hosted epochs' (below): a frame without faces,
		// or not in first person, submits the recording without that view's work.
		constexpr std::string_view kMainDepth[] = {
			"core.z.latched-copies",
			"texture-transforms.apply",
			"core.z.portal-queries",
			"portals.member-batch",
			"portals.member-evaluate",
			"core.z.build-draws",
			"core.z.index-pool",
			"tree-lod.cull",
			"grass.cull",
			"grass.args",
			"tree-wind.next-frame",
			"fades.portal-queries",
			"fades.portal-batch",
			"fades.portal-evaluate",
			"fades.state",
			"main-view.z.depth",
			"reflection/view.build-draws",
			"reflection/tree-lod.faces-cull",
			"gpu-occlusion.hzb",
			"core.build-draws-phase2",
			"tree-lod.cull-phase2",
			"grass.cull-phase2",
			"grass.args-phase2",
			"main-view.depth-phase2",
			"grass.args-colour",
			"fp.depth/view.build-draws",
			"fp.depth/tree-lod.faces-cull",
			"reflection/view.faces",
			"fp.depth/view.depth",
			"fp.depth/view.forward-sort",
			"tree-lod.counts",
			"grass.counts",
			"core.z.visibility-events",
			"core.z.visibility-readback",
			"fades.events",
			"diag.after-depth",
		};
		// The colour epoch: the local shadow masks, the build, the sort by pipeline, the forward range's sort, the colour draws.
		constexpr std::string_view kMainColour[] = {
			"core.latched-copies",
			"core.local-shadow-masks",
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
		// The shadow epoch: every shadow view's and the occlusion maps' (Skylighting's sky map, the precipitation mask) latched copies,
		// culling (one dispatch: the maps' views are rows of the views' table) and draws. The maps are drawn here, not at the engine's
		// occlusion point after RenderDepth: Skylighting sets their views up and clears them before the engine's shadow maps (Deferred's
		// Main_RenderShadowMaps hook), the occluders are this epoch's payload, and nothing reads a map before the Prepass. One epoch less a
		// frame where the maps draw (exteriors), its entry and exit barriers, latched copies and culling dispatch with it.
		constexpr std::string_view kShadow[] = {
			"shadows.latched-copies",
			"shadows.index-pool",
			"shadows.build-draws",
			"shadows.view",
			"shadows.cull-counters",
		};
		// First person's colour epoch: the colour draws, then the forward range (its depth epoch's passes, the cull, the depth draw and the
		// forward range's sort, are the depth epoch's: kMainDepth).
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
			// The reflection's faces, hosted by the depth epoch (its passes in kMainDepth: the faces' cull, tree LOD's per face, the faces'
			// draws; its latched copies the depth epoch's): one epoch less a frame, its entry and exit barriers and latched copies with it.
			{ .id = "reflection", .label = "reflection", .trigger = "depth.committed", .segment = SegmentOf(Segment::ZPrepass), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::Reflection), .hostedBy = "main.depth" },
			{ .id = "forward", .label = "forward", .trigger = "blended.draw", .segment = SegmentOf(Segment::Forward), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Colour), .sharesWith = "main.colour", .gate = Api::ObjectClass::Forward, .claims = true, .passes = kForward },
			// First person's depth, hosted by the depth epoch (its passes in kMainDepth: the cull, the depth draw, the forward range's sort;
			// its latched copies the depth epoch's), committed there when its inputs are (the first-person camera is set in Main::Draw before
			// RenderDepth; its depth range is its shape's): one epoch less a frame in first person, its entry and exit
			// barriers and latched copies with it. Its colour epoch keeps sharing its shape.
			{ .id = "fp.depth", .label = "first-person depth", .trigger = "depth.committed", .segment = SegmentOf(Segment::ZPrepass), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::FirstPerson), .hostedBy = "main.depth" },
			{ .id = "fp.colour", .label = "first-person colour", .trigger = "fp.colour-render", .segment = SegmentOf(Segment::FirstPerson), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::FirstPerson), .sharesWith = "fp.depth", .passes = kFirstPersonColour },
		};

		// Light Limit Fix's light culling: live (its ticket prepared on the host's thread), not a revision's.
		constexpr Api::ForeignEpoch kForeign[] = {
			{ "light-culling", SegmentOf(Segment::LightCulling) },
		};
		// The order a frame runs them: the shadow views and the occlusion maps at AfterShadowMaps, the Z-prepass at the depth pass (the
		// water reflection's faces and first person's depth in it), Light Limit Fix's culling at Prepass, the colour pass where the main
		// pass's opaque batches end, the forward epoch at the engine's blended draw after the deferred composite, first person's colour. A
		// hosted epoch is in neither order.
		constexpr std::string_view kFrameOrder[] = {
			"shadow",
			"main.depth",
			"light-culling",
			"main.colour",
			"forward",
			"fp.colour",
		};
		// The host's async epochs, in its ticket loop's order; the revision-driven ones are those with revision.
		constexpr std::string_view kAsyncOrder[] = {
			"shadow",
			"main.depth",
			"light-culling",
			"main.colour",
			"forward",
			"fp.colour",
		};
	}

	const Api::FrameSchedule& Frame()
	{
		static constexpr Api::FrameSchedule kFrame{ kTriggers, kEpochs, kForeign, kFrameOrder, kAsyncOrder };
		return kFrame;
	}
}
