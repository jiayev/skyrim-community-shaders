#pragma once

// The primary view contract's Core side (Api/PrimaryView.h): its entry, resolved once at InstallFeatures (none registered: FrameDriver's
// main pass points do only Core's bookkeeping), which FrameDriver calls at the main pass's points.

#include "Features/DrawcallLimitFix/Api/PrimaryView.h"

namespace DCLF::Extensions
{
	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no handlers: none). Once. */
	void ResolvePrimaryView(const Api::PrimaryViewContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): the main view's handlers; null when none is registered. */
	const Api::PrimaryViewContribution* PrimaryView();
}
