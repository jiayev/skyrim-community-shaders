// FrameData: the render thread's per-frame write counters (see the header).

#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"

#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <Render/LatchBlock.h>

#include <atomic>
#include <cstring>
#include <map>
#include <vector>

namespace DCLF
{
	namespace
	{
		using Counts = FrameData::Counts;
		// Render thread only.
		std::map<std::string, Counts, std::less<>> counts;
		std::uint64_t frames = 0;
		std::atomic<std::uint32_t> renderThread{ 0 };

		// The names seen since the last Take, in front of the map: most writes name a string literal, so the pointer matches and no
		// string is made. The text is compared too, so a buffer reused for another name never counts against the wrong one.
		struct Seen
		{
			const char* data;
			std::string name;  // without the prefix
			bool latch;
			Counts* counts;  // a node of `counts` (std::map nodes stay put)
		};
		std::vector<Seen> seen;

		Counts& CountsOf(std::string_view a_where, bool a_latch)
		{
			for (auto& entry : seen)
				if (entry.latch == a_latch && entry.name.size() == a_where.size() &&
					(entry.data == a_where.data() || std::memcmp(entry.name.data(), a_where.data(), a_where.size()) == 0)) {
					entry.data = a_where.data();
					return *entry.counts;
				}
			std::string key = a_latch ? "latch: " : "";
			key += a_where;
			auto it = counts.find(key);
			if (it == counts.end())
				it = counts.emplace(std::move(key), Counts{}).first;
			seen.push_back({ a_where.data(), std::string(a_where), a_latch, &it->second });
			return it->second;
		}

		void Count(std::string_view a_where, bool a_latch, std::size_t a_bytes)
		{
			if (!a_bytes || !FrameData::Enabled() || ::GetCurrentThreadId() != renderThread.load(std::memory_order_relaxed))
				return;
			auto& c = CountsOf(a_where, a_latch);
			c.bytes += a_bytes;
			++c.writes;
		}
	}

	bool FrameData::Enabled()
	{
		return Diagnostics::Observe().stats;
	}

	void FrameData::EndFrame()
	{
		renderThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		++frames;
	}

	void FrameData::Note(std::string_view a_where, std::size_t a_bytes)
	{
		Count(a_where, false, a_bytes);
	}

	FrameData::Taken FrameData::Take()
	{
		if (!frames || counts.empty())
			return {};
		Taken taken{ frames, std::move(counts) };
		counts.clear();
		seen.clear();
		frames = 0;
		return taken;
	}

	void LatchWrite(const org::LatchBlock& a_block, std::string_view a_what, std::uint32_t a_slot, std::uint32_t a_offset, std::span<const std::byte> a_bytes)
	{
		a_block.Write(a_slot, a_offset, a_bytes);
		Count(a_what, true, a_bytes.size());
	}
}
