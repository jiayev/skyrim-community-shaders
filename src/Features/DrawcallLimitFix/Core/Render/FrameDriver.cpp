// FrameDriver: the render thread's DCLF work at each engine point of the frame (see the header).

#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Scene.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PrimaryView.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPoints.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		}
	}

	FrameDriver& FrameDriver::Get()
	{
		static FrameDriver driver;
		return driver;
	}

	bool FrameDriver::BeginFrame(bool a_running, bool a_installed)
	{
		Diagnostics::Notify(Diagnostics::FrameBegin{ a_running });
		// The graph's build point, every frame (loaded or not): an extension added or removed during the last frame is built here, before
		// anything decides what the frame's recordings cover.
		IndirectDraws::Get().BuildPoint();
		auto& scene = Scene::Coordinator::Get();
		auto& view = Scene::FrameView::Get();
		// The scene work on the render thread (Policy::ScenePassInline): the pump's wakes are dropped then, and the frame's start runs the
		// pass itself (Coordinator::RunScenePassInline).
		const bool inlinePass = Policy::ScenePassInline();
		if (!a_running) {
			// The passes apply the events alone while DCLF does not run (the tracked set stays current), and the switch catch-ups they ask
			// for are still served (engine writes), as tree LOD's mirror is drained.
			if (a_installed) {
				scene.SetScenePassMode(false, inlinePass);
				if (!view.NoteLoadingScreen())
					view.ServeFrameRequests(false);
			}
			// No frame values: nothing this frame submits waits for them.
			FrameValues::Get().Skip();
			return false;
		}
		// The engine's update is done: workers may read its scene graph until Present (EngineReadWindow).
		EngineReadWindow::Open();
		// The coordinator's pump runs the scene passes whenever its producers wake it. The frame's posts below (the frame inputs, the ahead
		// context, the captures, the root states' task's) wake it once, when this scope ends.
		ScenePassWakeBatch wakes;
		scene.SetScenePassMode(true, inlinePass);
		// The requested toggles applied, before the frame inputs carry them to the coordinator, which commits its next publication under
		// them; the frames keep the installed publication's (Toggles::Adopt at the hand-over) until one committed under them is installed,
		// drawing and covering as it says meanwhile (a toggle withdraws nothing). A change that enters the classification drops the cached
		// verdicts (the coordinator drops them at its next pass, told by the frame inputs); so does a classify input's (Api/ClassifyInputs.h:
		// a CS feature switched on or off that holds objects back, moving them between their held groups and the native loop).
		static Extensions::ClassifyInputSet inputsSeen = Extensions::SampleClassifyInputs();
		const auto inputs = Extensions::SampleClassifyInputs();
		const bool togglesClassify = Toggles::Get().BeginFrame();
		if (togglesClassify || inputs != std::exchange(inputsSeen, inputs))
			view.RequestVerdictsInvalidated();
		ScopedPerfEvent event("CS DCLF: scene tables");
		// The frame number, the frame's globals captured and the frame inputs posted (an engine capture: the budget's scene captures).
		{
			RenderThreadBudget::Part captures(RenderThreadBudget::Bucket::SceneCaptures);
			view.BeginFrame();
		}
		// The frame's camera duties from the main camera, under the frame's number: the camera cut, then the root states' frame inputs
		// (Features/Fades: the fade test's eye and tree height test), with the primary cull installed.
		PrimaryCull::Get().BeginFrameCamera();
		if (PrimaryCull::Get().Installed())
			Extensions::RootStatesFrameStart();
		// The palette rows classes' frame start (Features/Particles: the main camera the frame's particle quads face, which FrameValues'
		// producer expands them for, and the renderer's quads once).
		Extensions::PaletteRowsFrameStart();
		auto& draws = IndirectDraws::Get();
		// A load screen's frames are claims-less whatever their snapshot (NoteLoadingScreen): neither waited for nor required covered.
		const bool loading = Scene::FrameView::IsLoadingScreenUp();
		{
			// The newest complete snapshot the builder posted, adopted whole - its revision, its draws and its publication (installed here
			// with the log walked up to it, its toggles the frame's). Once DCLF draws, a frame whose newest is stale or none for the graph
			// as built (a graph build, new main resources, targets or shadow format) waits here for the builder's next current one: a
			// one-off stall on a rare event, never a frame handed back to the engine. Everything the builder needs is posted by now (the
			// build point's revision inputs, the frame inputs above; AdoptCurrentSnapshot says why nothing later is).
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			view.HandOverAtFrameStart(draws.AdoptCurrentSnapshot(!view.NotDrawingYet() && !loading));
			view.SyncFrameTables();
		}
		// The views' frame start before the coverage that decides whether their members are withheld (Features/FirstPerson's: the view's
		// latch, targets, pipelines and resources).
		RunViewWork(Api::ViewPoint::FrameStartBeforeCoverage);
		// The candidate entries changed since the sun exclusion's snapshot leave it (the engine culls them); the rest stand (the exclusion
		// contribution's frameStart: Features/Shadows/Local's SunExclusionSlot::BeginFrame).
		Extensions::ShadowExclusionFrameStart();
		// The lookups are the installed publication's; what only the frame has goes to the scene lane (PostLookupInputs).
		Diagnostics::Notify<Diagnostics::FramePoint::LookupsInstalled>();
		draws.PostLookupInputs();
		{
			// The claims are installed on every covered frame. Before the first (NotDrawingYet: tables, resources and the first revision
			// being made) a frame has none, and the engine draws everything; after it, DCLF never hands drawing back: the frame waited for
			// a snapshot it can draw from (AdoptCurrentSnapshot), so one it does not cover is a defect, reported with the coverage state
			// (a load screen's frames aside: their claims are none anyway). A toggle withdraws nothing: the installed publication's toggles
			// are the frame's.
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			if (draws.DecideCoverage() && view.HasInstalled())
				view.InstallClaims();
			else if (!view.NotDrawingYet() && !loading)
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: frame {} is not covered by the snapshot it adopted after waiting for a current one, and DCLF "
												 "does not hand drawing back to the engine once it draws: {}",
					view.GetFrame(), draws.CoverageState()));
			// The releases' report (the render thread's: the flip to its release), with the engine side's, once a report window.
			Reports::GateReleases(view.GetFrame());
			// The components' adoption of the installed publications' deltas, before the culls (in the frame whose claims are post-swap):
			// the LOD gates the installed publications flipped, released.
			Extensions::ComponentsAdopt(view.GetFrame(), view.AdoptedClaims(), view.AdoptedDeltas());
			view.ClearAdoptedDeltas();
		}
		Diagnostics::Notify<Diagnostics::FramePoint::ClaimsInstalled>();
		// The CategoryRoot views' members the frame's epochs draw (the first person's): their rows and palettes taken by the frame values'
		// kick (below) on this thread, before Main::Draw moves the skeleton into its root's space.
		bool rootViewsCovered = false;
		for (std::uint32_t i = 0; i < Extensions::Views().rootCount; ++i)
			rootViewsCovered = rootViewsCovered || draws.ViewCovered(Extensions::Views().rootViews[i]);
		FrameValues::Get().SetRootViewClaims(rootViewsCovered ? view.FrameClaims() : nullptr);
		// What the next builds ahead take from the frame.
		draws.PostAheadContext();
		// The root states' task (Features/Fades' write-back of the stood-in roots) and the visibility readbacks: tasks on DCLF's executor,
		// never joined.
		Extensions::RootStatesKick();
		draws.KickVisibilityReadback();
		// What the coordinator's passes asked of the render thread, served, and its captures posted for them (not while a load screen is
		// up): the switch catch-ups (engine writes, before the culls), the category capture, the mirror's capture requests, tree LOD's mirror.
		{
			const auto eventsStart = std::chrono::steady_clock::now();
			if (!view.NoteLoadingScreen())
				view.ServeFrameRequests(true);
			counters.timing.eventsMs += MillisecondsSince(eventsStart);
		}
		// The shadow sources' frame start, in source order (Features/Shadows/Local's point-light culls: this frame's paraboloid exclusion).
		Extensions::ShadowSourcesFrameStart(view.GetFrame());
		PassCapture::Get().RefreshMainRenderers();
		// The draw families' for the frame (Features/TreeLod's), before the main camera's cull registers its passes; the views' work after
		// it (Features/Reflection's: the water reflection's forward pipelines).
		draws.DecideDrawFamilies(view.GetFrame());
		RunViewWork(Api::ViewPoint::FrameStartAfterTreeLod);
		PrimaryCull::Get().PublishListFilter();
		// The frame's values (FrameValues): the placements and the shading the frame draws with, made on the pool from the plans and named
		// slots the installed publications carried, and waited for by the GPU; the shading fields captured here (Integrations/AdvancedSkin's
		// wetness: Skin's cache is the render thread's). The scene payload is not the frame's to send: the installed snapshot carries it,
		// ready (PayloadVersions).
		(void)FrameValues::Get().Kick(view.TakePlacementPlans(), view.TakeShadingItems(), Extensions::CaptureShadingFields(view), view.FrameGlobalsOfFrame());
		// Under the parities the scene pass runs here, inline, so the render thread's observers keep their placement; the normal path
		// never waits for a pass.
		if (inlinePass)
			scene.RunScenePassInline();
		frameRan = true;
		return true;
	}

	void FrameDriver::DepthCommitted()
	{
		// The Z-prepass and the views' work here (the reflection's faces, which the depth epoch hosts) are submitted under depth.committed.
		Extensions::TriggerScope trigger(Draws::CoreTriggerOf(Draws::CoreTrigger::DepthCommitted));
		// The views' work at the depth commit (Features/Reflection's: the frame's reflection faces, every TESWaterReflections::Update of the
		// frame having run, two a frame, a face each, drawn by DCLF before the water reads the cube; dclf-architecture.md, "Water
		// reflections"), run by the depth commit inside its epoch's execution: the depth epoch hosts the faces' passes (Schedule/Frame.cpp),
		// which follow its index pool copies, so they draw the frame's publication whose scene list they cull (against the frame before's
		// pool, a slot the publication gave another geometry would draw its old indices against its new vertices: a device fault). A frame
		// whose depth epoch did not commit runs it after (its inputs are then stale: nothing drawn, the feature's frame kept).
		bool ran = false;
		const std::function<void()> work = [&] {
			ran = true;
			RunViewWork(Api::ViewPoint::DepthCommitted);
		};
		IndirectDraws::Get().CaptureDepthPass(work);
		if (!ran)
			RunViewWork(Api::ViewPoint::DepthCommitted);
	}

	void FrameDriver::RunViewWork(Api::ViewPoint a_point)
	{
		const auto& frame = Scene::FrameView::Get();
		const Api::ViewPointContext context{ frame.GetFrame(), IndirectDraws::Get().Failed(), &frame.GetTables(), FrameCatalog() };
		const auto point = static_cast<std::size_t>(a_point);
		for (const auto& view : Extensions::Views().views)
			for (const auto run : view.work[point])
				run(context);
	}

	void FrameDriver::EndFrame(bool a_running)
	{
		// The engine's next update follows Present: no worker may be inside its memory from here (EngineReadWindow).
		{
			RenderThreadBudget::Part boundary(RenderThreadBudget::Bucket::EngineBoundary);
			EngineReadWindow::Close();
		}
		const auto start = std::chrono::steady_clock::now();
		PrimaryCull::Get().EndFrame();
		// The growable versions and the payload the frame read: kept from reuse until what it submitted retires.
		IndirectDraws::Get().BindAdopted();
		// What the frame submitted: the GPU point its frame values' buffer is free again after.
		FrameValues::Get().EndFrame();
		FrameData::EndFrame();
		// The frame's reports, once a frame number (a menu or a load holds the number still while Presents go on).
		if (a_running) {
			static std::uint32_t reportedFrame = ~0u;
			if (const std::uint32_t frame = Scene::FrameView::Get().GetFrame(); std::exchange(reportedFrame, frame) != frame)
				Reports::RenderThread(frame);
		}
		// A load screen (in menus too): its marker posted once and the set withdrawn; the coordinator's passes drain the queues meanwhile.
		Scene::FrameView::Get().NoteLoadingScreen();
		// Under the parities the passes run inline at the frame's start alone: with no frame since the last Present (menus, load screens),
		// one applying the events alone runs here, so the queues do not wait for a frame.
		if (const bool ran = std::exchange(frameRan, false); Policy::ScenePassInline() && !ran)
			Scene::Coordinator::Get().ApplyEventsInline();
		// What the scene work let go of: dropped on the engine's main thread with nothing reading them.
		EngineReleases::Release();
		counters.timing.eventsMs += MillisecondsSince(start);
	}

	void FrameDriver::AddShadowViewsTime(std::chrono::steady_clock::time_point a_start)
	{
		shadowViewsMs += MillisecondsSince(a_start);
	}

	void FrameDriver::BeforeShadowCasters()
	{
		const auto start = std::chrono::steady_clock::now();
		shadowViewsMs = 0.0;
		// The frame's shadow views begun (Core's), then their coverage decided (the shadow epochs': Features/Shadows/Engine), before the
		// engine keeps its first light: its registrations, the sun's claims and the census test this frame's coverage.
		Draws::BeginShadowViews();
		if (const auto* epochs = Extensions::ShadowEpochs())
			epochs->beforeShadowCasters();
		AddShadowViewsTime(start);
	}

	void FrameDriver::ShadowCasterKept(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
	{
		const auto start = std::chrono::steady_clock::now();
		Draws::ShadowLightKept(a_light, a_firstSlot, a_slots);
		AddShadowViewsTime(start);
	}

	void FrameDriver::ShadowCastersSelected()
	{
		const auto start = std::chrono::steady_clock::now();
		// The frame's shadow views listed from DCLF's selection (Core's), in the order the engine renders them at the shadow draw.
		Draws::ListShadowViews();
		AddShadowViewsTime(start);
	}

	void FrameDriver::BeforeShadowMaps()
	{
		ScopedPerfEvent event("CS DCLF: shadow views");
		const auto start = std::chrono::steady_clock::now();
		Diagnostics::Notify<Diagnostics::FramePoint::ShadowViewsBegin>();
		// The revision inputs the listed views move (Core's), then the shadow frame begun (the shadow epochs': Features/Shadows/Engine).
		Draws::NoteShadowRevisionInputs();
		if (const auto* epochs = Extensions::ShadowEpochs())
			epochs->beforeShadowMaps();
		// The frame's shadow views' points, whole (BeforeShadowCasters on).
		const double ms = std::exchange(shadowViewsMs, 0.0) + MillisecondsSince(start);
		counters.timing.sceneMs += ms;
		counters.timing.sceneMaxMs = std::max(counters.timing.sceneMaxMs, ms);
	}

	void FrameDriver::AfterShadowMaps()
	{
		// The frame's shadow epoch (Features/Shadows/Engine, under its shadow-maps.after trigger).
		if (const auto* epochs = Extensions::ShadowEpochs())
			epochs->afterShadowMaps();
	}

	void FrameDriver::EarlyPrepass()
	{
		// Once the registration jobs have finished: the frame inputs posted for the next scene pass (Light Limit Fix's room map, the
		// capture's drain), then the captures the last pass asked for, answered for the next. Nothing here reads the coordinator's state.
		ScopedPerfEvent event("CS DCLF: accumulator tables and pipelines");
		const auto start = std::chrono::steady_clock::now();
		auto& view = Scene::FrameView::Get();
		view.PostAccumulateInputs();
		view.PrepareAccumulatePhase();
		const double ms = MillisecondsSince(start);
		counters.timing.buildMs += ms;
		counters.timing.buildMaxMs = std::max(counters.timing.buildMaxMs, ms);
		++counters.timing.frames;
	}

	void FrameDriver::Prepass()
	{
		auto& view = Scene::FrameView::Get();
		// The one point in the frame where `currentAccumulator` is the main camera's accumulator: latched for the frame's readers.
		view.LatchAccumulator();
		// The camera-dependent half of the per-frame constants: the main camera's shadow state is current here.
		view.RefreshFrameConstants();
		Diagnostics::Notify<Diagnostics::FramePoint::PrepassEnd>();
	}

	bool FrameDriver::OcclusionReady(std::uint32_t a_map, bool a_running)
	{
		auto& capture = PassCapture::Get();
		capture.SetOcclusionPhase(0);
		if (!a_running || !Extensions::OcclusionOn(ActiveToggles(), a_map))
			return false;
		// DCLF draws the map's members of the set this frame (none until its phase is drawn: a frame after the first that drew it).
		const auto* epochs = Extensions::ShadowEpochs();
		const bool ready = epochs && epochs->occlusionReady(a_map);
		occlusionFrames.native[a_map] += ready ? 0 : 1;
		occlusionWanted |= ready ? 1u << a_map : 0u;
		const std::uint8_t phase = Extensions::OcclusionMapOf(a_map).phase;
		const auto set = capture.CurrentSet();
		if (ready && set && (set->drawn & phase))
			capture.SetOcclusionPhase(phase);
		return ready;
	}

	bool FrameDriver::OcclusionNeedsEngine(std::uint32_t a_map, bool a_running)
	{
		const std::uint8_t phase = a_map < Extensions::kOcclusionMaps ? Extensions::OcclusionMapOf(a_map).phase : std::uint8_t{ 0 };
		auto& view = Scene::FrameView::Get();
		const auto set = view.FrameClaims();
		// Occluders not yet members of the map's phase (joining) are left out of DCLF's map until they join: the engine's cull is no
		// fallback for them (it would cull the whole scene for a few objects).
		const bool needed = !a_running || !Extensions::OcclusionOn(ActiveToggles(), a_map) || !(occlusionWanted & (1u << a_map)) || !set || !(set->drawn & phase);
		// The engine's cull of the map reads the scene lists: whole again.
		if (needed)
			PrimaryCull::Get().RestoreSceneLists();
		occlusionFrames.engine[a_map] += needed ? 1 : 0;
		occlusionFrames.joining[a_map] += !needed && view.SetLacking(phase) > 0 ? 1 : 0;
		return needed;
	}

	FrameDriver::OcclusionFrames FrameDriver::TakeOcclusionFrames()
	{
		auto frames = std::exchange(occlusionFrames, OcclusionFrames{});
		for (std::uint32_t map = 0; map < Extensions::kOcclusionMaps; ++map)
			frames.lacking[map] = Scene::FrameView::Get().SetLacking(Extensions::OcclusionMapOf(map).phase);
		frames.withheld = PassCapture::Get().GetStats().occlusionWithheld;
		return frames;
	}

	void FrameDriver::DrawOcclusion()
	{
		// Every occlusion map whose Ready said DCLF draws it this frame, in one epoch.
		const std::uint32_t wanted = std::exchange(occlusionWanted, 0u);
		PassCapture::Get().SetOcclusionPhase(0);
		// The maps' epoch (Features/Shadows/Engine, under its occlusion.draw trigger).
		const auto* epochs = Extensions::ShadowEpochs();
		const std::uint32_t drawn = wanted && epochs ? epochs->drawOcclusion(wanted) : 0u;
		Diagnostics::Notify(Diagnostics::OcclusionMapsDrawn{ drawn });
	}

	void FrameDriver::BeforeOpaquePass()
	{
		// The main pass's bindings for this frame's colour epoch, where the opaque batches start. The engine binds the G-buffer (and clears
		// it) only when a draw applies its state, so the state is applied here as the first draw would apply it: a frame whose every
		// lighting draw in view is DCLF's has no native draw to take them from. The end of the pass is no substitute: by then the sky's
		// clouds have rebound Cloud Shadows' t26.
		Engine::ApplyPendingState();
		globals::EndDCLFDepthTrace();
		// The main view's capture (Features/MainView: the targets' formats, the bindings, the engine's states); the frame it was taken in,
		// for the native draws' observers.
		if (const auto* view = Extensions::PrimaryView(); view && view->beforeOpaque())
			captureFrame = Scene::FrameView::Get().GetFrame();
	}

	void FrameDriver::AfterOpaquePass()
	{
		// The main view's colour epoch (Features/MainView, under its opaque.after trigger).
		if (const auto* view = Extensions::PrimaryView())
			view->afterOpaque();
	}

	void FrameDriver::NativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
	{
		auto& view = Scene::FrameView::Get();
		// A ProjectedUV draw has SetupGeometry's four projected textures bound here: kept for the pipelines DCLF builds with that bit (the
		// Hair technique binds none).
		if (globals::deferred->deferredPass && (PassDescriptorOf(a_pass->passEnum) & 0x8000u) && ((PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f) != 6)
			view.NoteProjectedTextures();
		if (Diagnostics::Armed<Diagnostics::NativeLightingDraw>()) {
			const std::uint32_t frame = view.GetFrame();
			Diagnostics::Notify(Diagnostics::NativeLightingDraw{ a_pass, a_renderFlags, frame, globals::deferred->deferredPass, captureFrame == frame });
		}
	}
}
