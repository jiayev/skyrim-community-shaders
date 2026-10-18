#pragma once

// Forward (CS_DCLF_FORWARD, CS_DCLF_REFRACTION; dclf-architecture.md, "The forward epoch"): DCLF's blended objects, drawn by the forward
// epoch (a GPU depth sort, back to front, before the engine's blended draws), and refraction (kRefraction, kTempRefraction), drawn into
// the refraction normals by the forward epoch's refraction range. The feature registers its switches and live toggles, the forward and
// refraction object classes' contributions (Api::ObjectClass: the forward epoch's coverage, recording and execution and the Lighting
// port's derivation read the toggles through them, as data), and the engine's two forward points (ForwardHooks.cpp): the main pass's
// blended draw, where the epoch runs (ForwardEpoch.cpp: SubmitForwardEpoch, through Core's IndirectDraws::SubmitSharedEpoch) and the
// refraction normals it drew are marked for the engine, and Main::Draw's refraction point. It owns the epoch's passes (ForwardPasses.cpp:
// forward.draw, forward.refraction), refraction's preparation and counters, and the epoch's report lines.
//
// Core keeps what blended Lighting, Features/Effects (Features/Particles and Features/Billboards through it) and Features/FirstPerson's
// forward part share: the forward and refraction ranges, the kRasterForward and refraction record bits and keys, ForwardSortCS and the
// forward sort (core.forward-sort), the shared epoch's submission, recording, coverage and counters, refraction's pipeline (DrawPipelines:
// dclf-layout.md, "Remaining architectural flaws") and normals import, DeriveForward. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Forward
{
	// ForwardFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// ForwardHooks.cpp: the main pass's blended draw (both its call sites; the forward epoch's trigger, blended.draw) and Main::Draw's
	// refraction point, from the feature's install.
	void InstallHooks(Api::InstallContext& a_context);

	// ForwardPasses.cpp: the forward epoch's passes (forward.draw, forward.refraction), from the feature's install.
	void RegisterPasses(Api::InstallContext& a_context);

	/**
	 * @brief ForwardEpoch.cpp: the forward epoch (CS_DCLF_FORWARD), at the engine's blended draw (FUN_1414b39d0, after the deferred composite and
	 * the water): the visible forward objects, sorted back to front by the colour epoch, drawn before the engine's own blended draws.
	 * Render thread; only after this frame's colour epoch, whose commit wrote everything these draws read. Returns whether this execution
	 * drew the refraction normals (the caller marks them for the engine).
	 */
	bool SubmitForwardEpoch();
	/** @brief Whether this frame's forward epoch drew the refraction normals (render thread, at Main::Draw's refraction point). */
	bool RefractionDrawnThisFrame();
	// The forward epoch's report lines (ReportPoint::AsyncWorkForward), from the feature's install.
	void RegisterReport(Api::InstallContext& a_context);
}
