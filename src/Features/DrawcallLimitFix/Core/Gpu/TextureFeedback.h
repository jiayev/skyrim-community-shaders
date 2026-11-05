#pragma once

#include <cstdint>

// Texture streaming's feedback buffer: one word per material slot, in host-visible memory the colour pass writes directly and the streaming
// worker reads directly, so feedback adds no pass, copy or wait to the frame. A material row's header names the buffer (DrawPipelines.h,
// MaterialRowHeader). Neither side orders with the other: a value lost to a race comes back on a later frame.
namespace DCLF::TextureFeedback
{
	// Word 0 turns the writes on (the toggle); a material slot's word follows at its slot + 1. Slots are 16 bits (the draw's input).
	inline constexpr std::uint32_t kSlots = 1u << 16;
	inline constexpr std::uint32_t kWords = kSlots + 1;
	inline constexpr std::uint32_t kEmpty = ~0u;

	// A word's value: the footprint (log2 of the UV one pixel covers), as Lighting.hlsl stores it.
	inline constexpr float kFootprintOffset = 32.0f, kFootprintScale = 256.0f;

	/** @brief Render thread: makes the buffer, once, when the render graph's host is up. */
	void Ensure();

	/** @brief Any thread: the buffer's device address (0 before Ensure, or if it could not be made). */
	std::uint64_t Address();

	/** @brief Any thread: the mapped words, kWords of them (null: no buffer). */
	volatile std::uint32_t* Words();
}
