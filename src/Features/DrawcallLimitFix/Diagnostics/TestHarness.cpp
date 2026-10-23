// The test harness: the CS_DCLF_TEST_* switches' scripted commands, toggles, turns, moves and exit, run every Present on the render
// (main) thread, DCLF installed or not.
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "TestHarness.h"

#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/PlayerParity.h"
#include "Globals.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "State.h"

#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Render/RenderGraph/ExperimentalRhiExecution.h>

#include <mutex>
#include <string>
#include <vector>

namespace DCLF
{
	namespace
	{
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
				if (!DCLF::SwitchValue(DCLF::Switch::TestMove).empty())
					commands.push_back({ 1, "tgm" });
				const std::string commandList = DCLF::SwitchValue(DCLF::Switch::TestCommands);
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
			 * The counter lives here rather than being the scene's frame number, because the commands have to fire
			 * at the same point in a run whether or not the feature is installed (a CS_DCLF=0 baseline included).
			 * Loading screens do not count, so the two runs stay in step.
			 */
			void OnFrame()
			{
				if (DCLF::Scene::FrameView::IsLoadingScreenUp())
					return;
				const std::uint32_t a_frame = ++frame;
				while (next < commands.size() && a_frame >= commands[next].frame)
					Run(commands[next++].command, a_frame);
			}

			/** @brief Runs one command: a "dclf:" command here, any other the console's, on the main thread. */
			static void Run(std::string command, std::uint32_t a_frame)
			{
				{
					logger::info("[DCLF] test command at frame {}: {}", a_frame, command);
					// "dclf:<toggle name>=<0|1>": a live toggle (Toggles.h, ToggleInfo::name), as the menu sets it, so one run can
					// compare a class drawn by DCLF and by the engine from the same camera.
					// "dclf:pov=first" / "dclf:pov=third": the camera switched as the POV key does (PlayerCamera::ForceFirstPerson/ForceThirdPerson,
					// on the main thread); no console command switches it.
					if (command == "dclf:pov=first" || command == "dclf:pov=third") {
						if (auto* tasks = SKSE::GetTaskInterface())
							tasks->AddTask([first = command == "dclf:pov=first"] {
								auto* camera = RE::PlayerCamera::GetSingleton();
								if (camera && first)
									camera->ForceFirstPerson();
								else if (camera)
									camera->ForceThirdPerson();
							});
						return;
					}
					// "dclf:draw=1" / "dclf:draw=0": the player's weapons or magic hands drawn or sheathed (Actor::DrawWeaponMagicHands, on
					// the main thread), so a scripted first-person run shows the arms (they are drawn only with a weapon or spell out).
					if (command == "dclf:draw=1" || command == "dclf:draw=0") {
						if (auto* tasks = SKSE::GetTaskInterface())
							tasks->AddTask([draw = command == "dclf:draw=1"] {
								if (auto* player = RE::PlayerCharacter::GetSingleton())
									player->DrawWeaponMagicHands(draw);
							});
						return;
					}
					// "dclf:kill-scene-pump": the scene pump stopped for good, as a pass throwing past its handler stops it, for the watchdog
					// (DrawcallLimitFix::WatchScenePump) to hand the game back.
					if (command == "dclf:kill-scene-pump") {
						Scene::Coordinator::Get().ThrowNextScenePass();
						return;
					}
					// "dclf:compat=0|1": the compatibility monitor off or on, as its menu checkbox; "dclf:compat-report": its report written.
					if (command == "dclf:compat=0" || command == "dclf:compat=1") {
						Diagnostics::PlayerParity::Request(command == "dclf:compat=1");
						return;
					}
					if (command == "dclf:compat-report") {
						logger::info("[DCLF] test command: compatibility report {}", Diagnostics::PlayerParity::WriteReport());
						return;
					}
					// "dclf:foreign-write=<n>": a foreign plugin's write, for the monitor to find: on the main thread, the alpha threshold of
					// the first alpha-tested geometry of up to n references near the player, stored straight into the property (no hook sees it).
					if (command.rfind("dclf:foreign-write=", 0) == 0) {
						const auto wanted = static_cast<std::uint32_t>(std::strtoul(command.c_str() + 19, nullptr, 10));
						if (auto* tasks = SKSE::GetTaskInterface())
							tasks->AddTask([wanted] {
								auto* tes = RE::TES::GetSingleton();
								auto* player = RE::PlayerCharacter::GetSingleton();
								if (!tes || !player)
									return;
								std::uint32_t written = 0;
								tes->ForEachReferenceInRange(player, 4096.0f, [&](RE::TESObjectREFR* a_reference) {
									auto* root = a_reference && a_reference != player ? a_reference->Get3D() : nullptr;
									if (!root)
										return RE::BSContainer::ForEachResult::kContinue;
									RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
										auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
										if (!alpha)
											return RE::BSVisit::BSVisitControl::kContinue;
										alpha->alphaThreshold = static_cast<std::uint8_t>(alpha->alphaThreshold ^ 1);
										const auto* file = a_reference->GetFile(0);
										logger::info("[DCLF] test command: foreign write, alpha threshold of '{}' under reference {:08X} ({}) now {}", a_geometry->name.c_str(),
											a_reference->GetFormID(), file ? file->GetFilename() : "created in game", alpha->alphaThreshold);
										++written;
										return RE::BSVisit::BSVisitControl::kStop;
									});
									return written < wanted ? RE::BSContainer::ForEachResult::kContinue : RE::BSContainer::ForEachResult::kStop;
								});
							});
						return;
					}
					// "dclf:rebuild": a graph rebuild requested (PersistentGraphHost::RequestRebuild, here at Present on the graph's owner
					// thread), which the next frame's build point performs: every recording invalidated, so that frame waits at its start for
					// a snapshot recorded on the new build (IndirectDraws::AdoptCurrentSnapshot; the strict epochs line counts the waits).
					if (command == "dclf:rebuild") {
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						else
							logger::warn("[DCLF] test command: no render graph host to rebuild");
						return;
					}
					// Shader hot reload (dclf-hot-reload.md), served at the next present: "[ORG] shader reload: N of M pipelines rebuilt (K failed,
					// U unchanged) in S s" marks the rebuilds done, "[ORG] shader reload: N pipelines swapped in at graph build B" the swap.
					// "dclf:reload-shaders": every live pipeline, as the menu's button asks it (the unchanged ones stay). "=all": every one built
					// and swapped even when unchanged (the swap exercised without an edit). "=<file>": those built from that shader file, as the
					// shader watcher asks for an edit (an include: every one).
					if (command == "dclf:reload-shaders" || command.rfind("dclf:reload-shaders=", 0) == 0) {
						const std::string value = command.size() > 20 ? command.substr(20) : std::string{};
						if (value == "all")
							RenderGraphRuntime::RequestShaderReload({}, true);
						else
							RenderGraphRuntime::RequestShaderReload(value);
						return;
					}
					// "dclf:ts-dump=<epochs>": the next <epochs> completed epochs log their graphics timestamp ranges in GPU order ("[TSDUMP]").
					if (command.rfind("dclf:ts-dump=", 0) == 0) {
						RenderGraphRuntime::RequestTimestampDump(static_cast<std::uint32_t>(std::max(0, std::atoi(command.c_str() + 13))));
						return;
					}
					// "dclf:async-compute=0|1|default": the passes preferring async compute on the graphics queue (0), the compute queue (1)
					// or as CS_DCLF_ASYNC_COMPUTE says, from a graph rebuild on (dclf:rebuild's), so one run compares the queues.
					if (command.rfind("dclf:async-compute=", 0) == 0) {
						const std::string value = command.substr(19);
						DCLF::Draws::asyncComputeOverride.store(value == "default" ? -1 : value != "0", std::memory_order_relaxed);
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						return;
					}
					// Cost attribution (diagnostics; a frozen camera, since a cut build leaves the draws reading an older frame's outputs):
					// "dclf:cut=<prefixes>": the pass key prefixes the graph leaves out, comma-separated ("*" every pass, empty none), from a
					// rebuild at the next build point (Draws::GraphConditions); "dclf:epochs=0|1": DCLF's epochs submitted or not (nothing they
					// claim is drawn while not); "dclf:early-flush=0|1|default": the D3D11 flush before each epoch.
					if (command.rfind("dclf:cut=", 0) == 0) {
						DCLF::Draws::SetPassCuts(command.substr(9));
						return;
					}
					if (command.rfind("dclf:epochs=", 0) == 0) {
						// "dclf:epochs=flush|enqueue|signal": not submitted, each still closing DXVK's command list at its point, then
						// enqueuing an empty submission, or one signalling a timeline (RenderGraphRuntime::SkipDclfEpochs).
						const std::string value = command.substr(12);
						const std::uint32_t boundary = value == "flush" ? 1u : value == "enqueue" ? 2u : value == "signal" ? 3u : value == "cmd" ? 4u :
													   value == "cmd3" ? 5u : 0u;
						RenderGraphRuntime::SkipDclfEpochs(value == "1" ? 0u : ~0u, boundary);
						return;
					}
					// "dclf:submit-dump=<epochs>": the next epochs log their enqueued submissions' waits and signals ("[ORG] submission dump").
					if (command.rfind("dclf:submit-dump=", 0) == 0) {
						RenderGraphRuntime::RequestSubmissionDump(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 17, nullptr, 10)));
						return;
					}
					// "dclf:worker-spin=<us>" / "dclf:worker-sync=0|1": each skipped epoch loads DXVK's worker thread that long, and/or
					// waits for it there (RenderGraphRuntime::SetSkippedEpochWorkerLoad).
					if (command.rfind("dclf:worker-spin=", 0) == 0 || command.rfind("dclf:worker-sync=", 0) == 0) {
						static std::uint32_t spinUs = 0;
						static bool sync = false;
						if (command[13] == 'p')
							spinUs = static_cast<std::uint32_t>(std::strtoul(command.c_str() + 17, nullptr, 10));
						else
							sync = command.substr(17) == "1";
						RenderGraphRuntime::SetSkippedEpochWorkerLoad(spinUs, sync);
						return;
					}
					// "dclf:epoch-spin=<us>": each skipped epoch busy-waits that long on the render thread (0: none).
					if (command.rfind("dclf:epoch-spin=", 0) == 0) {
						RenderGraphRuntime::SetSkippedEpochSpin(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 16, nullptr, 10)));
						return;
					}
					// "dclf:pass-queries=0|1": the graph's lists recorded with their pass timestamp queries and Tracy GPU zones, or without.
					if (command.rfind("dclf:pass-queries=", 0) == 0) {
						org::experimental::DiagnosticSkipPassQueries().store(command.substr(18) == "0", std::memory_order_relaxed);
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						return;
					}
					// "dclf:drop-lists=0|1": DCLF's epochs submit their signals alone (RenderGraphRuntime::DropDclfLists).
					if (command.rfind("dclf:drop-lists=", 0) == 0) {
						RenderGraphRuntime::DropDclfLists(command.substr(16) == "1");
						return;
					}
					// "dclf:coalesce=0|1": each epoch's submissions as one (RenderGraphRuntime::CoalesceEpochSubmits; unsafe: empty epochs only).
					if (command.rfind("dclf:coalesce=", 0) == 0) {
						RenderGraphRuntime::CoalesceEpochSubmits(command.substr(14) == "1");
						return;
					}
					// "dclf:split-enqueue=0|1": each epoch submission its own enqueue (DXVK's submission trace times each).
					if (command.rfind("dclf:split-enqueue=", 0) == 0) {
						RenderGraphRuntime::CoalesceEpochSubmits(false, command.substr(19) == "1");
						return;
					}
					// "dclf:submitted-spin=<us>": each submitted DCLF epoch busy-waits that long on the render thread after its enqueue.
					if (command.rfind("dclf:submitted-spin=", 0) == 0) {
						RenderGraphRuntime::SetSkippedEpochSpin(static_cast<std::uint32_t>(std::strtoul(command.c_str() + 20, nullptr, 10)), true);
						return;
					}
					// "dclf:hold=<shadow,z,colour,forward,fp>": those epochs' submissions go with the next epoch's enqueue (empty: none held).
					if (command.rfind("dclf:hold=", 0) == 0) {
						using Segment = RenderGraphRuntime::Segment;
						const std::string list = "," + command.substr(10) + ",";
						std::uint32_t mask = 0;
						for (const auto& [name, segment] : { std::pair{ "shadow", Segment::ShadowView }, std::pair{ "z", Segment::ZPrepass },
								 std::pair{ "colour", Segment::MainOpaque }, std::pair{ "forward", Segment::Forward }, std::pair{ "fp", Segment::FirstPerson } })
							if (list.find(std::string(",") + name + ",") != std::string::npos)
								mask |= 1u << static_cast<std::uint32_t>(segment);
						RenderGraphRuntime::HoldDclfSubmissions(mask);
						return;
					}
					// "dclf:heap-dedupe=0|1": BasicRHI's descriptor heap binds of the heaps a list already bound emit nothing, from a rebuild.
					if (command.rfind("dclf:heap-dedupe=", 0) == 0) {
						rhi::DiagnosticDedupeHeapBinds().store(command.substr(17) == "1", std::memory_order_relaxed);
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						return;
					}
					// "dclf:skip-epochs=<shadow,z,colour,forward,fp>": those epochs alone not submitted (empty: every one submitted).
					if (command.rfind("dclf:skip-epochs=", 0) == 0) {
						using Segment = RenderGraphRuntime::Segment;
						const std::string list = "," + command.substr(17) + ",";
						std::uint32_t mask = 0;
						for (const auto& [name, segment] : { std::pair{ "shadow", Segment::ShadowView }, std::pair{ "z", Segment::ZPrepass },
								 std::pair{ "colour", Segment::MainOpaque }, std::pair{ "forward", Segment::Forward }, std::pair{ "fp", Segment::FirstPerson } })
							if (list.find(std::string(",") + name + ",") != std::string::npos)
								mask |= 1u << static_cast<std::uint32_t>(segment);
						RenderGraphRuntime::SkipDclfEpochs(mask);
						return;
					}
					// "dclf:list-heaps=0|1": recordings bind the descriptor heaps at each command list's start, or not (unsafe off: only
					// passes binding their own heaps then work; timing an empty-epoch run only), from a rebuild.
					if (command.rfind("dclf:list-heaps=", 0) == 0) {
						org::experimental::DiagnosticSkipListHeapBinds().store(command.substr(16) == "0", std::memory_order_relaxed);
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						return;
					}
					// "dclf:list-barriers=external|all|default": recordings leave out their full entry and exit barriers (the closed
					// execution's too) or every barrier (unsafe: empty-epoch timing only), from a rebuild.
					if (command.rfind("dclf:list-barriers=", 0) == 0) {
						const std::string value = command.substr(19);
						org::experimental::DiagnosticSkipListBarriers().store(value == "external" ? 1 : value == "all" ? 2 : 0, std::memory_order_relaxed);
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						return;
					}
					// "dclf:rhi-globals=0|1": BasicRHI records global barriers or none (the upload lists' too; unsafe off: empty-epoch
					// timing only), recordings from a rebuild.
					if (command.rfind("dclf:rhi-globals=", 0) == 0) {
						rhi::DiagnosticSkipGlobalBarriers().store(command.substr(17) == "0", std::memory_order_relaxed);
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						return;
					}
					// "dclf:queue-boundary=0|1": the graph's external entry and exit barriers (unsafe off: timing only), from a rebuild.
					if (command.rfind("dclf:queue-boundary=", 0) == 0) {
						const bool on = command.substr(20) != "0";
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->SetQueueBoundary({ .entry = on, .exit = on });
						return;
					}
					if (command.rfind("dclf:early-flush=", 0) == 0) {
						const std::string value = command.substr(17);
						RenderGraphRuntime::SetEarlyFlush(value == "default" ? -1 : value != "0");
						return;
					}
					// "dclf:shadow-min-texels=<texels>": the sun cascades' size cull (0: off), from the next shadow commit.
					if (command.rfind("dclf:shadow-min-texels=", 0) == 0) {
						DCLF::Draws::shadowMinTexels.store(std::strtof(command.c_str() + 23, nullptr), std::memory_order_relaxed);
						return;
					}
					// "dclf:shadow-probe=<label>": the sun views' size probe, the readback pinned to that shadow view label; -1 off.
					if (command.rfind("dclf:shadow-probe=", 0) == 0) {
						DCLF::Draws::shadowSizeProbe.store(std::atoi(command.c_str() + 18), std::memory_order_relaxed);
						return;
					}
					// "dclf:cull=off|frustum|occlusion": the GPU culling's mode (CS_DCLF_CULL's live value).
					if (command.rfind("dclf:cull=", 0) == 0) {
						const std::string value = command.substr(10);
						DCLF::Toggles::Get().Requested().cullMode = value == "occlusion" ? 2 : value == "frustum" ? 1 : 0;
						return;
					}
					if (command.rfind("dclf:", 0) == 0) {
						const auto equals = command.find('=');
						const auto name = command.substr(5, equals == std::string::npos ? std::string::npos : equals - 5);
						const bool on = equals != std::string::npos && command.substr(equals + 1) != "0";
						bool found = false;
						for (const auto& info : DCLF::ToggleTable())
							if (info.name == name) {
								DCLF::SetToggleValue(DCLF::Toggles::Get().Requested(), info, on);
								found = true;
							}
						if (!found)
							logger::warn("[DCLF] test command: no toggle named '{}'", name);
						return;
					}
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
				const std::string value = DCLF::SwitchValue(DCLF::Switch::TestToggle);
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
				if (frames.empty() || DCLF::Scene::FrameView::IsLoadingScreenUp())
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
				Parse(DCLF::SwitchValue(DCLF::Switch::TestTurn), ranges);
				Parse(DCLF::SwitchValue(DCLF::Switch::TestMove), moves);
			}

			void OnFrame()
			{
				if ((ranges.empty() && moves.empty()) || DCLF::Scene::FrameView::IsLoadingScreenUp())
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
	}

	namespace
	{
		std::mutex queuedMutex;
		std::vector<std::string> queuedCommands;
	}

	void QueueTestCommand(std::string a_command)
	{
		std::lock_guard lock(queuedMutex);
		queuedCommands.push_back(std::move(a_command));
	}

	void RunTestHarness(std::string_view a_feature)
	{
		{
			std::vector<std::string> queued;
			{
				std::lock_guard lock(queuedMutex);
				queued.swap(queuedCommands);
			}
			for (auto& command : queued) {
				// "dclf:feature=0|1": the feature's menu toggle (as CS_DCLF_TEST_TOGGLE flips it), for in-session on/off A/Bs.
				if (command.rfind("dclf:feature=", 0) == 0) {
					const bool off = command.substr(13) == "0";
					logger::info("[DCLF] test command: feature {}", off ? "off" : "on");
					globals::state->SetFeatureDisabled(std::string(a_feature), off);
					continue;
				}
				TestCommands::Run(std::move(command), 0);
			}
		}
		// CS_DCLF_TEST_EXIT=<frame>: a startup loop's run ends here, loading screens not counted. The log is flushed, then the
		// process ends without the engine's shutdown, which is slow and not what such a run tests.
		static const std::uint32_t exitFrame = static_cast<std::uint32_t>(std::strtoul(DCLF::SwitchValue(DCLF::Switch::TestExit).c_str(), nullptr, 10));
		static std::uint32_t exitCount = 0;
		if (exitFrame && !DCLF::Scene::FrameView::IsLoadingScreenUp() && ++exitCount >= exitFrame) {
			logger::info("[DCLF] test exit at frame {}", exitCount);
			spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& a_logger) { a_logger->flush(); });
			::TerminateProcess(::GetCurrentProcess(), 0);
		}
		static TestCommands testCommands;
		testCommands.OnFrame();
		static TestToggle testToggle;
		testToggle.OnFrame(std::string(a_feature));
		static TestTurn testTurn;
		testTurn.OnFrame();
	}
}
