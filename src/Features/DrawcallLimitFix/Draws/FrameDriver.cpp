// FrameDriver: the render thread's DCLF work at each engine point of the frame (see the header).

#include "Features/DrawcallLimitFix/Draws/FrameDriver.h"

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Draws/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Draws/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Draws/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/Access/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Engine/Capture/ParticleSnapshots.h"
#include "Features/DrawcallLimitFix/Engine/Cull/LocalLightCull.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Engine/Cull/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Engine/Views/ShadowViews.h"
#include "Features/DrawcallLimitFix/Runtime/EngineReleases.h"
#include "Features/DrawcallLimitFix/Runtime/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Runtime/SceneWake.h"
#include "Features/DrawcallLimitFix/Runtime/Toggles.h"
#include "Features/DrawcallLimitFix/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
		}

		TargetFormats CurrentTargetFormats()
		{
			TargetFormats formats;
			ID3D11RenderTargetView* views[8] = {};
			ID3D11DepthStencilView* depth = nullptr;
			globals::d3d::context->OMGetRenderTargets(8, views, &depth);
			for (std::uint32_t i = 0; i < 8; ++i) {
				if (!views[i])
					continue;
				D3D11_RENDER_TARGET_VIEW_DESC desc;
				views[i]->GetDesc(&desc);
				formats.colors[i] = desc.Format;
				formats.colorCount = i + 1;
				views[i]->Release();
			}
			if (depth) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc;
				depth->GetDesc(&desc);
				formats.depth = desc.Format;
				depth->Release();
			}
			return formats;
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
		// context, the captures, the fade write-back's) wake it once, when this scope ends.
		ScenePassWakeBatch wakes;
		scene.SetScenePassMode(true, inlinePass);
		// The frame's toggles, before anything reads them. A change that enters the classification drops the cached verdicts (the
		// coordinator drops them at its next pass, told by the frame inputs); so does Terrain Blending switched on or off, which moves the
		// terrain and its no-blend meshes between their terrain groups and the native loop (TerrainBlendingDefersTerrain).
		static bool terrainDefersSeen = TerrainBlendingDefersTerrain();
		const bool terrainDefers = TerrainBlendingDefersTerrain();
		const bool togglesClassify = Toggles::Get().BeginFrame();
		if (togglesClassify || terrainDefers != std::exchange(terrainDefersSeen, terrainDefers))
			view.RequestVerdictsInvalidated();
		// The claims committed before a toggle are another capability's: the frames are the engine's from here until a publication
		// committed under the new toggles is installed (its commit's toggles generation, which the frame inputs carry to the coordinator).
		toggleGeneration = Toggles::Get().Generation();
		ScopedPerfEvent event("CS DCLF: scene tables");
		// The frame number, the frame's globals captured and the frame inputs posted (an engine capture: the budget's scene captures).
		{
			RenderThreadBudget::Part captures(RenderThreadBudget::Bucket::SceneCaptures);
			view.BeginFrame();
		}
		// The frame's camera duties (FadeEye, the camera cut, the tree height test) from the main camera, under the frame's number.
		PrimaryCull::Get().BeginFrameCamera();
		// The main camera the frame's particle quads face (FrameValues' producer expands them), and the renderer's quads once.
		ParticleSnapshots::Get().BeginFrame();
		auto& draws = IndirectDraws::Get();
		{
			// The newest complete snapshot the builder posted, adopted whole - its revision, its draws and its publication (installed here
			// with the log walked up to it). A stale one is not adopted and the frame is the engine's until the builder's next.
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			view.HandOverAtFrameStart(draws.AdoptSnapshot(toggleGeneration));
			view.SyncFrameTables();
		}
		// The first-person view's latch, targets, pipelines and resources, before the coverage that decides whether its members are withheld.
		draws.BeginFirstPersonFrame();
		// The candidate entries changed since the sun exclusion's snapshot leave it (the engine culls them); the rest stand.
		SunAccumulation::Get().BeginFrame(view.GetSunCandidates());
		// The lookups are the installed publication's; what only the frame has goes to the scene lane (PostLookupInputs).
		Diagnostics::Notify<Diagnostics::FramePoint::LookupsInstalled>();
		draws.PostLookupInputs();
		{
			// A frame the adopted snapshot does not cover, or whose publication was committed under older toggles or before a LOD gate's
			// forced release, has no claims: the engine draws everything, and the next covered frame installs the whole set again.
			RenderThreadBudget::Part exchange(RenderThreadBudget::Bucket::Exchange);
			if (!draws.DecideCoverage() || !view.HasInstalled() || view.InstalledToggles() < toggleGeneration || view.InstalledLodForced() < view.FrameLodForced())
				view.WithdrawSet();
			else
				view.InstallClaims();
			// The LOD gates the installed publications flipped, released before the culls (in the frame whose claims are post-swap).
			view.ReleaseGates();
		}
		Diagnostics::Notify<Diagnostics::FramePoint::ClaimsInstalled>();
		// The first-person members the frame's epochs draw: their rows and palettes taken by the frame values' kick (below) on this thread,
		// before Main::Draw moves the skeleton into its root's space.
		FrameValues::Get().SetFirstPersonClaims(draws.FirstPersonCovered() ? view.FrameClaims() : nullptr);
		// What the next builds ahead take from the frame.
		draws.PostAheadContext();
		// The stood-in fade roots' write-back and the visibility readbacks: tasks on DCLF's executor, never joined.
		draws.KickFadeWriteBack();
		draws.KickVisibilityReadback();
		// What the coordinator's passes asked of the render thread, served, and its captures posted for them (not while a load screen is
		// up): the switch catch-ups (engine writes, before the culls), the category capture, the mirror's capture requests, tree LOD's mirror.
		{
			const auto eventsStart = std::chrono::steady_clock::now();
			if (!view.NoteLoadingScreen())
				view.ServeFrameRequests(true);
			counters.timing.eventsMs += MillisecondsSince(eventsStart);
		}
		LocalLightCull::SelectFrame(view.GetFrame());
		PassCapture::Get().RefreshMainRenderers();
		// Tree LOD's for the frame, before the main camera's cull registers its passes; the water reflection's forward pipelines.
		draws.DecideTreeLod();
		draws.PrepareReflection();
		PrimaryCull::Get().PublishListFilter();
		// The frame's values (FrameValues): the placements and the shading the frame draws with, made on the pool from the plans and named
		// slots the installed publications carried, and waited for by the GPU; the wetness captured here (Skin's cache is the render
		// thread's). The scene payload is not the frame's to send: the installed snapshot carries it, ready (PayloadVersions).
		(void)FrameValues::Get().Kick(view.TakePlacementPlans(), view.TakeShadingItems(), view.CaptureWetness(), view.TakeFadeSeeds(), view.TakeTreeSeeds(),
			view.FrameGlobalsOfFrame());
		// Under the parities the scene pass runs here, inline, so the render thread's observers keep their placement; the normal path
		// never waits for a pass.
		if (inlinePass)
			scene.RunScenePassInline();
		frameRan = true;
		return true;
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

	void FrameDriver::BeforeShadowMaps()
	{
		ScopedPerfEvent event("CS DCLF: shadow views");
		const auto start = std::chrono::steady_clock::now();
		Diagnostics::Notify<Diagnostics::FramePoint::ShadowViewsBegin>();
		// The frame's shadow views, in the order the engine is about to render them. Everything downstream - the capture's attribution,
		// the withholding, the epochs - identifies a view by this list.
		auto& draws = IndirectDraws::Get();
		ShadowViews::Get().SetViewCapacity(ActiveToggles().shadows ? draws.ShadowViewCapacity() : UINT32_MAX);
		ShadowViews::Get().Rebuild();
		// The shadow views' candidates: a scene revision's input (its main latch holds a cascade and a shadow volume each).
		draws.NoteRevisionInputs();
		// The shadow epoch commits the installed publication's shadow payload (built ahead with it).
		if (ActiveToggles().shadows) {
			draws.DecideShadowCoverage();
			draws.BeginShadowFrame();
		}
		const double ms = MillisecondsSince(start);
		counters.timing.sceneMs += ms;
		counters.timing.sceneMaxMs = std::max(counters.timing.sceneMaxMs, ms);
	}

	void FrameDriver::AfterShadowMaps()
	{
		// The frame's shadow epoch: every view the accumulator hook captured, drawn in one graph execution.
		IndirectDraws::Get().ExecuteShadowFrame();
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
		if (!a_running || !OcclusionEnabled(a_map))
			return false;
		// DCLF draws the map's members of the set this frame (none until its phase is drawn: a frame after the first that drew it).
		const bool ready = IndirectDraws::Get().OcclusionReady(a_map);
		counters.occlusionNativeFrames[a_map] += ready ? 0 : 1;
		occlusionWanted |= ready ? 1u << a_map : 0u;
		const std::uint8_t phase = a_map == kOcclusionSky ? kSetOccluderSky : kSetOccluderPrecipitation;
		const auto set = capture.CurrentSet();
		if (ready && set && (set->drawn & phase))
			capture.SetOcclusionPhase(phase);
		return ready;
	}

	bool FrameDriver::OcclusionNeedsEngine(std::uint32_t a_map, bool a_running)
	{
		const std::uint8_t phase = a_map == kOcclusionSky ? kSetOccluderSky : kSetOccluderPrecipitation;
		auto& view = Scene::FrameView::Get();
		const auto set = view.FrameClaims();
		const bool needed = !a_running || !OcclusionEnabled(a_map) || !(occlusionWanted & (1u << a_map)) || !set || !(set->drawn & phase) ||
		                    view.SetLacking(phase) > 0;
		// The engine's cull of the map reads the scene lists: whole again.
		if (needed)
			PrimaryCull::Get().RestoreSceneLists();
		counters.occlusionEngineFrames[a_map] += needed ? 1 : 0;
		return needed;
	}

	void FrameDriver::DrawOcclusion()
	{
		// Every occlusion map whose Ready said DCLF draws it this frame, in one epoch.
		const std::uint32_t wanted = std::exchange(occlusionWanted, 0u);
		PassCapture::Get().SetOcclusionPhase(0);
		const std::uint32_t drawn = wanted ? IndirectDraws::Get().ExecuteOcclusion(wanted) : 0u;
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
		if (!CaptureMainPass() && !loggedCaptureFailure) {
			loggedCaptureFailure = true;
			logger::warn("[DCLF] the main pass's targets are not bound where its opaque batches start; the colour epoch is skipped");
		}
	}

	bool FrameDriver::CaptureMainPass()
	{
		const auto formats = CurrentTargetFormats();
		if (formats.colorCount == 0 || formats.depth == DXGI_FORMAT_UNKNOWN)
			return false;
		captureFrame = Scene::FrameView::Get().GetFrame();
		DrawPipelines::Get().SetTargetFormats(formats);
		// What the main pass binds, for this frame's indirect draws (run before the composite).
		IndirectDraws::Get().CaptureMainPass();
		// The engine's state objects the pipeline lane asked for, read here because this is inside the deferred pass, where the blend table
		// holds the deferred variants.
		DrawPipelines::Get().CaptureEngineStates();
		return true;
	}

	void FrameDriver::AfterOpaquePass()
	{
		// DCLF's objects must be in the G-buffer and the depth before Terrain Blending draws its held terrain (alpha blended onto the
		// G-buffer, its alpha the distance to the object beneath, with depth writes): drawn after that terrain, a buried object would be
		// missing from what the terrain blended onto, and the terrain's depth writes would fail DCLF's EQUAL test where it overlapped.
		auto& draws = IndirectDraws::Get();
		globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);  // the epoch writes the G-buffer the context has bound
		draws.ProbeTargets("before colour");
		draws.ExecuteColour();  // the colour pass, against the depth written at the first draw
		draws.ProbeTargets("after colour");
		// DCLF's epoch leaves the context's bindings to the engine's state tracking: its targets rebound for the rest of the pass.
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
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
