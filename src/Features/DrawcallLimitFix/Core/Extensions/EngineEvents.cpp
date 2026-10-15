// The engine side's calls to the registered stand-in observers (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written once on the main thread at PostPostLoad (PrimaryCull::Install, before InstallFeatures reads it).
		bool standInInstalled = false;
		// Written once on the main thread at PostPostLoad (the hidden-node source's install, before InstallFeatures reads it).
		std::uint32_t hiddenSourceSites = 0;
	}

	void NoteStandInInstalled() { standInInstalled = true; }
	bool StandInInstalled() { return standInInstalled; }
	void NoteHiddenSourceSites(std::uint32_t a_sites) { hiddenSourceSites = a_sites; }
	std::uint32_t HiddenSourceSites() { return hiddenSourceSites; }

	AdoptionGenerations SampleAdoptionGuards()
	{
		AdoptionGenerations generations{};
		const auto guards = RegisteredAdoptionGuards();
		for (std::size_t g = 0; g < guards.size(); ++g)
			generations[g] = guards[g].generation();
		return generations;
	}

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
