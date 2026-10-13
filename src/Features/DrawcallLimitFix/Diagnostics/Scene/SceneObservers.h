#pragma once

#include "SceneView.h"
#include "Features/DrawcallLimitFix/Engine/Cull/PassCapture.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <utility>

// The scene area's observers (Diagnostics/Scene/*.cpp): what SceneInstall.cpp registers, and what one observer shares with another.

namespace DCLF::Diagnostics
{
	void InstallTimeline();
	void InstallRegistrations(bool a_residue, bool a_parity, bool a_decalOrderProbe);
	void InstallCensus();
	void InstallWalkParity();
	void InstallChangeLogParity();
	void InstallResidentParity();
	void InstallSlotProbe();
	void InstallSlotDrainCheck();
	void InstallFrameParity();
	void InstallCandidateParity();
	void InstallDecalOrderParity();
	void InstallLodSegmentParity();
	void InstallShadowSetParity();
	void InstallReplayParity();
	void InstallGateObservers();
	void InstallFanoutParity();
	void InstallMirrorReads();
	void InstallMirrorParity();
	void InstallShadowVolumeParity();
	void InstallTreeLodParity();
	void InstallFirstPersonFiling();
	void InstallLightMaskCheck();
	void InstallHiddenWatchArming();
	void InstallCommitParity();
	void InstallMirrorWatchPlacement();

	/**
	 * @brief CS_DCLF_DECAL_ORDER_PROBE: the main registration's decal chains against the order the scene lists give them. Render thread,
	 * once a frame, with the drained capture.
	 */
	void ProbeDecalOrder(std::span<const PassCapture::Entry> a_entries, const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>& a_main);

	/** @brief CS_DCLF_COVERAGE_PROBE, the coordinator's report: the coverage census's lines (newline-separated). */
	std::string CoverageCensus(const Scene::Coordinator& a_store);

	/** @brief The coordinator's report: the mirror parity's lines since the last call (newline-terminated), or empty. */
	std::string TakeMirrorParityLines();

	/** @brief The timeline's: the last show on a_geometry's chain up to its category node {frame, store}, or {0, ~0u}; and whether the chain is hidden now. */
	std::pair<std::uint32_t, std::uint32_t> LastShowOnChain(const Scene::Coordinator& a_store, const RE::BSGeometry& a_geometry, const SceneView::Tracked& a_entry,
		bool& a_hiddenNow);
}
