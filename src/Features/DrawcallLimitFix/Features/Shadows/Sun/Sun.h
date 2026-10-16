#pragma once

// Shadows/Sun: the sun's shadow views, drawn by Core's shadow engine. The feature registers the Sun source (Api::ShadowSourceContribution,
// Api/ShadowViews.h: the directional views draw while it is registered) and its source hooks: the accumulation's (SunAccumulation.h:
// Accumulate, its cascade culls and registrations, the full-frustum cull, and the entry exclusion; it reads the board after its
// bitsReady), then its camera hook (SunViews: BSShadowDirectionalLight::UpdateCamera, which writes the sun's cascades and full-frustum
// planes into Core's board, ShadowCameras::Sun, from the cameras it set up).
// The registration thunk also does the main registration's work for others: Core's (PrimaryCull's owned masks) and Features/Shadows/Local's
// (the point lights' bits, LocalLightCull::NoteMainRegistration, a static call per main registration: so this feature DEPENDS on
// Shadows/Local and is listed after it). Without this feature the point lights' culls stay off (LocalLightCull's install warns, at the
// Point source's hooks), as when the call sites fail their check.
// Its report lines are the accumulation's and the entry exclusion's (Api::ReportPoint::RenderThread), and its parity is its own
// (Diagnostics/): the sun views' line, the accumulation's cascades against the cascade culls' and the entry rule's planes against the
// full-frustum processes', under CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY, and the exclusion's watch (SunCasters) under
// CS_DCLF_PERSISTENT_PARITY.
//
// Core keeps the shadow engine: the shadow epoch and its capability and captures, the views list, the light selection, the boards and
// the camera math (Core/Render/Shadow/ShadowCameras.h), the views' tallies the shadow epochs' checks fill (Diagnostics::ShadowViewTally),
// the exclusion the builder publishes and the frame start forwards (SunExclusionSlot, over Core's EntrySpace), the caster phases, view
// bits and modes, and the shadows' and primary exclusion's toggles. The source owns no phase or view bit; its switches and toggles are the
// accumulation's (CS_DCLF_SUN_SKIP, "skip sun accumulation") and the entry exclusion's (CS_DCLF_SUN_EXCLUDE, "exclude sun entries"),
// listed where Core's were, after the shadows'; Core's "exclude primary entries" needs the second by name, so without this feature it
// never takes effect. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Sun
{
	// SunFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// Diagnostics/SunViewParity.cpp, SunEntryPlanesParity.cpp: CS_DCLF_SET_PARITY's or CS_DCLF_PERSISTENT_PARITY's observers, subscribed
	// by the feature's install.
	void SubscribeSunViewParity();
	void SubscribeSunEntryPlanesParity();

	// Diagnostics/SunAccumulationReport.cpp: the accumulation's and the entry exclusion's lines (Api::ReportPoint::RenderThread).
	void SunAccumulationLines(std::uint32_t a_frame);
}
