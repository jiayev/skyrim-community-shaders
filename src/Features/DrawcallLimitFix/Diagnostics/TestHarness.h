#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace DCLF
{
	/**
	 * @brief The test harness for unattended runs, every Present: CS_DCLF_TEST_COMMANDS, CS_DCLF_TEST_TOGGLE,
	 * CS_DCLF_TEST_TURN, CS_DCLF_TEST_MOVE and CS_DCLF_TEST_EXIT (Switches.h). a_feature is the feature's short name, which the
	 * toggle switches.
	 */
	void RunTestHarness(std::string_view a_feature);

	/**
	 * @brief Any thread: a_command run at the next Present as a CS_DCLF_TEST_COMMANDS command is (a "dclf:" command or a console
	 * command), for a remote client (DevBench's communityshaders.dclf).
	 */
	void QueueTestCommand(std::string a_command);

	/** @brief CS_DCLF_TEST_COMMANDS' progress as of the last Present (frames as the list counts them: loading screens not counted). */
	struct TestHarnessProgress
	{
		std::uint32_t frame = 0;
		std::uint32_t shift = 0;      // frames the list's waits held so far: every later command runs that much later
		std::uint32_t next = 0;       // the next command's index
		std::uint32_t commands = 0;   // the list's length
		std::uint32_t nextFrame = 0;  // the frame the next command runs at, as of now (a wait still holding moves it)
		std::string nextCommand;
		std::string waiting;  // the condition the list holds on; empty when none
	};

	/** @brief Any thread: the list's progress (DevBench's communityshaders.dclf replies with it). */
	TestHarnessProgress TestHarnessStatus();
}
