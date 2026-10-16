// The root states' entry, list view and calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <algorithm>
#include <iterator>
#include <limits>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveRootStates (InstallFeatures, before DCLF's threads exist), read-only after.
		struct Resolved
		{
			RootStatesEntry entry;
			bool registered = false;
		};

		Resolved& State()
		{
			static Resolved resolved;
			return resolved;
		}

		// Written only by NoteRootStatesInstalled (the early hooks, before DCLF's threads exist), read-only after.
		bool rootStatesInstalled = false;
	}

	void ResolveRootStates(const Api::RootStatesContribution& a_registered)
	{
		auto& resolved = State();
		resolved.registered = !a_registered.name.empty();
		if (!resolved.registered)
			return;
		auto& entry = resolved.entry;
		entry.name = std::string(a_registered.name);
		entry.contribution = a_registered;
		entry.contribution.name = entry.name;
	}

	const RootStatesEntry* RootStates()
	{
		const auto& resolved = State();
		return resolved.registered ? &resolved.entry : nullptr;
	}

	void NoteRootStatesInstalled() { rootStatesInstalled = true; }
	bool RootStatesInstalled() { return rootStatesInstalled; }

	Api::RootListView RootListOf(const TableFamilies& a_families)
	{
		Api::RootListView view;
		const auto* states = RootStates();
		if (!states)
			return view;
		const auto* family = a_families.Find(states->contribution.family);
		if (!family || !family->columns)
			return view;
		states->contribution.view(family->columns.get(), view);
		view.objectRoot = family->objectRow;
		view.contentEpoch = family->ContentEpoch();
		return view;
	}

	Api::RootListView RootListOf(const SceneTables& a_tables)
	{
		return RootListOf(a_tables.families);
	}

	void RootStatesTest(const SceneCapture::NodeRecord& a_node, const FrameGlobals& a_globals, Api::RootTestOutput& a_out)
	{
		a_out = {};
		if (const auto* states = RootStates())
			states->contribution.test({ &a_node, a_globals }, a_out);
	}

	void RootStatesGlobalsKey(const FrameGlobals& a_globals, std::uint32_t& a_witness)
	{
		if (const auto* states = RootStates(); states && states->contribution.globalsKey)
			states->contribution.globalsKey({ nullptr, a_globals }, a_witness);
	}

	std::uint32_t RootStatesObjectFlags()
	{
		const auto* states = RootStates();
		return states ? states->contribution.objectFlags : 0u;
	}

	Api::TableFamilyId RootStatesFamily()
	{
		const auto* states = RootStates();
		return states ? states->contribution.family : Api::TableFamilyId{};
	}

	void RootStatesKick()
	{
		if (const auto* states = RootStates(); states && states->contribution.kick)
			states->contribution.kick();
	}

	void RootStatesWait()
	{
		if (const auto* states = RootStates(); states && states->contribution.wait)
			states->contribution.wait();
	}

	void RootStatesFrameStart()
	{
		if (const auto* states = RootStates(); states && states->contribution.frameStart)
			states->contribution.frameStart();
	}

	void RootStatesLatchGlobals(float (&a_eye)[4], float (&a_treeHeight)[2])
	{
		if (const auto* states = RootStates(); states && states->contribution.latchGlobals) {
			states->contribution.latchGlobals(a_eye, a_treeHeight);
			return;
		}
		// No camera seen, and the height test off.
		std::fill(std::begin(a_eye), std::end(a_eye), 0.0f);
		a_treeHeight[0] = 0.0f;
		a_treeHeight[1] = std::numeric_limits<float>::infinity();
	}
}
