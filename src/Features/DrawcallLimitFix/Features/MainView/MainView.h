#pragma once

// MainView: what only the main view's draws use. Its draw passes (MainViewPasses.cpp: MainOpaquePass, the Z-prepass's draws in both phases
// and the main colour pass's, under main-view.z.depth, main-view.depth-phase2 and main-view.colour), and the main pass's points
// (MainViewPoints.cpp, the primary view contract, Api/PrimaryView.h, which Core's FrameDriver calls from the shell's hooks): the main pass's
// capture where its opaque batches start, and the colour epoch where they end, under the opaque.after trigger.
//
// Core keeps what every view shares: the frame commits (the depth commit, the colour commit, IndirectDraws::ExecuteColour), the BuildDraws
// passes, the latched copies, the index pool, the sorts, visibility, the frame constants, the z-bucket counts, the native targets and depth,
// the shapes and the revisions. The in-pass callbacks MainOpaquePass calls stay where they are: the draw families' (Api/DrawFamilies.h) and
// the main ranges' (Api/MainRanges.h). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::MainView
{
	// MainViewFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// MainViewPasses.cpp: the main view's draw passes (main-view.*), from the feature's install.
	void RegisterPasses(Api::InstallContext& a_context);

	// MainViewPoints.cpp: the main pass's points (Api::PrimaryViewContribution) and the opaque.after trigger, from the feature's install.
	void RegisterPrimaryView(Api::InstallContext& a_context);
}
