#pragma once

#include <string>

/**
 * @brief Tests of what it costs when a given resource is in system memory, as a driver under memory pressure would leave it.
 *
 * The commands run from the test harness (CS_DCLF_TEST_COMMANDS). Nothing happens until one does.
 *
 *   dclf:spill=<filter>         Moves the D3D11 resources a filter picks to system memory, and holds them there.
 *   dclf:demote=<filter>        Moves the images a filter picks into DXVK's spill pool: still video memory, demoted first under pressure.
 *   dclf:spill-clear            Brings back everything held.
 *   dclf:spill-check            Logs how many of those held are in system memory now.
 *   dclf:frame-window=<label>   Logs the frame times since the last window, under its label, and starts a new one. "-" logs nothing.
 *   dclf:ballast=<MiB>[:<pri>]  Holds that much device memory at that priority (default 1), to make real memory pressure. 0 frees it.
 *
 * A filter is alternatives separated by '|'. Each is terms separated by '&'. A term is text found in "owner/name" without regard to
 * case, or one of class:sampled, rt, ds, storage, buffer, vb, ib, ubo, ssbo, indirect.
 *
 * What a filter cannot reach: CS_VK_SPILL=<module>,... (VkDriverTrace) places a module's own allocations in system memory from their
 * creation, for FidelityFX and DLSS-G. See docs/development/vram-spill-lab.md.
 */
namespace Vram::SpillLab
{
	/** @brief Runs a command if it is one of these. Render thread. */
	bool Command(const std::string& a_command);

	/** @brief Every Present. */
	void OnPresent();
}
