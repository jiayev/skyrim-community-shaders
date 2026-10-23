#pragma once

// TEMP (fade-in investigation, CS_FADE_PROBE=1): the engine's fade steps and the stipple techniques begun, logged every 300 frames
// by Hooks' BSShader_BeginTechnique.

#include <atomic>
#include <cstdint>

namespace DCLF::Diagnostics::FadeProbe
{
	struct Counters
	{
		std::atomic<std::uint64_t> stepped{ 0 };       // a fade update or OnVisible left currentFade strictly between 0 and 1
		std::atomic<std::uint64_t> steppedNoDoor{ 0 };  // of them, with the screen-door byte (+0x154) 0
		std::atomic<std::uint64_t> snappedIn{ 0 };      // currentFade went from at most 0 to 1 in one call
		std::atomic<std::uint64_t> stippleTechniques{ 0 };  // Lighting techniques begun with AdditionalAlphaMask in the pixel descriptor
		std::atomic<std::uint64_t> lightingTechniques{ 0 };
	};
	inline Counters counters;

	inline void NoteFade(float a_before, float a_after, std::uint8_t a_door)
	{
		if (a_after > 0.0f && a_after < 1.0f) {
			counters.stepped.fetch_add(1, std::memory_order_relaxed);
			if (!a_door)
				counters.steppedNoDoor.fetch_add(1, std::memory_order_relaxed);
		} else if (a_before <= 0.0f && a_after >= 1.0f) {
			counters.snappedIn.fetch_add(1, std::memory_order_relaxed);
		}
	}
}
