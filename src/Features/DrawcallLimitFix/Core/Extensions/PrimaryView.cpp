// The primary view's entry (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/PrimaryView.h"

#include <utility>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolvePrimaryView (InstallFeatures, before DCLF's threads exist), read-only after.
		Api::PrimaryViewContribution resolved;
		bool registered = false;
	}

	void ResolvePrimaryView(const Api::PrimaryViewContribution& a_registered)
	{
		static bool once = false;
		if (std::exchange(once, true))
			stl::report_and_fail("Drawcall Limit Fix: the primary view was resolved twice");
		if (!a_registered.beforeOpaque)
			return;
		resolved = a_registered;
		registered = true;
	}

	const Api::PrimaryViewContribution* PrimaryView() { return registered ? &resolved : nullptr; }
}
