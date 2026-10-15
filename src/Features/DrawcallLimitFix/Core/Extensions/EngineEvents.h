#pragma once

// The engine event contracts' Core side (Api/EngineEvents.h): what a stand-in observer is handed, and the calls the engine side makes.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"

#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Api
{
	/** @brief The stood-in roots' change (PrimaryCull::SyncFadeOwnership): the roots that left and joined, and the BSTreeNodes among them. */
	class StandInRootsChange
	{
	public:
		const std::vector<const RE::NiAVObject*>& left;
		const std::vector<const RE::NiAVObject*>& joined;
		const std::vector<const RE::NiAVObject*>& treesLeft;
		const std::vector<const RE::NiAVObject*>& treesJoined;
	};
}

namespace DCLF::Extensions
{
	/** @brief Main thread (PrimaryCull::Install): the primary cull installed, so the stand-in observers will be called. */
	void NoteStandInInstalled();
	/** @brief Main thread, at install: whether NoteStandInInstalled came (InstallContext::RegisterStandInObserver's answer). */
	bool StandInInstalled();
	// The primary cull's calls, each to every stand-in observer in order (Api::StandInObserver).
	void StandInRootsChanged(const Api::StandInRootsChange& a_change);
	void StandInRootsCleared();
	void StandInUpdateEntry(bool a_running);
}
