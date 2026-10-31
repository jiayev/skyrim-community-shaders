#pragma once

// The render thread's frame clock: the frame number FrameView::BeginFrame counts (through SceneCapture::SetFrame), which the records'
// diagnostics and the features' engine sides stamp what they see with (a LOD gate's request, open and flip frames). Any thread reads it.

#include <atomic>
#include <cstdint>

namespace DCLF::FrameClock
{
	namespace Detail
	{
		inline std::atomic<std::uint32_t> frame{ 0 };
	}

	/** @brief Render thread, the frame's start (SceneCapture::SetFrame): the frame now. */
	inline void Set(std::uint32_t a_frame) { Detail::frame.store(a_frame, std::memory_order_relaxed); }
	/** @brief Any thread: the render thread's frame as it last set it (0 before the first). */
	inline std::uint32_t Frame() { return Detail::frame.load(std::memory_order_relaxed); }
}
