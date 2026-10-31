#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace org
{
	class LatchBlock;
}

namespace DCLF
{
	/**
	 * @brief The bytes the render thread writes for the GPU each frame, by where they go: what the frame's epochs carry through the
	 * render thread (dclf-architecture.md, "Frame data through the render thread"). Fed at the three ways a commit writes
	 * per-frame data - a latched copy (LatchedUploads: "latched: <target>", the frame-constants buffer by slot), a staged upload
	 * (CommitUploads: "staged: <target>") and a latch block's write (LatchWrite: "latch: <what>") - so nothing the render thread
	 * sends is left out. What a worker or the pool sends (staged batches, FrameValues) is not the render thread's and is not counted.
	 */
	namespace FrameData
	{
		/** @brief Render thread, at Present: a frame ended (the thread that calls it is the one counted). */
		void EndFrame();
		/** @brief a_bytes written by the render thread to a_where this frame; ignored on any other thread. */
		void Note(std::string_view a_where, std::size_t a_bytes);
		/** @brief What a frame's writes to one place came to. */
		struct Counts
		{
			std::uint64_t bytes = 0, writes = 0;
		};
		/** @brief The counts since the last take, by where, over a number of frames. */
		struct Taken
		{
			std::uint64_t frames = 0;
			std::map<std::string, Counts, std::less<>> counts;
		};
		/** @brief The report's take (Reports/DrawsFrameValues.cpp): the counts, reset; nothing (and nothing reset) before a frame wrote. */
		Taken Take();
	}

	/** @brief org::LatchBlock::Write, counted under "latch: a_what" (FrameData). */
	void LatchWrite(const org::LatchBlock& a_block, std::string_view a_what, std::uint32_t a_slot, std::uint32_t a_offset, std::span<const std::byte> a_bytes);
	template <class T>
	void LatchWriteValue(const org::LatchBlock& a_block, std::string_view a_what, std::uint32_t a_slot, std::uint32_t a_offset, const T& a_value)
	{
		LatchWrite(a_block, a_what, a_slot, a_offset, std::as_bytes(std::span<const T, 1>(&a_value, 1)));
	}
}
