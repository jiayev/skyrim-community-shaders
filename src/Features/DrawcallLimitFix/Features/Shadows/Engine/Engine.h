#pragma once

// Shadows/Engine: the shadow engine's epoch half, the shadow epoch, which draws the occlusion maps with the shadow views. Its passes
// (ShadowEpochPasses.cpp: the latched copies, the index pool's copies, the culling, the views' draws and the culling counters' readback,
// under shadows.*), its frame points (ShadowEpochPoints.cpp, the shadow epochs contract, Api/ShadowEpochs.h, which Core's FrameDriver calls
// from the shell's hooks and the feature's own): the frame's shadow views' coverage, decided before the engine's shadow lights register, the
// shadow frame begun, the occlusion maps' readiness (at the shadow maps, before the engine draws them) and the shadow epoch under
// shadow-maps.after, with the maps taken; and the hooks in the engine's shadow-light selection that reach FrameDriver there
// (ShadowCasterPoints.cpp). Shadows/Local, Shadows/Sun and OcclusionMaps build on it (DEPENDS: listed after it).
//
// Core keeps the scene side: the shadow engine's revision participant (requirements, latch, shapes, payload), the shadow table columns, the
// walk's caster verdicts, PassCapture's withholding, the views list the capture builds (Handoff/ShadowViews, listed in the engine's
// shadow-light selection by Core's ShadowPoints), the captures at the shell's FinishAccumulating hook, the shadow pipelines (published into the shapes), the shadow
// extension's resources and their registration, and the epoch's commit (IndirectDraws::ExecuteShadowFrame, the occlusion maps' views
// latched with the shadow views', with its counters and checks), which the handlers call. Its namespace hides DCLF::Engine (the engine
// access) from DCLF::Features::Shadows: a source there that includes this header names the engine access DCLF::Engine. Included by the
// feature's sources and the feature list only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Engine
{
	// EngineFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// ShadowEpochPasses.cpp: the shadow epoch's passes (shadows.*), from the feature's install.
	void RegisterPasses(Api::InstallContext& a_context);

	// ShadowEpochPoints.cpp: the epoch's frame points (Api::ShadowEpochsContribution) and its trigger (shadow-maps.after), from the
	// feature's install.
	void RegisterShadowEpochs(Api::InstallContext& a_context);

	// ShadowCasterPoints.cpp: the engine's shadow-light selection hooked (FrameDriver's shadow casters' points), from the feature's install.
	void InstallCasterHooks();
}
