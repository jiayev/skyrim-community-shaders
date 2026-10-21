#pragma once

// The shadow epochs contract's Core side (Api/ShadowEpochs.h): its entry, resolved once at InstallFeatures (none registered: FrameDriver's
// shadow points do only Core's bookkeeping, and the engine draws every shadow view and occlusion map; the shadow casters'
// points are reached only from the registering feature's hooks), which FrameDriver calls at those points.

#include "Features/DrawcallLimitFix/Api/ShadowEpochs.h"

namespace DCLF::Extensions
{
	/** @brief Main thread, the end of InstallFeatures: the entry from the registration (no handlers: none). Once. */
	void ResolveShadowEpochs(const Api::ShadowEpochsContribution& a_registered);
	/** @brief Any thread, after InstallFeatures (null before): the shadow epochs' handlers; null when none is registered. */
	const Api::ShadowEpochsContribution* ShadowEpochs();
}
