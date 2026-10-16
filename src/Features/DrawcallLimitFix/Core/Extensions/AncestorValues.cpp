// The ancestor value's entry (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveAncestorValue (InstallFeatures, before DCLF's threads exist), read-only after.
		AncestorValueEntry resolved;
	}

	void ResolveAncestorValue(const Api::AncestorValueContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the ancestor value was resolved twice");
		if (a_registered.name.empty())
			return;
		auto& entry = resolved;
		entry.registered = true;
		entry.name = std::string(a_registered.name);
		entry.recordWord = a_registered.recordWord;
		entry.none = a_registered.none;
		for (std::size_t c = 0; c < entry.ancestorRtti.size(); ++c)
			entry.ancestorRtti[c] = a_registered.ancestorRtti[c] ? a_registered.ancestorRtti[c]() : nullptr;
		entry.whileOn = a_registered.whileOn;
		entry.generation = a_registered.generation;
		entry.fill = a_registered.fill;
	}

	const AncestorValueEntry* AncestorValue() { return resolved.registered ? &resolved : nullptr; }

	bool AncestorValueOn() { return resolved.registered && ClassifyInputOn(resolved.whileOn); }
}
