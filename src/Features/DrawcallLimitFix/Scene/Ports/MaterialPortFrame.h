#pragma once

#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPortFeature.h"
#include "Features/DrawcallLimitFix/Scene/Ports/MaterialPortVanilla.h"

namespace DCLF::MaterialPort
{
	/** @brief The per-frame sources SetupMaterial reads (sampled by the render thread at the frame's start: FrameGlobals::material). */
	struct MaterialFrame
	{
		VanillaFrame vanilla;  // MaterialPortVanilla.h
		FeatureFrame feature;  // MaterialPortFeature.h
	};
}
