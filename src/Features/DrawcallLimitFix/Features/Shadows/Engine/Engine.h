#pragma once

// Shadows/Engine: the shadow engine's epoch half, the shadow and occlusion epochs. Their passes (ShadowEpochPasses.cpp: the latched copies,
// the index pool's copies, the culling, the views' draws and the culling counters' readback, under shadows.* and shadows.sky.*), and their
// frame points (ShadowEpochPoints.cpp, the shadow epochs contract, Api/ShadowEpochs.h, which Core's FrameDriver calls from the shell's
// hooks): the frame's shadow views' coverage and the shadow frame begun, the shadow epoch under shadow-maps.after, the occlusion maps'
// readiness and their epoch under occlusion.draw. Shadows/Local, Shadows/Sun and OcclusionMaps build on it (DEPENDS: listed after it).
//
// Core keeps the scene side: the shadow engine's revision participant (requirements, latch, shapes, payload), the shadow table columns, the
// walk's caster verdicts, PassCapture's withholding, the views list the capture builds (Handoff/ShadowViews, listed at BeforeShadowMaps by
// Core's ShadowPoints), the captures at the shell's FinishAccumulating hook, the shadow pipelines (published into the shapes), the shadow
// extension's resources and their registration, and the epochs' commits (IndirectDraws::ExecuteShadowFrame and ExecuteOcclusion, with their
// counters and checks), which the handlers call. Its namespace hides DCLF::Engine (the engine access) from DCLF::Features::Shadows: a
// source there that includes this header names the engine access DCLF::Engine. Included by the feature's sources and the feature list only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Shadows::Engine
{
	// EngineFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// ShadowEpochPasses.cpp: the shadow and occlusion epochs' passes (shadows.*), from the feature's install.
	void RegisterPasses(Api::InstallContext& a_context);

	// ShadowEpochPoints.cpp: the epochs' frame points (Api::ShadowEpochsContribution) and their triggers (shadow-maps.after,
	// occlusion.draw), from the feature's install.
	void RegisterShadowEpochs(Api::InstallContext& a_context);
}
