// The main cull input's entry and the main walk's install note (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveMainCullInput (InstallFeatures, before DCLF's threads exist), read-only after.
		struct Resolved
		{
			MainCullInputEntry entry;
			bool registered = false;
		};

		Resolved& State()
		{
			static Resolved resolved;
			return resolved;
		}

		// Written once on the main thread at PostPostLoad (a feature's early hooks, before PrimaryCull::Install reads it).
		bool mainWalkInstalled = false;
	}

	void ResolveMainCullInput(const Api::MainCullInputContribution& a_registered)
	{
		auto& resolved = State();
		resolved.registered = !a_registered.name.empty();
		if (!resolved.registered)
			return;
		auto& entry = resolved.entry;
		entry.name = std::string(a_registered.name);
		entry.programsName = std::string(a_registered.programsName);
		entry.zeroHeader.assign(a_registered.headerWords, 0u);
		entry.contribution = a_registered;
		entry.contribution.name = entry.name;
		entry.contribution.programsName = entry.programsName;
	}

	const MainCullInputEntry* MainCullInput()
	{
		const auto& resolved = State();
		return resolved.registered ? &resolved.entry : nullptr;
	}

	std::span<const std::uint32_t> MainCullPrograms()
	{
		const auto* input = MainCullInput();
		return input ? input->contribution.programs() : std::span<const std::uint32_t>{};
	}

	void NoteMainWalkInstalled() { mainWalkInstalled = true; }
	bool MainWalkInstalled() { return mainWalkInstalled; }
}
