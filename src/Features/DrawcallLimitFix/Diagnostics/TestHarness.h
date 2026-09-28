#pragma once

#include <string_view>

namespace DCLF
{
	/**
	 * @brief The test harness for unattended runs, every Present: CS_DCLF_TEST_COMMANDS, CS_DCLF_TEST_TOGGLE,
	 * CS_DCLF_TEST_TURN and CS_DCLF_TEST_MOVE (Switches.h). a_feature is the feature's short name, which the
	 * toggle switches.
	 */
	void RunTestHarness(std::string_view a_feature);
}
