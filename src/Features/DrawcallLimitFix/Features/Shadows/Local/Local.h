#pragma once

// Shadows/Local: the local shadow lights' views and the focus views, drawn by Core's shadow engine. The feature registers the Spot, Point
// and Focus sources (Api::ShadowSourceContribution, Api/ShadowViews.h: the frustum, parabolic and focus views draw while their source is
// registered): the spot and point sources' lightKept (LightViews: each light the selection keeps, its views from the cameras its
// UpdateCamera set up, into Core's board, ShadowCameras::Lights), and the focus source's camera hook (FocusViews:
// BSShadowDirectionalLight::sub, which writes the focus views into ShadowCameras::Focus, from the engine's focus targets:
// EngineFocusTarget). Its parity is its own (Diagnostics/): the light and focus views' lines, the focus views' membership and the local
// shadow volumes, under CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY.
//
// Core keeps the shadow engine: the shadow epoch and its capability and captures, the views list, the light selection (LightSelection,
// which calls lightKept), the boards and the camera math (Core/Render/Shadow/ShadowCameras.h), the views' tallies the shadow epochs'
// checks fill (Diagnostics::ShadowViewTally), the caster phases, view bits and modes, and the shadow toggles; the local shadows' port
// (Scene/Ports/LocalShadows) moves with LocalLights. The sources own no phase or view bit and no toggle. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Local
{
	// LocalFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// Diagnostics/LightParity.cpp, FocusParity.cpp, ShadowVolumeParity.cpp: CS_DCLF_SET_PARITY's or CS_DCLF_PERSISTENT_PARITY's observers,
	// subscribed by the feature's install.
	void SubscribeLightParity();
	void SubscribeFocusParity();
	void SubscribeShadowVolumeParity();
}
