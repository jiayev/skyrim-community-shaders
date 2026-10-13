#pragma once

// The scene area's observers (Diagnostics/Scene/*.cpp): what SceneInstall.cpp registers, and what one observer shares with another.

#include "SceneView.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <utility>

namespace DCLF::Diagnostics
{
	void InstallRegistrations(bool a_residue, bool a_parity);
	void InstallCensus();
	void InstallWalkParity();
	void InstallChangeLogParity();
	void InstallResidentParity();
	void InstallSlotDrainCheck();
	void InstallFrameParity();
	void InstallCandidateParity();
	void InstallDecalOrderParity();
	void InstallLodSegmentParity();
	void InstallShadowSetParity();
	void InstallReplayParity();
	void InstallGateObservers();
	void InstallMirrorReads();
	void InstallMirrorParity();
	void InstallShadowVolumeParity();
	void InstallTreeLodParity();
	void InstallFirstPersonFiling();
	void InstallHiddenWatchArming();
	void InstallCommitParity();
	void InstallMirrorWatchPlacement();

	/** @brief The coordinator's report: the mirror parity's lines since the last call (newline-terminated), or empty. */
	std::string TakeMirrorParityLines();

}
