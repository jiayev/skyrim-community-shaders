#include "Draws.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#	include "Features/DrawcallLimitFix/Draws/Gpu/FrameData.h"
#	include "Features/DrawcallLimitFix/Draws/Gpu/FrameValues.h"

// The frame values' and the frame data's report lines.

namespace DCLF::Reports
{
	std::string FrameValuesLine()
	{
		const auto c = FrameValues::Get().TakeCounters();
		if (!c.frames)
			return {};
		const double n = static_cast<double>(c.frames);
		auto line = fmt::format("[DCLF] frame values: {} frames; a frame {:.0f} items sampled ({:.1f} written slots kept for their transforms to settle), {:.1f} fade roots seeded, {:.1f} rows changed, {:.0f} skins ({:.1f} palette rows changed), "
								"{:.1f} shading items sampled ({:.1f} rows changed, {:.1f} wetness changed), "
								"{:.1f} runs ({:.1f} KB) sent; {:.0f} us sampling, {:.0f} us waiting for a buffer ({} waits), {:.0f} us sending, at most {} us; "
								"{} refused leases, {} palette size defects, {} failures",
			c.frames, c.sampled / n, c.settled / n, c.seeded / n, c.rowsChanged / n, c.skins / n, c.paletteRowsChanged / n, c.shadingSampled / n, c.shadingChanged / n,
			c.wetnessChanged / n, c.runs / n, c.bytes / n / 1024.0, c.sampleUs / n, c.pointWaitUs / n, c.pointWaits, c.uploadUs / n, c.maxTotalUs, c.refused, c.defects,
			c.failures);
		if (auto& s = Diagnostics::DrawsState::Get().frameValues; s.checks) {
			line += fmt::format("; parity against the engine: {} rows checked, {} differ{}{} ({} written this frame, the next frame's); {} palettes checked, {} differ{}{}",
				s.checks, s.differs, s.differs ? " <- DIFFER; first: " : " <- OK", s.differs ? s.first : std::string(), s.fresh, s.paletteChecks, s.paletteDiffers,
				s.paletteDiffers ? " <- DIFFER; first: " : " <- OK", s.paletteDiffers ? s.paletteFirst : std::string());
			s = {};
		}
		return line;
	}

	std::string FrameDataLine()
	{
		auto taken = FrameData::Take();
		if (!taken.frames)
			return {};
		const double n = static_cast<double>(taken.frames);
		std::vector<std::pair<const std::string*, FrameData::Counts>> sorted;
		std::uint64_t total = 0, writes = 0;
		for (const auto& [where, c] : taken.counts) {
			sorted.emplace_back(&where, c);
			total += c.bytes;
			writes += c.writes;
		}
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
		auto line = fmt::format("[DCLF] frame data through the render thread: {:.1f} KB a frame in {:.0f} writes over {} frames; by where:", total / n / 1024.0,
			writes / n, taken.frames);
		for (const auto& [where, c] : sorted)
			line += fmt::format(" {} {:.2f} KB ({:.1f} writes);", *where, c.bytes / n / 1024.0, c.writes / n);
		line.pop_back();
		return line;
	}
}
#else
namespace DCLF::Reports
{
	std::string FrameValuesLine() { return {}; }
	std::string FrameDataLine() { return {}; }
}
#endif
