#pragma once

#include <cstdint>
#include <string>

namespace DCLF::Diagnostics::PortalParity
{
	/**
	 * @brief The portal views' parity (CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY): at each Build (PortalWalkBuilt, before the list
	 * jobs), DCLF's programs against the engine's portal-graph entry as Main::Update's walk left it - the rooms in its map, each room's
	 * and unbound space's BSCompoundFrustum (the structure exactly, the planes within rounding), the boxes' corners and silhouette sets,
	 * the rooms the walk popped - with a frame whose camera moved after the engine's walk tallied apart. The line every 300 builds
	 * (Features::Portals::PortalViewsLine).
	 */
	void Install();

	struct Tally
	{
		std::uint64_t frames = 0, rooms = 0, roomsMissing = 0, roomsExtra = 0, programs = 0, programsDiffer = 0;
		std::uint64_t unboundDiffer = 0, flagDiffer = 0, accumDiffer = 0;
		// Unbound space's programs that differ only by graph plane states the engine's other occluder tests wrote before its walk: built
		// again with the engine's states, DCLF's matches (PortalParity.cpp, CompareProgram). Not a difference.
		std::uint64_t unboundEngineStates = 0;
		std::uint64_t ops = 0, sets = 0;  // what the compared programs held
		float largest = 0.0f;             // the largest plane difference among the programs that matched in structure
		std::uint64_t eyeMoved = 0, movedDiffer = 0;  // frames whose camera moved after the engine's walk, and their differences
		float largestMove = 0.0f;
		std::uint64_t turned = 0, boxChecks = 0, boxCornersDiffer = 0, boxSetsDiffer = 0;
		float largestCorner = 0.0f;
		std::string boxFirst;
		std::uint64_t planesDiffer = 0;
		std::string planesFirst;
		std::string occluderFirst;
		std::string first;
	};
	/** @brief The tally since the last Take, reset. */
	Tally Take();
}
