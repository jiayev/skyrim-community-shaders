#pragma once

// The skin partitions about to be replaced outside the engine's writers.
//
// RaceMenu's body morphs and face sculpting (skee's BodyMorphInterface and FaceMorphInterface, called by RaceMenu itself, BodyGen,
// Papyrus and plugins such as OBody) deep-copy a geometry's NiSkinPartition with NiObject::CreateDeepCopy, write the morphed vertices
// into the copy's new buffers, and then store the copy straight into NiSkinInstance::skinPartition on the main thread. That happens
// at once, or in an SKSE task a frame or more later when the call came from another thread. No engine writer runs and skee sends no
// event.
//
// FrameValues' SkinPartitionMoved compares the live partition with the planned one, but only for the skins it samples (the ones that
// move). A detour on CreateDeepCopy names each partition being copied. For a few seconds after, FrameValues samples every kept item
// that holds that partition, moving or not, so a morphed corpse, paralyzed actor or mannequin draws its new shape too.
//
// Engine deep copies are rare (skee's morphs, a few loaders), so the detour costs nothing in steady state. The producer reads one
// relaxed atomic while nothing is watched.

#include <cstdint>
#include <vector>

namespace DCLF::SkinPartitionWatch
{
	/** @brief PostPostLoad (InstallSceneEvents): the CreateDeepCopy detour. */
	void Install();

	/** @brief Any thread: true while some partition copied in the last few seconds is watched. One relaxed load. */
	bool Any();

	/**
	 * @brief The FrameValues producer: the watched partitions (never dereferenced: compared with the plan's), in a_out. Expired
	 * ones are dropped first.
	 */
	void Collect(std::vector<const void*>& a_out);

	/** @brief Counters for the report and the capture: partitions watched since start, and how many are watched now. */
	struct Counts
	{
		std::uint64_t copies = 0;
		std::uint32_t watched = 0;
	};
	Counts GetCounts();
}
