#pragma once

// Fades' records beside Core's root-state rows (Core/Scene/Model/Records.h: FadeNodeState, FadeRootStatic, FadeFrame, the kFadeVerdict* and
// kFadeRoot* bits, which the cull reads): the animation job's inputs the fade writers record, the parity's log entry and the write-back's
// events.

#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

#include <cstdint>

namespace DCLF
{
	/** @brief One animation update's inputs, as the fade writers record them (FadeWriters.cpp; FadeFrame anim*). */
	struct AnimatedFadeInputs
	{
		float eye[3]{};
		float lodAdjust = 0.0f;
		std::int32_t counter = 0;
		float deltaTime = 0.0f;
		std::uint32_t valid = 0;
		std::uint32_t varied = 0;  // a later update of the batch had other inputs (the first's stand for all)
	};

	/** @brief CS_DCLF_FADE_PARITY: one root's update as FadeStateCS made it, for the C++ port to make again. */
	struct FadeLogEntry
	{
		FadeNodeState before;
		FadeNodeState after;
		float centre[3]{};
		std::uint32_t root = ~0u;
	};
	static_assert(sizeof(FadeLogEntry) == 112);
	inline constexpr std::uint32_t kFadeLogEntries = 64;

	/**
	 * @brief The fade write-back (dclf-architecture.md, "The fade write-back"): a stood-in root whose fade crossed a milestone
	 * (faded out, fading, faded in) or whose leaf LOD level or transition changed. Appended after a header (count, the depth
	 * commit's scene frame - always written - and two of padding); the host writes them onto the nodes in frame order.
	 */
	struct FadeEvent
	{
		std::uint32_t root = 0;        // the root slot
		std::uint32_t generation = 0;  // its static row's: a slot listed again since is not the event's
		std::uint32_t flags = 0;       // the state's (kFadeFlagMask)
		float currentFade = 0.0f;
		std::uint32_t levels = 0;      // the state's +0x152 | +0x153 << 8 (FadeNodeState::levels)
		float blend = 0.0f;            // +0x14C, the LOD cross-fade
		std::uint32_t padding[2]{};
	};
	static_assert(sizeof(FadeEvent) == 32);
	inline constexpr std::uint32_t kFadeEventWords = sizeof(FadeEvent) / sizeof(std::uint32_t);
	inline constexpr std::uint32_t kFadeEventHeaderWords = 4;
	inline constexpr std::uint32_t kInitialFadeEvents = 1024;
}
