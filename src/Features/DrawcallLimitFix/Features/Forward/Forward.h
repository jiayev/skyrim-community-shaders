#pragma once

// Forward (CS_DCLF_FORWARD, CS_DCLF_REFRACTION; drawcall-limit-fix.md, "The forward epoch"): DCLF's blended objects, drawn by the forward
// epoch (a GPU depth sort, back to front, before the engine's blended draws), and refraction (kRefraction, kTempRefraction), drawn into
// the refraction normals by the forward epoch's refraction range. The feature registers its switches and live toggles, the forward and
// refraction object classes' contributions (Api::ObjectClass: the forward epoch's coverage, recording and execution and the Lighting
// port's derivation read the toggles through them, as data), and the engine's two forward points (ForwardHooks.cpp): the main pass's
// blended draw, where the epoch runs (IndirectDraws::ExecuteForward) and the refraction normals it drew are marked for the engine, and
// Main::Draw's refraction point.
//
// Core keeps the forward epoch itself, which blended Lighting, Features/Effects (Features/Particles and Features/Billboards through it)
// and Features/FirstPerson's forward part share: the forward and refraction ranges, the kRasterForward and refraction record bits and
// keys, ForwardSortCS, the passes (Forward.cpp), the epoch's recording, coverage and stats, refraction's pipeline and normals import,
// DeriveForward and the engine addresses (EngineAccess.h). Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Forward
{
	// ForwardFeature.cpp: the feature's entry (Features/FeatureList.cpp).
	Api::Feature Feature();

	// ForwardHooks.cpp: the main pass's blended draw (both its call sites; the forward epoch's trigger, blended.draw) and Main::Draw's
	// refraction point, from the feature's install.
	void InstallHooks(Api::InstallContext& a_context);
}
