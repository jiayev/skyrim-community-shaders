#pragma once

// Shadows/Local: the local shadow lights' views and the focus views, drawn by Core's shadow engine. The feature registers the Spot, Point
// and Focus sources (Api::ShadowSourceContribution, Api/ShadowViews.h: the frustum, parabolic and focus views draw while their source is
// registered): the spot and point sources' lightKept (LightViews: each light the selection keeps, its views from the cameras its
// UpdateCamera set up, into Core's board, ShadowCameras::Lights), and the focus source's camera hook (FocusViews:
// BSShadowDirectionalLight::sub, which writes the focus views into ShadowCameras::Focus, from the engine's focus targets:
// EngineFocusTarget). The Point source's culls are the feature's too (LocalLightCull.h: the point lights' shadow culls without DCLF's
// entries), installed by its source hooks once the Sun's registration hooks are in (Api::ShadowSourceHooks::CullsInstalled), run by its
// frameStart (this frame's exclusion, from Core's ParabolicExclusionSlot) and frameEnd, told of the structure changes by a stand-in
// observer (Api::StandInObserver::structure); their lines are at Api::ReportPoint::PrimaryCullSources (Diagnostics/PointCullLines.cpp).
// Its parity is its own (Diagnostics/): the light and focus views' lines, the focus views' membership and the local shadow volumes, under
// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY, and the point lights' culls' (LightCulls) under CS_DCLF_PERSISTENT_PARITY.
//
// Core keeps the shadow engine: the shadow epoch and its capability and captures, the views list, the boards and the camera math
// (Core/Render/Shadow/ShadowCameras.h), the exclusions the builder publishes (SunExclusion.h), the views' tallies the shadow epochs'
// checks fill (Diagnostics::ShadowViewTally), the caster phases, view bits and modes, and the shadow toggles; the light selection (which
// calls lightKept) and the local shadows' port are Features/LocalLights'. The Sun's registration thunk calls
// LocalLightCull::NoteMainRegistration directly (Features/Shadows/Sun, listed after this feature). The sources own no phase or view bit
// and no toggle. Included by the feature's sources only.

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

	// Diagnostics/PointCullLines.cpp: the point lights' culls' lines (Api::ReportPoint::PrimaryCullSources).
	void PointCullLines(std::uint32_t a_frame);
}
