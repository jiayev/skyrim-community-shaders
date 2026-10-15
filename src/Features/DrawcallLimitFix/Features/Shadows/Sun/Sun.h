#pragma once

// Shadows/Sun: the sun's shadow views, drawn by Core's shadow engine. The feature registers the Sun source (Api::ShadowSourceContribution,
// Api/ShadowViews.h: the directional views draw while it is registered) and its camera hook (SunViews: BSShadowDirectionalLight::
// UpdateCamera, which writes the sun's cascades and full-frustum planes into Core's board, ShadowCameras::Sun, from the cameras it set up).
// Its parity is its own (Diagnostics/): the sun views' line, the accumulation's cascades against the cascade culls' and the entry rule's
// planes against the full-frustum processes', under CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY.
//
// Core keeps the shadow engine: the shadow epoch and its capability and captures, the views list, the light selection, the boards and
// the camera math (Core/Render/Shadow/ShadowCameras.h), the views' tallies the shadow epochs' checks fill (Diagnostics::ShadowViewTally),
// the sun's accumulation (SunAccumulation, which reads the board after its bitsReady), the caster phases, view bits and modes, and the
// shadow toggles. The source owns no phase or view bit and no toggle. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Sun
{
	// SunFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// Diagnostics/SunViewParity.cpp, SunEntryPlanesParity.cpp: CS_DCLF_SET_PARITY's or CS_DCLF_PERSISTENT_PARITY's observers, subscribed
	// by the feature's install.
	void SubscribeSunViewParity();
	void SubscribeSunEntryPlanesParity();
}
