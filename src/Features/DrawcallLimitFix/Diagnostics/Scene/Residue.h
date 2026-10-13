#pragma once

#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <cstdint>
#include <string>

namespace DCLF::Diagnostics
{
	/**
	 * @brief The reflection residue (Residue.cpp): what the engine registered into the reflection faces under the LOD land and objects
	 * roots that is no reflection member, classified by the scene pass that takes the frame's drain. Watched under the set or persistent
	 * parity and the timeline (IndirectDraws::ReflectionRootsOwned); the reflection report takes the classes.
	 */
	inline constexpr std::size_t kResidueUntracked = static_cast<std::size_t>(Ineligible::Count), kResidueUnbound = kResidueUntracked + 1,
								 kResidueNotReflection = kResidueUntracked + 2, kResidueReflection = kResidueUntracked + 3, kResidueKinds = kResidueUntracked + 4;
	inline constexpr std::size_t kResidueAgeBuckets = 8;
	/** @brief The furthest stage a residue geometry reached (CS_DCLF_TIMELINE). */
	enum ResidueStage : std::uint8_t
	{
		kStageUntracked,
		kStageIneligible,     // a verdict other than hidden
		kStageHiddenNow,      // hidden, and its chain is hidden now
		kStageHiddenStale,    // hidden, and its chain is shown now: a show the verdict was not taken again for
		kStageNoRecord,       // eligible, no record
		kStageUnbound,        // a record, not bound
		kStageWaiting,        // bound, waiting for readiness (waitingBy)
		kStageNotReflection,  // bound, not waiting, no reflection phase
		kStageApplied,        // a reflection member in the coordinator's tables, not installed yet
		kStageUngated,        // shown since, not ready, and no LOD gate ever tagged it (a show the gates do not cover)
		kStageCount
	};
	inline constexpr std::array<const char*, kStageCount> kStageNames{ "untracked", "ineligible", "hidden", "hidden, shown since", "no record", "a record, unbound",
		"bound, waiting", "bound, no reflection phase", "a member, not installed", "shown with no gate" };
	struct ResidueClasses
	{
		std::array<std::uint64_t, kResidueKinds> counts{};
		std::array<std::string, kResidueKinds> first;
		struct Seen
		{
			std::uint32_t frame = ~0u, frames = 0;
		};
		ankerl::unordered_dense::map<const void*, Seen> seen;
		std::uint64_t persistent = 0;
		// CS_DCLF_TIMELINE: by the furthest stage reached, with the frames since it was tracked (Scene::kAgeBuckets).
		std::array<std::uint64_t, kStageCount> stages{};
		std::array<std::array<std::uint64_t, kResidueAgeBuckets>, kStageCount> ages{};
		std::array<std::string, kStageCount> stageFirst;
		std::array<std::uint64_t, 14> waitingBy{};  // kStageWaiting: Coordinator::SetStats::waitingBy's reasons
		std::array<std::uint64_t, 6> unboundBy{};   // kStageUnbound: its last membership pass's failure (0: none failed)
		// kStageUnbound: the accumulate join's verdict that left it without bindings since its record (Tracked::accumulateReason).
		std::array<std::uint64_t, static_cast<std::size_t>(Ineligible::Count)> unboundJoin{};
		// kStageHiddenStale: the store of the last show on its chain (HiddenStoreSiteAt's index; ~0u: none seen), and the frames since.
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> staleSites;
		std::array<std::uint64_t, kResidueAgeBuckets> sinceShow{};
	};
	/** @brief Render thread, the reflection report: the classes since the last call. */
	ResidueClasses TakeResidueClasses();
}
