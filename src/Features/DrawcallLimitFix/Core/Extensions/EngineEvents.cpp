// The engine side's calls to the registered stand-in observers (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written once on the main thread at PostPostLoad (PrimaryCull::Install, before InstallFeatures reads it).
		bool standInInstalled = false;
	}

	void NoteStandInInstalled() { standInInstalled = true; }
	bool StandInInstalled() { return standInInstalled; }

	void StandInRootsChanged(const Api::StandInRootsChange& a_change)
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.changed)
				observer.changed(a_change);
	}

	void StandInRootsCleared()
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.cleared)
				observer.cleared();
	}

	void StandInUpdateEntry(bool a_running)
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.updateEntry)
				observer.updateEntry(a_running);
	}
}
