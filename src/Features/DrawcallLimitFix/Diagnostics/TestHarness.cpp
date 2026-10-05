#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "TestHarness.h"

#include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Globals.h"
#include "State.h"

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
					// "dclf:<toggle name>=<0|1>": a live toggle (Toggles.h, ToggleInfo::name), as the menu sets it, so one run can
					// compare a class drawn by DCLF and by the engine from the same camera.
					if (command.rfind("dclf:", 0) == 0) {
						const auto equals = command.find('=');
						const auto name = command.substr(5, equals == std::string::npos ? std::string::npos : equals - 5);
						const bool on = equals != std::string::npos && command.substr(equals + 1) != "0";
						bool found = false;
						for (const auto& info : DCLF::ToggleTable())
							if (info.name == name) {
								DCLF::Toggles::Get().Requested().*info.member = on;
								found = true;
							}
						if (!found)
							logger::warn("[DCLF] test command: no toggle named '{}'", name);
						continue;
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
				Parse(DCLF::SwitchValue(DCLF::Switch::TestTurn), ranges);
				Parse(DCLF::SwitchValue(DCLF::Switch::TestMove), moves);
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
	}

	void RunTestHarness(std::string_view a_feature)
	{
		// CS_DCLF_TEST_EXIT=<frame>: a startup loop's run ends here, loading screens not counted. The log is flushed, then the
		// process ends without the engine's shutdown, which is slow and not what such a run tests.
		static const std::uint32_t exitFrame = static_cast<std::uint32_t>(std::strtoul(DCLF::SwitchValue(DCLF::Switch::TestExit).c_str(), nullptr, 10));
		static std::uint32_t exitCount = 0;
		if (exitFrame && !DCLF::SceneStore::IsLoadingScreenUp() && ++exitCount >= exitFrame) {
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
