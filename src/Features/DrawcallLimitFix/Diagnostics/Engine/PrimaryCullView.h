#pragma once

#include "Features/DrawcallLimitFix/Engine/Cull/PrimaryCull.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief The engine observers' read-only view of PrimaryCull's private state (PrimaryCull befriends it). Render thread unless a
	 * function says otherwise, at the observer's hook point; a list job reads the cut only between PrepareFrame and the jobs' Finish.
	 */
	struct PrimaryCullView
	{
		static const PrimaryCull& Self() { return PrimaryCull::Get(); }
		/** @brief The cut: candidates, plans, roots, members, memberObject, admitted, mixed, walk, eligible, rejectReason, mixedBy, processCount. */
		static const auto& Cut() { return Self().cut; }
		/** @brief Whether a plan is a fade root's (BSFadeNode, BSLeafAnimNode, BSTreeNode). */
		static bool FadePlan(std::uint32_t a_entry)
		{
			const auto plan = Self().cut.plans[a_entry];
			return plan == PrimaryCull::EntryPlan::FadeRoot || plan == PrimaryCull::EntryPlan::LeafRoot || plan == PrimaryCull::EntryPlan::TreeRoot;
		}
		static bool Rejected(std::uint32_t a_entry) { return Self().cut.plans[a_entry] == PrimaryCull::EntryPlan::Rejected; }
		/** @brief The cut planned whole for its snapshot, the cut itself left as it was (PrimaryCull::PlanWholeCut). */
		static auto PlanWholeCut() { return PrimaryCull::Get().PlanWholeCut(); }
		/** @brief DCLF's enumeration of the object root's roots (PrimaryCull::ListRoots). The build's job. */
		static bool ListRoots(const RE::NiAVObject*& a_objectRoot, std::vector<const RE::NiAVObject*>& a_roots) { return PrimaryCull::ListRoots(a_objectRoot, a_roots); }
		/** @brief The cut's frames since its counters were last taken. */
		static std::uint64_t CutFrames() { return Self().cutStats.frames; }
		/** @brief This frame's snapshot is current: the stand-in runs. */
		static bool StandInLive() { return Self().standInLive; }
		/** @brief Every entry walked this frame (Policy::WalkEveryEntry). */
		static bool WalkEverything() { return Self().walkEverything; }
		/** @brief This frame's members in view under stood-in entries (AfterListJobs). */
		static const auto& FrameVisible() { return Self().frameVisible; }
		/** @brief The frame's set (PrepareFrame), or null. */
		static const auto& FrameSet() { return Self().frameSet; }
		/** @brief Whether the cut stands in for a_node whole this frame: admitted, nothing engine-drawn (the engine's cull does not write its bit). */
		static bool StoodInWhole(const void* a_node);
		/** @brief The stood-in fade roots. */
		static const std::vector<const void*>& StandInList() { return Self().standInList; }
		/** @brief The main camera BeginFrameCamera read this frame. */
		static const void* FrameCamera() { return Self().frameCamera; }
	};
}
