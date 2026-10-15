#pragma once

// Shadows/Sun: the sun's shadow views, drawn by Core's shadow engine. The feature registers the Sun source (Api::ShadowSourceContribution,
// Api/ShadowViews.h: the directional views draw while it is registered) and its camera hook (SunViews: BSShadowDirectionalLight::
// UpdateCamera, which writes the sun's cascades and full-frustum planes into Core's board, ShadowCameras::Sun, from the cameras it set up).
//
// Core keeps the shadow engine: the shadow epoch and its capability and captures, the views list, the light selection, the boards and
// the camera math (Core/Render/Shadow/ShadowCameras.h), the sun's accumulation (SunAccumulation, which reads the board after its
// bitsReady), the caster phases, view bits and modes, and the shadow toggles. The source owns no phase or view bit and no toggle.
// Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Sun
{
	// SunFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();
}
