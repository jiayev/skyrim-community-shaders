#include "FrameData.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include <Render/LatchBlock.h>
#endif

#include <algorithm>
#include <atomic>
#include <map>
#include <vector>

namespace DCLF
{
	namespace
	{
		struct Counts
		{
			std::uint64_t bytes = 0, writes = 0;
		};
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

	std::string FrameData::Report()
	{
		if (!frames || counts.empty())
			return {};
		const double n = static_cast<double>(frames);
		std::vector<std::pair<const std::string*, Counts>> sorted;
		std::uint64_t total = 0, writes = 0;
		for (const auto& [where, c] : counts) {
			sorted.emplace_back(&where, c);
			total += c.bytes;
			writes += c.writes;
		}
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
		auto line = fmt::format("[DCLF] frame data through the render thread: {:.1f} KB a frame in {:.0f} writes over {} frames; by where:", total / n / 1024.0,
			writes / n, frames);
		for (const auto& [where, c] : sorted)
			line += fmt::format(" {} {:.2f} KB ({:.1f} writes);", *where, c.bytes / n / 1024.0, c.writes / n);
		line.pop_back();
		counts.clear();
		frames = 0;
		return line;
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
