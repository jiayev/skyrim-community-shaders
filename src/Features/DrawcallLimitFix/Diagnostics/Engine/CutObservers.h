#pragma once

#include "Features/DrawcallLimitFix/Scene/Ports/FadeState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Diagnostics::CutObservers
{
	/**
	 * @brief The primary cull's observers (PrimaryCull's hook points; Events.h):
	 * - CS_DCLF_PERSISTENT_PARITY:
	 *   - <- CUT: every 60th frame (offset 11) the kept cut against one planned whole for the same snapshot, entry by entry (the plan,
	 *     the members - geometries, engine-drawn or not, objects, switch paths -, the switches, mixed, the eligible roots; admission and
	 *     the walk are history, not compared);
	 *   - <- STAND-IN WALK: with every entry walked (Policy::WalkEveryEntry), a geometry handed to the registration from an entry the
	 *     walk's decision (Cut::walk) leaves alone;
	 *   - <- LIGHT MASKS: the owned members in view whose activeLightMask is not 0 after the registration jobs;
	 *   - <- LIST FILTER: on the frames the engine builds its scene lists whole (Policy::FilterSceneLists), the roots the filter would
	 *     have left out must be ones the stand-in does nothing for (the dry run), and the engine's lists must hold DCLF's enumeration;
	 * - CS_DCLF_FADE_PARITY:
	 *   - <- FADE PORT: every 30th frame, up to 64 fade roots, the port against the engine's functions on node copies;
	 *   - the fade roots' nodes at the list jobs' end every 30th frame, for the fade state's comparison (NodeSnapshot).
	 */
	void Install(bool a_scene, bool a_fade);

	/** @brief The fade roots' nodes as the list jobs left them in a_frame (null: not taken that frame). */
	const std::vector<FadeNodeState>* NodeSnapshot(std::uint32_t a_frame);

	/** @brief The observers' counts since the last Take (render thread, the report). */
	struct Counts
	{
		std::uint64_t cutChecks = 0, cutMismatches = 0;
		std::uint64_t cutLagging = 0;  // entries that differed at one check only (the snapshot behind the engine)
		std::uint64_t walkMissed = 0;
		std::string walkMissedFirst;
		std::uint64_t maskChecked = 0, maskSun = 0, maskOther = 0;
		std::string maskFirst;
		std::uint64_t filterChecked = 0, filterMissed = 0;
		std::uint64_t listChecks = 0, listMissing = 0, listExtra = 0;  // missing: the engine's not in DCLF's; extra: DCLF's, not hidden, not the engine's
		std::string listFirst;
		// The kept lists against a rebuild's (KeptSceneListsBuilt): missing, a root a rebuild lists; extra, one it does not.
		std::uint64_t keptChecks = 0, keptMissing = 0, keptExtra = 0;
		std::string keptFirst;
		FadeState::PortCheck fadePort;
	};
	Counts Take();
}
