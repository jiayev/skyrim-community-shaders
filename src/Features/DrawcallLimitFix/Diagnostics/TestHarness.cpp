// The test harness: the CS_DCLF_TEST_* switches' scripted commands, toggles, turns, moves and exit, run every Present on the render
// (main) thread, DCLF installed or not.
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "TestHarness.h"

#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Globals.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "State.h"

#include <OpenRenderGraph/PersistentGraphHost.h>

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
					// "dclf:async-compute=0|1|default": the passes preferring async compute on the graphics queue (0), the compute queue (1)
					// or as CS_DCLF_ASYNC_COMPUTE says, from a graph rebuild on (dclf:rebuild's), so one run compares the queues.
					if (command.rfind("dclf:async-compute=", 0) == 0) {
						const std::string value = command.substr(19);
						DCLF::Draws::asyncComputeOverride.store(value == "default" ? -1 : value != "0", std::memory_order_relaxed);
						if (auto* host = RenderGraphRuntime::Get().Host())
							host->RequestRebuild();
						return;
					}
					// "dclf:shadow-min-texels=<texels>": the sun cascades' size cull (0: off), from the next shadow commit.
					if (command.rfind("dclf:shadow-min-texels=", 0) == 0) {
						DCLF::Draws::shadowMinTexels.store(std::strtof(command.c_str() + 23, nullptr), std::memory_order_relaxed);
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
