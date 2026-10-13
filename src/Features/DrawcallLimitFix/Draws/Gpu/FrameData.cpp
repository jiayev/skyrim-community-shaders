#include "FrameData.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include <Render/LatchBlock.h>
#endif

#include <atomic>
#include <map>

namespace DCLF
{
	namespace
	{
		using Counts = FrameData::Counts;
		// Render thread only.
		std::map<std::string, Counts, std::less<>> counts;
		std::uint64_t frames = 0;
		std::atomic<std::uint32_t> renderThread{ 0 };
	}

	void FrameData::EndFrame()
	{
		renderThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		++frames;
	}

	void FrameData::Note(std::string_view a_where, std::size_t a_bytes)
	{
		if (!a_bytes || ::GetCurrentThreadId() != renderThread.load(std::memory_order_relaxed))
			return;
		auto it = counts.find(a_where);
		if (it == counts.end())
			it = counts.emplace(std::string(a_where), Counts{}).first;
		it->second.bytes += a_bytes;
		++it->second.writes;
	}

	FrameData::Taken FrameData::Take()
	{
		if (!frames || counts.empty())
			return {};
		Taken taken{ frames, std::move(counts) };
		counts.clear();
		frames = 0;
		return taken;
	}

#if defined(CS_HAS_RENDER_GRAPH)
	void LatchWrite(const org::LatchBlock& a_block, std::string_view a_what, std::uint32_t a_slot, std::uint32_t a_offset, std::span<const std::byte> a_bytes)
	{
		a_block.Write(a_slot, a_offset, a_bytes);
		if (!a_bytes.empty()) {
			std::string where = "latch: ";
			where += a_what;
			FrameData::Note(where, a_bytes.size());
		}
	}
#endif
}
