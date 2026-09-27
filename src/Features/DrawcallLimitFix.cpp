#include "DrawcallLimitFix.h"

#include "Deferred.h"
#include "DrawcallLimitFix/AsyncWorker.h"
#include "DrawcallLimitFix/CaptureParity.h"
#include "DrawcallLimitFix/ConstantEvaluator.h"
#include "DrawcallLimitFix/DecalProbe.h"
#include "DrawcallLimitFix/SkinProbe.h"
#include "DrawcallLimitFix/NativeProbe.h"
#include "DrawcallLimitFix/MaterialSources.h"
#include "DrawcallLimitFix/TreeTrace.h"
#include "DrawcallLimitFix/DrawPipelines.h"
#include "DrawcallLimitFix/FaceSnapshots.h"
#include "DrawcallLimitFix/SunAccumulation.h"
#include "DrawcallLimitFix/PrimaryCull.h"
#include "Features/Skylighting.h"

#include <filesystem>
#include <fstream>
#include "DrawcallLimitFix/GpuResources.h"
#include "DrawcallLimitFix/GpuTextures.h"
#include "DrawcallLimitFix/IndirectDraws.h"
#include "DrawcallLimitFix/ShaderPrograms.h"
#include "DrawcallLimitFix/ShadowProbe.h"
#include "DrawcallLimitFix/ShadowViews.h"
#include "DrawcallLimitFix/Toggles.h"
#include "DrawcallLimitFix/PassCapture.h"
#include "DrawcallLimitFix/SceneStore.h"
#include "DrawcallLimitFix/SceneTracker.h"
#include "DrawcallLimitFix/Switches.h"
#include "DrawcallLimitFix/VolumetricProbe.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "State.h"
#include "TerrainBlending.h"
#include "VolumetricShadows.h"

namespace
{
	bool StatsEnabled()
	{
		static const bool enabled = [] {
			return DCLF::SwitchEnabled("CS_DCLF_STATS");
		}();
		return enabled;
	}

	constexpr std::uint32_t kReportInterval = 300;

	/**
	 * @brief Test switch CS_DCLF_TEST_COMMANDS="<frame>:<console command>;...": runs each console command
	 * once the main pass has run that many frames (loading screens do not count), on the main thread.
	 * Used to take coverage runs to other cells from the auto-loaded save, e.g.
	 * "600:coc WhiterunDragonsreach;1500:coc BleakFallsBarrow01".
	 */
	class TestCommands
	{
	public:
		TestCommands()
		{
			// CS_DCLF_TEST_MOVE carries the player faster than it can survive (falls, collisions): god mode
			// first, once, before anything else runs.
			if (!DCLF::SwitchValue("CS_DCLF_TEST_MOVE").empty())
				commands.push_back({ 1, "tgm" });
			const std::string commandList = DCLF::SwitchValue("CS_DCLF_TEST_COMMANDS");
			std::string_view text = commandList;
			if (text.empty())
				return;
			while (!text.empty()) {
				const auto end = text.find(';');
				const auto item = text.substr(0, end);
				const auto colon = item.find(':');
				if (colon != std::string_view::npos)
					commands.push_back({ static_cast<std::uint32_t>(std::strtoul(std::string(item.substr(0, colon)).c_str(), nullptr, 10)), std::string(item.substr(colon + 1)) });
				text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			}
		}

		/**
		 * @brief Advances the counter and runs whatever is due.
		 *
		 * The counter lives here rather than being SceneStore's frame number, because the commands have to
		 * fire at the same point in a run whether or not the feature is installed. Driven off SceneStore's
		 * frame, a CS_DCLF=0 run never ran them at all, so a "baseline" pinned to a given hour was in fact
		 * whatever hour the save happened to be at - which is precisely the drift that makes screenshot
		 * comparisons worthless. Loading screens do not count, so the two runs stay in step.
		 */
		void OnFrame()
		{
			if (DCLF::SceneStore::IsLoadingScreenUp())
				return;
			const std::uint32_t a_frame = ++frame;
			while (next < commands.size() && a_frame >= commands[next].frame) {
				auto command = commands[next++].command;
				logger::info("[DCLF] test command at frame {}: {}", a_frame, command);
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([command] {
						const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
						if (auto* script = factory ? static_cast<RE::Script*>(factory->Create()) : nullptr) {
							script->SetCommand(command);
							script->CompileAndRun(nullptr);
							delete script;
						}
					});
				}
			}
		}

	private:
		struct Command
		{
			std::uint32_t frame;
			std::string command;
		};
		std::vector<Command> commands;
		std::size_t next = 0;
		std::uint32_t frame = 0;
	};

	/**
	 * @brief [TEMP] Test switch CS_DCLF_TEST_HARVEST=<frame>: at that frame (loading screens not counted), the player
	 * activates the unharvested flora and trees within 4000 units whose produce an NiSwitchNode selects (the root's
	 * first child, two children). Each harvest stores the switch's index in FUN_1401e8ef0, one of the stores the switch
	 * events patch (dclf-cull-job-elimination.md, "Phase 3"). 150 frames later the same switches select child 0 again
	 * through the patched stores' handler with no update pass after it, as the tree manager's stores leave a switch, so
	 * the walk brings the child up to date (SceneStore::CatchUpSwitch).
	 */
	class TestHarvest
	{
	public:
		TestHarvest()
		{
			if (const auto value = DCLF::SwitchValue("CS_DCLF_TEST_HARVEST"); !value.empty())
				at = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
		}

		void OnFrame()
		{
			if (!at || DCLF::SceneStore::IsLoadingScreenUp())
				return;
			++frame;
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks)
				return;
			if (frame == at + 150) {
				tasks->AddTask([this] {
					for (auto& node : harvested) {
						auto* switchNode = node->AsSwitchNode();
						DCLF::SceneStore::SwitchState before, after;
						DCLF::SceneStore::ReadSwitch(*switchNode, before);
						DCLF::SceneStore::TestSwitchStore(switchNode, 0);
						DCLF::SceneStore::ReadSwitch(*switchNode, after);
						logger::info("[DCLF][TEMP] test harvest: switch {:#x} back to child 0: index {} -> {}, revID {}, childRevID[0] {}",
							reinterpret_cast<std::uintptr_t>(switchNode), before.index, after.index, after.revID, after.childRevID ? after.childRevID[0] : 0u);
					}
					harvested.clear();
				});
				return;
			}
			if (frame != at)
				return;
			tasks->AddTask([this] {
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* tes = RE::TES::GetSingleton();
				if (!player || !tes)
					return;
				auto switchOf = [](RE::TESObjectREFR* a_ref) -> RE::NiSwitchNode* {
					auto* root = a_ref->Get3D();
					auto* node = root ? root->AsNode() : nullptr;
					if (!node || node->GetChildren().empty() || !node->GetChildren()[0])
						return nullptr;
					auto* switchNode = node->GetChildren()[0]->AsSwitchNode();
					return switchNode && switchNode->GetChildren().size() == 2 ? switchNode : nullptr;
				};
				std::vector<RE::TESObjectREFR*> refs;
				tes->ForEachReferenceInRange(player, 4000.0f, [&](RE::TESObjectREFR* a_ref) {
					const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
					if (base && (base->GetFormType() == RE::FormType::Flora || base->GetFormType() == RE::FormType::Tree) &&
						!(a_ref->formFlags & RE::TESObjectREFR::RecordFlags::kHarvested) && switchOf(a_ref) && refs.size() < 8)
						refs.push_back(a_ref);
					return RE::BSContainer::ForEachResult::kContinue;
				});
				logger::info("[DCLF][TEMP] test harvest: {} harvestable references with a produce switch in range", refs.size());
				for (auto* ref : refs) {
					auto* switchNode = switchOf(ref);
					DCLF::SceneStore::SwitchState before, after;
					DCLF::SceneStore::ReadSwitch(*switchNode, before);
					ref->ActivateRef(player, 0, nullptr, 1, false);
					DCLF::SceneStore::ReadSwitch(*switchNode, after);
					logger::info("[DCLF][TEMP] test harvest: {:08X} '{}' switch {:#x}: index {} -> {}, revID {} -> {}", ref->GetFormID(),
						ref->GetBaseObject()->GetName(), reinterpret_cast<std::uintptr_t>(switchNode), before.index, after.index, before.revID, after.revID);
					if (switchNode->GetFlags().none(RE::NiAVObject::Flag::kHidden) && (after.flags & 1))
						harvested.emplace_back(switchNode);
				}
			});
		}

	private:
		std::uint32_t at = 0;
		std::uint32_t frame = 0;
		std::vector<RE::NiPointer<RE::NiAVObject>> harvested;
	};

	/**
	 * @brief Test switch CS_DCLF_TEST_TOGGLE="<off frame>:<on frame>[:<off frame>:<on frame>...]": flips the
	 * feature's menu toggle (the same disabled flag the feature list writes) at those frames, loading screens not
	 * counted, to exercise switching DCLF off and back on in a running session: off at the first, on at the
	 * second, and so on alternately.
	 */
	class TestToggle
	{
	public:
		TestToggle()
		{
			const std::string value = DCLF::SwitchValue("CS_DCLF_TEST_TOGGLE");
			std::size_t start = 0;
			while (start < value.size()) {
				const auto colon = value.find(':', start);
				const auto part = value.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
				if (const auto f = static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 10)))
					frames.push_back(f);
				if (colon == std::string::npos)
					break;
				start = colon + 1;
			}
		}

		void OnFrame(const std::string& a_feature)
		{
			if (frames.empty() || DCLF::SceneStore::IsLoadingScreenUp())
				return;
			++frame;
			for (std::size_t i = 0; i < frames.size(); ++i) {
				if (frame != frames[i])
					continue;
				const bool off = (i % 2) == 0;
				logger::info("[DCLF] test toggle at frame {}: {}", frame, off ? "off" : "on");
				globals::state->SetFeatureDisabled(a_feature, off);
			}
		}

	private:
		std::vector<std::uint32_t> frames;
		std::uint32_t frame = 0;
	};

	/**
	 * @brief Test switch CS_DCLF_TEST_TURN="<start frame>:<end frame>:<degrees per frame>": turns the player's
	 * heading every frame between the two (loading screens not counted), so a scripted run has steady camera
	 * motion. Several ranges may be given, separated by ';'.
	 */
	class TestTurn
	{
	public:
		TestTurn()
		{
			Parse(DCLF::SwitchValue("CS_DCLF_TEST_TURN"), ranges);
			Parse(DCLF::SwitchValue("CS_DCLF_TEST_MOVE"), moves);
		}

		void OnFrame()
		{
			if ((ranges.empty() && moves.empty()) || DCLF::SceneStore::IsLoadingScreenUp())
				return;
			++frame;
			// CS_DCLF_TEST_MOVE: the player carried forward along its heading, that many units a frame, so that
			// objects cross their fade distances in view (a turn alone never fades anything).
			for (const auto& move : moves) {
				if (frame < move.start || frame >= move.end)
					continue;
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([units = move.degrees] {
						if (auto* player = RE::PlayerCharacter::GetSingleton()) {
							auto position = player->GetPosition();
							const float heading = player->GetAngleZ();
							position.x += std::sin(heading) * units;
							position.y += std::cos(heading) * units;
							player->SetPosition(position, true);
						}
					});
				}
			}
			for (const auto& range : ranges) {
				if (frame < range.start || frame >= range.end)
					continue;
				const float radians = range.degrees * 0.017453292f;
				if (auto* tasks = SKSE::GetTaskInterface()) {
					tasks->AddTask([radians] {
						if (auto* player = RE::PlayerCharacter::GetSingleton())
							player->SetHeading(player->GetAngleZ() + radians);
					});
				}
			}
		}

	private:
		struct Range
		{
			std::uint32_t start = 0, end = 0;
			float degrees = 0.0f;
		};
		static void Parse(const std::string& a_value, std::vector<Range>& a_out)
		{
			std::string_view text = a_value;
			while (!text.empty()) {
				const auto end = text.find(';');
				const std::string item(text.substr(0, end));
				Range range;
				if (std::sscanf(item.c_str(), "%u:%u:%f", &range.start, &range.end, &range.degrees) == 3)
					a_out.push_back(range);
				text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			}
		}
		std::vector<Range> ranges;
		std::vector<Range> moves;  // CS_DCLF_TEST_MOVE="start:end:units per frame;..."
		std::uint32_t frame = 0;
	};

	// Render-thread CPU spent on scene capture, averaged over a report interval.
	struct CaptureTiming
	{
		double eventsMs = 0.0;
		double buildMs = 0.0;  // the accumulate phase, at EarlyPrepass
		double buildMaxMs = 0.0;
		double sceneMs = 0.0;  // the scene phase, before the shadow maps
		double sceneMaxMs = 0.0;
		std::uint32_t frames = 0;
	} timing;

	DCLF::TargetFormats CurrentTargetFormats()
	{
		DCLF::TargetFormats formats;
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

	double MillisecondsSince(std::chrono::steady_clock::time_point a_start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_start).count();
	}
}

void DrawcallLimitFix::PostPostLoad()
{
	if (REL::Module::IsVR()) {
		logger::info("[DCLF] Not supported on VR; scene tracking stays off");
		return;
	}
	// CS_DCLF=0 is the off switch every gate in this work compares against, and until now nothing read it:
	// the switch appeared in the summary line below and nowhere else, so runs labelled "CS_DCLF=0 baseline"
	// had the feature fully installed and were not baselines at all. Unset still means on, which is the
	// behaviour everything else here was built against.
	if (DCLF::SwitchValue("CS_DCLF") == "0") {
		logger::info("[DCLF] CS_DCLF=0; the feature stays off and the game renders natively");
		return;
	}
	DCLF::SceneTracker::Get().Install();
	DCLF::SceneStore::InstallSceneEvents();
	// Capture at registration, the foundation for static ownership: withholding a pass from the batch
	// renderer removes the very data the tables are built from today, so the capture has to prove itself
	// first (it claims nothing and withholds nothing yet).
	DCLF::PassCapture::Get().Install();
	DCLF::MaterialSources::Install();
	DCLF::ShadowProbe::Get().Install();
	DCLF::VolumetricProbe::Get().Install();
	DCLF::FaceSnapshots::Get().Install();
	DCLF::SunAccumulation::Get().Install();
	DCLF::PrimaryCull::Get().Install();
	Hooks::Install();
	installed = true;
	// The switches this process actually sees, once. Several reports below are gated on them, so without
	// this a silent log is indistinguishable from a switch that never reached the game - which is exactly
	// what happened when they came from the environment alone (see Switches.h).
	logger::info("[DCLF] switches: {}", DCLF::SwitchSummary());
	if (const auto reduced = DCLF::ReducedFeatures(); reduced.empty())
		logger::info("[DCLF] featureset: full (every feature on; the menu can still turn some off live)");
	else
		logger::warn("[DCLF] featureset: REDUCED by {}; this run does not exercise DCLF's full featureset", reduced);
}

void DrawcallLimitFix::SetupResources()
{
	DCLF::reuseKeptStorage = DCLF::SwitchValue("CS_DCLF_CPU_STORAGE_REUSE") != "0";
	if (installed && RenderGraphRuntime::Get().IsActive())
		DCLF::ShaderPrograms::Get().StartPrecompile();
	// Runs right after the render graph tried to adopt DXVK's device (State::SetupResources). The hooks went in
	// at PostPostLoad, before any device existed; without the graph there is nothing to draw with, and left
	// on, DCLF would track, classify and skip for nothing while the frame silently stays native. So it is
	// forced off here, with the reason in the log and in the menu instead of only a line of zeroes in the stats.
	if (!installed || RenderGraphRuntime::Get().IsActive())
		return;
	unavailableReason = RenderGraphRuntime::Get().GetDisabledReason();
	installed = false;
	DCLF::SceneTracker::Get().Stop();
	DCLF::PassCapture::Get().SetBypassed(true);
	logger::warn("[DCLF] Forced off: the render graph is unavailable ({}); the game renders natively", unavailableReason);
}

void DrawcallLimitFix::Reset()
{
	// The test commands run before the installed check, so a CS_DCLF=0 baseline reaches the same place at
	// the same in-game hour as the run it is compared against.
	static TestCommands testCommands;
	testCommands.OnFrame();
	static TestToggle testToggle;
	testToggle.OnFrame(GetShortName());
	static TestTurn testTurn;
	testTurn.OnFrame();
	static TestHarvest testHarvest;
	testHarvest.OnFrame();

	// Every Present, in menus too: the tracker's queue holds references to attached subtrees and
	// must not grow while the world is not rendered.
	if (!installed)
		return;
	const auto start = std::chrono::steady_clock::now();
	// A worker job the frame never joined must not cross into the next frame's tables.
	DCLF::SceneStore::Get().JoinScenePhase();
	DCLF::PrimaryCull::Get().EndFrame();
	DCLF::IndirectDraws::Get().EndFrame();
	DCLF::SceneStore::Get().ProcessEvents();
	timing.eventsMs += MillisecondsSince(start);
	// The menu's toggle, between frames. Scene events keep flowing above while off, so the tracked set is
	// current the moment it comes back on.
	UpdateActive();
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
	if (!a_active) {
		DCLF::SceneStore::Get().AbandonSceneJob();
		DCLF::IndirectDraws::Get().DrainAsync();
	}
	auto& capture = DCLF::PassCapture::Get();
	// Off: every pass reaches the batch renderers again, and no claim outlives the switch. Back on, the claims
	// are empty until the first frame republishes them, so nothing is withheld that DCLF has not drawn.
	capture.SetBypassed(!a_active);
	capture.ClearFrameClaims();
	capture.PublishClaims(nullptr);
	for (std::uint32_t mode = 0; mode < DCLF::PassCapture::kShadowModes; ++mode)
		capture.PublishShadowClaims(mode, nullptr);
	if (a_active)
		DCLF::SceneStore::Get().InvalidateVerdicts();
	skipCounters = {};
	logger::info("[DCLF] {} from the menu", a_active ? "Switched on" : "Switched off; the game renders natively");
}

namespace
{
	// CS_DCLF_EARLY_SCENE=0: the scene phase starts at BeforeShadowMaps, as it did before the early hook.
	bool EarlySceneEnabled()
	{
		static const bool enabled = [] {
			const char* value = std::getenv("CS_DCLF_EARLY_SCENE");
			return !(value && value[0] == '0');
		}();
		return enabled;
	}
}

std::int64_t DrawcallLimitFix::Hooks::Main_Draw_Early::thunk(void* a_main)
{
	const auto result = func(a_main);
	auto& feature = globals::features::drawcallLimitFix;
	if (EarlySceneEnabled()) {
		feature.sceneFrameBegun = true;
		feature.BeginSceneFrame();
	}
	return result;
}

bool DrawcallLimitFix::BeginSceneFrame()
{
	// Called every frame, loaded or not: the one place an unload (which stops Reset from being called) is
	// noticed.
	UpdateActive();
	if (!Running())
		return false;
	// The scene half of the tables. From Main::Draw's early hook it starts before the main camera's cull,
	// which is what gives the walk its time on the worker: everything the walk reads is final from Main::Draw
	// on (the world update is done, and the palette update's frame counter moves only at Renderer::End),
	// except what is written between here and BeforeShadowMaps: BSFadeNode::currentFade (the main cull),
	// which the walk's cached verdicts already take up to kCandidateRefreshFrames late; a billboard's rotation
	// (the main cull), which keeps billboards native (SceneStore::FindCategoryNode); and animated texture
	// transforms, which the shadow build reads off the material itself. The accumulator's half follows at
	// EarlyPrepass, once the registration jobs have finished.
	auto& store = DCLF::SceneStore::Get();
	// The frame's toggles, before anything reads them. A change that enters the classification drops the
	// cached verdicts, so the next frame classifies every object under the new switches.
	if (DCLF::Toggles::Get().BeginFrame())
		store.InvalidateVerdicts();
	ScopedPerfEvent event("CS DCLF: scene tables");
	if (DCLF::TreeTrace::Enabled())
		DCLF::TreeTrace::Get().BeforeScene();
	// The scene events of this frame's world update, before the walk reads the tracked set. Present's Reset
	// applies them too, but a cell attached during the update would otherwise be drawn natively for its first
	// frame and join the tables only on the next one (capture parity's "untracked eligible": every such
	// geometry was tracked by its attach event at that frame's Present). Nothing of DCLF's is in flight here,
	// as after Reset: the previous frame's jobs were joined there, and this frame's start below.
	store.AbandonSceneJob();  // (a walk never joined; there is none after Reset, but the tracked set must not move under one)
	const auto eventsStart = std::chrono::steady_clock::now();
	store.ProcessEvents();
	timing.eventsMs += MillisecondsSince(eventsStart);
	// Registration hooks may run on cull workers. They must all read one
	// ownership selection even when an epoch publishes next frame's claims.
	DCLF::PassCapture::Get().SelectLegacyFrameClaims();
	const auto start = std::chrono::steady_clock::now();
	store.BuildFrame(DCLF::SceneStore::Phase::Scene);
	const double sceneMs = MillisecondsSince(start);
	timing.sceneMs += sceneMs;
	timing.sceneMaxMs = std::max(timing.sceneMaxMs, sceneMs);
	return true;
}

void DrawcallLimitFix::BeforeShadowMaps()
{
	if (!std::exchange(sceneFrameBegun, false)) {
		if (!BeginSceneFrame())
			return;
	} else if (!Running()) {
		return;
	}
	ScopedPerfEvent event("CS DCLF: shadow views");
	const auto start = std::chrono::steady_clock::now();
	// The frame's shadow views, in the order the engine is about to render them. Everything downstream -
	// the capture's attribution, the claims, the epochs - identifies a view by this list.
	DCLF::ShadowViews::Get().Rebuild();
	if (DCLF::IndirectDraws::ShadowsEnabled()) {
		DCLF::IndirectDraws::Get().BeginShadowFrame(globals::game::shadowState->GetRuntimeData().posAdjust.getEye());
		// The shadow epoch's build, on the worker, while the engine draws the shadow maps (CS_DCLF_ASYNC).
		DCLF::IndirectDraws::Get().KickShadowBuild();
	}
	const double sceneMs = MillisecondsSince(start);
	timing.sceneMs += sceneMs;
	timing.sceneMaxMs = std::max(timing.sceneMaxMs, sceneMs);
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().OnBeforeShadowMaps();
}

void DrawcallLimitFix::AfterShadowMaps()
{
	RenderGraphRuntime::EpochBodyScope body(RenderGraphRuntime::Segment::ShadowView);
	// The scene walk (CS_DCLF_ASYNC), before anything reads the tables it writes.
	DCLF::SceneStore::Get().JoinScenePhase();
	if (!Running())
		return;
	// The frame's shadow epoch: every view the 0x2A hook captured, drawn in one graph execution.
	DCLF::IndirectDraws::Get().ExecuteShadowFrame();
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().OnAfterShadowMaps();
}

void DrawcallLimitFix::EarlyPrepass()
{
	DCLF::SceneStore::Get().JoinScenePhase();  // normally joined at AfterShadowMaps already
	if (!Running())
		return;

	// The tables are built here, from Main_RenderShadowMaps, rather than in Prepass from StartDeferred.
	// Both DCLF epochs then read the same generation: the Z-prepass runs inside Main_RenderDepth,
	// which is after this and before Prepass, so it used to see the *previous* frame's tables and the
	// per-object visibility verdicts it wrote could not be applied by index in the colour epoch.
	//
	// This is only possible because the accumulator is already complete here - the cull job finishes
	// before the shadow maps (Main::Draw) - and because the tables read the latched accumulator rather
	// than `currentAccumulator`, which is not set this early.
	auto& store = DCLF::SceneStore::Get();
	ScopedPerfEvent event("CS DCLF: accumulator tables and pipelines");
	const auto start = std::chrono::steady_clock::now();
	store.BuildFrame(DCLF::SceneStore::Phase::Accumulate);
	const double buildMs = MillisecondsSince(start);
	timing.buildMs += buildMs;
	timing.buildMaxMs = std::max(timing.buildMaxMs, buildMs);
	++timing.frames;
	// The shadow probe reads this frame's tables (the main pass's kept objects) and the Utility
	// registrations, which are complete now that the thunk has returned.
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().OnFrameBuilt();

	// Phase 2: SPIR-V programs and indirect pipelines for the pipelines drawn this frame (built once, asynchronously).
	auto& programs = DCLF::ShaderPrograms::Get();
	auto& pipelines = DCLF::DrawPipelines::Get();
	if (auto* lighting = DCLF::ConstantEvaluator::Get().GetLightingShader(); lighting && programs.Enabled()) {
		ZoneScopedN("CS.DCLF.Accumulate.Pipelines");
		TracyCZoneN(requestZone, "CS.DCLF.Accumulate.RequestLighting", true);
		const auto& tables = store.GetTables();
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			if (!tables.PipelineUsed(p, store.GetFrame()))
				continue;
			if (const auto* program = programs.Find(tables.pipelines[p], *lighting))
				pipelines.Find(tables.pipelines[p], *program);
		}
		TracyCZoneEnd(requestZone);
		TracyCZoneN(updateZone, "CS.DCLF.Accumulate.PublishPipelines", true);
		programs.Update();
		pipelines.Update();
		TracyCZoneEnd(updateZone);
		TracyCZoneN(lookupZone, "CS.DCLF.Accumulate.PipelineLookups", true);
		// The pipeline lookups an epoch's build reads (Lookups.h): the set index of every pipeline used this
		// frame, its shaders' constant tables and its register usage, after Update has admitted this frame's
		// finished builds. Resolved here, where the GPU is busy with the shadow maps, rather than in the epoch.
		auto& lookups = store.MutableLookups();
		if (lookups.pipelineSetGeneration != pipelines.Generation()) {
			// The set was recreated (a target change): every index a build may hold is stale.
			lookups.pipelineSetGeneration = pipelines.Generation();
			++lookups.generation;
		}
		lookups.pipelines.resize(tables.pipelines.size());
		auto& cache = SIE::ShaderCache::Instance();
		for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
			auto& entry = lookups.pipelines[p];
			if (!tables.PipelineUsed(p, store.GetFrame())) {
				if (entry.setIndex != DCLF::Lookups::kNone)
					entry.version = lookups.NextVersion();
				entry.setIndex = DCLF::Lookups::kNone;
				continue;
			}
			const auto& key = tables.pipelines[p];
			const auto* program = programs.Find(key, *lighting);
			const std::uint32_t setIndex = program ? pipelines.Find(key, *program) : DCLF::DrawPipelines::kNotReady;
			auto* vs = cache.GetVertexShader(*lighting, key.vertexDescriptor);
			auto* ps = cache.GetPixelShader(*lighting, key.pixelDescriptor);
			const std::uint32_t resolved = (setIndex != DCLF::DrawPipelines::kNotReady && vs && ps) ? setIndex : DCLF::Lookups::kNone;
			// Written only where it differs, so an entry's version is new only when it changed.
			bool changed = false;
			if (!(entry.key == key)) {
				entry.key = key;
				entry.setIndex = DCLF::Lookups::kNone;
				entry.shadowMaskIndex = DCLF::Lookups::kNone;
				changed = true;
			}
			if (entry.setIndex != DCLF::Lookups::kNone && entry.setIndex != resolved)
				++lookups.generation;  // a build may hold the old index
			changed |= entry.setIndex != resolved;
			entry.setIndex = resolved;
			if (resolved != DCLF::Lookups::kNone) {
				auto assign = [&](std::vector<std::uint8_t>& a_table, const auto& a_source) {
					if (!std::equal(a_table.begin(), a_table.end(), a_source.begin(), a_source.end())) {
						a_table.assign(a_source.begin(), a_source.end());
						changed = true;
					}
				};
				assign(entry.vsTable, vs->constantTable);
				assign(entry.psTable, ps->constantTable);
				for (std::uint32_t variant = 0; variant < 2; ++variant) {
					const auto& usage = pipelines.Usage(resolved, variant);
					auto& bits = entry.usage[variant];
					changed |= bits.vertexConstants != usage.vertexConstants || bits.pixelConstants != usage.pixelConstants || bits.textures != usage.textures ||
					           bits.samplers != usage.samplers;
					bits.vertexConstants = usage.vertexConstants;
					bits.pixelConstants = usage.pixelConstants;
					bits.textures = usage.textures;
					bits.samplers = usage.samplers;
				}
			}
			if (changed)
				entry.version = lookups.NextVersion();
		}
		TracyCZoneEnd(lookupZone);
	}

	// What the native loop was told to leave to DCLF but DCLF cannot draw this frame goes back to it now,
	// before the depth and main passes: an object DCLF has no bindings for, or whose pipeline is not built.
	if (DCLF::PassCapture::WithholdingEnabled())
		DCLF::PassCapture::Get().HandBackUndrawable(DrawableThisFrame);

	// The shadow views' programs: one Utility build per technique of the frame's casters, per render mode
	// among the views the engine drew. Requested here, beside the Lighting builds, so they are compiled
	// long before a shadow epoch would draw with them.
	if (auto* utility = globals::game::utilityShader; utility && programs.Enabled()) {
		ZoneScopedN("CS.DCLF.Accumulate.ShadowPipelines");
		std::uint32_t modeBits = 0;
		for (const auto& view : DCLF::ShadowViews::Get().All()) {
			// Clamped for cascades and spot lights, the paraboloid warp for point lights; the engine
			// picks the render mode from the light, not from the descriptor (engine notes: shadow maps).
			modeBits |= DCLF::ShadowModeBits(view.kind == DCLF::ShadowViews::Kind::Parabolic ? 0xFu : 0xEu);
		}
		const auto& tables = store.GetTables();
		// The shadow map array the views draw into; its format is what a shadow pipeline is built for.
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		if (auto* renderer = globals::game::renderer) {
			const auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
			// The depth-stencil view's format, not the texture's: the texture is typeless (R16_TYPELESS)
			// and a pipeline is built against the view it renders through (D16_UNORM), as the epoch does.
			if (auto* view = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				view->GetDesc(&desc);
				shadowFormat = desc.Format;
			}
		}
		// Under every rasterizer state the mode's views have drawn with (DrawPipelines::ShadowRasterStateId):
		// the states are only known once a view has been seen, so the first frame's are requested by the epoch.
		for (const auto& key : tables.shadowKeysUsed) {
			for (std::uint32_t mode : { 0xEu, 0xFu }) {
				const std::uint32_t bits = DCLF::ShadowModeBits(mode);
				if (!(modeBits & bits))
					continue;
				const auto* program = programs.FindShadow(key.technique | bits, *utility);
				if (!program)
					continue;
				for (std::uint32_t states = pipelines.ShadowRasterStatesOfMode(mode); states; states &= states - 1) {
					const DCLF::ShadowPipelineKey viewKey{ key.technique | bits, DCLF::WithShadowState(key.rasterFlags, std::countr_zero(states)), key.vertexLayout };
					pipelines.FindShadow(viewKey, *program, shadowFormat);
				}
			}
		}
	}

	if (DCLF::TreeTrace::Enabled())
		DCLF::TreeTrace::Get().AfterAccumulate();

	// The Z-prepass epoch's build, on the worker, from here to the Z-prepass in Main_RenderDepth (CS_DCLF_ASYNC).
	DCLF::IndirectDraws::Get().KickZPrepassBuild();
}

namespace
{
	/**
	 * CS_DCLF_SHADOWMASK_PROBE=x,y: the sun's shadow mask (kSHADOW_MASK) at a screen pixel, read at the start of
	 * the main pass, where the engine has built it and before anything samples it. Every 240 frames, with DCLF
	 * running or not, so one run with CS_DCLF_TEST_TOGGLE gives both sides. The pixel is in the main target's
	 * coordinates and scaled to the mask's size (iShadowMaskQuarter).
	 */
	void ProbeShadowMask(bool a_running)
	{
		static const std::string pixel = DCLF::SwitchValue("CS_DCLF_SHADOWMASK_PROBE");
		if (pixel.empty())
			return;
		static winrt::com_ptr<ID3D11Texture2D> staging;
		static std::uint32_t framesLeft = 0, frames = 0, x = 0, y = 0, bytes = 0;
		static bool stagingRunning = false;
		static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		auto* context = globals::d3d::context;
		if (staging) {
			if (--framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				std::string hex;
				for (std::uint32_t b = 0; b < std::min(bytes, mapped.RowPitch); ++b)
					hex += fmt::format("{:02X}", static_cast<const std::uint8_t*>(mapped.pData)[b]);
				logger::info("[DCLF] shadow mask at mask texel ({}, {}), DCLF {}: {} (format {})", x, y, stagingRunning ? "on" : "off", hex,
					static_cast<std::uint32_t>(format));
				context->Unmap(staging.get(), 0);
			}
			staging = nullptr;
			return;
		}
		if ((frames++ % 240) != 0)
			return;
		const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
		auto* mask = reinterpret_cast<ID3D11Texture2D*>(targets[RE::RENDER_TARGETS::kSHADOW_MASK].texture);
		auto* main = reinterpret_cast<ID3D11Texture2D*>(targets[RE::RENDER_TARGETS::kMAIN].texture);
		if (!mask || !main)
			return;
		D3D11_TEXTURE2D_DESC maskDesc{}, mainDesc{};
		mask->GetDesc(&maskDesc);
		main->GetDesc(&mainDesc);
		const auto sep = pixel.find_first_of(",x");
		if (sep == std::string::npos)
			return;
		// Pixels, or fractions of the screen when both are at most 1.
		double px = std::strtod(pixel.substr(0, sep).c_str(), nullptr);
		double py = std::strtod(pixel.substr(sep + 1).c_str(), nullptr);
		if (px <= 1.0 && py <= 1.0) {
			px *= mainDesc.Width;
			py *= mainDesc.Height;
		}
		x = std::min<std::uint32_t>(static_cast<std::uint32_t>(px * maskDesc.Width / mainDesc.Width), maskDesc.Width - 1);
		y = std::min<std::uint32_t>(static_cast<std::uint32_t>(py * maskDesc.Height / mainDesc.Height), maskDesc.Height - 1);
		D3D11_TEXTURE2D_DESC desc = maskDesc;
		desc.Width = desc.Height = 1;
		desc.MipLevels = desc.ArraySize = 1;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, staging.put())))
			return;
		const D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
		context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, mask, 0, &box);
		format = maskDesc.Format;
		bytes = 8;  // the texel and whatever follows it in the row; the format says how many are the texel
		stagingRunning = a_running;
		framesLeft = 4;
	}
}

namespace
{
	/**
	 * CS_DCLF_SHADOWMAP_PROBE=1: what the frame's directional shadow maps hold, read at the start of the main
	 * pass: per slice of the engine's cascade texture (kSHADOWMAPS_ESRAM), the mean depth and the share of
	 * texels at the clear value (no caster), the same per slice of the volumetric lighting copy
	 * (kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM), and per mip of Volumetric Shadows' copy (one cascade each, the
	 * nearer of the two maps) the mean first moment. Every 240 frames, with DCLF running or not.
	 */
	void ProbeShadowMaps(bool a_running)
	{
		static const bool enabled = DCLF::SwitchEnabled("CS_DCLF_SHADOWMAP_PROBE");
		if (!enabled)
			return;
		struct Pending
		{
			winrt::com_ptr<ID3D11Texture2D> depth, volumetric, vsm;
			D3D11_TEXTURE2D_DESC depthDesc{}, volumetricDesc{}, vsmDesc{};
			bool running = false;
			std::uint32_t framesLeft = 0;
		};
		static std::optional<Pending> pending;
		static std::uint32_t frames = 0;
		auto* context = globals::d3d::context;
		if (pending) {
			if (--pending->framesLeft)
				return;
			std::string text;
			auto summarise = [&](ID3D11Texture2D* a_texture, const D3D11_TEXTURE2D_DESC& desc, const char* a_label) {
				if (!a_texture)
					return;
				for (std::uint32_t slice = 0; slice < desc.ArraySize; ++slice) {
					D3D11_MAPPED_SUBRESOURCE mapped{};
					const auto sub = D3D11CalcSubresource(0, slice, desc.MipLevels);
					if (FAILED(context->Map(a_texture, sub, D3D11_MAP_READ, 0, &mapped)))
						continue;
					double sum = 0;
					std::uint64_t cleared = 0, count = 0;
					for (std::uint32_t row = 0; row < desc.Height; row += 4) {
						const auto* line = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row) * mapped.RowPitch;
						for (std::uint32_t column = 0; column < desc.Width; column += 4) {
							double depth = 0;
							if (desc.Format == DXGI_FORMAT_R16_TYPELESS) {
								depth = reinterpret_cast<const std::uint16_t*>(line)[column] / 65535.0;
							} else {
								const std::uint32_t word = reinterpret_cast<const std::uint32_t*>(line)[column];
								depth = desc.Format == DXGI_FORMAT_R24G8_TYPELESS ? double(word & 0xFFFFFFu) / 16777215.0 : double(std::bit_cast<float>(word));
							}
							sum += depth;
							cleared += depth >= 0.99999 ? 1 : 0;
							++count;
						}
					}
					context->Unmap(a_texture, sub);
					text += fmt::format(" {}slice {}: mean {:.5f}, {:.1f}% clear;", a_label, slice, sum / double(count), 100.0 * double(cleared) / double(count));
				}
			};
			summarise(pending->depth.get(), pending->depthDesc, "");
			summarise(pending->volumetric.get(), pending->volumetricDesc, "volumetric ");
			if (pending->vsm) {
				const auto& desc = pending->vsmDesc;
				for (std::uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(pending->vsm.get(), mip, D3D11_MAP_READ, 0, &mapped)))
						continue;
					const std::uint32_t width = std::max(1u, desc.Width >> mip), height = std::max(1u, desc.Height >> mip);
					double sum = 0;
					for (std::uint32_t row = 0; row < height; ++row) {
						const auto* texels = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row) * mapped.RowPitch);
						for (std::uint32_t column = 0; column < width; ++column)
							sum += texels[column * 2] / 65535.0;
					}
					context->Unmap(pending->vsm.get(), mip);
					text += fmt::format(" VSM mip {}: mean {:.5f};", mip, sum / double(width * height));
				}
			}
			logger::info("[DCLF] shadow maps, DCLF {} (depth format {}):{}", pending->running ? "on" : "off", static_cast<std::uint32_t>(pending->depthDesc.Format), text);
			pending.reset();
			return;
		}
		if ((frames++ % 240) != 0)
			return;
		Pending next;
		next.running = a_running;
		next.framesLeft = 4;
		auto copy = [&](ID3D11Texture2D* a_source, winrt::com_ptr<ID3D11Texture2D>& a_staging, D3D11_TEXTURE2D_DESC& a_desc) {
			if (!a_source)
				return;
			a_source->GetDesc(&a_desc);
			D3D11_TEXTURE2D_DESC desc = a_desc;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (SUCCEEDED(globals::d3d::device->CreateTexture2D(&desc, nullptr, a_staging.put())))
				context->CopyResource(a_staging.get(), a_source);
		};
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		copy(reinterpret_cast<ID3D11Texture2D*>(depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM].texture), next.depth, next.depthDesc);
		copy(reinterpret_cast<ID3D11Texture2D*>(depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM].texture), next.volumetric,
			next.volumetricDesc);
		if (globals::features::volumetricShadows.loaded)
			copy(globals::features::volumetricShadows.shadowCopyTexture, next.vsm, next.vsmDesc);
		pending = std::move(next);
	}
}

namespace
{
	/** @brief CS_DCLF_SKYLIGHT_PARITY: the two renders of one map, kept and compared once readable. */
	struct SkyParity
	{
		std::array<winrt::com_ptr<ID3D11Texture2D>, 2> staging;
		D3D11_TEXTURE2D_DESC desc{};
		std::uint32_t framesLeft = 0;
		bool pending = false;
		bool diagnose = false;  // the next DCLF map lists what it draws that the engine did not register
	};
	SkyParity skyParity;
	std::uint32_t skyNativeFrames = 0;  // Skylighting maps left to the engine (DCLF not ready), per report interval

	bool SkyParityEnabled()
	{
		static const bool enabled = DCLF::SwitchEnabled("CS_DCLF_SKYLIGHT_PARITY");
		return enabled;
	}

	void CompareSkyParity()
	{
		auto& parity = skyParity;
		if (!parity.pending || --parity.framesLeft)
			return;
		parity.pending = false;
		auto* context = globals::d3d::context;
		std::array<D3D11_MAPPED_SUBRESOURCE, 2> mapped{};
		if (FAILED(context->Map(parity.staging[0].get(), 0, D3D11_MAP_READ, 0, &mapped[0])))
			return;
		if (FAILED(context->Map(parity.staging[1].get(), 0, D3D11_MAP_READ, 0, &mapped[1]))) {
			context->Unmap(parity.staging[0].get(), 0);
			return;
		}
		// Depth as the texture holds it: 16-bit UNORM (the engine's D16 maps), else the top 24 of 32 bits, else a float.
		const auto format = parity.desc.Format;
		const bool unorm16 = format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_D16_UNORM || format == DXGI_FORMAT_R16_UNORM;
		const bool float32 = format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_D32_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
		auto depthAt = [&](std::uint32_t a_image, std::uint32_t x, std::uint32_t y) {
			const auto* row = static_cast<const std::uint8_t*>(mapped[a_image].pData) + std::size_t(y) * mapped[a_image].RowPitch;
			if (unorm16)
				return reinterpret_cast<const std::uint16_t*>(row)[x] / 65535.0;
			const std::uint32_t word = reinterpret_cast<const std::uint32_t*>(row)[x];
			if (float32)
				return static_cast<double>(std::bit_cast<float>(word));
			return (word & 0xFFFFFF) / 16777215.0;
		};
		std::uint64_t texels = 0, equal = 0, dclfFarther = 0, dclfNearer = 0, engineFar = 0, dclfFar = 0, bigDiff = 0;
		double maxDiff = 0.0, sumDiff = 0.0;
		for (std::uint32_t y = 0; y < parity.desc.Height; ++y)
			for (std::uint32_t x = 0; x < parity.desc.Width; ++x) {
				const double engine = depthAt(0, x, y), dclf = depthAt(1, x, y);
				++texels;
				engineFar += engine >= 1.0 ? 1 : 0;
				dclfFar += dclf >= 1.0 ? 1 : 0;
				const double diff = std::abs(engine - dclf);
				if (diff == 0.0) {
					++equal;
					continue;
				}
				(dclf > engine ? dclfFarther : dclfNearer) += 1;
				bigDiff += diff > 1.0 / 256.0 ? 1 : 0;
				sumDiff += diff;
				maxDiff = std::max(maxDiff, diff);
			}
		// Which occluders the differing texels fall under: ranked by the texels in their footprint DCLF drew nearer.
		{
			auto& footprints = DCLF::IndirectDraws::Get().skyFootprints;
			std::vector<std::pair<std::uint64_t, std::string>> ranked;
			for (const auto& f : footprints) {
				std::uint64_t nearer = 0, inside = 0;
				for (std::int32_t y = std::max(f.y0, 0); y <= std::min<std::int32_t>(f.y1, parity.desc.Height - 1); ++y)
					for (std::int32_t x = std::max(f.x0, 0); x <= std::min<std::int32_t>(f.x1, parity.desc.Width - 1); ++x) {
						++inside;
						nearer += depthAt(0, x, y) - depthAt(1, x, y) > 1.0 / 256.0 ? 1 : 0;
					}
				if (nearer)
					ranked.emplace_back(nearer, fmt::format("{} texels of {} in ({} {})-({} {}): {}", nearer, inside, f.x0, f.y0, f.x1, f.y1, f.label));
			}
			std::sort(ranked.rbegin(), ranked.rend());
			for (std::size_t i = 0; i < ranked.size() && i < 12; ++i)
				logger::info("[DCLF][TEMP]   nearer under {}", ranked[i].second);
			footprints.clear();
		}
		// A map that differs over more than 5% of its texels: both written out (16-bit PGM, the depth scaled to 0..65535).
		if (bigDiff * 20 > texels) {
			static std::uint32_t dumps = 0;
			if (dumps < 4) {
				const auto directory = std::filesystem::path(DCLF::SwitchValue("CS_DCLF_SKYLIGHT_DUMP_DIR").empty() ? "." : DCLF::SwitchValue("CS_DCLF_SKYLIGHT_DUMP_DIR"));
				for (std::uint32_t image = 0; image < 2; ++image) {
					std::ofstream out(directory / fmt::format("sky-parity-{}-{}.pgm", dumps, image ? "dclf" : "engine"), std::ios::binary);
					out << "P5\n" << parity.desc.Width << " " << parity.desc.Height << "\n65535\n";
					for (std::uint32_t y = 0; y < parity.desc.Height; ++y)
						for (std::uint32_t x = 0; x < parity.desc.Width; ++x) {
							const auto value = static_cast<std::uint16_t>(std::clamp(depthAt(image, x, y), 0.0, 1.0) * 65535.0);
							const char bytes[2] = { static_cast<char>(value >> 8), static_cast<char>(value & 0xFF) };
							out.write(bytes, 2);
						}
				}
				logger::info("[DCLF][TEMP] Skylighting occlusion parity: map {} written to {}", dumps, directory.string());
				++dumps;
			}
		}
		context->Unmap(parity.staging[0].get(), 0);
		context->Unmap(parity.staging[1].get(), 0);
		logger::info("[DCLF][TEMP] Skylighting occlusion parity: {}x{} format {}: {} texels equal of {}, DCLF farther {} nearer {} ({} by more than 1/256), max diff {:.6f}, mean diff {:.6f}; clear texels engine {} DCLF {}",
			parity.desc.Width, parity.desc.Height, static_cast<std::uint32_t>(format), equal, texels, dclfFarther, dclfNearer, bigDiff, maxDiff,
			texels - equal ? sumDiff / double(texels - equal) : 0.0, engineFar, dclfFar);
	}
}

bool DrawcallLimitFix::SkyOcclusionReady()
{
	if (!Running() || !DCLF::SceneStore::SkyOcclusionEnabled())
		return false;
	const bool ready = DCLF::IndirectDraws::Get().SkyOcclusionReady();
	skyNativeFrames += ready ? 0 : 1;
	return ready;
}

void DrawcallLimitFix::DrawSkyOcclusion()
{
	DCLF::IndirectDraws::Get().ExecuteSkyOcclusion(std::exchange(skyParity.diagnose, false));
}

bool DrawcallLimitFix::SkyOcclusionParityFrame()
{
	static std::uint32_t frames = 0;
	if (!SkyParityEnabled() || skyParity.pending || (frames++ % 120) != 60)
		return false;
	(void)DCLF::SunAccumulation::Get().TakeSkyRegistrations();  // this map's registrations only
	return true;
}

void DrawcallLimitFix::CopySkyOcclusion(std::uint32_t a_stage)
{
	auto* texture = globals::features::skylighting.texOcclusion ? globals::features::skylighting.texOcclusion->resource.get() : nullptr;
	if (!texture || a_stage > 1)
		return;
	auto& parity = skyParity;
	if (a_stage == 0) {
		texture->GetDesc(&parity.desc);
		D3D11_TEXTURE2D_DESC desc = parity.desc;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& staging : parity.staging) {
			staging = nullptr;
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, staging.put())))
				return;
		}
	}
	if (!parity.staging[a_stage])
		return;
	parity.diagnose = a_stage == 0;
	globals::d3d::context->CopyResource(parity.staging[a_stage].get(), texture);
	if (a_stage == 1) {
		parity.pending = true;
		parity.framesLeft = 4;
	}
}

void DrawcallLimitFix::Prepass()
{
	CompareSkyParity();
	ProbeShadowMask(Running());
	ProbeShadowMaps(Running());
	if (DCLF::VolumetricProbe::Enabled())
		DCLF::VolumetricProbe::Get().EndFrame(Running());
	if (!Running())
		return;

	auto& store = DCLF::SceneStore::Get();
	// The one point in the frame where the main camera's accumulator is identifiable: EarlyPrepass, where
	// the tables are now built, is too early for `currentAccumulator` to be set.
	store.LatchAccumulator();
	// The camera-dependent half of the per-frame constants, now that the main camera's shadow state is
	// current (BuildFrame ran at EarlyPrepass, where it still belonged to the shadow-map camera).
	store.RefreshFrameConstants();
	// The colour epoch's build, on the worker, from here to the epoch (CS_DCLF_ASYNC).
	DCLF::IndirectDraws::Get().KickColourBuild();
	skipStats = skipCounters;
	skipCounters = {};
	if ((store.GetFrame() % kReportInterval) == 1)
		skipSamples.clear();  // collected again for the next report

	const std::uint32_t frame = store.GetFrame();
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().Report(frame, kReportInterval);
	if (DCLF::DecalProbe::Enabled())
		DCLF::DecalProbe::Get().Report(frame, kReportInterval);
	if (DCLF::SkinProbe::Enabled())
		DCLF::SkinProbe::Get().Report(frame, kReportInterval);
	if (DCLF::NativeProbe::Enabled())
		DCLF::NativeProbe::Get().Report(frame, kReportInterval);
	if (DCLF::TreeTrace::Enabled())
		DCLF::TreeTrace::Get().Report(frame, kReportInterval);
	if (DCLF::ShadowProbe::Enabled())
		DCLF::ShadowProbe::Get().Report(frame, kReportInterval);
	DCLF::SunAccumulation::Get().Report(frame, kReportInterval);
	DCLF::PrimaryCull::Get().Report(frame, kReportInterval);

	// The culling's counters, reported whether or not the full statistics are on: they are what says
	// whether GPU culling is running and how much it rejects.
	if ((frame % kReportInterval) == 0) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.cullDrawn || draws.cullRejected || draws.cullOccluded) {
			const std::uint32_t rejected = draws.cullRejected + draws.cullOccluded;
			logger::info("[DCLF] culling: {} draws written, {} of {} tested were rejected ({:.1f}%: {} outside the frustum, {} occluded, {} of those the engine had kept); against the engine: {} it culled were gated out, {} it culled were kept, {} it kept the frustum test rejected{}",
				draws.cullDrawn, rejected, draws.cullTested,
				draws.cullTested ? 100.0 * rejected / draws.cullTested : 0.0,
				draws.cullRejected, draws.cullOccluded, draws.cullOccludedVisible,
				draws.cullEngineCulled, draws.cullRescued, draws.cullFalseNegatives,
				draws.cullFalseNegatives ? " <- FALSE NEGATIVES" : "");
			if (draws.cullRescuedByPhaseTwo || draws.cullDrawnPhaseTwo)
				logger::info("[DCLF] two-phase culling: phase 2 brought back {} objects the stale HZB had rejected, and drew depth for {} of them",
					draws.cullRescuedByPhaseTwo, draws.cullDrawnPhaseTwo);
			if (draws.hzbSampled)
				logger::info("[DCLF] HZB: {} footprints sampled, {} came back all-near (~0), {} all-far (~1)",
					draws.hzbSampled, draws.hzbNear, draws.hzbFar);
			if (draws.hzbSample.valid) {
				const auto& sample = draws.hzbSample;
				logger::info("[DCLF] HZB rejection sample: farthest {:.6f} against nearest {:.6f}, uv ({:.4f} {:.4f})-({:.4f} {:.4f}), mip {}, engine {}",
					sample.farthest, sample.nearestZ, sample.uvMin[0], sample.uvMin[1], sample.uvMax[0], sample.uvMax[1],
					sample.mip, sample.nativeVisible ? "kept it" : "culled it");
			}
		}
	}
	// The bindless record's own gate, reported whenever it is on: it says whether the table the shaders
	// read holds what the constant buffer path would have given them, which no screenshot can say.
	if ((frame % kReportInterval) == 0) {
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		if (draws.recordParityChecks) {
			if (draws.recordParityMismatches)
				logger::warn("[DCLF] record dedup parity MISMATCH: {} of {} rebuilt records differ from their pair's", draws.recordParityMismatches, draws.recordParityChecks);
			else
				logger::info("[DCLF] record dedup parity OK: {} rebuilt records match their pair's byte for byte", draws.recordParityChecks);
		}
		if (draws.bindlessParityChecks) {
			if (draws.bindlessParityMismatches)
				logger::warn("[DCLF] bindless record parity MISMATCH: {} of {} components differ", draws.bindlessParityMismatches, draws.bindlessParityChecks);
			else
				logger::info("[DCLF] bindless record parity OK: {} components match the constant groups", draws.bindlessParityChecks);
		}
	}
	// What the GPU spent on each segment, measured by the graph itself; the menu shows the latest window.
	if ((frame % kReportInterval) == 0)
		RenderGraphRuntime::Get().ReportGpuTimings(kReportInterval, StatsEnabled());
	if ((frame % kReportInterval) == 0 && !StatsEnabled()) {
		timing = {};
		store.ResetTimes();
	}
	if (StatsEnabled() && (frame % kReportInterval) == 0) {
		const auto& stats = store.GetStats();
		std::string reasons;
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				reasons += fmt::format(" {}={}({} drawn)", DCLF::kIneligibleNames[i], stats.ineligible[i], stats.ineligibleDrawn[i]);
		}
		const double frames = std::max(1u, timing.frames);
		// The per-part breakdown appears only under CS_DCLF_PROFILE=1, because that is the only time it is
		// measured. Timing from inside the loop perturbs it, so compare a profiled run's parts against an
		// unprofiled run's total rather than treating the parts as free.
		std::string parts;
		if (DCLF::SceneStore::ProfileEnabled()) {
			for (std::size_t i = 0; i < stats.partMs.size(); ++i)
				parts += fmt::format("{}{} {:.3f}", parts.empty() ? "; by part: " : ", ", DCLF::kBuildPartNames[i], stats.partMs[i] / frames);
			for (std::size_t i = 0; i < stats.accumulatePartMs.size(); ++i)
				parts += fmt::format("{}{} {:.3f}", i ? ", " : "; accumulate by part: ", DCLF::kBuildPartNames[i], stats.accumulatePartMs[i] / frames);
		}
		const auto& shaders = DCLF::ShaderPrograms::Get().GetStats();
		logger::info("[DCLF] SPIR-V programs: {} requested, {} ready ({} stages from cache), {} failed; shadow (Utility): {} requested, {} ready, {} failed",
			shaders.requested, shaders.ready, shaders.fromCache, shaders.failed, shaders.shadowRequested, shaders.shadowReady, shaders.shadowFailed);
		const auto& indirect = DCLF::DrawPipelines::Get().GetStats();
		logger::info("[DCLF] indirect pipelines: {} requested, {} in the set, {} failed; target changes {}; shadow: {} requested, {} in the set, {} failed; "
					 "set versions published {} (waited {} frames), shadow {} (waited {})",
			indirect.requested, indirect.ready, indirect.failed, indirect.targetChanges, indirect.shadowRequested, indirect.shadowReady, indirect.shadowFailed,
			indirect.setPublishes, indirect.setWaits, indirect.shadowSetPublishes, indirect.shadowSetWaits);
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		std::string skipped;
		for (std::size_t i = 0; i < draws.skipped.size(); ++i) {
			if (draws.skipped[i])
				skipped += fmt::format(" {}={}", DCLF::kSkipNames[i], draws.skipped[i]);
		}
		const auto& textures = DCLF::GpuTextures::Get().GetStats();
		logger::info("[DCLF] indirect draws (last frame): {} candidates built from {} binding records, skipped:{} (missing t{} t{} t{} t{}, VS b{:04X} PS b{:04X}); {:.1f} MB uploaded, {:.3f} ms CPU; {} epochs, {} not ready; textures {} cached, rejected {}/{}/{}, {} samplers",
			draws.drawn, draws.records, skipped, draws.missingTextures[0], draws.missingTextures[1], draws.missingTextures[2], draws.missingTextures[3], draws.missingVertexConstants,
			draws.missingPixelConstants, draws.uploadBytes / 1048576.0,
			draws.cpuMs, draws.epochs, draws.notReady, textures.cached, textures.rejected[1], textures.rejected[2], textures.rejected[3], textures.samplers);
		if (draws.frameTexturesMissing) {
			std::string registers;
			for (std::uint32_t t = 0; t < 128; ++t) {
				if ((draws.frameTexturesMissingRegisters[t >> 6] >> (t & 63)) & 1)
					registers += fmt::format(" t{}", t);
			}
			logger::warn("[DCLF] frame textures (last colour epoch): {} reads of registers the commit could not resolve, which read zero:{}", draws.frameTexturesMissing, registers);
		}
		std::string epochParts;
		for (std::size_t i = 0; i < draws.partMs.size(); ++i)
			epochParts += fmt::format("{}{} {:.3f} ms", epochParts.empty() ? "" : ", ", DCLF::kEpochPartNames[i], draws.partMs[i]);
		// Under CS_DCLF_ASYNC the build's parts are measured on the worker when it built the payload, and
		// "graph execute" is then everything the render thread spent on the epoch.
		logger::info("[DCLF] indirect epoch CPU by part{}: {}", DCLF::AsyncModeSetting() != DCLF::AsyncMode::Off ? " (build parts on the worker when it built)" : "", epochParts);
		if (draws.commitEpochs) {
			static constexpr std::array<const char*, 7> kCommitParts{ "join", "lookups", "frame textures and patches", "frame blocks",
				"payload uploads", "drawn set", "rest" };
			std::string commitParts;
			for (std::size_t i = 0; i < kCommitParts.size(); ++i)
				commitParts += fmt::format("{}{} {:.1f}", commitParts.empty() ? "" : ", ", kCommitParts[i], draws.commitUs[i] / draws.commitEpochs);
			logger::info("[DCLF] main epoch commit on the render thread, us per epoch over {} epochs: {}", draws.commitEpochs, commitParts);
			DCLF::IndirectDraws::Get().ResetCommitTimings();
		}
		if (DCLF::AsyncModeSetting() != DCLF::AsyncMode::Off) {
			std::istringstream asyncLines(DCLF::IndirectDraws::Get().AsyncReport() + DCLF::SceneStore::Get().SceneAsyncReport());
			for (std::string line; std::getline(asyncLines, line);)
				logger::info("{}", line);
		}
		if (DCLF::IndirectDraws::ShadowsEnabled()) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			std::string notReadyReasons;
			for (std::size_t r = 0; r < shadow.notReadyReasons.size(); ++r) {
				if (shadow.notReadyReasons[r])
					notReadyReasons += fmt::format(" {}={}", DCLF::kShadowNotReadyNames[r], shadow.notReadyReasons[r]);
			}
			logger::info("[DCLF] shadow views: {} offered, {} drawn in {} epochs, {} not ready ({}), {} focus and {} volumetric views left native; last mode {} inputs ({} without a pipeline, {} without a texture), {} records; CPU {:.3f} ms per frame ({:.3f} capturing, {:.3f} preparing, {:.3f} inputs, {:.3f} blocks, {:.3f} graph, {:.3f} claiming)",
				shadow.views, shadow.viewsDrawn, shadow.epochs, shadow.notReady, notReadyReasons.empty() ? "-" : notReadyReasons.c_str() + 1, shadow.focusSkipped, shadow.volumetricSkipped, shadow.inputs, shadow.skippedPipeline,
				shadow.skippedTexture, shadow.records, (shadow.cpuMs + shadow.captureMs) / frames, shadow.captureMs / frames, shadow.prepareMs / frames, shadow.inputsMs / frames, shadow.blocksMs / frames,
				shadow.executeMs / frames, shadow.claimMs / frames);
			if (shadow.cullTested)
				logger::info("[DCLF] shadow culling (view {} mode {:#x}, sampled): {} tested, {} drawn, {} rejected by the frustum",
					shadow.cullSampledView, shadow.cullSampledMode, shadow.cullTested, shadow.cullDrawn, shadow.cullRejected);
			if (shadow.sunEntryChecks)
				logger::info("[DCLF] sun entry on the GPU: {} inputs checked against the CPU's verdict (sampled frames), {} differ{}", shadow.sunEntryChecks,
					shadow.sunEntryMismatches, shadow.sunEntryMismatches ? " <- DIFFER" : " <- OK");
			if (DCLF::PassCapture::ShadowWithholdingEnabled()) {
				const auto& captured = DCLF::PassCapture::Get().GetStats();
				logger::info("[DCLF] shadow ownership: withheld plain {} / clamped {} / paraboloid {} passes, {} volumetric-only passes and {} of hints 11, 7 and 3 (last frame); claimed {} / {} / {} casters; {} face regions uploaded; {} views not ready under ownership{}",
					captured.shadowWithheld[0], captured.shadowWithheld[1], captured.shadowWithheld[2], captured.volumetricWithheld, captured.directWithheld, shadow.claimed[0], shadow.claimed[1],
					shadow.claimed[2], shadow.faceUploads, shadow.notReady, shadow.notReady ? " <- HOLES" : "");
			}
			if (shadow.skyDrawn || shadow.skyNotReady || skyNativeFrames)
				logger::info("[DCLF] Skylighting occlusion: DCLF drew {} maps ({} occluders, last), {} it could not draw, {} left to the engine; render thread {:.3f} ms per map",
					shadow.skyDrawn, shadow.skyInputs, shadow.skyNotReady, skyNativeFrames, shadow.skyDrawn ? shadow.skyMs / shadow.skyDrawn : 0.0);
			skyNativeFrames = 0;
			DCLF::IndirectDraws::Get().ResetShadowStats();
		}
		if (stats.projectedUV || stats.landBlend)
			logger::info("[DCLF] projected UV / terrain (last frame): {} projected candidates, {} terrain candidates, projected textures {}",
				stats.projectedUV, stats.landBlend, store.GetProjectedTextures().valid ? "captured" : "not seen yet");
		if (const auto [fading, fadingFrames] = store.TakeFadingDrawn(); fading)
			logger::info("[DCLF] fading: {} screen-door fading objects drawn by DCLF over {} frames", fading, fadingFrames);
		if (stats.skinned || draws.boneRows)
			logger::info("[DCLF] skinned (last frame): {} candidates, {} palette rows in the tables, {} rows uploaded by the epoch", stats.skinned, stats.boneRows, draws.boneRows);
		if (stats.decals[0] || stats.decals[1] || draws.decalsDrawn)
			logger::info("[DCLF] decals (last frame): {} candidates ({} in the opaque group, {} in the blended group), {} submitted to the second pass, {} of {} tested were culled",
				stats.decals[0] + stats.decals[1], stats.decals[0], stats.decals[1], draws.decalsDrawn, draws.decalsCulled, draws.decalsTested);
		if (draws.sunTested || draws.sunCpuTested)
			logger::info("[DCLF] sun on the GPU (sampled frame): {} synthetic draws tested, {} missed every cascade; the CPU over the same inputs: {} tested, {} missed{}",
				draws.sunTested, draws.sunMissed, draws.sunCpuTested, draws.sunCpuMissed,
				(draws.sunTested == draws.sunCpuTested && draws.sunMissed == draws.sunCpuMissed) ? " <- OK" : " <- DIFFERS");
		if (draws.residentInputs || draws.residentResyncs)
			logger::info("[DCLF] persistent draws (last frame, colour): {} kept inputs, {} sequences, {} pairs, {} not drawable; since the start {} region "
						 "versions uploaded, {} resyncs; parity {} entries checked, {} differ, {} missing{}",
				draws.residentInputs, draws.residentDraws, draws.residentPairs, draws.residentUndrawable, draws.residentVersions, draws.residentResyncs,
				draws.residentParityChecks, draws.residentParityMismatches, draws.residentMissing,
				draws.residentParityChecks ? (draws.residentParityMismatches || draws.residentMissing ? " <- RESIDENT DRAW PARITY" : " <- OK") : "");
		if (draws.fadeTested)
			logger::info("[DCLF] fade on the GPU (sampled frame): {} resident draws under a fade root in view, {} dropped past their fade-out distance",
				draws.fadeTested, draws.fadeHidden);
		if (DCLF::IndirectDraws::Hybrid())
			logger::info("[DCLF] hybrid (last frame): {} of {} native passes left to the indirect draws ({} in the depth pass, {} in the opaque pass)",
				skipStats.skipped, skipStats.offered, skipStats.skippedInDepth, skipStats.skippedInOpaque);
		if (DCLF::IndirectDraws::Hybrid() && !skipSamples.empty()) {
			std::string names;
			for (const auto& name : skipSamples)
				names += fmt::format("{}'{}'", names.empty() ? "" : ", ", name);
			logger::info("[DCLF] hybrid: passes left to the indirect draws include {}", names);
		}
		logger::info("[DCLF] parity: {} native passes kept, {} skipped", skipStats.kept, skipStats.skipped);
		if (DCLF::IndirectDraws::Hybrid() && skipStats.notInTables)
			logger::warn("[DCLF] hybrid (last frame): {} native passes were kept because their geometry left the tables", skipStats.notInTables);
		if (DCLF::IndirectDraws::Hybrid() && skipStats.undrawable)
			logger::info("[DCLF] hybrid (last frame): {} native passes were kept because DCLF could not draw their object this frame", skipStats.undrawable);
		if (draws.shortBuffers)
			logger::warn("[DCLF] {} draws of the last epoch reach past their vertex or index buffer slice", draws.shortBuffers);
		const auto& gpu = DCLF::GpuResources::Get().GetStats();
		logger::info("[DCLF] game buffers for the render graph: {} stable, {} rejected; {} resolved so far in {:.1f} ms (slowest {:.3f} ms)", gpu.cached, gpu.rejected,
			gpu.resolvedTotal, gpu.resolveMsTotal, gpu.resolveMsMax);
		// Two halves, because they mean different things: outside kRuntimePassBits the derivation is meant
		// to be exact and any difference is a defect; inside it the derivation is guessing at what
		// GetRenderPasses computes per frame, and that number is what decides whether GetRenderPasses can
		// be skipped outright rather than merely withheld from the batch renderer.
		std::string bitBreakdown;
		// Only when CS_DCLF_DERIVE_PROBE measured it. Printed unconditionally this line read
		// "0 objects compared, 0 differ" with the probe off, which scans as a passing check rather
		// than as one that never ran.
		if (DCLF::SwitchEnabled("CS_DCLF_DERIVE_PROBE")) {
			for (std::uint32_t bit = 0; bit < 32; ++bit) {
				if (!stats.derivationBitCounts[bit])
					continue;
				bitBreakdown += std::format("{} bit {}{}={}", bitBreakdown.empty() ? "" : ",", bit,
					((1u << bit) & DCLF::kRuntimePassBits) ? "*" : "", stats.derivationBitCounts[bit]);
			}
			logger::info("[DCLF] derivation (last frame): {} objects compared, {} would stay native; property bits: {} differ ({:08X}); runtime bits: {} differ ({:08X}); per bit (* = runtime):{}",
				stats.derivationChecked, stats.derivationNative, stats.derivationDiffers, stats.derivationBits,
				stats.derivationRuntimeDiffers, stats.derivationRuntimeBits, bitBreakdown.empty() ? std::string(" none") : bitBreakdown);
		}
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		logger::info("[DCLF] pass capture: {} registrations from {} threads ({} overflowed); against the accumulator: {} compared, {} missing, {} extra, {} technique differs, {} subPass differs{}",
			capture.captured, capture.threads, capture.overflowed, capture.compared, capture.missing, capture.extra,
			capture.techniqueDiffers, capture.subPassDiffers,
			(capture.missing || capture.techniqueDiffers || capture.subPassDiffers) ? "" : " <- OK");
		// CS_DCLF_REGISTER_PROBE=1: what reaches RegisterPass besides the lighting passes, and how much of
		// it lands in one of the main camera's batch renderers. This is what says whether the depth pass
		// can be owned the same way the opaque pass is.
		if (DCLF::SwitchEnabled("CS_DCLF_REGISTER_PROBE")) {
			auto& probeCapture = DCLF::PassCapture::Get();
			std::string byType;
			for (std::size_t i = 0; i < probeCapture.probeCounts.size(); ++i) {
				const auto total = probeCapture.probeCounts[i].exchange(0);
				const auto main = probeCapture.probeMain[i].exchange(0);
				if (total)
					byType += fmt::format(" type{}={}/{}", i, main, total);
			}
			logger::info("[DCLF] RegisterPass by shader type (into a main renderer / total):{}", byType.empty() ? " none" : byType);
		}
		if (DCLF::PassCapture::WithholdingEnabled())
			logger::info("[DCLF] static ownership: {} passes withheld from the batch renderer, {} objects claimed, {} claimed but not drawn{}",
				capture.withheld, capture.claimed, capture.holes, capture.holes ? " <- HOLES" : "");
		if (DCLF::PassCapture::WithholdingEnabled())
			logger::info("[DCLF] claim churn: +{} -{} ({} of the drops still had an engine pass, so the native loop takes them back)",
				capture.claimsAdded, capture.claimsDropped, capture.droppedAfterCull);
		logger::info("[DCLF] derived cache (last frame): {} served, {} recomputed and compared, {} differ{}; slots alive {} geometries / {} pipelines / {} materials, {} swept, {} geometries refreshed in place, {} slot violations{}",
			stats.derivedHits, stats.derivedChecked, stats.derivedDiffers, stats.derivedDiffers ? " <- STALE" : "",
			stats.geometriesAlive, stats.pipelinesAlive, stats.materialsAlive, stats.slotsSwept, stats.geometriesRefreshed,
			stats.slotViolations, stats.slotViolations ? " <- SLOT VIOLATION" : "");
		if (stats.classifyHits || stats.classifyChecked)
			logger::info("[DCLF] classification cache (last frame): {} served from the cache, {} recomputed and compared, {} differ{}; RTTI casts walked {}",
				stats.classifyHits, stats.classifyChecked, stats.classifyDiffers, stats.classifyDiffers ? " <- STALE" : "", stats.castResolved);
		// The Stage 4c gate. A pipeline's per-frame lighting template must come from an object the engine
		// itself kept; anything else hands a culled object's scene light list to the visible objects drawn
		// on that pipeline, which is the blown-out interior lighting defect.
		{
			static constexpr const char* kTechniqueNames[20] = { "none", "envmap", "glowmap", "parallax", "facegen",
				"facegenRGBTint", "hair", "parallaxOcc", "MTLand", "LODLand", "snow", "multilayerParallax", "treeAnim",
				"LODObjects", "multiIndexSparkle", "LODObjectHD", "eye", "cloud", "LODLandNoise", "MTLandLODBlend" };
			std::string techniques;
			for (std::size_t t = 0; t < stats.techniqueRejects.size(); ++t) {
				if (!stats.techniqueRejects[t])
					continue;
				const char* name = t == 63 ? "refraction" : (t < 20 ? kTechniqueNames[t] : "?");
				techniques += fmt::format("{}{}({})={}", techniques.empty() ? "" : " ", name, t, stats.techniqueRejects[t]);
			}
			if (!techniques.empty())
				logger::info("[DCLF] left native by technique: {}", techniques);
		}
		if (!stats.propertyRejects.empty()) {
			std::vector<std::pair<const RE::NiRTTI*, std::uint32_t>> sorted(stats.propertyRejects.begin(), stats.propertyRejects.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string byProperty;
			for (const auto& [rtti, count] : sorted)
				byProperty += fmt::format(" {}={}", rtti && rtti->name ? rtti->name : "?", count);
			logger::info("[DCLF] left native by property type:{}; {} alpha blended, {} opaque ({} alpha tested)",
				byProperty, stats.rejectedBlended, stats.rejectedOpaque, stats.rejectedOpaqueAlphaTest);
		}
		logger::info("[DCLF] pipeline templates: {} of {} drawn only by culled candidates, {} taken over this frame; {} visible objects on a culled template{}",
			stats.pipelinesCulledOnly, stats.pipelines, stats.templateUpgrades, stats.templateDefects,
			stats.templateDefects ? " <- CULLED TEMPLATE" : "");
		// The material cache and its standing alarm. materialCacheStale must be 0: it is the count of
		// entries that were re-evaluated live and disagreed with what the cache would have served.
		logger::info("[DCLF] materials (last frame): {} evaluated, {} served from the cache, {} skipped as undrawable; {} written ({} re-evaluated, {} dropped), {} frame samples; cache {} entries (+{} evicted); validated {}, stale {}{}",
			stats.materialsEvaluated, stats.materialsFromCache, stats.materialsSkipped, stats.materialWrites, stats.materialsRewritten, stats.materialsDropped,
			stats.frameMaterialSamples,
			stats.materialCacheEntries, stats.materialCacheEvicted, stats.materialsValidated, stats.materialCacheStale,
			stats.materialCacheStale ? " <- STALE MATERIAL" : "");
		if (stats.materialCacheStale)
			logger::warn("[DCLF] material cache staleness is in: {}{}{}{}{}{}",
				(stats.materialDiffMask & 1) ? "vs " : "", (stats.materialDiffMask & 2) ? "ps " : "",
				(stats.materialDiffMask & 4) ? "textures " : "", (stats.materialDiffMask & 8) ? "address " : "",
				(stats.materialDiffMask & 16) ? "filter " : "", (stats.materialDiffMask & 32) ? "written" : "");
		logger::info("[DCLF] tracked {} under {} category nodes: {} objects ({} the engine also kept, {} of them with the sun's shadow mask; {} with derived descriptors), {} geometries, {} pipelines, {} materials; left native:{}; events +{} -{}, validation drops {}; CPU per frame: events {:.3f} ms, tables {:.3f} ms (scene {:.3f}, max {:.3f}; accumulate {:.3f}, max {:.3f}){}",
			stats.tracked, stats.categoryNodes, stats.objects, stats.nativeVisible, stats.nativeShadowMasked, stats.derivedDescriptors, stats.geometries, stats.pipelines, stats.materials, reasons,
			stats.attachedEvents, stats.detachedEvents, stats.validationDrops, timing.eventsMs / frames,
			(timing.sceneMs + timing.buildMs) / frames, timing.sceneMs / frames, timing.sceneMaxMs, timing.buildMs / frames, timing.buildMaxMs,
			parts);
		{
			std::string rejects;
			for (std::size_t r = 1; r < stats.shadowRejects.size(); ++r) {
				if (stats.shadowRejects[r])
					rejects += fmt::format(" {}={}", DCLF::ShadowRejectName(static_cast<DCLF::ShadowReject>(r)), stats.shadowRejects[r]);
			}
			logger::info("[DCLF] shadow casters (last frame): {} of {} records would be drawn into a shadow map; not casters:{}; {} views this frame",
				stats.shadowCasters, stats.objects, rejects.empty() ? " none" : rejects, DCLF::ShadowViews::Get().All().size());
		}
		if (stats.accumulatedWithoutRecord)
			logger::warn("[DCLF] two-phase frame: {} objects the engine accumulated had no record from the scene phase, so they stayed native",
				stats.accumulatedWithoutRecord);
		timing = {};
		store.ResetTimes();
	}
}

bool DrawcallLimitFix::DrawableThisFrame(const RE::BSGeometry* a_geometry)
{
	const auto& store = DCLF::SceneStore::Get();
	const auto& tables = store.GetTables();
	const auto& lookups = store.GetLookups();
	const auto object = store.FindObject(a_geometry);
	if (object < 0 || static_cast<std::size_t>(object) >= tables.objects.size())
		return false;
	const auto& record = tables.objects[object];
	if (record.flags & DCLF::kObjectNoBindings)
		return false;
	return record.pipelineIndex < lookups.pipelines.size() && lookups.pipelines[record.pipelineIndex].setIndex != DCLF::Lookups::kNone;
}

bool DrawcallLimitFix::SkipNativePass(RE::BSRenderPass* a_pass)
{
	// CS_DCLF_HYBRID_NOSKIP=1: DCLF draws into the frame but the native loop keeps drawing everything, so
	// what DCLF fails to draw is still visible. It tells apart the two ways an object can go missing.
	const auto toggles = DCLF::Toggles::Get().Active();
	const bool noSkip = toggles.hybridNoSkip;
	// CS_DCLF_ONLY_ELIGIBLE=1: the reverse skip, for parity. The native loop draws only what the indirect
	// draws also draw, so the native depth and G-buffer hold the same object set as DCLF's own targets and
	// the two can be compared pixel for pixel.
	const bool onlyEligible = toggles.onlyEligible;
	if (noSkip && !onlyEligible)
		return false;
	if (!Running() || !a_pass || !a_pass->geometry || (!DCLF::IndirectDraws::Hybrid() && !onlyEligible))
		return false;
	if (!inDepthPass && !globals::deferred->deferredPass)
		return false;  // shadows, reflections and cubemaps keep drawing everything
	// A decal's passes offered in the depth pass (blended ones with kZBufferWrite) draw nothing there: the Lighting shader
	// never reaches SetupGeometry in the depth pass (engine notes, "Decals"), so they are skipped like any other pass.
	if (onlyEligible) {
		// Membership of this frame's tables, not what the epoch drew: the epoch only runs once the main pass
		// has drawn, so a rule based on the frame before would skip everything and never start.
		const auto& parityStore = DCLF::SceneStore::Get();
		const auto parityObject = parityStore.FindObject(a_pass->geometry);
		if (parityObject >= 0 && !(parityStore.GetTables().objects[parityObject].flags & DCLF::kObjectShadowOnly)) {
			++skipCounters.kept;
			if (skipCounters.kept == 1)
				logger::info("[DCLF] parity: keeping '{}' (depth pass {}, deferred {})", a_pass->geometry->name.c_str() ? a_pass->geometry->name.c_str() : "?",
					inDepthPass, globals::deferred->deferredPass);
			return false;
		}
		++skipCounters.skipped;
		return true;
	}
	// Never a pass DCLF does not model, whoever draws its object: a LOD cross-fade's copy of the old level
	// (hint 10) is the native loop's while DCLF draws the object's own pass.
	if (DCLF::PassCapture::FadingAtRegistration(a_pass))
		return false;
	auto& store = DCLF::SceneStore::Get();
	if (!DCLF::IndirectDraws::Get().DrewLastFrame(a_pass->geometry, store.GetFrame()))
		return false;
	// A geometry the epoch drew in the frame before but that is not in this frame's tables would be left
	// out of the frame entirely: the pass stays native and the mismatch is reported.
	if (skipSamples.size() < 12 && a_pass->geometry->name.c_str())
		skipSamples.emplace_back(a_pass->geometry->name.c_str());
	if (store.FindObject(a_pass->geometry) < 0) {
		++skipCounters.notInTables;
		if (skipCounters.notInTables == 1 && a_pass->geometry->name.c_str())
			logger::warn("[DCLF] hybrid: '{}' was drawn last frame but is not in this frame's tables; it stays native", a_pass->geometry->name.c_str());
		return false;
	}
	// Nor one DCLF cannot draw this frame although it has a record: the accumulate phase gave it no bindings
	// (it started fading - a tree's LOD cross-fade begins at a fixed distance - or its switch no longer
	// selects it) or its pipeline is not built. The skip set is last frame's draws, so this is the frame it
	// would be drawn by nobody: the flicker trees showed at their LOD distances.
	if (!DrawableThisFrame(a_pass->geometry)) {
		++skipCounters.undrawable;
		return false;
	}
	return true;
}

void DrawcallLimitFix::Hooks::Main_RenderDepth::thunk(bool a_firstPerson, bool a_a2)
{
	auto& feature = globals::features::drawcallLimitFix;
	feature.inDepthPass = true;
	feature.zPrepassInDepthPass = false;
	func(a_firstPerson, a_a2);
	feature.inDepthPass = false;
	if (!feature.Running() || feature.zPrepassInDepthPass)
		return;
	// Where Main_RenderDepth_WorldDrawn is not installed (SE): the Z-prepass at the end of the native depth
	// pass. This thunk is the inner one of the chain on this call site (DCLF installs before Terrain Blending,
	// so Terrain Blending wraps it), which puts the prepass ahead of the blended depth Terrain Blending builds.
	// In first person the camera here is the first-person model's, and the prepass draws the world with it.
	if (a_firstPerson) {
		static bool warned = false;
		if (!std::exchange(warned, true))
			logger::warn("[DCLF] First person on a runtime without the depth pass's inner hook: DCLF's objects are drawn with the first-person camera and do not show");
	}
	feature.RunZPrepass(true);
}

void DrawcallLimitFix::Hooks::Main_RenderDepth_WorldDrawn::thunk(void* a_accumulator, bool a_a2)
{
	func(a_accumulator, a_a2);
	auto& feature = globals::features::drawcallLimitFix;
	if (!feature.inDepthPass || !feature.Running())
		return;
	// The world's depth draws are done and its camera is still current. The engine's own copy of the depth
	// (kPOST_ZPREPASS_COPY), at the end of the pass, now includes DCLF's objects, and the first-person model's
	// depth goes in after the world's, as it does natively, so there is nothing to refresh.
	feature.zPrepassInDepthPass = true;
	feature.RunZPrepass(false);
	if (DCLF::SceneStore::Get().GetTables().objects.size() >= 400)
		globals::BeginDCLFDepthTrace(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture);
	// DCLF's epoch leaves the context's bindings to the engine's state tracking: rebind its targets for the
	// rest of the pass.
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::RunZPrepass(bool a_refreshConsumers)
{
	// DCLF's objects go into the depth buffer before anything is derived from it.
	DCLF::IndirectDraws::Get().CaptureDepthPass();
	if (a_refreshConsumers)
		RefreshDepthConsumers();
}

template <int N>
void DrawcallLimitFix::Hooks::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	auto& feature = globals::features::drawcallLimitFix;
	++feature.skipCounters.offered;
	if (DCLF::DecalProbe::Enabled())
		DCLF::DecalProbe::Get().OnPassOffered(a_pass, feature.inDepthPass);
	if (feature.SkipNativePass(a_pass)) {
		++(feature.inDepthPass ? feature.skipCounters.skippedInDepth : feature.skipCounters.skippedInOpaque);
		++feature.skipCounters.skipped;
		return;
	}
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void DrawcallLimitFix::Hooks::BSShaderAccumulator_FinishAccumulating::thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags)
{
	// [TEMP] CS_DCLF_VOLUMETRIC_PROBE: the engine's CPU time drawing each shadow view, by target.
	if (DCLF::VolumetricProbe::Enabled()) {
		const auto probeMode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
		if (probeMode >= 0xD && probeMode <= 0xF) {
			const auto target = static_cast<std::uint32_t>(globals::game::shadowState->GetRuntimeData().depthStencil);
			DCLF::VolumetricProbe::Get().BeginFinish(target);
			LARGE_INTEGER start{}, end{};
			QueryPerformanceCounter(&start);
			func(a_accumulator, a_renderFlags);
			QueryPerformanceCounter(&end);
			DCLF::VolumetricProbe::Get().OnFinish(a_accumulator, a_renderFlags, target, end.QuadPart - start.QuadPart);
		} else
			func(a_accumulator, a_renderFlags);
	} else
		func(a_accumulator, a_renderFlags);
	// TEMP (CS_DCLF_CASCADE_PROBE): the fixed-function state the engine drew this shadow view with - the
	// rasterizer bound on the context after its passes, and the renderer's bias and cull modes.
	if (static const bool probe = DCLF::SwitchEnabled("CS_DCLF_CASCADE_PROBE"); probe) {
		const auto probeMode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
		static std::uint32_t probeCalls = 0;
		if (probeMode >= 0xD && probeMode <= 0xF && globals::d3d::context) {
			if (const auto call = probeCalls++; call < 64 || (call % 1200) < 8) {
				auto& shadow = globals::game::shadowState->GetRuntimeData();
				winrt::com_ptr<ID3D11RasterizerState> rs;
				globals::d3d::context->RSGetState(rs.put());
				D3D11_RASTERIZER_DESC desc{};
				if (rs)
					rs->GetDesc(&desc);
				logger::info("[DCLF] cascade probe native state: mode {:#x} target {} slice {}: bias mode {} cull mode {} fill {}; bound rasterizer {}: DepthBias {} clamp {} slope {} cull {} depthClip {}",
					probeMode, static_cast<std::uint32_t>(shadow.depthStencil), shadow.depthStencilSlice, shadow.rasterStateDepthBiasMode, shadow.rasterStateCullMode,
					shadow.rasterStateFillMode, rs ? "yes" : "no", desc.DepthBias, desc.DepthBiasClamp, desc.SlopeScaledDepthBias, static_cast<int>(desc.CullMode), desc.DepthClipEnable);
			}
		}
	}
	if (!globals::features::drawcallLimitFix.Running())
		return;
	const auto mode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
	// Skylighting's occlusion map (render mode 0x1C, the precipitation accumulator while Skylighting draws its own map).
	if (mode == 0x1C) {
		auto* sky = globals::game::sky;
		auto* precip = sky ? sky->precip : nullptr;
		if (globals::features::skylighting.inOcclusion && precip &&
			precip->occlusionData.accumulator.get() == reinterpret_cast<RE::BSShaderAccumulator*>(a_accumulator))
			DCLF::IndirectDraws::Get().CaptureSkyOcclusion();
		return;
	}
	if (mode < 0xD || mode > 0xF)
		return;  // shadow-map modes only: plain, clamped, paraboloid (engine notes: shadow maps)
	const auto view = DCLF::ShadowViews::Get().ViewOfAccumulator(a_accumulator);
	if (view != ~0u)
		DCLF::IndirectDraws::Get().ExecuteShadowView(view, mode);
}

void DrawcallLimitFix::Hooks::Install()
{
	// Always installed: the shadow views are a live toggle (Toggles.h), and the thunk costs one compare per
	// accumulator finish when they are off.
	stl::write_vfunc<0x2A, BSShaderAccumulator_FinishAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);
	logger::info("[DCLF] shadow view hook installed on BSShaderAccumulator::FinishAccumulatingPreResolveDepth");
	// The main camera's depth pass, hooked where Terrain Blending hooks it.
	stl::write_thunk_call<Main_RenderDepth>(REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3));
	// AE only: the SE offset of this call inside Main::RenderDepth is unverified (no database for it).
	if (REL::Module::IsAE()) {
		stl::write_thunk_call<Main_RenderDepth_WorldDrawn>(REL::RelocationID(100421, 107139).address() + 0x1AA);
		logger::info("[DCLF] Z-prepass hook installed inside Main::RenderDepth, after the world's depth draws");
	}
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<1>>(REL::RelocationID(100877, 107667).address() + REL::Relocate(0x1E5, 0xED));
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<2>>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
	if (REL::Module::IsSE())  // this call site only exists in SE, as Light Limit Fix's hooks show
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately<3>>(REL::RelocationID(100871, 107661).address() + 0xEE);
	if (REL::Module::IsAE()) {
		stl::write_thunk_call<Main_Draw_Early>(REL::RelocationID(35560, 36559).address() + 0xD3);
		logger::info("[DCLF] scene phase hook installed on Main::Draw ({})", EarlySceneEnabled() ? "the walk starts there" : "off: CS_DCLF_EARLY_SCENE=0");
	}
	logger::info("[DCLF] Native pass hooks installed");
}

void DrawcallLimitFix::OnNativeLightingDraw(RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
{
	if (!Running() || DCLF::ConstantEvaluator::Evaluating())
		return;
	if (globals::deferred->deferredPass) {
		auto& store = DCLF::SceneStore::Get();
		// A ProjectedUV draw has SetupGeometry's four projected textures bound now: keep them for the
		// pipelines DCLF builds with that bit (the Hair technique binds none).
		if ((DCLF::PassDescriptorOf(a_pass->passEnum) & 0x8000u) && ((DCLF::PassDescriptorOf(a_pass->passEnum) >> 24) & 0x3f) != 6)
			store.NoteProjectedTextures();
		// CS_DCLF_CAPTURE_POINT_PARITY: the bindings at the frame's first lighting draw the engine makes, against what
		// BeforeOpaquePass captured for the colour epoch.
		static const bool captureParity = DCLF::SwitchEnabled("CS_DCLF_CAPTURE_POINT_PARITY");
		if (captureParity && captureFrame == store.GetFrame() && parityFrame != store.GetFrame()) {
			parityFrame = store.GetFrame();
			DCLF::IndirectDraws::Get().CheckCapturePoint();
		}
	}
	if (DCLF::CaptureParity::Enabled())
		DCLF::CaptureParity::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
	if (DCLF::DecalProbe::Enabled())
		DCLF::DecalProbe::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
	if (DCLF::SkinProbe::Enabled())
		DCLF::SkinProbe::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
	if (DCLF::NativeProbe::Enabled())
		DCLF::NativeProbe::Get().OnNativeLightingDraw(a_pass, a_renderFlags);
	if (DCLF::TreeTrace::Enabled())
		DCLF::TreeTrace::Get().OnNativeLightingDraw(a_pass);
}

void DrawcallLimitFix::RefreshDepthConsumers()
{
	// The Z-prepass has just run, inside the native depth pass, so whatever is derived from the depth buffer
	// after this point already sees DCLF's objects. Terrain Blending's blended depth is one of those: it is
	// built further up this same call site, after this thunk returns.
	//
	// The engine's prepass copy is not, unless Terrain Blending is on to redirect its SRV: the copy is taken
	// inside the depth pass, before this runs. Refresh it so that the effects reaching it through
	// Util::GetCurrentSceneDepthSRV see the same scene the depth buffer holds.
	auto& terrainBlending = globals::features::terrainBlending;
	if (terrainBlending.loaded && terrainBlending.settings.Enabled)
		return;
	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer)
		return;
	const auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
	const auto& main = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	const auto& prepassCopy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	if (main.texture && prepassCopy.texture) {
		ScopedPerfEvent event("CS DCLF: post-Z-prepass depth copy");
		context->CopyResource(prepassCopy.texture, main.texture);
	}
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::BeforeOpaquePass()
{
	if (!Running())
		return;
	// The main pass's bindings for this frame's colour epoch, where the opaque batches start. The engine binds the
	// G-buffer (and clears it) only when a draw applies its state, so the state is applied here as the first draw would
	// apply it. They were taken at the first lighting draw the engine made, which a frame whose every lighting draw in
	// view is DCLF's (an interior, facing away from anything left native) does not have: its colour epoch was dropped,
	// and the objects it withheld from the engine went undrawn. The end of the pass is no substitute: by then the sky's
	// clouds have rebound Cloud Shadows' t26. CS_DCLF_CAPTURE_POINT_PARITY checks this capture against the first
	// lighting draw's.
	static REL::Relocation<void (*)(bool)> SetDirtyStates{ REL::RelocationID(75580, 77386) };
	SetDirtyStates(false);
	globals::EndDCLFDepthTrace();
	if (!CaptureMainPass() && !loggedCaptureFailure) {
		loggedCaptureFailure = true;
		logger::warn("[DCLF] the main pass's targets are not bound where its opaque batches start; the colour epoch is skipped");
	}
}

bool DrawcallLimitFix::CaptureMainPass()
{
	const auto formats = CurrentTargetFormats();
	if (formats.colorCount == 0 || formats.depth == DXGI_FORMAT_UNKNOWN)
		return false;
	auto& store = DCLF::SceneStore::Get();
	captureFrame = store.GetFrame();
	DCLF::DrawPipelines::Get().SetTargetFormats(formats);
	// Phase 2: what the main pass binds, for this frame's indirect draws (run before the composite).
	DCLF::IndirectDraws::Get().CaptureMainPass();
	// The engine's state objects behind any key that carries state bits (decals), read here
	// because this is inside the deferred pass, where the blend table holds the deferred variants.
	DCLF::DrawPipelines::Get().CaptureEngineStates(store.GetTables().pipelines);
	return true;
}

void DrawcallLimitFix::AfterOpaquePass()
{
	if (!Running())
		return;
	// Natively, DCLF's objects are drawn among the opaque batches, so they are in the G-buffer and the depth
	// before Terrain Blending draws its held terrain: with alpha blending onto the G-buffer, a LESS_EQUAL test
	// and depth writes, its alpha the distance to the object beneath. Drawn after that terrain, a buried
	// object was missing from what the terrain blended onto (the G-buffer's clear values showed through,
	// whitish-blue), and the terrain's depth writes then failed DCLF's EQUAL test where it overlapped.
	auto& draws = DCLF::IndirectDraws::Get();
	auto* context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // the epoch writes the G-buffer the context has bound
	draws.ProbeTargets("before colour");
	draws.ExecuteColour();  // the colour pass, against the depth written at the first draw
	draws.ProbeTargets("after colour");
	// DCLF's epoch leaves the context's bindings to the engine's state tracking: rebind its targets for the
	// rest of the pass.
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void DrawcallLimitFix::ProbeOpaqueTarget(bool a_afterDCLF)
{
	static const std::string pixel = DCLF::SwitchValue("CS_DCLF_TARGET_PROBE");
	if (pixel.empty())
		return;
	constexpr std::uint32_t kTargets = 8;
	constexpr std::uint32_t kBlock = 64;  // the mean over a block: one pixel is too noisy under TAA jitter
	struct Slot
	{
		std::array<winrt::com_ptr<ID3D11Texture2D>, kTargets> staging;
		std::array<DXGI_FORMAT, kTargets> format{};
		bool running = false;
	};
	static std::array<Slot, 2> slots;  // before and after DCLF's colour epoch
	static std::uint32_t frames = 0, framesLeft = 0;
	auto* context = globals::d3d::context;
	auto half = [](std::uint16_t a_h) {
		const std::uint32_t sign = (a_h >> 15) & 1, exponent = (a_h >> 10) & 0x1F, mantissa = a_h & 0x3FF;
		float value = exponent == 0 ? std::ldexp(float(mantissa), -24) : exponent == 31 ? std::numeric_limits<float>::infinity() : std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
		return sign ? -value : value;
	};
	if (!a_afterDCLF) {
		if (framesLeft && --framesLeft == 0) {
			for (std::uint32_t i = 0; i < 2; ++i) {
				auto& slot = slots[i];
				std::string text;
				for (std::uint32_t t = 0; t < kTargets; ++t) {
					if (!slot.staging[t])
						continue;
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (SUCCEEDED(context->Map(slot.staging[t].get(), 0, D3D11_MAP_READ, 0, &mapped))) {
						// The block's mean per channel.
						auto f11 = [](std::uint32_t a_bits, std::uint32_t a_mantissaBits) {
							const std::uint32_t exponent = a_bits >> a_mantissaBits, mantissa = a_bits & ((1u << a_mantissaBits) - 1);
							return exponent == 0 ? std::ldexp(float(mantissa), -14 - int(a_mantissaBits)) : std::ldexp(1.0f + float(mantissa) / float(1u << a_mantissaBits), int(exponent) - 15);
						};
						std::array<double, 4> sum{};
						std::uint32_t channels = 0;
						for (std::uint32_t row = 0; row < kBlock; ++row) {
							const auto* line = static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(row) * mapped.RowPitch;
							for (std::uint32_t column = 0; column < kBlock; ++column) {
								switch (slot.format[t]) {
								case DXGI_FORMAT_R16G16B16A16_FLOAT:
									for (std::uint32_t c = 0; c < 4; ++c)
										sum[c] += half(reinterpret_cast<const std::uint16_t*>(line)[column * 4 + c]);
									channels = 4;
									break;
								case DXGI_FORMAT_R16G16_FLOAT:
									for (std::uint32_t c = 0; c < 2; ++c)
										sum[c] += half(reinterpret_cast<const std::uint16_t*>(line)[column * 2 + c]);
									channels = 2;
									break;
								case DXGI_FORMAT_R10G10B10A2_UNORM: {
									const std::uint32_t w = reinterpret_cast<const std::uint32_t*>(line)[column];
									sum[0] += (w & 1023) / 1023.0;
									sum[1] += ((w >> 10) & 1023) / 1023.0;
									sum[2] += ((w >> 20) & 1023) / 1023.0;
									sum[3] += (w >> 30) / 3.0;
									channels = 4;
									break;
								}
								case DXGI_FORMAT_R11G11B10_FLOAT: {
									const std::uint32_t w = reinterpret_cast<const std::uint32_t*>(line)[column];
									sum[0] += f11(w & 2047, 6);
									sum[1] += f11((w >> 11) & 2047, 6);
									sum[2] += f11(w >> 22, 5);
									channels = 3;
									break;
								}
								case DXGI_FORMAT_R16_UNORM:
									sum[0] += reinterpret_cast<const std::uint16_t*>(line)[column] / 65535.0;
									channels = 1;
									break;
								case DXGI_FORMAT_R8G8B8A8_UNORM:
									for (std::uint32_t c = 0; c < 4; ++c)
										sum[c] += line[column * 4 + c] / 255.0;
									channels = 4;
									break;
								default:
									break;
								}
							}
						}
						const double n = double(kBlock) * kBlock;
						if (channels == 0)
							text += fmt::format(" rt{}=f{}", t, static_cast<std::uint32_t>(slot.format[t]));
						else if (channels == 1)
							text += fmt::format(" rt{}=({:.4f})", t, sum[0] / n);
						else if (channels == 2)
							text += fmt::format(" rt{}=({:.4f} {:.4f})", t, sum[0] / n, sum[1] / n);
						else if (channels == 3)
							text += fmt::format(" rt{}=({:.4f} {:.4f} {:.4f})", t, sum[0] / n, sum[1] / n, sum[2] / n);
						else
							text += fmt::format(" rt{}=({:.4f} {:.4f} {:.4f} {:.4f})", t, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
						context->Unmap(slot.staging[t].get(), 0);
					}
					slot.staging[t] = nullptr;
				}
				logger::info("[TEMP] targets {} DCLF's colour epoch, DCLF {}:{}", i ? "after " : "before", slot.running ? "on" : "off", text);
			}
		}
		if ((frames++ % 240) != 0)
			return;
		framesLeft = 4;
	} else if (framesLeft != 4) {
		return;
	}
	// The epoch unbinds the targets, so the read after it reuses the textures the read before it found.
	static std::array<winrt::com_ptr<ID3D11Texture2D>, kTargets> textures;
	if (!a_afterDCLF) {
		ID3D11RenderTargetView* views[kTargets] = {};
		context->OMGetRenderTargets(kTargets, views, nullptr);
		for (std::uint32_t t = 0; t < kTargets; ++t) {
			textures[t] = nullptr;
			if (!views[t])
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			views[t]->GetResource(resource.put());
			views[t]->Release();
			textures[t] = resource.try_as<ID3D11Texture2D>();
		}
	}
	const auto sep = pixel.find_first_of(",x");
	auto& slot = slots[a_afterDCLF ? 1 : 0];
	slot.running = Running();
	for (std::uint32_t t = 0; t < kTargets; ++t) {
		slot.staging[t] = nullptr;
		if (!textures[t])
			continue;
		D3D11_TEXTURE2D_DESC desc{};
		textures[t]->GetDesc(&desc);
		const auto x = std::min<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10), desc.Width - kBlock);
		const auto y = std::min<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10), desc.Height - kBlock);
		D3D11_TEXTURE2D_DESC stagingDesc = desc;
		stagingDesc.Width = stagingDesc.Height = kBlock;
		stagingDesc.MipLevels = stagingDesc.ArraySize = 1;
		stagingDesc.SampleDesc = { 1, 0 };
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = stagingDesc.MiscFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(globals::d3d::device->CreateTexture2D(&stagingDesc, nullptr, slot.staging[t].put())))
			continue;
		const D3D11_BOX box{ x, y, 0, x + kBlock, y + kBlock, 1 };
		context->CopySubresourceRegion(slot.staging[t].get(), 0, 0, 0, 0, textures[t].get(), 0, &box);
		slot.format[t] = desc.Format;
	}
}

void DrawcallLimitFix::BeforeDeferredComposite()
{
	if (!Running())
		return;
	auto& draws = DCLF::IndirectDraws::Get();
	draws.Execute();  // off the hybrid path: assemble and draw into the off-screen targets
	// Publish what DCLF owns now that the colour epoch has said what it actually drew. The registration
	// hook reads this on the next frame, before BuildFrame - which is the point: a claim is a standing
	// statement of ownership, not a per-frame decision.
	draws.PublishClaims();
	draws.ShowDebugView();
}

void DrawcallLimitFix::DrawSettings()
{
	const auto& stats = DCLF::SceneStore::Get().GetStats();
	if (!unavailableReason.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.3f, 1.0f));
		ImGui::TextUnformatted("Unavailable this session: the render graph could not start, so everything renders natively.");
		ImGui::PopStyleColor();
		ImGui::TextWrapped("Reason: %s", unavailableReason.c_str());
		return;
	}
	if (!installed) {
		ImGui::TextUnformatted("Not installed (CS_DCLF=0 or VR).");
		return;
	}
	if (!switchedOn) {
		ImGui::TextWrapped("Switched off with the feature's toggle: the game renders natively and DCLF does no frame work. Switch it back on to resume; no restart needed.");
		return;
	}
	// The live toggles: every non-default feature, for A/B comparisons without a restart. They are the
	// REQUESTED set; the render thread applies them at the start of the next frame (Toggles.h).
	auto& toggles = DCLF::Toggles::Get().Requested();
	const auto active = DCLF::Toggles::Get().Active();
	if (ImGui::TreeNodeEx("Live toggles (A/B)", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Each is seeded from its CS_DCLF_* switch and applied at the next frame. A change to an object class drops the classification caches, so the frame after it re-classifies everything.");
		ImGui::SeparatorText("Main pass");
		ImGui::Checkbox("Hybrid: draw into the frame, skip natively (CS_DCLF_HYBRID)", &toggles.hybrid);
		ImGui::BeginDisabled(!toggles.hybrid);
		ImGui::Checkbox("Static ownership: withhold claimed passes (CS_DCLF_OWNERSHIP=static)", &toggles.ownership);
		ImGui::EndDisabled();
		int cull = toggles.cullMode;
		if (ImGui::Combo("GPU culling (CS_DCLF_CULL)", &cull, "off\0frustum\0frustum + occlusion\0"))
			toggles.cullMode = static_cast<std::uint8_t>(cull);
		ImGui::Checkbox("Cull the tracked set, not only what the engine kept (CS_DCLF_CULL_INPUT=tracked)", &toggles.cullTracked);
		ImGui::SeparatorText("Object classes");
		ImGui::Checkbox("Skinned (CS_DCLF_SKINNED)", &toggles.skinned);
		ImGui::Checkbox("Trees (CS_DCLF_TREES)", &toggles.trees);
		ImGui::Checkbox("Decals (CS_DCLF_DECALS)", &toggles.decals);
		ImGui::Checkbox("Projected UV (CS_DCLF_PROJECTED_UV)", &toggles.projectedUv);
		ImGui::Checkbox("Terrain (CS_DCLF_MTLAND)", &toggles.mtLand);
		ImGui::Checkbox("Under switch nodes: trees, harvestables (CS_DCLF_SWITCH_NODES)", &toggles.switchNodes);
		ImGui::BeginDisabled(!toggles.skinned);
		ImGui::Checkbox("Skins of several partitions: LOD trees, actor bodies (CS_DCLF_SKIN_PARTITIONS)", &toggles.skinPartitions);
		ImGui::EndDisabled();
		ImGui::Checkbox("Actors (CS_DCLF_ACTORS)", &toggles.actors);
		ImGui::Checkbox("Fading objects: the screen-door fade (CS_DCLF_FADING)", &toggles.fading);
		ImGui::Checkbox("LOD cross-fades: keep the object, leave the copy native (CS_DCLF_LOD_CROSSFADE)", &toggles.lodCrossfade);
		ImGui::SeparatorText("Shadow views");
		ImGui::Checkbox("Draw the shadow views (CS_DCLF_SHADOWS)", &toggles.shadows);
		ImGui::BeginDisabled(!toggles.shadows);
		ImGui::Checkbox("Static shadow ownership: withhold claimed casters (CS_DCLF_SHADOW_OWNERSHIP=static)", &toggles.shadowOwnership);
		ImGui::BeginDisabled(!toggles.shadowOwnership);
		ImGui::Checkbox("Skip the engine's sun shadow culling and registration (CS_DCLF_SUN_SKIP)", &toggles.skipSunAccumulation);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("The engine stops building sun shadow passes for the casters DCLF draws; it still sets their shadow bits for the main pass.");
		ImGui::BeginDisabled(!toggles.skipSunAccumulation);
		ImGui::Checkbox("Take DCLF's objects out of the engine's sun culls (CS_DCLF_SUN_EXCLUDE)", &toggles.excludeSunEntries);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("The sun's cascade culls skip every reference whose shadows DCLF draws entirely; DCLF sets those objects' sun shadow bits for the main pass.");
		ImGui::BeginDisabled(!toggles.excludeSunEntries || !toggles.ownership);
		ImGui::Checkbox("Take DCLF's objects out of the engine's main camera cull (CS_DCLF_PRIMARY_EXCLUDE)", &toggles.excludePrimaryEntries);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("The main camera's cull and registration skip every reference DCLF draws entirely; DCLF builds their main passes itself and runs their fade updates.");
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::EndDisabled();
		ImGui::Checkbox("Draw Skylighting's occlusion map (CS_DCLF_SKYLIGHT)", &toggles.skyOcclusion);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("With Skylighting loaded, DCLF draws its sky occlusion height map from its own tables on the GPU, and the engine no longer culls or registers the scene for it.");
		ImGui::EndDisabled();
		ImGui::SeparatorText("Diagnostics");
		ImGui::Checkbox("Debug view: show DCLF's targets (CS_DCLF_DEBUG_VIEW)", &toggles.debugView);
		ImGui::Checkbox("Hybrid without skipping: the native loop draws everything too (CS_DCLF_HYBRID_NOSKIP)", &toggles.hybridNoSkip);
		ImGui::Checkbox("Only eligible: the native loop draws only DCLF's set (CS_DCLF_ONLY_ELIGIBLE)", &toggles.onlyEligible);
		ImGui::Checkbox("No Z-prepass (CS_DCLF_NO_ZPREPASS)", &toggles.noZPrepass);
		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("This frame", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Active: hybrid %d, ownership %d, cull %u%s, skinned %d, trees %d, decals %d, projected %d, terrain %d, switch nodes %d, skin partitions %d, actors %d, fading %d, LOD cross-fade %d, shadows %d, shadow ownership %d, skip sun accumulation %d, exclude sun entries %d",
			active.hybrid, active.ownership, active.cullMode, active.cullTracked ? " (tracked)" : "", active.skinned, active.trees, active.decals,
			active.projectedUv, active.mtLand, active.switchNodes, active.skinPartitions, active.actors, active.fading, active.lodCrossfade, active.shadows,
			active.shadowOwnership, active.skipSunAccumulation, active.excludeSunEntries);
		ImGui::Text("Tracked geometry: %u (under %u category nodes)", stats.tracked, stats.categoryNodes);
		ImGui::Text("Objects this frame: %u (%u the engine's culling also kept), geometries: %u, pipelines: %u", stats.objects, stats.nativeVisible, stats.geometries, stats.pipelines);
		for (std::size_t i = 1; i < stats.ineligible.size(); ++i) {
			if (stats.ineligible[i])
				ImGui::Text("Left native (%s): %u", DCLF::kIneligibleNames[i].data(), stats.ineligible[i]);
		}
		const auto& draws = DCLF::IndirectDraws::Get().GetStats();
		ImGui::Text("Indirect draws: %u drawn from %u records, %u epochs; culling tested %u, rejected %u, false negatives %u",
			draws.drawn, draws.records, draws.epochs, draws.cullTested, draws.cullRejected, draws.cullFalseNegatives);
		if (const auto& gpu = RenderGraphRuntime::Get().GpuTimingSummary(); !gpu.empty())
			ImGui::TextUnformatted(("GPU (ORG pass timestamps, last report):\n" + gpu).c_str());
		const auto& capture = DCLF::PassCapture::Get().GetStats();
		if (active.ownership)
			ImGui::Text("Ownership: %u withheld, %u claimed, %u holes", capture.withheld, capture.claimed, capture.holes);
		if (active.shadows) {
			const auto& shadow = DCLF::IndirectDraws::Get().GetShadowStats();
			ImGui::Text("Shadow views (since the last report): %u offered, %u drawn, %u not ready; last view %u inputs, %u records; sampled culling: %u tested, %u rejected",
				shadow.views, shadow.viewsDrawn, shadow.notReady, shadow.inputs, shadow.records, shadow.cullTested, shadow.cullRejected);
			if (active.shadowOwnership)
				ImGui::Text("Shadow ownership: withheld %u / %u / %u (plain / clamped / paraboloid), claimed %u / %u / %u",
					capture.shadowWithheld[0], capture.shadowWithheld[1], capture.shadowWithheld[2], shadow.claimed[0], shadow.claimed[1], shadow.claimed[2]);
		}
		ImGui::TreePop();
	}
}
