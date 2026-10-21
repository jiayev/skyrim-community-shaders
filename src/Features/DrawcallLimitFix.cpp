// The feature shell: install, the engine hook thunks and their installation, the menu toggle and the settings UI. The lifecycle
// points forward to Draws/FrameDriver on the render thread. The one file that decides whether DCLF is built (CS_HAS_DCLF).
#include "DrawcallLimitFix.h"

#if defined(CS_HAS_DCLF)

#	include "Deferred.h"
#	include "Features/DrawcallLimitFix/Api/MaterialWrites.h"
#	include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#	include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#	include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#	include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#	include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#	include "Features/DrawcallLimitFix/Features/FeatureList.h"
#	include "Features/DrawcallLimitFix/Integrations/IntegrationList.h"
#	include "Features/DrawcallLimitFix/Schedule/Schedule.h"
#	include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#	include "DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#	include "DrawcallLimitFix/Diagnostics/Hooks.h"
#	include "DrawcallLimitFix/Diagnostics/Policy.h"
#	include "DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#	include "DrawcallLimitFix/Core/Shading/MaterialSources.h"
#	include "DrawcallLimitFix/Diagnostics/TestHarness.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#	include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#	include "DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#	include "Features/Skylighting.h"

#	include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#	include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#	include "DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#	include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#	include "DrawcallLimitFix/Core/Handoff/PassCapture.h"
#	include "DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#	include "DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#	include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#	include "DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#	include "DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#	include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#	include "RenderGraph/ScheduleWindow.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "State.h"
#	include "DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#	include "DrawcallLimitFix/Diagnostics/Scene/CaptureParity.h"
#	include "DrawcallLimitFix/Diagnostics/Feature/TargetDump.h"
#	include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"

#	include <ORGModuleServices/BackingPool.h>

void DrawcallLimitFix::PostPostLoad()
{
	// Every engine hook and offset DCLF uses is AE's (1.6.1170).
	if (!REL::Module::IsAE()) {
		unavailableReason = "DCLF supports Skyrim AE only";
		logger::info("[DCLF] {}; the game renders natively", unavailableReason);
		return;
	}
	// CS_DCLF=0: the baseline the gates compare against; nothing is installed. Unset means on.
	if (DCLF::SwitchValue(DCLF::Switch::Dclf) == "0") {
		logger::info("[DCLF] CS_DCLF=0; the feature stays off and the game renders natively");
		return;
	}
	DCLF::SceneTracker::Get().Install();
	DCLF::Scene::InstallSceneEvents();
	// Capture at registration: where the set's members' passes are withheld from the views DCLF draws.
	DCLF::PassCapture::Get().Install();
	// Whether DCLF runs, for the engine hooks outside the shell (the features' own thunks), before any of them is installed.
	DCLF::ShellState::InstallRunningQuery([] { return globals::features::drawcallLimitFix.Running(); });
	// The features' engine hooks that must precede DCLF's own engine side (Features/Portals: Main::Update's portal walk, first, the call
	// site the primary cull requires; Features/Reflection: the water reflection's update, add-roots and face render, right after it;
	// Features/Actors: Actor::SetAlpha and the actors' move writers, on the slots it reserves, where Core's scene events installed them;
	// Features/Billboards: its parity's NiBillboardNode::OnVisible thunk, before the primary cull takes the slot; Features/Lod: object
	// LOD's segment writers, its engine channel's source; Features/TreeLod: tree LOD's channel's source, the refill's, block update's and
	// group slots' hooks;
	// Features/HiddenNodes: the hidden stores, right after the scene events as before; Features/Decals: the decal array edits, which Core's
	// scene events installed (no other hook shares their sites)), then the observers
	// this run's switches ask for, and the engine hooks only they use (before DCLF's threads start).
	DCLF::Extensions::InstallEarlyHooks(DCLF::Features::List());
	DCLF::Diagnostics::Install();
	DCLF::MaterialWrites::Install();
	// The frame-sourced float positions built now, off the render thread's first frame.
	(void)DCLF::MaterialSources::FramePSFloats();
	(void)DCLF::MaterialSources::FrameVSFloats();
	// The shadow sources' hooks, by source (Api::Feature::sourceHooks): Features/Shadows/Sun's accumulation and UpdateCamera, Features/LocalLights'
	// light selection (Spot), then Features/Shadows/Local's point culls (Point) and focus cameras (Focus), in that order; the point culls
	// write NiNode's OnVisible slot before the primary cull reads it.
	DCLF::Extensions::InstallSourceHooks(DCLF::Features::List());
	DCLF::PrimaryCull::Get().Install();
	// The features' extension points (Features/FeatureList.cpp), registered before DCLF's threads start and read-only after, and
	// their engine hooks (after DCLF's own: Features/Portals' Main::IsRoomVisible detours, first; Features/TreeWind's tree manager hooks
	// follow the primary cull's stand-in; Features/Forward's main pass blended draw and Main::Draw's refraction point, before Hooks::Install's;
	// Features/Particles' snapshots of NiParticleSystem's update; Features/Lod's LOD gates, the engine's LOD swaps gated on DCLF's claims;
	// Features/Faces' thunks in the face morphing).
	// The integrations after them (Integrations/IntegrationList.cpp: Integrations/AdvancedSkin's wetness, Integrations/TerrainBlending's
	// classify input, toggle and held groups, Integrations/LightLimitFix's room index and light buffers).
	DCLF::Extensions::InstallFeatures(DCLF::Features::List(), DCLF::Integrations::List(), DCLF::Schedule::Frame());
	// The frame's epoch orders are the schedule's, handed to the render graph before it initializes (State::Setup, at the renderer's
	// targets' creation, after PostPostLoad).
	{
		const auto& orders = DCLF::Extensions::ScheduledFrameOrders();
		RenderGraphRuntime::ConfigureFrame({ orders.epochOrder, orders.asyncOrder, orders.revisionEpochs });
	}
	// DCLF's threads and its state graph, before the first frame dispatches a job (dclf-architecture.md, "The design").
	(void)DCLF::SceneScheduler::Graph();
	Hooks::Install();
	installed = true;
	// The coordinator's pump, from here woken by its producers (the hooks above push from now on); its passes apply the events
	// alone until the first frame runs DCLF (SetScenePassMode).
	DCLF::Scene::Coordinator::Get().SetScenePassMode(false, DCLF::Policy::ScenePassInline());
	DCLF::Scene::Coordinator::Get().StartScenePump();
	// The switches this process actually sees, once: a silent log is otherwise indistinguishable from a switch that never reached
	// the game (Switches.h).
	logger::info("[DCLF] switches: {}", DCLF::SwitchSummary());
	if (const auto reduced = DCLF::ReducedFeatures(); reduced.empty())
		logger::info("[DCLF] featureset: full (every feature on; the menu can still turn some off live)");
	else
		logger::warn("[DCLF] featureset: REDUCED by {}; this run does not exercise DCLF's full featureset", reduced);
}

void DrawcallLimitFix::SetupResources()
{
	if (installed && RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::EpochsEnabled())
		DCLF::ShaderPrograms::Get().StartPrecompile();
	// Runs right after the render graph tried to adopt DXVK's device (State::SetupResources). The hooks went in at PostPostLoad,
	// before any device existed; without the graph (or its epochs: CS_ORG_EPOCHS) there is nothing to draw with, so DCLF is forced
	// off here, with the reason in the log and the menu.
	if (!installed || (RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::EpochsEnabled()))
		return;
	unavailableReason = RenderGraphRuntime::Get().IsActive() ? std::string("the render graph runs without epochs (CS_ORG_EPOCHS=0)") :
	                                                           RenderGraphRuntime::Get().GetDisabledReason();
	installed = false;
	DCLF::SceneTracker::Get().Stop();
	// The scene passes apply what the hooks still push (the scene queues: kept bounded), and nothing else.
	DCLF::Scene::Coordinator::Get().SetScenePassMode(false, false);
	DCLF::PassCapture::Get().SetBypassed(true);
	logger::warn("[DCLF] Forced off: the render graph is unavailable ({}); the game renders natively", unavailableReason);
}

void DrawcallLimitFix::Reset()
{
	// The test commands run before the installed check, so a CS_DCLF=0 baseline reaches the same place at
	// the same in-game hour as the run it is compared against.
	DCLF::RunTestHarness(GetShortName());
	// The frame's last target point, then Present's observers (with DCLF on or off).
	DCLF::Diagnostics::Notify(DCLF::Diagnostics::TargetPoint{ "present" });
	DCLF::Diagnostics::Notify<DCLF::Diagnostics::FramePoint::Present>();
	// Every Present, in menus too: the coordinator's queues hold references to attached subtrees and must not grow while the world is
	// not rendered (FrameDriver::EndFrame keeps its passes draining them).
	if (!installed)
		return;
	{
		DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Present);
		DCLF::FrameDriver::Get().EndFrame(Running());
		// The menu's toggle, between frames. Scene events keep flowing while off, so the tracked set is current the moment it comes back on.
		UpdateActive();
	}
	DCLF::RenderThreadBudget::Get().EndFrame();
}

void DrawcallLimitFix::UpdateActive()
{
	if (!installed)
		return;
	if (const bool wanted = loaded && !globals::state->IsFeatureDisabled(GetShortName()); wanted != switchedOn)
		SetActive(wanted);
}

void DrawcallLimitFix::SetActive(bool a_active)
{
	switchedOn = a_active;
	// While off the scene passes apply the events alone (the tracked set stays current); whole again once on.
	DCLF::Scene::Coordinator::Get().SetScenePassMode(Running(), DCLF::Policy::ScenePassInline());
	if (!a_active)
		DCLF::IndirectDraws::Get().DrainAsync();
	auto& capture = DCLF::PassCapture::Get();
	// Off: every pass reaches the batch renderers again, and no set or claim outlives the switch. Back on, nothing is
	// withheld until the first frame's commit publishes the set again.
	capture.SetBypassed(!a_active);
	capture.PublishSet(nullptr);
	if (a_active)
		DCLF::Scene::FrameView::Get().RequestVerdictsInvalidated();
	logger::info("[DCLF] {} from the menu", a_active ? "Switched on" : "Switched off; the game renders natively");
}

void DrawcallLimitFix::Hooks::Main_Update_Entry::thunk(void* a_stages)
{
	func(a_stages);
	auto& feature = globals::features::drawcallLimitFix;
	if (feature.installed)
		DCLF::PrimaryCull::Get().AtUpdateEntry(feature.Running());
}

std::int64_t DrawcallLimitFix::Hooks::Main_Draw_Early::thunk(void* a_main)
{
	const auto result = func(a_main);
	globals::features::drawcallLimitFix.BeginSceneFrame();
	return result;
}

bool DrawcallLimitFix::BeginSceneFrame()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::SceneFrame);
	// Called every frame, loaded or not: the one place an unload (which stops Reset from being called) is noticed.
	UpdateActive();
	return DCLF::FrameDriver::Get().BeginFrame(Running(), installed);
}

void DrawcallLimitFix::BeforeShadowMaps()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::BeforeShadowMaps);
	if (Running())
		DCLF::FrameDriver::Get().BeforeShadowMaps();
}

void DrawcallLimitFix::AfterShadowMaps()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::AfterShadowMaps);
	RenderGraphRuntime::EpochBodyScope body(RenderGraphRuntime::Segment::ShadowView);
	if (Running())
		DCLF::FrameDriver::Get().AfterShadowMaps();
}

void DrawcallLimitFix::EarlyPrepass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::EarlyPrepass);
	if (Running())
		DCLF::FrameDriver::Get().EarlyPrepass();
}

bool DrawcallLimitFix::OcclusionReady(OcclusionMap a_map)
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Occlusion);
	static_assert(kSkyOcclusion == static_cast<std::uint32_t>(DCLF::Api::OcclusionMap::Sky) &&
				  kPrecipitationOcclusion == static_cast<std::uint32_t>(DCLF::Api::OcclusionMap::Precipitation));
	return DCLF::FrameDriver::Get().OcclusionReady(a_map, Running());
}

void DrawcallLimitFix::OcclusionView(OcclusionMap a_map)
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Occlusion);
	// The map's camera (its entry's: Features/OcclusionMaps' is the precipitation accumulator's).
	const auto& map = DCLF::Extensions::OcclusionMapOf(a_map);
	if (!Running() || !map.camera)
		return;
	DCLF::Api::OcclusionCamera camera;
	map.camera(camera);
	if (!camera.camera)
		return;
	DCLF::IndirectDraws::Get().OcclusionView(a_map, *camera.camera);
}

bool DrawcallLimitFix::OcclusionNeedsEngine(OcclusionMap a_map)
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Occlusion);
	return DCLF::FrameDriver::Get().OcclusionNeedsEngine(a_map, Running());
}

bool DrawcallLimitFix::OcclusionReferenceFrame(OcclusionMap a_map)
{
	// The map's own parity's (its entry's referenceDue: CS_DCLF_SKYLIGHT_PARITY's for Features/OcclusionMaps' maps).
	const auto& map = DCLF::Extensions::OcclusionMapOf(a_map);
	const bool due = map.referenceDue && map.referenceDue();
	// The engine's reference map registers every occluder, from the whole scene lists.
	if (due) {
		DCLF::PrimaryCull::Get().RestoreSceneLists();
		DCLF::PassCapture::Get().SetOcclusionPhase(0);
	}
	return due;
}

void DrawcallLimitFix::OcclusionReferenceRendered(OcclusionMap a_map)
{
	DCLF::Diagnostics::Notify(DCLF::Diagnostics::OcclusionReferenceRendered{ a_map });
}

void DrawcallLimitFix::Prepass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Prepass);
	DCLF::Diagnostics::Notify(DCLF::Diagnostics::PrepassBegin{ Running() });
	if (Running())
		DCLF::FrameDriver::Get().Prepass();
}

void DrawcallLimitFix::Hooks::Main_RenderDepth::thunk(bool a_firstPerson, bool a_a2)
{
	auto& passes = DCLF::ShellState::Passes();
	passes.inDepthPass = true;
	passes.depthControlDriven = a_a2;
	func(a_firstPerson, a_a2);
	passes.inDepthPass = false;
}

void DrawcallLimitFix::Hooks::Main_RenderDepth_WorldDrawn::thunk(void* a_accumulator, bool a_a2)
{
	func(a_accumulator, a_a2);
	auto& feature = globals::features::drawcallLimitFix;
	if (!DCLF::ShellState::Passes().inDepthPass || !feature.Running())
		return;
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::ZPrepass);
	// The world's depth draws are done and its camera is still current. The engine's own copy of the depth
	// (kPOST_ZPREPASS_COPY), at the end of the pass, now includes DCLF's objects, and the first-person model's
	// depth goes in after the world's, as it does natively (DCLF's in this epoch, after the world's depth; the engine's residue at
	// its first-person draw), so there is nothing to refresh. The views' work at the depth commit runs inside it (the frame's
	// reflection faces and first-person depth, which the depth epoch hosts).
	DCLF::FrameDriver::Get().DepthCommitted();
	if (DCLF::Scene::FrameView::Get().GetTables().objects.size() >= 400)
		globals::BeginDCLFDepthTrace(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture);
	// DCLF's epoch leaves the context's bindings to the engine's state tracking: rebind its targets for the
	// rest of the pass.
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

template <int N>
void DrawcallLimitFix::Hooks::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	auto& feature = globals::features::drawcallLimitFix;
	if (feature.Running())
		DCLF::Diagnostics::Notify(DCLF::Diagnostics::NativePassOffered{ a_pass, a_technique, DCLF::ShellState::Passes().inDepthPass });
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
	DCLF::Diagnostics::Notify(DCLF::Diagnostics::NativePassDrawn{ a_pass, DCLF::ShellState::Passes().inDepthPass });
}

void DrawcallLimitFix::Hooks::BSShaderAccumulator_FinishAccumulating::thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags)
{
	func(a_accumulator, a_renderFlags);
	if (!globals::features::drawcallLimitFix.Running())
		return;
	const auto mode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
	// The occlusion maps (render mode 0x1C): the map whose entry owns the accumulator, in slot order (Features/OcclusionMaps': the
	// precipitation accumulator's, Skylighting's own map while it draws it, else the precipitation mask).
	if (mode == 0x1C) {
		DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::Occlusion);
		const DCLF::Api::OcclusionAccumulator accumulator{ reinterpret_cast<RE::BSShaderAccumulator*>(a_accumulator) };
		for (std::uint32_t v = 0; v < DCLF::kOcclusionViews; ++v)
			if (const auto& map = DCLF::Extensions::OcclusionMapOf(v); map.ownsAccumulator && map.ownsAccumulator(accumulator)) {
				DCLF::IndirectDraws::Get().CaptureOcclusion(v);
				break;
			}
		return;
	}
	if (mode < 0xD || mode > 0xF)
		return;  // shadow-map modes only: plain, clamped, paraboloid (engine notes: shadow maps)
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::ShadowViewCapture);
	const auto view = DCLF::ShadowViews::Get().ViewOfAccumulator(a_accumulator);
	if (view != ~0u)
		DCLF::IndirectDraws::Get().CaptureShadowView(view, mode);
}

void DrawcallLimitFix::Hooks::Install()
{
	// Always installed: the shadow views are a live toggle (Toggles.h), and the thunk costs one compare per
	// accumulator finish when they are off.
	stl::write_vfunc<0x2A, BSShaderAccumulator_FinishAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);
	logger::info("[DCLF] shadow view hook installed on BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
	// The main camera's depth pass, hooked where Terrain Blending hooks it.
	stl::write_thunk_call<Main_RenderDepth>(REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3));
	stl::write_thunk_call<Main_RenderDepth_WorldDrawn>(REL::RelocationID(100421, 107139).address() + 0x1AA);
	logger::info("[DCLF] Z-prepass hook installed inside Main::RenderDepth, after the world's depth draws");
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<1>>(REL::RelocationID(100877, 107667).address() + REL::Relocate(0x1E5, 0xED));
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<2>>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
	// A geometry group's pass list (FUN_1414f19d0, AE 1.6.1170): the main pass's groups 9, 8, 1, 0 and 13 when they hold a
	// plain list, which is where terrain LOD's colour passes are drawn (dclf-architecture.md, "Terrain LOD").
	if (DCLF::Engine::CallsTo(REL::Offset(DCLF::Engine::Address::kGeometryGroupDraw).address(), REL::Offset(DCLF::Engine::Address::kRenderPassImmediately).address()))
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<3>>(REL::Offset(DCLF::Engine::Address::kGeometryGroupDraw).address());
	else
		logger::warn("[DCLF] the geometry groups' pass draw is not where expected; their passes stay native (terrain LOD among them)");
	stl::write_thunk_call<Main_Draw_Early>(REL::RelocationID(35560, 36559).address() + 0xD3);
	logger::info("[DCLF] scene phase hook installed on Main::Draw");
	// The stood-in roots' kAccumulated, written at Main::Update's entry.
	if (DCLF::Engine::CallsTo(REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address(), REL::Offset(DCLF::Engine::Address::kStagesReset).address()))
		stl::write_thunk_call<Main_Update_Entry>(REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address());
	else
		logger::warn("[DCLF] Main::Update's first call is not where expected; the stood-in roots' kAccumulated is not written (T7b2)");
	// (The main pass's blended draw and Main::Draw's refraction point are Features/Forward's, the first-person view's two points
	// Features/FirstPerson's, installed with the features.)
	logger::info("[DCLF] Native pass hooks installed");
}

void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
{
	if (Running())
		DCLF::FrameDriver::Get().NativeLightingDraw(a_pass, a_renderFlags);
}

void DrawcallLimitFix::BeforeOpaquePass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::BeforeOpaque);
	if (Running())
		DCLF::FrameDriver::Get().BeforeOpaquePass();
}

void DrawcallLimitFix::AfterOpaquePass()
{
	DCLF::RenderThreadBudget::Hook budget(DCLF::RenderThreadBudget::Site::AfterOpaque);
	if (Running())
		DCLF::FrameDriver::Get().AfterOpaquePass();
}

void DrawcallLimitFix::DrawSettings()
{
	const auto& stats = DCLF::Scene::Coordinator::Get().GetStats();
	if (!unavailableReason.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
		ImGui::TextUnformatted("Unavailable this session: everything renders natively.");
		ImGui::PopStyleColor();
		ImGui::TextWrapped("Reason: %s", unavailableReason.c_str());
		return;
	}
	if (!installed) {
		ImGui::TextUnformatted("Not installed (CS_DCLF=0).");
		return;
	}
	if (ImGui::Button("Reload shaders"))
		RenderGraphRuntime::RequestShaderReload();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("Builds every live render graph pipeline (DCLF's graphics pipelines, every compute program) again from Data/Shaders as it is now. The "
							   "current pipelines draw until the new ones are swapped in at the graph's next build; one that fails to compile keeps its previous "
							   "pipeline (the log names the error).");
	ImGui::SameLine();
	if (ImGui::Button("Render graph schedule"))
		RenderGraphSchedule::OpenWindow();
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("Opens the persistent schedule inspector: each executable's batches per queue, their passes, the barriers before each pass and "
							   "every cross-queue wait with its cause. The graph traces its schedule while the window is open.");
	if (!switchedOn) {
		ImGui::TextWrapped("Switched off with the feature's toggle: the game renders natively and DCLF does no frame work. Switch it back on to resume; no restart needed.");
		return;
	}
	// The live toggles, for A/B comparisons without a restart. They are the REQUESTED set; the render thread applies them at the start of the next frame (Toggles.h).
	auto& toggles = DCLF::Toggles::Get().Requested();
	const auto active = DCLF::ActiveToggles();
	if (ImGui::TreeNodeEx("Live toggles (A/B)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Each is seeded from its CS_DCLF_* switch and applied at the next frame. A change to an object class drops the classification caches, so the frame after it re-classifies everything.");
		int cull = toggles.cullMode;
		if (ImGui::Combo("GPU culling (CS_DCLF_CULL)", &cull, "off\0frustum\0frustum + occlusion\0"))
			toggles.cullMode = static_cast<std::uint8_t>(cull);
		for (const auto& toggle : DCLF::ToggleTable()) {
			if (!toggle.section.empty())
				ImGui::SeparatorText(toggle.section.data());
			ImGui::BeginDisabled(!DCLF::Toggles::Get().Editable(toggle));
			if (bool value = DCLF::ToggleValue(toggles, toggle); ImGui::Checkbox(toggle.label, &value))
				DCLF::SetToggleValue(toggles, toggle, value);
			if (toggle.tooltip)
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(toggle.tooltip);
			ImGui::EndDisabled();
		}
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("This frame", ImGuiTreeNodeFlags_DefaultOpen)) {
		std::string flags = fmt::format("Active: cull {}", active.cullMode);
		for (const auto& toggle : DCLF::ToggleTable())
			flags += fmt::format(", {} {}", toggle.name, DCLF::ToggleValue(active, toggle) ? 1 : 0);
		ImGui::TextWrapped("%s", flags.c_str());
		ImGui::Text("Tracked geometry: %u (under %u category nodes)", stats.tracked, stats.categoryNodes);
		ImGui::Text("Objects this frame: %u, geometries: %u, pipelines: %u", stats.objects, stats.geometries, stats.pipelines);
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				ImGui::Text("Left native (%s): %u", DCLF::kIneligibleNames[i].data(), stats.ineligible[i]);
		}
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		ImGui::Text("Indirect draws: %u drawn from %u records, %u epochs; culling tested %u, rejected %u",
			draws.drawn, draws.records, draws.epochs, draws.cullTested, draws.cullRejected);
		if (const auto& gpu = RenderGraphRuntime::Get().GpuTimingSummary(); !gpu.empty())
			ImGui::TextUnformatted(("GPU (ORG pass timestamps, last report):\n" + gpu).c_str());
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		const auto set = DCLF::Scene::FrameView::Get().FrameClaims();
		ImGui::Text("DCLF set: %u members; main-view passes withheld %u", set ? static_cast<std::uint32_t>(set->phases.size()) : 0u, capture.mainWithheld);
		if (active.shadows) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			ImGui::Text("Shadow views (since the last report): %u offered, %u drawn, %u not ready; last view %u inputs, %u records; sampled culling: %u tested, %u rejected",
				shadow.views, shadow.viewsDrawn, shadow.notReady, shadow.inputs, shadow.records, shadow.cullTested, shadow.cullRejected);
			ImGui::Text("Shadow views: withheld %u / %u / %u passes (plain / clamped / paraboloid); the set's casters %u / %u / %u",
				capture.shadowWithheld[0], capture.shadowWithheld[1], capture.shadowWithheld[2], shadow.casters[0], shadow.casters[1], shadow.casters[2]);
		}
		ImGui::TreePop();
	}
}

void DrawcallLimitFix::ClearShaderCache()
{
	// Every live render graph pipeline (RenderGraphRuntime's registry: DCLF's, the compute programs), whether DCLF is installed or not.
	RenderGraphRuntime::RequestShaderReload();
}

void DrawcallLimitFix::NoteMaterialWritten(const RE::BSShaderMaterial* a_material)
{
	// CS's PBR materials (src/TruePBR) reach Core through the material-write sink, as the features' write hooks do.
	DCLF::Api::NoteMaterialWritten(a_material);
}

void DrawcallLimitFix::PrecompileShader(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
{
	DCLF::ShaderPrograms::Get().Precompile(a_shader, a_pixel, a_descriptor);
}

void DrawcallLimitFix::OnMap(ID3D11Resource* a_resource, void* a_data)
{
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnMap(a_resource, a_data);
	if (auto& mirror = DCLF::ConstantMirror::Get(); mirror.Any())
		mirror.OnMap(a_resource, a_data);
}

void DrawcallLimitFix::OnUnmap(ID3D11Resource* a_resource)
{
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnUnmap(a_resource);
	if (auto& mirror = DCLF::ConstantMirror::Get(); mirror.Any())
		mirror.OnUnmap(a_resource);
}

void DrawcallLimitFix::OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data)
{
	if (auto& mirror = DCLF::ConstantMirror::Get(); mirror.Any())
		mirror.OnUpdateSubresource(a_resource, a_box, a_data);
}

bool DrawcallLimitFix::TargetDumpArmed()
{
	return DCLF::TargetDump::Armed();
}

void DrawcallLimitFix::TargetDumpPoint(const char* a_label, ID3D11Texture2D* a_extra)
{
	DCLF::TargetDump::Point(a_label, a_extra);
}

void DrawcallLimitFix::OnGraphSubmissionsRetired(std::uint64_t a_submissions)
{
	DCLF::FrameRetirement()->Advance(a_submissions);
}

#else  // !CS_HAS_DCLF: built without the render graph or ORGModuleServices; nothing is installed and every entry point is inert.

#	include "RenderGraph/RenderGraphRuntime.h"

void DrawcallLimitFix::PostPostLoad()
{
	unavailableReason = "built without the render graph (CS_HAS_DCLF off)";
	logger::info("[DCLF] {}; the game renders natively", unavailableReason);
}
void DrawcallLimitFix::SetupResources() {}
void DrawcallLimitFix::Reset() {}
void DrawcallLimitFix::Prepass() {}
void DrawcallLimitFix::EarlyPrepass() {}
void DrawcallLimitFix::DrawSettings()
{
	ImGui::TextWrapped("Unavailable: %s", unavailableReason.c_str());
}
void DrawcallLimitFix::ClearShaderCache()
{
	// The render graph's live pipelines (Light Limit Fix's compute programs), when there is a graph.
	RenderGraphRuntime::RequestShaderReload();
}
void DrawcallLimitFix::BeforeOpaquePass() {}
void DrawcallLimitFix::AfterOpaquePass() {}
void DrawcallLimitFix::ProbeOpaqueTarget(std::uint32_t) {}
void DrawcallLimitFix::BeforeShadowMaps() {}
void DrawcallLimitFix::AfterShadowMaps() {}
void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass*, std::uint32_t) {}
bool DrawcallLimitFix::OcclusionReady(OcclusionMap) { return false; }
bool DrawcallLimitFix::OcclusionNeedsEngine(OcclusionMap) { return true; }
void DrawcallLimitFix::OcclusionView(OcclusionMap) {}
bool DrawcallLimitFix::OcclusionReferenceFrame(OcclusionMap) { return false; }
void DrawcallLimitFix::OcclusionReferenceRendered(OcclusionMap) {}
void DrawcallLimitFix::NoteMaterialWritten(const RE::BSShaderMaterial*) {}
void DrawcallLimitFix::PrecompileShader(const RE::BSShader&, bool, std::uint32_t) {}
void DrawcallLimitFix::OnMap(ID3D11Resource*, void*) {}
void DrawcallLimitFix::OnUnmap(ID3D11Resource*) {}
void DrawcallLimitFix::OnUpdateSubresource(ID3D11Resource*, const D3D11_BOX*, const void*) {}
bool DrawcallLimitFix::TargetDumpArmed() { return false; }
void DrawcallLimitFix::TargetDumpPoint(const char*, ID3D11Texture2D*) {}
void DrawcallLimitFix::OnGraphSubmissionsRetired(std::uint64_t) {}

#endif
