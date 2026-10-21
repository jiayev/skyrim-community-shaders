#pragma once

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
}
