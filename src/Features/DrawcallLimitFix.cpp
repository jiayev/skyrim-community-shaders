// The feature shell: install, the engine hook thunks and their installation, the menu toggle and the settings UI. The lifecycle
// points forward to Draws/FrameDriver on the render thread. The one file that decides whether DCLF is built (CS_HAS_DCLF).
#include "DrawcallLimitFix.h"

#if defined(CS_HAS_DCLF)

#	include "Deferred.h"
#	include "Features/DrawcallLimitFix/Api/MaterialWrites.h"
#	include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#	include "Features/DrawcallLimitFix/Core/Engine/WindowWriters.h"
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
#	include "DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#	include "DrawcallLimitFix/Diagnostics/Hooks.h"
#	include "DrawcallLimitFix/Diagnostics/PlayerParity.h"
#	include "DrawcallLimitFix/Diagnostics/Policy.h"
#	include "DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#	include "DrawcallLimitFix/Core/Shading/MaterialSources.h"
#	include "DrawcallLimitFix/Diagnostics/TestHarness.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#	include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#	include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#	include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#	include "DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#	include "Features/Skylighting.h"
#	include "ShaderCache.h"

#	include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#	include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#	include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#	include "DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#	include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#	include "Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatches.h"
#	include "DrawcallLimitFix/Core/Handoff/PassCapture.h"
#	include "DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#	include "DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#	include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#	include "DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#	include "DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"
#	include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#	include "RenderGraph/MemoryWindow.h"
#	include "RenderGraph/ScheduleWindow.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include <OpenRenderGraph/GraphHost.h>
#	include "State.h"
#	include "DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#	include "DrawcallLimitFix/Diagnostics/Scene/CaptureParity.h"
#	include "DrawcallLimitFix/Diagnostics/Feature/TargetDump.h"
#	include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"

#	include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"

#	include <ORGModuleServices/BackingPool.h>

namespace
{
	// Every engine hook and offset DCLF uses is an Engine::Offset: Address Library ids, with the positions of the sites inside functions
	// checked against SE 1.5.97, AE 1.6.1170 and AE 1.7.104 only (another build may move them, or renumber an id). SceneWriterFramework
	// has tables for the same three.
	bool SupportedGame()
	{
		return DCLF::Engine::Supported();
	}
}

void DrawcallLimitFix::PostLoad()
{
	// SceneWriterFramework's election, only when PostPostLoad will install: without CS's, the engine's stores stay unpatched unless
	// another plugin's copy of SWF elects.
	if (SupportedGame() && DCLF::SwitchValue(DCLF::Switch::Dclf) != "0")
		DCLF::SceneWriter::Elect();
}

void DrawcallLimitFix::DataLoaded()
{
	// The patches another plugin rewrote after SWF's install, each logged with its module (SWF's lines), kept for the menu.
	DCLF::SceneWriter::CheckIntegrity();
}

namespace
{
	// The scene writer framework's provider, its families and the integrity check's findings (the menu).
	void DrawSceneWriterPanel()
	{
		if (!ImGui::TreeNodeEx("Scene writer framework"))
			return;
		ImGui::TextWrapped("Provider: %s", DCLF::SceneWriter::ProviderLine().c_str());
		ImGui::TextWrapped("Hook families: %s", DCLF::SceneWriter::CapsLine().c_str());
		const auto& changes = DCLF::SceneWriter::IntegrityChanges();
		if (!DCLF::SceneWriter::IntegrityChecked())
			ImGui::TextUnformatted("Integrity: not checked yet (it runs once the game's data has loaded).");
		else if (changes.empty())
			ImGui::TextUnformatted("Integrity: every patch is as installed.");
		else {
			ImGui::TextWrapped(
				"Integrity: %zu patch(es) rewritten by another plugin after install. A later hook that calls on still reaches SWF's; "
				"one that does not hides those writes from DCLF.",
				changes.size());
			for (const auto& change : changes)
				ImGui::BulletText("%s +0x%x: now leads to %s", change.family.c_str(), change.offset, change.module.empty() ? "no module" : change.module.c_str());
		}
		ImGui::TreePop();
	}
}

void DrawcallLimitFix::PostPostLoad()
{
	if (!SupportedGame()) {
		const auto game = REL::Module::get().version();
		unavailableReason = fmt::format("DCLF supports Skyrim SE 1.5.97, AE 1.6.1170 and AE 1.7.104 only (this game is {})", game.string("."));
		logger::info("[DCLF] {}; the game renders natively", unavailableReason);
		return;
	}
	// CS_DCLF=0: the baseline the gates compare against; nothing is installed. Unset means on.
	if (DCLF::SwitchValue(DCLF::Switch::Dclf) == "0") {
		logger::info("[DCLF] CS_DCLF=0; the feature stays off and the game renders natively");
		return;
	}
	// The patch sites DCLF cannot run without, checked before any hook is written: another plugin's patch at one leaves the game
	// native rather than half hooked. (Optional sites warn and skip at their own install.) The engine's stores are SceneWriterFramework's,
	// verified by the provider at its election: the families DCLF requires must be installed.
	if (const auto missing = DCLF::SceneWriter::MissingReason(); !missing.empty()) {
		unavailableReason = missing;
		logger::warn("[DCLF] {}; the game renders natively", unavailableReason);
		return;
	}
	for (auto missing : { DCLF::PassCapture::SitesMissing() })
		if (!missing.empty()) {
			unavailableReason = fmt::format("{} DCLF hooks are not the engine's code (another plugin patched them first)", missing);
			logger::warn("[DCLF] {}; the game renders natively", unavailableReason);
			return;
		}
	// Every other site DCLF's hooks write (Core's, then the features'): a chained hook needs a CALL there, whatever it calls; one that skips
	// or replaces the engine's code needs the engine's own. A conflict leaves the game native rather than half hooked (a producer whose
	// consumer never went in).
	Hooks::Preflight();
	DCLF::Extensions::PreflightFeatures(DCLF::Features::List());
	if (const auto conflicts = DCLF::HookSites::Conflicts(); !conflicts.empty()) {
		unavailableReason = fmt::format("its hooks conflict with another plugin's ({})", conflicts);
		logger::error("[DCLF] {}; DCLF stays off and the game renders natively", unavailableReason);
		return;
	}
	logger::info("[DCLF] hook preflight: {} sites checked, no conflicts", DCLF::HookSites::Checked());
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
	// The menu's saved toggles, now that every toggle is seeded (their switches win where set).
	ApplySavedSettings();
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
	// A hook whose site passed the preflight but did not go in (a detour's attach): the others are written, so DCLF is forced off.
	if (const auto failed = DCLF::HookSites::Conflicts(); !failed.empty()) {
		unavailableReason = fmt::format("its hooks did not all install ({})", failed);
		logger::error("[DCLF] {}", unavailableReason);
		ForceOff();
	}
}

void DrawcallLimitFix::SetupResources()
{
	if (installed && RenderGraphRuntime::Get().IsActive() && RenderGraphRuntime::EpochsEnabled())
		DCLF::ShaderPrograms::Get().StartPrecompile();
	// Runs right after the render graph tried to adopt DXVK's device (State::SetupResources). The hooks went in at PostPostLoad,
	// before any device existed; without the graph (or its epochs: CS_ORG_EPOCHS, async: CS_ORG_ASYNC_EPOCHS) or the device's
	// indirect pipelines there is nothing to draw with, while the claims would stand and withhold the engine's passes. So DCLF is
	// forced off here, with the reason in the log and the menu.
	if (!installed)
		return;
	auto& graph = RenderGraphRuntime::Get();
	if (!graph.IsActive())
		unavailableReason = graph.GetDisabledReason();
	else if (!RenderGraphRuntime::EpochsEnabled())
		unavailableReason = "the render graph runs without epochs (CS_ORG_EPOCHS=0)";
	else if (!graph.Host() || !graph.Host()->AsyncEpochs())
		unavailableReason = "the render graph runs without async epochs (CS_ORG_ASYNC_EPOCHS=0)";
	else if (!DCLF::DrawPipelines::Get().Enabled())
		unavailableReason = "the GPU or driver lacks the indirect pipelines DCLF draws with (see the [DCLF] Indirect pipelines line)";
	else
		return;
	ForceOff();
}

void DrawcallLimitFix::ForceOff()
{
	installed = false;
	DCLF::SceneTracker::Get().Stop();
	// The scene passes apply what the hooks still push (the scene queues: kept bounded), and nothing else.
	DCLF::Scene::Coordinator::Get().SetScenePassMode(false, false);
	DCLF::PassCapture::Get().SetBypassed(true);
	logger::warn("[DCLF] Forced off: {}; the game renders natively", unavailableReason);
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
	if (!installed || faulted)
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
	if (Running())
		WatchScenePump();
	// Called every frame, loaded or not: the one place an unload (which stops Reset from being called) is noticed.
	UpdateActive();
	return DCLF::FrameDriver::Get().BeginFrame(Running(), installed);
}

void DrawcallLimitFix::WatchScenePump()
{
	// Every running frame's inputs wake a scene pass (FrameView::BeginFrame), and a pass ends a frame or two behind, so a frame far ahead of
	// the last pass for long means the pump stopped (a pass threw past its handler, or hangs). The builder would never publish again and
	// every frame keep drawing the last snapshot while the scene changes; the game is handed back for the session instead. The bounds
	// leave room for a slow pass at a load (the frames that wait for a snapshot give up at 5 s on their own, Snapshot.cpp).
	constexpr std::uint32_t kLagFrames = 600;
	constexpr auto kLagTime = std::chrono::seconds(30);
	const auto frame = DCLF::Scene::FrameView::Get().GetFrame();
	const auto done = DCLF::IndirectDraws::Get().ScenePassDoneFrame();
	std::string reason;
	if (DCLF::Scene::Coordinator::Get().ScenePumpFailed())
		reason = "the scene pump stopped (see the [DCLF] error above)";
	else if (frame > done + kLagFrames) {
		const auto now = std::chrono::steady_clock::now();
		if (!std::exchange(pumpLagging, true))
			pumpLagSince = now;
		else if (now - pumpLagSince > kLagTime)
			reason = fmt::format("no scene pass has ended for {} frames (the last for frame {}, now {})", frame - done, done, frame);
	} else
		pumpLagging = false;
	if (reason.empty())
		return;
	unavailableReason = reason + "; DCLF is off for the rest of the session";
	logger::error("[DCLF] {}", unavailableReason);
	faulted = true;
	SetActive(false);
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

namespace
{
	/**
	 * @brief The precipitation object Main::Precipitation::RenderOcclusion (AE FUN_1404076c0, SE FUN_1403ae920) renders the mask for: with
	 * the occlusion map on (TogglePrecipitationOcclusionMap's flag) and either weather's precipitation rain or snow, the current one, else
	 * the last. Null: the engine renders none (it clears the mask once when switched off, and keeps its projection).
	 */
	RE::BSGeometry* EngineMaskObject()
	{
		static REL::Relocation<const bool*> maskOn{ REL::RelocationID(502538, 370126) };
		auto* sky = globals::game::sky;
		auto* precip = sky ? sky->precip : nullptr;
		if (!precip || !*maskOn)
			return nullptr;
		const auto falls = [](RE::TESWeather* a_weather) {
			using Data = RE::BGSShaderParticleGeometryData;
			auto* data = a_weather ? a_weather->precipitationData : nullptr;
			if (!data)
				return false;
			const auto type = data->GetSettingValue(Data::DataID::kParticleType).i;
			return type == static_cast<std::uint32_t>(Data::ParticleType::kRain) || type == static_cast<std::uint32_t>(Data::ParticleType::kSnow);
		};
		if (!falls(sky->currentWeather) && !falls(sky->lastWeather))
			return nullptr;
		return precip->currentPrecip ? precip->currentPrecip.get() : precip->lastPrecip.get();
	}
}

void DrawcallLimitFix::RenderPrecipitationMask(RE::Precipitation* a_precip, RE::BSGeometry* a_precipObject, bool a_dclf)
{
	// With DCLF drawing it (a_dclf), its native variant of the mask: DCLF draws every occluder from its own tables, GPU-culled, in its
	// shadow epoch, so the engine's cull and registration (SetupMask) are skipped and RenderMask only sets the camera and clears the mask.
	// Otherwise, or on a frame DCLF did not draw it, the engine does.
	const bool reference = a_dclf && OcclusionReferenceFrame(kPrecipitationOcclusion);
	if (!a_dclf || reference) {
		a_precip->SetupMask();
	} else {
		// What SetupMask sets: the projection, and the accumulator's camera.
		static REL::Relocation<void(RE::Precipitation*, RE::NiPointer<RE::NiCamera>)> computeProjection{ REL::RelocationID(25643, 26185) };
		computeProjection(a_precip, a_precip->occlusionData.camera);
		if (auto* accumulator = a_precip->occlusionData.accumulator.get())
			accumulator->camera = a_precip->occlusionData.camera.get();
	}
	OcclusionView(kPrecipitationOcclusion);
	auto* particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(a_precipObject->GetGeometryRuntimeData().shaderProperty.get());
	auto* rain = (RE::BSParticleShaderRainEmitter*)(particleShaderProperty->particleEmitter);

	globals::profiler->BeginPass("Skylighting::PrecipMask");
	a_precip->RenderMask(rain);
	if (reference) {
		// The engine's mask, announced; then the same view again, cleared, for DCLF's.
		OcclusionReferenceRendered(kPrecipitationOcclusion);
		a_precip->RenderMask(rain);
	}
	globals::profiler->EndPass();
}

void DrawcallLimitFix::RenderPrecipitationAtShadowMaps()
{
	// Skylighting's RenderOcclusionAtShadowMaps draws the maps when it is loaded; the engine renders the mask whole with the shader cache off.
	if (globals::features::skylighting.loaded || !globals::shaderCache->IsEnabled())
		return;
	auto* object = EngineMaskObject();
	if (!object || !OcclusionReady(kPrecipitationOcclusion))
		return;
	auto state = globals::state;
	TracyD3D11Zone(state->tracyCtx, "Precipitation Mask (Drawcall Limit Fix)");
	state->BeginPerfEvent("Precipitation Mask (Drawcall Limit Fix)");
	// As Skylighting's: before the engine's shadow maps, which set the renderer's state again; the camera they begin with is put back
	// (EarlyPrepasses reads its eye after them), and the mask's viewport is not scaled by dynamic resolution.
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	const auto posAdjust = shadowState.posAdjust;
	const auto previousPosAdjust = shadowState.previousPosAdjust;
	const auto cameraData = shadowState.cameraData;
	auto& graphicsState = globals::game::graphicsState->GetRuntimeData();
	const auto dynamicResolutionLock = graphicsState.dynamicResolutionLock;
	graphicsState.dynamicResolutionLock = 1;
	RenderPrecipitationMask(globals::game::sky->precip, object, true);
	graphicsState.dynamicResolutionLock = dynamicResolutionLock;
	shadowState.posAdjust = posAdjust;
	shadowState.previousPosAdjust = previousPosAdjust;
	shadowState.cameraData = cameraData;
	state->EndPerfEvent();
}

void DrawcallLimitFix::Hooks::Main_Precipitation_RenderOcclusion::thunk()
{
	// The mask DCLF drew in this frame's shadow epoch (RenderPrecipitationAtShadowMaps) is done; else the engine renders it whole.
	auto& dclf = globals::features::drawcallLimitFix;
	if (!globals::shaderCache->IsEnabled() || !EngineMaskObject() || dclf.OcclusionNeedsEngine(kPrecipitationOcclusion))
		func();
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

namespace
{
	// Hooks::Install's sites (Preflight checks the same ones).
	std::uintptr_t DepthPassSite() { return REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3); }
	std::uintptr_t WorldDrawnSite() { return REL::RelocationID(100421, 107139).address() + REL::Relocate(0x18E, 0x1AA); }
	std::uintptr_t ScenePhaseSite() { return REL::RelocationID(35560, 36559).address() + 0xD3; }
	std::uintptr_t PrecipitationMaskSite() { return REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x3A1, 0x3A1, 0x3BF); }
}

void DrawcallLimitFix::Hooks::Preflight()
{
	// Chained: the call's target (Terrain Blending's at the depth pass, ShadowBoost's at Main::Update's entry) runs before or around
	// DCLF's thunk. Each is one DCLF cannot run without: the depth pass and its commit point (the withheld objects' depth), the frame's
	// start, and Main::Update's entry (the only consumer of the visibility decoder's write lists, whose tables the retirement chain waits on).
	DCLF::HookSites::RequireCall("Main::Draw's depth pass call", DepthPassSite());
	DCLF::HookSites::RequireCall("Main::RenderDepth's world-drawn call", WorldDrawnSite());
	DCLF::HookSites::RequireCall("Main::Draw's first call", ScenePhaseSite());
	DCLF::HookSites::RequireCall("Main::Update's first call", REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address());
	// Not chained: on the frames DCLF drew the mask the engine's call is skipped, and with it any hook another plugin put there.
	// Skylighting (which drives the mask itself) takes the call instead.
	if (!globals::features::skylighting.loaded)
		DCLF::HookSites::RequireCallTo("Main::Draw's precipitation mask call", PrecipitationMaskSite());
	DCLF::WindowWriters::Preflight();
	DCLF::PassCapture::Preflight();
	DCLF::PrimaryCull::Preflight();
}

void DrawcallLimitFix::Hooks::Install()
{
	// Always installed: the shadow views are a live toggle (Toggles.h), and the thunk costs one compare per
	// accumulator finish when they are off.
	stl::write_vfunc<0x2A, BSShaderAccumulator_FinishAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);
	logger::info("[DCLF] shadow view hook installed on BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
	// The diagnostics' sites below are calls another plugin may already have redirected, so only the instruction form is checked: a CALL
	// rel32, as write_thunk_call needs. The required ones passed Preflight.
	const auto isCall = [](std::uintptr_t a_site) { return *reinterpret_cast<const std::uint8_t*>(a_site) == 0xE8; };
	// The main camera's depth pass, hooked where Terrain Blending hooks it.
	stl::write_thunk_call<Main_RenderDepth>(DepthPassSite());
	// Main::RenderDepth's call of BSShaderAccumulator::RenderFirstPersonView_End (SE +0x18E, AE +0x1AA).
	stl::write_thunk_call<Main_RenderDepth_WorldDrawn>(WorldDrawnSite());
	logger::info("[DCLF] Z-prepass hook installed inside Main::RenderDepth, after the world's depth draws");
	// Light Limit Fix's RenderPass3 site: the plain pass list loop (SE FUN_1413090c0, AE FUN_1414f4f20).
	if (const auto site = REL::RelocationID(100871, 107667).address() + REL::Relocate(0xEE, 0xED); isCall(site))
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<1>>(site);
	if (const auto site = REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F); isCall(site))
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<2>>(site);
	// A geometry group's pass list (AE FUN_1414f19d0): the main pass's groups 9, 8, 1, 0 and 13 when they hold a plain list, which is where
	// terrain LOD's colour passes are drawn (dclf-architecture.md, "Terrain LOD"). AE inlines the plain-list loop there; SE's
	// RenderPersistentPassList calls FUN_1413090c0 for it, whose call RenderPassImmediately<1> already hooks (no SE address here).
	// A diagnostics' observer (the geometry groups' passes), chained onto whatever the call holds.
	if (DCLF::Engine::Address::kGeometryGroupDraw.Known()) {
		if (const auto site = REL::Offset(DCLF::Engine::Address::kGeometryGroupDraw).address(); isCall(site))
			stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<3>>(site);
		else
			logger::warn("[DCLF] the geometry groups' pass draw is not a call; the diagnostics do not see their passes");
	}
	stl::write_thunk_call<Main_Draw_Early>(ScenePhaseSite());
	logger::info("[DCLF] scene phase hook installed on Main::Draw");
	// The stood-in roots' kAccumulated, written at Main::Update's entry. Chained onto whatever the call holds: another plugin's hook there
	// (ShadowBoost's) runs, and through it the engine's, before DCLF's. Without this hook nothing takes the decoder's write lists, whose
	// tables then stall the retirement chain (every engine reference DCLF lets go of is kept).
	if (const auto site = REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address(); !DCLF::Engine::CallsTo(site, REL::Offset(DCLF::Engine::Address::kStagesReset).address()))
		logger::info("[DCLF] Main::Update's first call is hooked by {}; DCLF's entry hook chains onto it", DCLF::HookSites::BranchTargetModule(site));
	stl::write_thunk_call<Main_Update_Entry>(REL::Offset(DCLF::Engine::Address::kUpdateEntryCall).address());
	// The engine's precipitation mask, without Skylighting (which hooks the same call, and drives the mask itself).
	if (!globals::features::skylighting.loaded) {
		stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(PrecipitationMaskSite());
		logger::info("[DCLF] precipitation mask hook installed on Main::Draw (without Skylighting)");
	}
	// The engine's writers inside the engine-read window, kept out of DCLF's reads there.
	DCLF::WindowWriters::Install();
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
		DrawSceneWriterPanel();
		return;
	}
	if (!installed) {
		ImGui::TextUnformatted("Not installed (CS_DCLF=0).");
		return;
	}
	// The developer's tools, under a header a player can leave closed.
	const auto drawTools = [] {
		if (ImGui::Button("Reload shaders"))
			RenderGraphRuntime::RequestShaderReload();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Builds every live render graph pipeline (DCLF's graphics pipelines, every compute program) again from Data/Shaders as it is now. The "
				"current pipelines draw until the new ones are swapped in at the graph's next build; one that fails to compile keeps its previous "
				"pipeline (the log names the error).");
		ImGui::SameLine();
		if (ImGui::Button("Render graph schedule"))
			RenderGraphSchedule::OpenWindow();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Opens the persistent schedule inspector: each executable's batches per queue, their passes, the barriers before each pass and "
				"every cross-queue wait with its cause. The graph traces its schedule while the window is open.");
		ImGui::SameLine();
		if (ImGui::Button("VRAM attribution"))
			RenderGraphMemory::OpenWindow();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Opens VRAM attribution: every D3D11 resource (DXVK's) and render graph resource (DCLF's tables, pools and targets among them) "
				"with its owner and the size of its allocation, reconciled against the driver's heap usage.");
	};
	if (!switchedOn) {
		ImGui::TextWrapped("Switched off with the feature's toggle: the game renders natively and DCLF does no frame work. Switch it back on to resume; no restart needed.");
		if (ImGui::TreeNodeEx("Advanced")) {
			drawTools();
			ImGui::TreePop();
		}
		return;
	}
	DCLF::Diagnostics::PlayerParity::DrawPanel();
	DrawSceneWriterPanel();
	// The live toggles, for A/B comparisons without a restart. They are the REQUESTED set; the render thread applies them at the start of the next frame (Toggles.h).
	auto& toggles = DCLF::Toggles::Get().Requested();
	const auto active = DCLF::ActiveToggles();
	if (ImGui::TreeNodeEx("What DCLF draws", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Each takes effect at the next frame, and Save Settings keeps it. What DCLF does not draw, the game draws as it would without DCLF.");
		int cull = toggles.cullMode;
		if (ImGui::Combo("GPU culling", &cull, "off\0frustum\0frustum + occlusion\0"))
			toggles.cullMode = static_cast<std::uint8_t>(cull);
		// The pipeline (Core/Render/Direct/Direct.h for the CPU-driven one): auto or manual (per kind of cell), the CPU-driven one's record
		// mode and per-view cadences; the test harness's dclf:pipeline, dclf:pipeline-interior/-exterior, dclf:cpu-record and
		// dclf:cpu-cadence-<view>. Save Settings keeps the pipeline choice; the CPU-driven one's options are seeded by their switches alone.
		int pipelineMode = toggles.pipelineMode == DCLF::PipelineMode::Manual ? 1 : 0;
		if (ImGui::Combo("Pipeline", &pipelineMode, "Auto\0Manual\0"))
			toggles.pipelineMode = pipelineMode == 1 ? DCLF::PipelineMode::Manual : DCLF::PipelineMode::Auto;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"GPU-driven: the draws culled and recorded on the GPU. CPU-driven: culled on DCLF's worker threads and drawn with direct draws "
				"recorded into DXVK's command buffer. Auto draws exteriors GPU-driven and interiors CPU-driven; Manual chooses for each. Takes "
				"effect at the next graph build (entering or leaving an interior builds it); Save Settings keeps it (CS_DCLF_PIPELINE seeds it).");
		if (toggles.pipelineMode == DCLF::PipelineMode::Manual) {
			ImGui::Indent();
			int interior = toggles.interiorCpu ? 1 : 0, exterior = toggles.exteriorCpu ? 1 : 0;
			if (ImGui::Combo("Interiors", &interior, "GPU-driven\0CPU-driven\0"))
				toggles.interiorCpu = interior == 1;
			if (ImGui::Combo("Exteriors", &exterior, "GPU-driven\0CPU-driven\0"))
				toggles.exteriorCpu = exterior == 1;
			ImGui::Unindent();
		}
		ImGui::Text("Now: %s-driven (%s)", DCLF::Draws::CurrentGraphConditions().cpuPipeline ? "CPU" : "GPU", DCLF::Draws::CurrentGraphConditions().interior ? "interior" : "exterior");
		if (DCLF::Draws::CpuPipelineForced())
			ImGui::TextWrapped("This GPU or driver has no device-generated commands with pipeline sets: every cell draws CPU-driven.");
		// The CPU-driven pipeline's options, while it draws some kind of cell.
		if (toggles.pipelineMode == DCLF::PipelineMode::Auto || toggles.interiorCpu || toggles.exteriorCpu) {
			ImGui::Indent();
			int record = toggles.cpuRecord;
			if (ImGui::Combo("CPU recording", &record, "serial\0parallel\0"))
				toggles.cpuRecord = static_cast<std::uint8_t>(record);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"Serial: the workers cull, and DXVK's CS thread records every batch's draws. Parallel: the workers also record each batch into "
					"a secondary command list, which the CS thread only executes (decals, forward and refraction stay serial: one ordered stream). "
					"CS_DCLF_CPU_RECORD seeds it.");
			// Applied when entered (or stepped), from the next snapshot: each change makes the view batches again.
			int lanes = static_cast<int>(DCLF::Draws::BatchLanes());
			if (ImGui::InputInt("Batch lanes", &lanes, 256, 1024, ImGuiInputTextFlags_EnterReturnsTrue))
				DCLF::Draws::SetBatchLanes(static_cast<std::uint32_t>(std::max(lanes, 8)));
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"The draws a culled batch holds (a multiple of 8, 8 to 65528; default 4096). More coalesce more draws into each batch and, in "
					"parallel recording, into each secondary command list; fewer spread a view's culls over more jobs. Takes effect from the next "
					"scene snapshot; CS_DCLF_CPU_BATCH seeds it.");
			if (ImGui::TreeNodeEx("Views culled once per snapshot")) {
				ImGui::TextWrapped("A view checked here is culled and recorded once per scene snapshot and replayed until the next; unchecked, every frame. "
								   "The main view is always culled every frame. CS_DCLF_CPU_CADENCE seeds them.");
				for (std::size_t v = 1; !DCLF::CpuViewName(v).empty(); ++v) {
					const auto bit = static_cast<std::uint16_t>(1u << v);
					bool snapshot = (toggles.cpuCadence & bit) != 0;
					if (ImGui::Checkbox(std::string(DCLF::CpuViewName(v)).c_str(), &snapshot))
						toggles.cpuCadence = snapshot ? static_cast<std::uint16_t>(toggles.cpuCadence | bit) : static_cast<std::uint16_t>(toggles.cpuCadence & ~bit);
				}
				ImGui::TreePop();
			}
			ImGui::Unindent();
		}
		for (const auto& toggle : DCLF::ToggleTable()) {
			if (!toggle.section.empty())
				ImGui::SeparatorText(toggle.section.data());
			ImGui::BeginDisabled(!DCLF::Toggles::Get().Editable(toggle));
			if (bool value = DCLF::ToggleValue(toggles, toggle); ImGui::Checkbox(toggle.label, &value))
				DCLF::SetToggleValue(toggles, toggle, value);
			// The tooltip names the toggle's switch, which overrides the saved setting at startup when set (validation runs).
			if (auto _tt = Util::HoverTooltipWrapper()) {
				if (toggle.tooltip)
					ImGui::TextUnformatted(toggle.tooltip);
				const auto name = toggle.member ? DCLF::SwitchName(toggle.seed) : DCLF::SwitchName(toggle.registeredSeed);
				const auto& seed = toggle.member ? DCLF::SwitchValue(toggle.seed) : DCLF::SwitchValue(toggle.registeredSeed);
				ImGui::TextDisabled("%.*s%s", static_cast<int>(name.size()), name.data(), seed.empty() ? "" : fmt::format(" = {} (set: it seeds this toggle, not the saved setting)", seed).c_str());
			}
			ImGui::EndDisabled();
		}
		ImGui::TreePop();
	}
	if (!ImGui::TreeNodeEx("Advanced"))
		return;
	drawTools();
	if (ImGui::TreeNodeEx("This frame", ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto pipelineOf = [&](bool a_interior) { return DCLF::CpuPipelineFor(active, a_interior) ? (active.cpuRecord ? "cpu parallel" : "cpu serial") : "gpu"; };
		std::string flags = fmt::format("Active: cull {}, pipeline {} (interiors {}, exteriors {})", active.cullMode,
			active.pipelineMode == DCLF::PipelineMode::Manual ? "manual" : "auto", pipelineOf(true), pipelineOf(false));
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
	ImGui::TreePop();
}

void DrawcallLimitFix::SaveSettings(json& o_json)
{
	// Not installed: nothing was seeded, so what was loaded is kept as it was.
	if (!installed) {
		o_json = savedSettings;
		return;
	}
	const auto& requested = DCLF::Toggles::Get().Requested();
	json toggles = json::object();
	for (const auto& toggle : DCLF::ToggleTable())
		toggles[std::string(toggle.name)] = DCLF::ToggleValue(requested, toggle);
	o_json["Toggles"] = std::move(toggles);
	o_json["CullMode"] = requested.cullMode;
	o_json["Pipeline"] = requested.pipelineMode == DCLF::PipelineMode::Manual ? "manual" : "auto";
	o_json["InteriorPipeline"] = requested.interiorCpu ? "cpu" : "gpu";
	o_json["ExteriorPipeline"] = requested.exteriorCpu ? "cpu" : "gpu";
	o_json["CompatibilityMonitor"] = DCLF::Diagnostics::PlayerParity::Requested();
	savedSettings = o_json;
}

void DrawcallLimitFix::LoadSettings(json& o_json)
{
	savedSettings = o_json;
	if (installed)
		ApplySavedSettings();
}

void DrawcallLimitFix::ApplySavedSettings()
{
	if (!savedSettings.is_object())
		return;
	auto& requested = DCLF::Toggles::Get().Requested();
	std::string applied;
	if (const auto toggles = savedSettings.find("Toggles"); toggles != savedSettings.end() && toggles->is_object())
		for (const auto& toggle : DCLF::ToggleTable()) {
			const auto& seed = toggle.member ? DCLF::SwitchValue(toggle.seed) : DCLF::SwitchValue(toggle.registeredSeed);
			const auto saved = toggles->find(std::string(toggle.name));
			if (!seed.empty() || saved == toggles->end() || !saved->is_boolean() || saved->get<bool>() == DCLF::ToggleValue(requested, toggle))
				continue;
			DCLF::SetToggleValue(requested, toggle, saved->get<bool>());
			applied += fmt::format("{}{} {}", applied.empty() ? "" : ", ", toggle.name, saved->get<bool>() ? 1 : 0);
		}
	if (const auto cull = savedSettings.find("CullMode"); cull != savedSettings.end() && cull->is_number_unsigned() && DCLF::SwitchValue(DCLF::Switch::Cull).empty() &&
														  cull->get<std::uint32_t>() <= 2 && cull->get<std::uint8_t>() != requested.cullMode) {
		requested.cullMode = cull->get<std::uint8_t>();
		applied += fmt::format("{}cull {}", applied.empty() ? "" : ", ", requested.cullMode);
	}
	// The pipeline choice, unless CS_DCLF_PIPELINE set it.
	if (DCLF::SwitchValue(DCLF::Switch::CpuPipeline).empty()) {
		const auto stringOf = [&](const char* a_key) {
			const auto found = savedSettings.find(a_key);
			return found != savedSettings.end() && found->is_string() ? found->get<std::string>() : std::string();
		};
		const auto mode = stringOf("Pipeline"), interior = stringOf("InteriorPipeline"), exterior = stringOf("ExteriorPipeline");
		const auto before = std::tuple(requested.pipelineMode, requested.interiorCpu, requested.exteriorCpu);
		if (mode == "auto" || mode == "manual")
			requested.pipelineMode = mode == "manual" ? DCLF::PipelineMode::Manual : DCLF::PipelineMode::Auto;
		if (interior == "cpu" || interior == "gpu")
			requested.interiorCpu = interior == "cpu";
		if (exterior == "cpu" || exterior == "gpu")
			requested.exteriorCpu = exterior == "cpu";
		if (std::tuple(requested.pipelineMode, requested.interiorCpu, requested.exteriorCpu) != before)
			applied += fmt::format("{}pipeline {} (interiors {}, exteriors {})", applied.empty() ? "" : ", ", requested.pipelineMode == DCLF::PipelineMode::Manual ? "manual" : "auto",
				requested.interiorCpu ? "cpu" : "gpu", requested.exteriorCpu ? "cpu" : "gpu");
	}
	if (const auto monitor = savedSettings.find("CompatibilityMonitor"); monitor != savedSettings.end() && monitor->is_boolean() &&
																		 DCLF::SwitchValue(DCLF::Switch::PlayerParity).empty() && monitor->get<bool>() != DCLF::Diagnostics::PlayerParity::Requested()) {
		DCLF::Diagnostics::PlayerParity::Request(monitor->get<bool>());
		applied += fmt::format("{}compatibility monitor {}", applied.empty() ? "" : ", ", monitor->get<bool>() ? 1 : 0);
	}
	if (!applied.empty())
		logger::info("[DCLF] from the saved settings (where no switch is set): {}", applied);
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

namespace
{
	/** @brief SceneWriterFramework's Write, when the provider serves it (ABI minor 2); false: the caller stores. */
	bool SceneWrite(std::uint32_t a_kind, void* a_object, std::uint64_t a_value)
	{
		const auto* api = DCLF::SceneWriter::Api();
		return api && api->abiMinor >= 2 && api->size > offsetof(SWF_Api, Write) && api->Write && api->Write(a_kind, a_object, a_value) == SWF_OK;
	}
}

void DrawcallLimitFix::WriteHidden(RE::NiAVObject* a_object, bool a_hidden)
{
	if (a_object && !SceneWrite(SWF_WRITE_HIDDEN, a_object, a_hidden ? 1 : 0))
		a_object->SetAppCulled(a_hidden);
}

void DrawcallLimitFix::WriteShaderProperty(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property)
{
	if (a_geometry && !SceneWrite(SWF_WRITE_SHADER_PROPERTY, a_geometry, reinterpret_cast<std::uintptr_t>(a_property)))
		a_geometry->GetGeometryRuntimeData().shaderProperty = RE::NiPointer(a_property);
}

void DrawcallLimitFix::WriteAlphaFlags(RE::NiAlphaProperty* a_alpha, std::uint16_t a_flags)
{
	if (a_alpha && !SceneWrite(SWF_WRITE_ALPHA_FLAGS, a_alpha, a_flags))
		a_alpha->alphaFlags = a_flags;
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
	// With the CPU pipeline's frames, a point retires once its frames' signals did too (FrameRetirement.h).
	DCLF::NoteGraphSubmissionsRetired(a_submissions);
}

#else  // !CS_HAS_DCLF: built without the render graph or ORGModuleServices; nothing is installed and every entry point is inert.

#	include "RenderGraph/RenderGraphRuntime.h"

void DrawcallLimitFix::PostLoad() {}
void DrawcallLimitFix::DataLoaded() {}
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
void DrawcallLimitFix::SaveSettings(json& o_json) { o_json = savedSettings; }
void DrawcallLimitFix::LoadSettings(json& o_json) { savedSettings = o_json; }
void DrawcallLimitFix::ApplySavedSettings() {}
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
void DrawcallLimitFix::RenderPrecipitationMask(RE::Precipitation* a_precip, RE::BSGeometry* a_precipObject, bool)
{
	a_precip->SetupMask();
	auto* particleShaderProperty = netimmerse_cast<RE::BSParticleShaderProperty*>(a_precipObject->GetGeometryRuntimeData().shaderProperty.get());
	globals::profiler->BeginPass("Skylighting::PrecipMask");
	a_precip->RenderMask((RE::BSParticleShaderRainEmitter*)(particleShaderProperty->particleEmitter));
	globals::profiler->EndPass();
}
void DrawcallLimitFix::RenderPrecipitationAtShadowMaps() {}
void DrawcallLimitFix::NoteMaterialWritten(const RE::BSShaderMaterial*) {}
void DrawcallLimitFix::WriteHidden(RE::NiAVObject* a_object, bool a_hidden)
{
	if (a_object)
		a_object->SetAppCulled(a_hidden);
}
void DrawcallLimitFix::WriteShaderProperty(RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property)
{
	if (a_geometry)
		a_geometry->GetGeometryRuntimeData().shaderProperty = RE::NiPointer(a_property);
}
void DrawcallLimitFix::WriteAlphaFlags(RE::NiAlphaProperty* a_alpha, std::uint16_t a_flags)
{
	if (a_alpha)
		a_alpha->alphaFlags = a_flags;
}
void DrawcallLimitFix::PrecompileShader(const RE::BSShader&, bool, std::uint32_t) {}
void DrawcallLimitFix::OnMap(ID3D11Resource*, void*) {}
void DrawcallLimitFix::OnUnmap(ID3D11Resource*) {}
void DrawcallLimitFix::OnUpdateSubresource(ID3D11Resource*, const D3D11_BOX*, const void*) {}
bool DrawcallLimitFix::TargetDumpArmed() { return false; }
void DrawcallLimitFix::TargetDumpPoint(const char*, ID3D11Texture2D*) {}
void DrawcallLimitFix::OnGraphSubmissionsRetired(std::uint64_t) {}

#endif
